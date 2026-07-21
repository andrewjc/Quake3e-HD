# Bot AI Revamp Plan — Modern Standards

Status: **PROPOSED** (awaiting review — no implementation started)
Date: 2026-07-21

---

## 1. Where we are (audited state)

The repo replaced the classic Quake 3 botlib with a custom native AI
(~15,800 lines in `src/game/ai/`, plus `src/engine/ai/ai_interface.c`
~2,600 lines keeping the `botlib_export_t` ABI). Findings from the audit:

| Subsystem | File(s) | State |
|---|---|---|
| Lifecycle / FSM core | `ai_main.c` (1040), `ai_implementation.c` (914) | Working loop: `AI_Frame` → per-bot `AI_BotThink` FSM (COMBAT/MOVING/SEARCHING/RETREATING/OBJECTIVE/IDLE) → `AI_UpdateBotInput` → `ClientThink_real` |
| **Navigation** | `Nav_LoadMesh` / `AAS_*_Bridge` stubs | **Absent.** Every AAS query returns dummy constants (`AAS_PointAreaNum`→1, travel time→100). The only pathing code is a naive straight-line 32u grid in a file that is not compiled. Bots cannot route around a map. |
| Perception | `perception/ai_perception.c` (1247) | Exists; needs audit for omniscience (direct `g_entities` access vs. sensed data) |
| Tactical | `tactical/tactical_combat.c` (903), `cover_system.c` (841), `movement_tactics.c` (1069) | Exists; built atop the stubbed navigation, so effectively inert in practice |
| Strategic | `strategic/strategic_planning.c` (1256) | Exists; same caveat |
| Team | `team/team_coordination.c` (1268) + header protocol (roles, squads, formations, comms) | Exists; not validated in play |
| Learning | `neural/nn_core.c` (822): real MLP fwd/backprop; `learning/rl_ppo.c` (846); `learning/skill_adaptation.c` (661) | Genuine CPU NN + PPO scaffolding; currently trains against whatever the (nav-less) bots do |
| Characters | `character/bot_character.c` (309) | Reads retail `.c` bot character files |
| Dead code | `game_interface.c` (495), `game_stubs.c`, `bot_input.c`, `trap_wrapper.c`, `ai_memory.c` | Not in `quake3e.vcxproj`; `game_interface.c` redefines live entry points (`BotAIStartFrame`, `Nav_LoadMesh`) — a landmine for confusion. CMake's `AI_SRCS` builds only `ai_stubs.c`. |

**Runtime path:** native in-engine game (no QVM). `sv_init.c` →
`SV_BotInitBotLib` → `GetBotLibAPI` (`ai_interface.c:2537`) →
`Export_BotLibSetup` → `AI_Init`; per frame `Export_BotLibStartFrame` →
`BotAIStartFrame` → `AI_Frame`.

**Core conclusion:** every intelligent-looking subsystem sits on a
navigation void. The revamp is sequenced so navigation lands first and
every later phase is measurable in real matches.

### 1b. Empirically verified integration reality (2026-07-21)

Investigation while starting Phase 0/1 established, by build-and-run plus a
debugger stack trace, several facts that reshape the plan:

- **Bots did not spawn at all.** The game (retail `qagame` VM) aborts
  `BotAISetupClient` because three botlib exports returned `1` for success
  where the contract requires `BLERR_NOERROR` (`0`):
  `BotLoadItemWeights`, `BotLoadWeaponWeights`, `BotLoadChatFile`.
  **Fixed** — with these corrected, bots complete setup and enter the game.
- **The elementary-action layer was entirely NULL** (`Init_EA_Export` was
  empty). The moment a bot thought, the game dereferenced null `EA_*`
  pointers and the server crashed. **Fixed** — implemented a faithful EA
  layer (`ea_botinputs[]`, `EA_Move/View/Attack/Jump/GetInput/...`) that
  accumulates elementary actions into a `bot_input_t` and returns it via
  `EA_GetInput`. This is also the injection point Path B needs.
- **`BotGetLevelItemGoal` never terminated.** The game iterates it as
  `for (i = f(-1); i != -1; i = f(i))`; the stub returned the index
  unchanged and `>= 0` forever → infinite loop. **Fixed** (returns `-1`).
