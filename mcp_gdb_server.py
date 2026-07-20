# mcp_gdb_server.py
# MCP stdio server that drives GDB/MI2 and connects to a running QEMU gdbstub.
# Tools: connect, send commands, breakpoints, stepping, memory, registers, detach/quit.

import asyncio
import os
import sys
import time
import threading
import subprocess
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple

from fastmcp.server import FastMCP
from mcp.types import TextContent

# ----------------------------
# Minimal GDB/MI2 controller
# ----------------------------

@dataclass
class GdbState:
    proc: Optional[subprocess.Popen] = None
    stdout_thread: Optional[threading.Thread] = None
    stderr_thread: Optional[threading.Thread] = None
    lock: threading.Lock = threading.Lock()
    outq: "asyncio.Queue[str]" = None  # lines from stdout
    errq: "asyncio.Queue[str]" = None
    token: int = 1
    live: bool = False
    connected: bool = False

STATE = GdbState(outq=asyncio.Queue(), errq=asyncio.Queue())

def start_gdb(gdb_path: str = "gdb", extra_args: Optional[List[str]] = None) -> None:
    if STATE.proc and STATE.proc.poll() is None:
        return
    args = [gdb_path, "--interpreter=mi2"]
    if extra_args:
        args.extend(extra_args)

    proc = subprocess.Popen(
        args,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,  # line-buffered
    )
    STATE.proc = proc
    STATE.live = True

    try:
        loop = asyncio.get_running_loop()
    except RuntimeError as exc:  # pragma: no cover - defensive; usage is always from async context
        raise RuntimeError("start_gdb() must be invoked while an event loop is running") from exc

    def submit_line(target_queue: "asyncio.Queue[str]", value: str) -> None:
        asyncio.run_coroutine_threadsafe(target_queue.put(value), loop)

    def pump(stream, queue: "asyncio.Queue[str]", name: str):
        for line in iter(stream.readline, ''):
            # Drop trailing newlines; MI uses \n line framing
            submit_line(queue, line.rstrip("\r\n"))
        submit_line(queue, f"[{name} closed]")

    STATE.stdout_thread = threading.Thread(target=pump, args=(proc.stdout, STATE.outq, "stdout"), daemon=True)
    STATE.stderr_thread = threading.Thread(target=pump, args=(proc.stderr, STATE.errq, "stderr"), daemon=True)
    STATE.stdout_thread.start()
    STATE.stderr_thread.start()

def stop_gdb() -> None:
    if STATE.proc and STATE.proc.poll() is None:
        try:
            STATE.proc.stdin.write("-gdb-exit\n")
            STATE.proc.stdin.flush()
        except Exception:
            pass
        try:
            STATE.proc.terminate()
        except Exception:
            pass
    STATE.live = False
    STATE.connected = False

def next_token() -> int:
    with STATE.lock:
        t = STATE.token
        STATE.token += 1
        return t

async def send_mi(cmd: str, timeout: float = 5.0) -> Dict[str, Any]:
    """
    Send an MI command (without leading '-') and collect the result record for our token.
    Returns a dict with keys: 'ok' (bool), 'token', 'class', 'payload', 'console', 'log', 'stream'
    """
    if not STATE.proc or STATE.proc.poll() is not None:
        raise RuntimeError("GDB process is not running")

    tok = next_token()
    line = f"{tok}-{cmd}\n"
    try:
        STATE.proc.stdin.write(line)
        STATE.proc.stdin.flush()
    except Exception as e:
        raise RuntimeError(f"GDB stdin error: {e}")

    # Collect lines until we see "^<class>" with matching token, and a "(gdb)" prompt marker ("(gdb)")
    # MI result records look like: "<token>^done[,payload]"
    deadline = time.time() + timeout
    result: Dict[str, Any] = {"ok": False, "token": tok, "class": "", "payload": {}, "console": [], "log": [], "stream": []}
    while time.time() < deadline:
        try:
            ln = await asyncio.wait_for(STATE.outq.get(), timeout=timeout)
        except asyncio.TimeoutError:
            break

        # Stream records:
        # ~"console text", @"target", &"log"
        if ln.startswith("~"):
            result["console"].append(strip_mi_string(ln[1:]))
            continue
        if ln.startswith("@"):
            result["stream"].append(strip_mi_string(ln[1:]))
            continue
        if ln.startswith("&"):
            result["log"].append(strip_mi_string(ln[1:]))
            continue

        # Prompt (plain GNU prompt "^(gdb)" appears on a bare line in MI as "(gdb)")
        if ln.strip() == "(gdb)":
            # ignore; we'll return on token match
            continue

        # Result record: "<tok>^done", "<tok>^error,msg=..."; optional ",payload"
        # Also async notify: "*stopped,reason=..."; we collect but don't end on it unless it's our token (no token on async)
        if ln.startswith(f"{tok}^"):
            # e.g. "7^done" or "7^error,msg="..."
            cls, payload = split_result_class_payload(ln[len(f"{tok}^"):])
            result["class"] = cls
            result["payload"] = parse_mi_map(payload) if payload else {}
            result["ok"] = (cls == "done") or (cls == "running")
            return result

        # Async notify records like "*stopped,..."
        # Record for info; not tied to token.
        # We don’t block on these.
        # =event,...
        # *stopped,reason="breakpoint-hit", ...
        # =thread-created,...
        # These can be useful to surface out-of-band state; stash in log.
        if ln.startswith("*") or ln.startswith("="):
            result["log"].append(ln)
            continue

        # Anything else—keep for diagnostics
        result["log"].append(ln)

    return result

