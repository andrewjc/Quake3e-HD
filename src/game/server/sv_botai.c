/*
===========================================================================
Copyright (C) 2026 Quake3e-HD Project

Engine-side bot AI (Path B).

The bots are driven entirely from the server: each server frame this module
reads the bot's real player state (origin, view, velocity, health, weapon)
and the real world (collision via SV_Trace, visibility via SV_inPVS), decides
what the bot should do, and produces a usercmd_t that is fed straight to
SV_ClientThink — the same entry point a networked player's movement uses. The
game VM then runs real pmove, weapons and item pickup for the bot.

This replaces the retail VM bot brain (which required the removed AAS
navigation system) and the disconnected in-tree "neural AI" scaffolding that
read fabricated entity data. See AI_REVAMP_PLAN.md.
===========================================================================
*/

#include "server.h"

// Per-bot persistent AI state, indexed by client number.
typedef struct {
	qboolean	active;

	// Locomotion
	float		moveYaw;		// current heading the bot is steering toward
	int			repathTime;		// next time to re-choose a heading (ms)
	int			stuckCheckTime;	// next time to sample progress (ms)
	vec3_t		lastOrigin;		// origin at the last progress sample
	int			stuckCount;		// consecutive stuck samples

	// Respawn pacing so a dead bot doesn't machine-gun the attack button
	int			respawnTime;
} sv_botai_t;

static sv_botai_t sv_botai[MAX_CLIENTS];

static cvar_t *sv_botDebug;

// Standard player bounding box (matches bg_pmove / the VM's client bounds).
static const vec3_t bot_mins = { -15, -15, -24 };
static const vec3_t bot_maxs = {  15,  15,  32 };

/*
==================
SV_BotAI_Init

Called when the server spawns a world; clears all bot AI state.
==================
*/
void SV_BotAI_Init( void ) {
	Com_Memset( sv_botai, 0, sizeof( sv_botai ) );
	if ( !sv_botDebug ) {
		sv_botDebug = Cvar_Get( "sv_botDebug", "0", CVAR_CHEAT );
	}
}

/*
==================
SV_BotAI_ClientActive

A bot became a live, in-world client: (re)initialize its AI state so it steers
from its current facing instead of a stale one.
==================
*/
static void SV_BotAI_ClientActive( int clientNum, const playerState_t *ps ) {
	sv_botai_t *b = &sv_botai[clientNum];

	b->active = qtrue;
	b->moveYaw = ps->viewangles[YAW];
	b->repathTime = 0;
	b->stuckCheckTime = 0;
	b->stuckCount = 0;
	VectorCopy( ps->origin, b->lastOrigin );
	b->respawnTime = 0;
}

/*
==================
SV_BotAI_ClearAngle

Encode a set of Euler view angles into a usercmd (16-bit fixed point).
==================
*/
static void SV_BotAI_SetViewAngles( usercmd_t *cmd, const vec3_t angles ) {
	cmd->angles[0] = ANGLE2SHORT( angles[0] );
	cmd->angles[1] = ANGLE2SHORT( angles[1] );
	cmd->angles[2] = ANGLE2SHORT( angles[2] );
}

/*
==================
SV_BotAI_WalkableAhead

Trace a player-sized box a short distance along a heading on the floor plane.
Returns qtrue if the path is clear.
==================
*/
static qboolean SV_BotAI_WalkableAhead( int clientNum, const vec3_t origin, float yaw, float dist ) {
	vec3_t fwd, end;
	trace_t tr;

	fwd[0] = cos( DEG2RAD( yaw ) );
	fwd[1] = sin( DEG2RAD( yaw ) );
	fwd[2] = 0.0f;

	VectorMA( origin, dist, fwd, end );
	SV_Trace( &tr, origin, bot_mins, bot_maxs, end, clientNum, MASK_PLAYERSOLID, qfalse );

	return ( tr.fraction >= 1.0f && !tr.startsolid ) ? qtrue : qfalse;
}

/*
==================
SV_BotAI_ChooseHeading

Pick a heading that is walkable, preferring to keep the current one. Sweeps
outward in alternating left/right offsets so the bot rounds corners instead
of grinding into them.
==================
*/
static float SV_BotAI_ChooseHeading( int clientNum, const vec3_t origin, float preferredYaw ) {
	if ( SV_BotAI_WalkableAhead( clientNum, origin, preferredYaw, 48.0f ) ) {
		return preferredYaw;
	}

	for ( int step = 1; step <= 8; step++ ) {
		float offset = step * 22.5f;
		if ( SV_BotAI_WalkableAhead( clientNum, origin, preferredYaw + offset, 48.0f ) ) {
			return preferredYaw + offset;
		}
		if ( SV_BotAI_WalkableAhead( clientNum, origin, preferredYaw - offset, 48.0f ) ) {
			return preferredYaw - offset;
		}
	}

	// Boxed in — turn around.
	return preferredYaw + 180.0f;
}