- **The retail VM bot brain cannot be driven by stubbed AAS.** After the
  above fixes, adding a bot hangs the server. A debugger attach shows the
  main thread spinning deep inside the VM's JIT-compiled navigation code:
  the brain walks AAS reachabilities between areas, and the stub bridges
  (constant "area 1, travel time 100, route found") describe a world where
  every goal is reachable but no reachability ever resolves, so the walk
  loops. Making the bridges report honest failure (no area / no route) did
  **not** stop it — the VM has many AAS entry points and its loop behavior
  under partial data is not tunable from the outside. **Conclusion: the
  retail brain is hard-wired to a complete AAS system; it cannot be
  stubbed. Either the full AAS system is reconstructed (infeasible — it is
  the ~30k-line subsystem that was deleted, and needs per-map `.aas` data
  or an AAS compiler), or the VM brain is bypassed and bots are driven
  entirely engine-side (Path B, this plan's direction).**
- **Interim stable state:** `AAS_Initialized` now honestly reports
  not-ready, so bot setup fails cleanly with "AAS not initialized" instead
  of hanging. This is the single flag to flip once the Phase 1 navmesh
  backend exists.

**Revised sequencing consequence:** Phase 1 is not "add a navmesh to a
working substrate" — it is "build the navigation backend AND the
engine-side drive path (real entity state from `svs.clients`/`sv.gentities`,
real collision via `SV_Trace`, usercmd injection via `EA_GetInput`) that
together replace the removed AAS/botlib." The engine-side AI's fake
`g_entities`/`trap_Trace`/`ClientThink_real` (in `ai_implementation.c`)
must be rewired to the server's real world before any behavior phase can be
verified. This is a multi-stage build; the fixes above are its first,
verified increment (bots spawn; action-injection path exists; server
stable).

**Guiding principle:** deterministic competence first, learning as an
adaptive layer on top — never as a substitute for a missing capability.

---

## 2. Phase 0 — Foundation hygiene & measurement harness

Everything later depends on being able to *see* and *measure* bot
behavior. Small phase, do it first.

### 0.1 Dead code reconciliation
- Diff `game_interface.c` / `game_stubs.c` / `bot_input.c` /
  `trap_wrapper.c` / `ai_memory.c` against the live implementations in
  `ai_implementation.c`. Salvage anything genuinely better (the grid-nav
  prototype is superseded by Phase 1; the rest is likely deletable).
- Delete the losers; update `CMakeLists.txt` `AI_SRCS` to build the same
  source set as `quake3e.vcxproj` (currently it builds only stubs).

### 0.2 Debug observability
- `ai_debugDraw` cvar: per-bot world-space overlay (uses the existing
  debug line/render API) showing: current state/goal label, path corridor
  polys, steering target, aim target + error cone, perception state of
  each known enemy (confidence-faded boxes at believed positions).
- `ai_debugBot <clientNum|all>` to scope the overlay.

### 0.3 Metrics harness (the regression gate for every later phase)
- `ai_metrics 1` writes `ai_metrics.csv` per match: per-bot kills,
  deaths, damage given/taken, item pickups by class (mega/RA/YA/weapons),
  **stuck events** (velocity < 20u/s for > 2s while goal distance > 100u),
  **nav failures** (path request returned no route), average think time
  (µs), goal-switch rate.
- Scripted match runner (extends the autodemo pattern):
  `rt_botmatch.cfg` — spawn N bots on a map list, run T minutes each,
  `quit`; a small tool (`tools/aimetrics.py`) aggregates the CSVs and
  prints deltas vs. a stored baseline.
- Baseline capture on current build (documents today's brokenness as the
  floor).

**Exit criteria:** dead files gone, both build systems build the same AI,
overlay renders, harness produces a baseline CSV on q3dm1/q3dm7/q3dm17.

---

## 3. Phase 1 — Navigation (the load-bearing phase)

Modern replacement for AAS: a Recast/Detour-style navmesh generated
in-engine from BSP collision geometry at map load, cached to disk.

### 1.1 Voxelization & mesh generation (`src/game/ai/nav/nav_build.c`)
- Input: world collision geometry. Reuse the traversal proven out by the
  path tracer's BSP extraction (`rt_bsp_loader.c` walks all world
  surfaces); collect triangles from collision brushes/patches
  (`cm_load.c` structures), not render surfaces.
- Pipeline (standard Recast stages, implemented lean):
  1. Rasterize triangles into a voxel heightfield (cell size 16u, cell
     height 8u — Q3 scale; player radius 15u, height 56u, crouch 40u).
  2. Filter walkable spans (slope < 45°, clearance ≥ agent height),
     erode by agent radius.
  3. Region partitioning (watershed), contour tracing, convex polygon
     mesh (max 6 verts/poly) + poly adjacency.
  4. Detail height mesh only if needed for slopes/stairs accuracy
     (evaluate; Q3 geometry is mostly planar — may skip).
- **Cache**: serialize to `<homepath>/baseq3/navcache/<mapname>.qnav`
  (versioned header + BSP checksum). Load-if-valid at map start;
  generation target < 3 s for the largest stock map, done during level
  load.