def strip_mi_string(s: str) -> str:
    # MI quoted C-string:  "text with escapes\n"
    s = s.strip()
    if s and s[0] == '"' and s[-1:] == '"':
        s = s[1:-1]
    return s.encode('utf-8', 'backslashreplace').decode('unicode_escape')

def split_result_class_payload(s: str) -> Tuple[str, str]:
    # split at first comma (class, payload)
    idx = s.find(",")
    if idx == -1:
        return s, ""
    return s[:idx], s[idx+1:]

def parse_mi_map(s: str) -> Any:
    """
    Parse a very small MI value subset:
      tuple: {key=value[,key=value]*}
      list:  [elem,elem,...]
      string: "..."
      const:  bareword
    Returns Python dict/list/str.
    """
    # Recursive descent over a simple grammar; enough for typical -break-insert/-data-/^done payloads
    s = s.strip()
    def parse_value(i: int) -> Tuple[Any, int]:
        if i >= len(s): return "", i
        c = s[i]
        if c == '"':  # string
            j = i + 1
            buf = []
            esc = False
            while j < len(s):
                ch = s[j]
                if esc:
                    buf.append(ch)
                    esc = False
                elif ch == '\\':
                    esc = True
                elif ch == '"':
                    return strip_mi_string('"' + ''.join(buf) + '"'), j + 1
                else:
                    buf.append(ch)
                j += 1
            return ''.join(buf), j
        if c == '{':  # tuple -> dict
            i += 1
            m: Dict[str, Any] = {}
            first = True
            while i < len(s) and s[i] != '}':
                if not first and s[i] == ',':
                    i += 1
                first = False
                # key=value
                kstart = i
                while i < len(s) and s[i] not in '=':
                    i += 1
                key = s[kstart:i]
                i += 1  # skip '='
                val, i = parse_value(i)
                m[key] = val
            if i < len(s) and s[i] == '}': i += 1
            return m, i
        if c == '[':  # list
            i += 1
            arr = []
            first = True
            while i < len(s) and s[i] != ']':
                if not first and s[i] == ',':
                    i += 1
                first = False
                val, i = parse_value(i)
                arr.append(val)
            if i < len(s) and s[i] == ']': i += 1
            return arr, i
        # bareword till ,}] end
        j = i
        while j < len(s) and s[j] not in ',}]':
            j += 1
        return s[i:j], j

    val, _ = parse_value(0)
    return val

# ----------------------------
# High-level GDB helpers
# ----------------------------

async def gdb_connect_remote(host: str, port: int) -> Dict[str, Any]:
    # Ensure gdb running
    start_gdb()
    # Set non-stop off, async on (safer defaults), though MI often manages
    await send_mi('gdb-set pagination off', 2.0)
    await send_mi('gdb-set confirm off', 2.0)
    # Connect
    res = await send_mi(f'target-select remote {host}:{port}', 5.0)
    STATE.connected = bool(res.get("ok"))
    return res

async def gdb_break_insert(location: str) -> Dict[str, Any]:
    # location examples: "*0x401000", "main", "file.c:123"
    return await send_mi(f'break-insert {location}', 3.0)

async def gdb_break_delete(id_or_loc: str) -> Dict[str, Any]:
    # Prefer id; if not numeric, try location→resolve id via -break-list
    if id_or_loc.isdigit():
        return await send_mi(f'break-delete {id_or_loc}', 3.0)
    bl = await send_mi('break-list', 3.0)
    # Try to find matching bkpt by 'fullname' or 'original-location'
    try:
        bkpts = bl["payload"]["BreakpointTable"]["body"]
        for b in bkpts:
            if id_or_loc in (b.get("fullname",""), b.get("original-location","")):
                return await send_mi(f'break-delete {b["number"]}', 3.0)
    except Exception:
        pass
    # Fallback attempt (might error)
    return await send_mi(f'break-delete {id_or_loc}', 3.0)