/*
==================
SV_BotAI_Think

Produce one usercmd for a live bot. This is the current interim brain: it
keeps the bot moving through the level with real collision-aware steering,
which exercises the full engine-side drive path (state -> decision -> usercmd
-> SV_ClientThink -> real pmove). Navigation, perception and combat layer on
top of this same structure.
==================
*/
static void SV_BotAI_Think( int clientNum, int time, usercmd_t *cmd ) {
	sv_botai_t *b = &sv_botai[clientNum];
	playerState_t *ps = SV_GameClientNum( clientNum );

	Com_Memset( cmd, 0, sizeof( *cmd ) );
	cmd->serverTime = time;
	cmd->weapon = (byte)ps->weapon;

	if ( !b->active ) {
		SV_BotAI_ClientActive( clientNum, ps );
	}

	// Dead: hold the current view and tap fire to respawn, paced so we don't
	// spam the button every frame.
	if ( ps->pm_type == PM_DEAD || ps->stats[STAT_HEALTH] <= 0 ) {
		SV_BotAI_SetViewAngles( cmd, ps->viewangles );
		if ( time >= b->respawnTime ) {
			cmd->buttons = BUTTON_ATTACK;
			b->respawnTime = time + 700;
		}
		b->active = qfalse; // re-init heading on the next live frame
		return;
	}

	// Periodically (or when blocked) re-choose a heading. Also detect being
	// wedged against geometry and force a fresh direction.
	if ( time >= b->stuckCheckTime ) {
		float moved = Distance( ps->origin, b->lastOrigin );
		if ( moved < 8.0f ) {
			b->stuckCount++;
		} else {
			b->stuckCount = 0;
		}
		VectorCopy( ps->origin, b->lastOrigin );
		b->stuckCheckTime = time + 250;
	}

	if ( time >= b->repathTime || b->stuckCount >= 2 ||
	     !SV_BotAI_WalkableAhead( clientNum, ps->origin, b->moveYaw, 40.0f ) ) {
		// Bias exploration by a small random turn so bots spread out.
		float bias = b->moveYaw + crandom() * 45.0f;
		b->moveYaw = SV_BotAI_ChooseHeading( clientNum, ps->origin, bias );
		b->repathTime = time + 1200 + (int)( random() * 800 );
		if ( b->stuckCount >= 2 ) {
			b->stuckCount = 0;
		}
	}

	// Face and walk the chosen heading.
	vec3_t viewangles = { 0.0f, b->moveYaw, 0.0f };
	SV_BotAI_SetViewAngles( cmd, viewangles );
	cmd->forwardmove = 127;

	// If wedged despite re-heading, hop — clears steps and small ledges.
	if ( b->stuckCount >= 3 && ps->groundEntityNum != ENTITYNUM_NONE ) {
		cmd->upmove = 127;
	}
}

/*
==================
SV_BotAI_Frame

Drive every active bot client for this server frame. Called from SV_Frame in
place of the retail VM bot brain.
==================
*/
void SV_BotAI_Frame( int time ) {
	int i;

	for ( i = 0; i < sv.maxclients; i++ ) {
		client_t *cl = &svs.clients[i];

		if ( cl->state != CS_ACTIVE ) {
			if ( sv_botai[i].active ) {
				sv_botai[i].active = qfalse;
			}
			continue;
		}
		if ( cl->netchan.remoteAddress.type != NA_BOT ) {
			continue;
		}

		usercmd_t cmd;
		SV_BotAI_Think( i, time, &cmd );
		SV_ClientThink( cl, &cmd );

		if ( sv_botDebug && sv_botDebug->integer && ( time % 1000 ) < 50 ) {
			const playerState_t *ps = SV_GameClientNum( i );
			Com_Printf( "botai: %s org=(%.0f %.0f %.0f) yaw=%.0f hp=%d\n",
				cl->name, ps->origin[0], ps->origin[1], ps->origin[2],
				sv_botai[i].moveYaw, ps->stats[STAT_HEALTH] );
		}
	}
}