- Cvars: `nav_rebuild` (force), `nav_cellSize`, `nav_debugDraw`
  (mesh/regions/links overlay).

### 1.2 Off-mesh links (`nav_links.c`)
Auto-annotated from entities + geometry probes:
- **Jump pads** (`trigger_push`): parabolic trajectory sim → landing poly,
  one-way link, traversal action = "walk onto pad".
- **Teleporters** (`trigger_teleport` → `misc_teleporter_dest`): zero-cost
  one-way link.
- **Drop-downs**: probe poly boundary edges; falls ≤ 400u (survivable)
  become one-way links costed by fall damage risk.
- **Gap/ledge jumps**: probe forward from boundary edges for landings
  reachable with run+jump (horizontal ≤ 220u, rise ≤ 48u); annotate
  action = jump with approach speed requirement.
- **Doors/buttons**: link through `func_door` volumes with action
  metadata; shootable buttons recorded with trigger position.
- Optional per-map hand overrides: `navcache/<map>.qnavpatch` text file
  (add/remove links) for pathological spots.

### 1.3 Query API (`nav_query.c`)
- `Nav_NearestPoly(point, extents)` (BV-tree over polys).
- `Nav_FindPath(startPoly, endPoly, costFlags)` — A* over poly graph.
  Cost modifiers: base distance; per-poly **danger cost** (recent damage
  taken/deaths at that poly, decaying — bots learn to avoid camped
  lines); water/lava/slime multipliers; link action costs.
- `Nav_StringPull(corridor)` — funnel algorithm → smoothed waypoints.
- `Nav_Raycast(from, to)` — walkability raycast for local steering.
- `Nav_RandomReachablePoint(origin, radius)` — roam/search support.
- Path corridor object per bot with incremental repair (replan only the
  invalidated tail when pushed off-path; full replan on goal change or
  corridor break). Budget: ≤ 2 full A* requests/bot/second amortized;
  queries time-sliced through the existing `next_think_time` gating.
- **Replace the `AAS_*_Bridge` dummies** in `ai_interface.c` with real
  implementations backed by this API (`PointAreaNum` → poly id,
  `AreaTravelTimeToGoalArea` → path cost, `PredictRoute` → corridor
  walk), so all legacy-shaped callers get truth.

### 1.4 Path executor (`nav_move.c`)
- Steering to the funnel waypoint with velocity-aware turn anticipation.
- Link action execution: jump timing (including run-up speed check),
  crouch sections, pad rides (hands-off until landing), teleporter walk-in,
  door waits.
- **Stuck detection**: progress watchdog (distance-to-corridor-head not
  decreasing over 1.5 s) → local unstick maneuvers (side-step, jump), then
  poly danger-mark + full replan; metric event logged.
- Ground movement uses proper `usercmd` generation through the existing
  `AI_UpdateBotInput` path (no teleport hacks, no velocity writes).

**Exit criteria:** 8 bots, 10 minutes, q3dm1/q3dm7/q3dm13/q3dm17/q3ctf2:
zero stuck events, zero nav failures on reachable goals, every major item
visited, jump pads/teleporters used correctly (observed in overlay +
demo), think time within budget. Metrics harness numbers recorded as the
new baseline.

---

## 4. Phase 2 — Perception & belief (honest sensing)

Goal: bots act on what they could plausibly know. Audit and refit
`ai_perception.c`:

### 2.1 Vision
- FOV cone (skill-scaled 90°–120°) + distance attenuation + PVS cluster
  pre-cull + trace confirmation (head/torso points, fog occlusion
  respected). Lighting-based visibility is a stretch goal (the path
  tracer knows surface luminance — darkness could genuinely conceal).

### 2.2 Hearing
- Event bus: weapon fire, jumps/footsteps (speed-gated), item pickups,
  teleporter/pad activations, door triggers. Each event: position,
  loudness radius, occlusion factor (portal distance through nav graph,
  not raw distance). Subscribed per-bot with skill-scaled radius.

### 2.3 Belief store (replaces omniscient reads)
- Per-enemy record: last confirmed position/velocity, timestamp,
  confidence (decays; boosted by sound events), predicted position
  (dead-reckoned along nav-plausible directions).
- All decision/combat code consumes **beliefs only**; direct `g_entities`
  position reads outside the perception module become lint-failures
  (grep-able convention: `AI_Belief*` accessors).
- Team belief sharing deferred to Phase 5 (with comm latency).

**Exit criteria:** overlay shows belief boxes diverging from true
positions when unseen; flanking works (approach from behind unnoticed
until FOV/sound); metrics stable vs. Phase 1 baseline.