async def gdb_step(count: int = 1) -> Dict[str, Any]:
    # -exec-step executes one source step; for instruction-level, use -exec-step-instruction
    if count <= 1:
        return await send_mi('exec-step-instruction', 5.0)
    # multi-step: repeat; collect last result
    last = {}
    for _ in range(count):
        last = await send_mi('exec-step-instruction', 5.0)
        if not last.get("ok"):
            break
    return last

async def gdb_continue() -> Dict[str, Any]:
    return await send_mi('exec-continue', 5.0)

async def gdb_read_memory(addr: str, length: int) -> Dict[str, Any]:
    # -data-read-memory-bytes ADDRESS LENGTH
    return await send_mi(f'data-read-memory-bytes {addr} {length}', 5.0)

async def gdb_list_registers() -> List[str]:
    r = await send_mi('data-list-register-names', 3.0)
    if r.get("ok"):
        return r["payload"].get("register-names", [])
    return []

async def gdb_read_registers(names: Optional[List[str]] = None) -> Dict[str, Any]:
    # If names provided, map to indices; else read all
    if names:
        # find indices
        rnames = await gdb_list_registers()
        idxs = [str(rnames.index(n)) for n in names if n in rnames]
        if not idxs:
            return {"ok": False, "error": "No matching register names"}
        r = await send_mi('data-list-register-values x ' + " ".join(idxs), 3.0)
    else:
        r = await send_mi('data-list-register-values x', 3.0)
    return r

async def gdb_dump_registers_console() -> Dict[str, Any]:
    # Use console command for human-readable set
    return await send_mi('interpreter-exec console "info registers"', 3.0)

async def gdb_send_console(cmd: str, timeout: float = 6.0) -> Dict[str, Any]:
    # Run a raw CLI command through MI
    # e.g. "x/16x $rsp"
    return await send_mi(f'interpreter-exec console "{cmd}"', timeout)

async def gdb_exec_run(timeout: float = 30.0) -> Dict[str, Any]:
    return await send_mi("exec-run", timeout)

async def gdb_wait_for_stop(timeout: float = 30.0) -> Dict[str, Any]:
    deadline = time.time() + timeout
    events: List[str] = []
    while time.time() < deadline:
        remaining = max(0.1, deadline - time.time())
        try:
            ln = await asyncio.wait_for(STATE.outq.get(), timeout=remaining)
        except asyncio.TimeoutError:
            break

        # Skip routine prompt echoes
        if ln.strip() == "(gdb)":
            continue
        if ln.startswith(("~", "@", "&")):
            events.append(ln)
            continue
        if ln.startswith("="):
            events.append(ln)
            continue
        if ln.startswith("*stopped"):
            payload = {}
            if "," in ln:
                payload = parse_mi_map(ln[len("*stopped,"):])
            return {"ok": True, "class": "stopped", "payload": payload, "events": events}
        events.append(ln)
    return {"ok": False, "class": "timeout", "payload": {}, "events": events}

# ----------------------------
# MCP tools
# ----------------------------


server = FastMCP(name="gdb-control")
mcp = server  # fastmcp compatibility alias
app = server  # some tooling expects 'app'


@server.tool(
    name="gdb_start",
    description="Ensure the embedded GDB subprocess is running. Args: gdb_path (str), gdb_args (optional list[str])."
)
async def gdb_start_tool(gdb_path: str = "gdb", gdb_args: Optional[List[str]] = None) -> List[TextContent]:
    start_gdb(gdb_path, gdb_args)
    return [TextContent(type="text", text="[start] ok=True")]

@server.tool(
    name="gdb_connect",
    description="Start GDB (if needed) and connect to a running QEMU gdbstub. Args: host (str), port (int), gdb_path (optional), gdb_args (optional list[str])."
)
async def gdb_connect(host: str, port: int, gdb_path: str = "gdb", gdb_args: Optional[List[str]] = None) -> List[TextContent]:
    start_gdb(gdb_path, gdb_args)
    res = await gdb_connect_remote(host, port)
    return [TextContent(type="text", text=render_result("connect", res))]

@server.tool(
    name="gdb_send_command",
    description='Send a raw GDB console command via MI (e.g., x/16x $rsp). Args: command (str). Returns console text and MI status.'
)
async def gdb_send_command(command: str, timeout_sec: float = 6.0) -> List[TextContent]:
    res = await gdb_send_console(command, float(timeout_sec))
    return [TextContent(type="text", text=render_result("command", res))]

@server.tool(
    name="gdb_run_program",
    description="Launch the debugged program via MI exec-run. Args: timeout_sec (optional float, default 30)."
)
async def gdb_run_program(timeout_sec: float = 30.0) -> List[TextContent]:
    res = await gdb_exec_run(float(timeout_sec))
    return [TextContent(type="text", text=render_result("exec-run", res))]