---

## 5. Phase 3 — Decision making: two-tier utility system

Replace the top-level FSM dispatch in `AI_BotThink` with utility scoring
(better fit than behavior trees for item-economy arena play). Keep the
existing subsystems as *providers*, re-wired as scorers/executors.

### 3.1 Strategic tier (~2 Hz per bot) — "what matters now"
Scored goals (0..1 utilities with weights, hysteresis ±0.1 to prevent
flapping):
- **Item control**: mega/RA/YA with real respawn-clock tracking (observed
  pickup events start timers; arrive-just-in-time routing). This is the
  defining behavior of strong arena play.
- **Fight**: engage known enemy (belief confidence × health/armor/weapon
  advantage estimate).
- **Recover**: retreat via low-danger path to health/armor when losing
  trade math.
- **Hunt/Probe**: search belief-decayed regions, sound-led investigation.
- **Objective** (team modes): defer to Phase 5 role assignment.
Feeds from `strategic_planning.c` where salvageable.

### 3.2 Tactical tier (per think) — "how to do it"
Within the active goal, score concrete actions: direct route vs. safe
route (danger-cost pathing), take cover (re-point `cover_system.c` at
real nav polys + trace validation), pick up en-route items (deviation
cost vs. value), kite along corridor while firing, push vs. hold angle.

### 3.3 Integration mechanics
- The FSM enum remains as a *readout label* for the overlay/metrics, not
  as control flow.
- Every scorer is a pure function of (belief store, nav queries, self
  state) → score + plan handle; the executor runs the winner's plan via
  the Phase-1 corridor API.
- `ai_utility_debug` prints live score tables in the overlay.

**Exit criteria:** in observed 10-min FFA matches, bots contest majors
within ±3 s of respawn > 60% of the time; no goal oscillation (> 3
switches/second) events in metrics; win-rate spread across equal-skill
bots within noise.

---

## 6. Phase 4 — Combat modernization (the "feels modern" phase)

### 4.1 Humanized aim model (`combat/aim_model.c`, new)
Replaces any instant-snap aiming:
- **Reaction delay** on first acquisition (skill-scaled 150–400 ms;
  +100 ms if target entered from outside FOV, − if sound-primed).
- **Angular tracking** with max angular velocity + acceleration (deg/s
  skill curve), critically-damped approach → natural overshoot at low
  skill.
- **Aim error**: smoothly wandering offset (sum of two sines / gradient
  noise — NOT per-frame gaussian jitter, which looks robotic), amplitude
  scaled by skill, target angular size, target lateral velocity, own
  velocity, and time-on-target (error shrinks as tracking settles).
- **Target switching cost**: full re-acquisition on switch; switch only on
  meaningful utility delta.
- Visibility loss → aim lingers at last position, then drifts to
  predicted re-emergence point (nav-aware: corridor exits, not through
  walls).

### 4.2 Weapon intelligence (`combat/weapon_select.c` refit)
- Utility matrix per weapon: effective-range envelope, DPS, splash
  utility vs. target on ground/air, ammo, self-damage risk at range,
  switch time cost. LG/rail/rocket personalities per character file
  weights (retain retail `.c` character flavor).
- **Projectile lead** computed from *believed* velocity with error
  proportional to belief confidence; rocket aim-at-feet vs. direct based
  on target ground proximity; grenade arcs only with nav-verified bounce
  utility (stretch).

### 4.3 Movement micro (`movement_tactics.c` refit on Phase-1 executor)
- Strafe-jump acceleration on straight corridor runs ≥ 256u (skill-gated
  proficiency: chain success probability per hop).
- Combat dodging: lateral impulses orthogonal to the believed enemy aim
  line, nav-validated (never dodge into lava/void); rhythm randomized.
- Circle-strafe orbit maintaining LOS + range band for the current
  weapon; crouch-peek at cover (from `cover_system.c` points).
- Rocket-jump for reachable-only-that-way goals at high skill (health
  math gated), using the Phase-1 link annotation framework.

**Exit criteria:** blind evaluation — a captured demo of skill-3 bots
should read as "decent human pubber": measurable rail accuracy in the
30–45% band at skill 3 (vs. ~100% snap today), reaction-time histogram
matches configured curve, K/D between aim-model-on vs. instant-aim bots
shows the handicap working. Autodemo + metrics regression clean.

---

## 7. Phase 5 — Team & objective play

Wire `team_coordination.c`'s existing role/squad/comms protocol into the
utility tiers:
- **Role assignment** (CTF): attacker/defender/mid-control from squad
  coordinator, entering the strategic scorer as strong utility priors;
  reassignment on death/flag events.
- **Belief sharing with latency**: a bot that *confirms* an enemy
  broadcasts position to squad after 300–600 ms "comm delay"; teammates
  merge into their belief store at reduced confidence. No shared
  omniscience.
- **Coordinated timing**: staggered pushes (leader gates squad readiness),
  crossfire setup via distinct approach corridors (nav API: k-alternative
  paths), item-denial assignments (defender covers RA cycle).
- FFA remains coordinator-free.

**Exit criteria:** CTF on q3ctf2: measurable role distribution (not all
bots chasing the carrier), flag-run success rate delta with coordination
on vs. off, comms visible in overlay.

---

## 8. Phase 6 — Learning layer re-integration

Reposition NN/PPO from "drive behavior" to "tune parameters":
- **Online skill adaptation** (`skill_adaptation.c` refit): adjust aim
  error/reaction/aggression parameter multipliers toward a target
  win-rate against the human player (rubber-band difficulty, off by
  default, `ai_skill_adapt`).
- **Offline self-play tuning**: headless bot-vs-bot harness (dedicated
  server binary + metrics CSVs) where PPO tunes utility weights and
  danger-cost coefficients per map pool; outputs a versioned parameter
  file (`ai/params_v*.cfg`) that ships as data — runtime stays
  deterministic when `ai_learning 0`.
- NN inference budget capped (existing nets are small MLPs — fine); GPU
  stubs remain stubs (not worth Vulkan compute for these sizes).

**Exit criteria:** self-play tuning demonstrably improves harness metrics
over hand-tuned baseline on ≥ 2 maps without degrading others;
adaptation converges to target win-rate in test sessions.

---

## 9. Phase 7 — Difficulty & humanization polish

- Map skills 1–5 to measured parameter curves (reaction ms, deg/s, aim
  wander amplitude, strafe-jump proficiency, item-clock accuracy ±s,
  belief decay rate). Document the table in `ai/README.md`.
- Low-skill deliberate imperfections: hesitation on acquisition, worse
  weapon choices, forgetting item timers, panic dodge randomness.
- Per-character flavor from retail `.c` files mapped onto the new
  parameter set (aggression, camper tendency, weapon preferences) so
  Klesk still feels like Klesk.

---

## 10. Cross-cutting: performance & stability budget

- All AI within existing `ai_think_time` slicing; target ≤ 1.5 ms total
  AI CPU per server frame with 16 bots (measure via Phase-0 think-time
  metric).
- Nav generation off the critical path where possible (cache hit = ~0).
- Zero per-frame heap allocation in steady state (the light-grid lesson);
  all per-bot buffers preallocated at `AI_CreateBot`.
- Every phase ends with: metrics harness regression vs. stored baseline,
  60 s autodemo (exit 0, zero Vulkan/hunk errors), and for
  movement-visible phases a spectated demo capture reviewed via
  screenshots.

## 11. Sequencing & rough effort

| Phase | Size | Depends on | Parallelizable? |
|---|---|---|---|
| 0 Hygiene + harness | S | — | — |
| 1 Navigation | XL | 0 | mesh-gen / links / executor internally |
| 2 Perception | M | 0 (audit), 1 (occlusion via nav) | with 3 |
| 3 Utility decisions | M | 1 | with 2 |
| 4 Combat | M–L | 2 (beliefs), 1 (movement) | aim model independent early |
| 5 Team | M | 3 | — |
| 6 Learning | M | 3, harness | offline track parallel |
| 7 Difficulty | S | 4 | — |

Fastest visible transformation: **0 → 1 → 4.1 (aim model)** — navigation
plus humanized aim changes the experience completely; everything else
compounds from there.

## 12. Key risks

1. **Navmesh correctness on patch-heavy maps** (curves, bounce pads over
   voids): mitigated by the `.qnavpatch` override file + per-map exit
   testing on the stock rotation.
2. **CPU spikes from pathfinding storms** (all bots replan on a big
   event): amortized request queue with per-frame budget; corridor repair
   instead of full replans.
3. **Legacy ABI callers** expecting AAS semantics we choose not to
   emulate exactly: the bridge functions get real data, but any caller
   depending on AAS area *numbering* specifics needs a shim audit
   (Phase 1.3).
4. **Learning regressions**: `ai_learning` defaults off until Phase 6
   exit criteria met; deterministic path always available.
5. **Scope creep in Phase 4**: the aim model has endless polish depth —
   time-box to the exit criteria, park extras in a backlog section here.

---

*Plan ends. No implementation has been started; awaiting review and
priority confirmation.*