@server.tool(
    name="gdb_wait_stop",
    description="Wait for the next GDB *stopped event. Args: timeout_sec (optional float, default 30)."
)
async def gdb_wait_stop_tool(timeout_sec: float = 30.0) -> List[TextContent]:
    res = await gdb_wait_for_stop(float(timeout_sec))
    lines = [f"[wait-stop] ok={res.get('ok')} class={res.get('class')} payload={res.get('payload')}"]
    events = res.get("events", [])
    if events:
        lines.append("events:\n" + "\n".join(events))
    return [TextContent(type="text", text="\n".join(lines))]

@server.tool(
    name="gdb_set_breakpoint",
    description='Set a breakpoint. Args: location (e.g., "main", "file.c:123", "*0x401000"). Returns bkpt info.'
)
async def gdb_set_breakpoint(location: str) -> List[TextContent]:
    res = await gdb_break_insert(location)
    return [TextContent(type="text", text=render_result("break-insert", res))]

@server.tool(
    name="gdb_clear_breakpoint",
    description='Clear a breakpoint. Args: id_or_location (e.g., "1" or "file.c:123").'
)
async def gdb_clear_breakpoint(id_or_location: str) -> List[TextContent]:
    res = await gdb_break_delete(id_or_location)
    return [TextContent(type="text", text=render_result("break-delete", res))]

@server.tool(
    name="gdb_step",
    description="Single-step one instruction. Returns stopped reason and PC."
)
async def gdb_step_tool() -> List[TextContent]:
    res = await gdb_step(1)
    return [TextContent(type="text", text=render_result("step", res))]

@server.tool(
    name="gdb_step_n",
    description="Step N instructions. Args: count (int)."
)
async def gdb_step_n(count: int) -> List[TextContent]:
    res = await gdb_step(max(1, int(count)))
    return [TextContent(type="text", text=render_result(f"step {count}", res))]

@server.tool(
    name="gdb_continue",
    description="Continue execution."
)
async def gdb_continue_tool() -> List[TextContent]:
    res = await gdb_continue()
    return [TextContent(type="text", text=render_result("continue", res))]

@server.tool(
    name="gdb_read_memory",
    description='Read memory bytes. Args: address (str, e.g., "0x7fff0000" or "$rsp"), length (int).'
)
async def gdb_read_memory_tool(address: str, length: int) -> List[TextContent]:
    res = await gdb_read_memory(address, int(length))
    return [TextContent(type="text", text=render_result("mem-read", res))]

@server.tool(
    name="gdb_read_registers",
    description='Read registers. Args: names (optional list[str]); if omitted, returns all.'
)
async def gdb_read_registers_tool(names: Optional[List[str]] = None) -> List[TextContent]:
    res = await gdb_read_registers(names)
    return [TextContent(type="text", text=render_result("regs", res))]

@server.tool(
    name="gdb_dump_registers",
    description='Dump human-readable registers (console "info registers").'
)
async def gdb_dump_registers_tool() -> List[TextContent]:
    res = await gdb_dump_registers_console()
    return [TextContent(type="text", text=render_result("regs-dump", res))]

@server.tool(
    name="gdb_detach",
    description="Detach from target (keeps GDB running)."
)
async def gdb_detach_tool() -> List[TextContent]:
    res = await send_mi("target-detach", 3.0)
    STATE.connected = False
    return [TextContent(type="text", text=render_result("detach", res))]

@server.tool(
    name="gdb_quit",
    description="Exit GDB and clear state."
)
async def gdb_quit_tool() -> List[TextContent]:
    try:
        res = await send_mi("gdb-exit", 3.0)
    except Exception as e:
        res = {"ok": False, "class": "error", "payload": {"msg": str(e)}, "console": [], "log": []}
    stop_gdb()
    return [TextContent(type="text", text=render_result("quit", res))]

def render_result(label: str, res: Dict[str, Any]) -> str:
    ok = res.get("ok", False)
    cls = res.get("class", "")
    payload = res.get("payload", {})
    console = "\n".join(res.get("console", []))
    log = "\n".join(res.get("log", []))
    lines = [f"[{label}] ok={ok} class={cls}"]
    if payload:
        lines.append(f"payload: {payload}")
    if console:
        lines.append("console:\n" + console)
    if log:
        lines.append("log:\n" + log)
    return "\n".join(lines)

# ----------------------------
# Main (stdio transport)
# ----------------------------

async def amain():
    await server.run_stdio_async(show_banner=False)

def main():
    try:
        asyncio.run(amain())
    except KeyboardInterrupt:
        pass

if __name__ == "__main__":
    main()
