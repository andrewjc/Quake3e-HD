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

#define BOT_MAX_PATH	256

// Per-bot persistent AI state, indexed by client number.
typedef struct {
	qboolean	active;

	// Navmesh path being followed
	int			path[BOT_MAX_PATH];
	int			pathLen;
	int			pathIndex;		// next node in path to reach
	int			goalNode;		// destination node
	int			repathTime;		// hard re-plan deadline (ms)

	// Locomotion fallback + stuck recovery
	float		moveYaw;		// heading when steering without a path
	int			stuckCheckTime;	// next progress sample (ms)
	vec3_t		lastOrigin;		// origin at last progress sample
	int			stuckCount;		// consecutive stuck samples

	// Combat
	int			enemy;			// client number of current target, or -1
	int			enemySeenTime;	// last time the enemy was visible (ms)
	vec3_t		aimAngles;		// smoothed view angles the bot is turning toward
	int			reactionTime;	// time at which the bot may first fire on a new enemy
	int			fireHoldTime;	// keeps firing briefly after LOS breaks

	// Respawn pacing so a dead bot doesn't machine-gun the attack button
	int			respawnTime;
} sv_botai_t;

static sv_botai_t sv_botai[MAX_CLIENTS];

static cvar_t *sv_botDebug;

// Forward declarations (helpers are defined lower down but used by combat).
static void SV_BotAI_SetViewAngles( usercmd_t *cmd, const vec3_t angles );
static qboolean SV_BotAI_WalkableAhead( int clientNum, const vec3_t origin, float yaw, float dist );
static float SV_BotAI_ChooseHeading( int clientNum, const vec3_t origin, float preferredYaw );

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
	SV_BotNav_Clear();
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
	b->pathLen = 0;
	b->pathIndex = 0;
	b->goalNode = -1;
	b->repathTime = 0;
	b->stuckCheckTime = 0;
	b->stuckCount = 0;
	VectorCopy( ps->origin, b->lastOrigin );
	b->enemy = -1;
	b->enemySeenTime = 0;
	b->reactionTime = 0;
	b->fireHoldTime = 0;
	VectorCopy( ps->viewangles, b->aimAngles );
	b->respawnTime = 0;
}

/*
==================
SV_BotAI_Visible

Line-of-sight test from the bot's eye to a target point, ignoring the bot and
target entities. Returns qtrue if nothing solid blocks the view.
==================
*/
static qboolean SV_BotAI_Visible( int clientNum, const vec3_t eye, const vec3_t target, int targetNum ) {
	trace_t tr;
	SV_Trace( &tr, eye, vec3_origin, vec3_origin, target, clientNum, MASK_SHOT, qfalse );
	return ( tr.fraction >= 0.99f || tr.entityNum == targetNum ) ? qtrue : qfalse;
}

/*
==================
SV_BotAI_FindEnemy

Pick the nearest live, visible opponent. Deathmatch: every other player is an
enemy; team games skip same-team clients.
==================
*/
static int SV_BotAI_FindEnemy( int clientNum, const playerState_t *ps ) {
	vec3_t eye;
	VectorCopy( ps->origin, eye );
	eye[2] += ps->viewheight;

	int myTeam = ps->persistant[PERS_TEAM];
	int best = -1;
	float bestDist = 1e30f;

	for ( int i = 0; i < sv.maxclients; i++ ) {
		if ( i == clientNum || svs.clients[i].state != CS_ACTIVE ) {
			continue;
		}
		playerState_t *eps = SV_GameClientNum( i );
		if ( eps->pm_type == PM_DEAD || eps->stats[STAT_HEALTH] <= 0 ) {
			continue;
		}
		// Skip teammates in team modes (PERS_TEAM matches; free-for-all leaves
		// everyone on TEAM_FREE, so all are fair game).
		if ( myTeam != TEAM_FREE && eps->persistant[PERS_TEAM] == myTeam ) {
			continue;
		}

		sharedEntity_t *ent = SV_GentityNum( i );
		vec3_t target;
		VectorCopy( ent->r.currentOrigin, target );
		target[2] += eps->viewheight;

		float dist = Distance( eye, target );
		if ( dist >= bestDist || dist > 3000.0f ) {
			continue;
		}
		if ( !SV_BotAI_Visible( clientNum, eye, target, i ) ) {
			continue;
		}
		bestDist = dist;
		best = i;
	}
	return best;
}

/*
==================
SV_BotAI_TurnTowards

Rotate a[] toward the target angles by at most maxStep degrees per axis,
handling wraparound. Returns the remaining yaw+pitch error magnitude.
==================
*/
static float SV_BotAI_TurnTowards( vec3_t a, const vec3_t target, float maxStep ) {
	float err = 0.0f;
	for ( int i = 0; i < 2; i++ ) {	// pitch, yaw
		float d = AngleSubtract( target[i], a[i] );
		float step = d;
		if ( step > maxStep ) step = maxStep;
		else if ( step < -maxStep ) step = -maxStep;
		a[i] = AngleNormalize180( a[i] + step );
		err += fabs( d );
	}
	a[ROLL] = 0.0f;
	return err;
}

/*
==================
SV_BotAI_Combat

If a target is visible, aim at it (turn-rate limited, with a reaction delay
before the first shot) and fire when on target. Returns qtrue if the bot is
engaging (movement should orient to the fight rather than the nav path).
==================
*/
static qboolean SV_BotAI_Combat( int clientNum, const playerState_t *ps, int time, usercmd_t *cmd ) {
	sv_botai_t *b = &sv_botai[clientNum];

	int enemy = SV_BotAI_FindEnemy( clientNum, ps );
	if ( enemy != b->enemy ) {
		// New target: the bot needs a moment to react before it can fire.
		if ( enemy >= 0 ) {
			b->reactionTime = time + 220;
		}
		b->enemy = enemy;
	}

	if ( enemy < 0 ) {
		// No visible enemy — keep firing for a short beat if we just lost one
		// (so a target ducking behind cover isn't instantly forgotten), then
		// hand movement back to navigation.
		return ( time < b->fireHoldTime ) ? qtrue : qfalse;
	}

	b->enemySeenTime = time;
	b->fireHoldTime = time + 400;

	// Aim point: enemy eye level.
	vec3_t eye, target, dir, want;
	VectorCopy( ps->origin, eye );
	eye[2] += ps->viewheight;
	sharedEntity_t *ent = SV_GentityNum( enemy );
	VectorCopy( ent->r.currentOrigin, target );
	target[2] += SV_GameClientNum( enemy )->viewheight;
	VectorSubtract( target, eye, dir );
	vectoangles( dir, want );

	// Turn-rate-limited aim (a full 360 takes a beat; snappier at short range).
	float dist = VectorLength( dir );
	float turn = ( dist < 400.0f ) ? 28.0f : 20.0f;
	float err = SV_BotAI_TurnTowards( b->aimAngles, want, turn );
	SV_BotAI_SetViewAngles( cmd, b->aimAngles );

	// Fire once the reaction delay has passed and we are pointed close enough
	// to the target.
	if ( time >= b->reactionTime && err < 12.0f ) {
		cmd->buttons |= BUTTON_ATTACK;
	}
	return qtrue;
}

/*
==================
SV_BotAI_Replan

Choose a new destination on the navmesh and plan a route to it. For now the
destination is any random reachable node, which keeps bots roaming the whole
map; goal selection (items, enemies) layers on top in later phases. Returns
qtrue if a route was found.
==================
*/
static qboolean SV_BotAI_Replan( int clientNum, int time ) {
	sv_botai_t *b = &sv_botai[clientNum];
	playerState_t *ps = SV_GameClientNum( clientNum );

	b->pathLen = 0;
	b->pathIndex = 0;

	if ( !SV_BotNav_Ready() ) {
		return qfalse;
	}

	int startNode = SV_BotNav_NearestNode( ps->origin );
	if ( startNode < 0 ) {
		return qfalse;
	}

	// Try a few random goals until one is routable and not trivially close.
	for ( int attempt = 0; attempt < 8; attempt++ ) {
		int goal = SV_BotNav_RandomNode();
		if ( goal < 0 || goal == startNode ) {
			continue;
		}
		int len = SV_BotNav_FindPath( startNode, goal, b->path, BOT_MAX_PATH );
		if ( len >= 2 ) {
			b->pathLen = len;
			b->pathIndex = 1;			// path[0] is the start node
			b->goalNode = goal;
			b->repathTime = time + 15000;
			return qtrue;
		}
	}
	return qfalse;
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
// Steer/move the bot toward a world target point, filling movement fields of
// the usercmd. Faces the target and runs forward; nudges sideways around
// local obstacles the path doesn't capture.
static signed char SV_BotAI_ClampMove( float v ) {
	if ( v > 127.0f ) return 127;
	if ( v < -127.0f ) return -127;
	return (signed char)v;
}

static void SV_BotAI_MoveToward( int clientNum, const playerState_t *ps,
                                 const vec3_t target, qboolean jump,
                                 qboolean engaging, const vec3_t viewAngles, usercmd_t *cmd ) {
	vec3_t delta;
	VectorSubtract( target, ps->origin, delta );
	delta[2] = 0.0f;
	float yaw = ( VectorLength( delta ) > 1.0f ) ? RAD2DEG( atan2( delta[1], delta[0] ) )
	                                             : ps->viewangles[YAW];

	// If the direct line to the target is blocked low, sidestep to the clearer
	// side so the bot rounds pillars and corners between path nodes.
	if ( !SV_BotAI_WalkableAhead( clientNum, ps->origin, yaw, 40.0f ) ) {
		yaw = SV_BotAI_ChooseHeading( clientNum, ps->origin, yaw );
	}

	if ( !engaging ) {
		// Not fighting: just face where we walk.
		vec3_t va = { 0.0f, yaw, 0.0f };
		SV_BotAI_SetViewAngles( cmd, va );
		cmd->forwardmove = 127;
	} else {
		// Fighting: the view is aimed at the enemy, so move in the aim frame —
		// this makes the bot strafe/advance toward its goal while keeping the
		// gun on target.
		vec3_t moveDir = { cos( DEG2RAD( yaw ) ), sin( DEG2RAD( yaw ) ), 0.0f };
		vec3_t fwd, right;
		AngleVectors( viewAngles, fwd, right, NULL );
		fwd[2] = 0.0f; right[2] = 0.0f;
		VectorNormalize( fwd );
		VectorNormalize( right );
		cmd->forwardmove = SV_BotAI_ClampMove( DotProduct( moveDir, fwd ) * 127.0f );
		cmd->rightmove   = SV_BotAI_ClampMove( DotProduct( moveDir, right ) * 127.0f );
	}

	if ( jump && ps->groundEntityNum != ENTITYNUM_NONE ) {
		cmd->upmove = 127;
	}
}

static void SV_BotAI_Think( int clientNum, int time, usercmd_t *cmd ) {
	sv_botai_t *b = &sv_botai[clientNum];
	playerState_t *ps = SV_GameClientNum( clientNum );

	Com_Memset( cmd, 0, sizeof( *cmd ) );
	cmd->serverTime = time;
	cmd->weapon = (byte)ps->weapon;

	if ( !b->active ) {
		SV_BotAI_ClientActive( clientNum, ps );
	}

	// Dead: hold view and tap fire to respawn, paced so we don't spam it.
	if ( ps->pm_type == PM_DEAD || ps->stats[STAT_HEALTH] <= 0 ) {
		SV_BotAI_SetViewAngles( cmd, ps->viewangles );
		if ( time >= b->respawnTime ) {
			cmd->buttons = BUTTON_ATTACK;
			b->respawnTime = time + 700;
		}
		b->active = qfalse; // re-plan on the next live frame
		return;
	}

	// Progress / stuck sampling.
	if ( time >= b->stuckCheckTime ) {
		float moved = Distance( ps->origin, b->lastOrigin );
		b->stuckCount = ( moved < 10.0f ) ? b->stuckCount + 1 : 0;
		VectorCopy( ps->origin, b->lastOrigin );
		b->stuckCheckTime = time + 250;
	}

	// Combat: if engaging, aim and fire. The bot still advances along its nav
	// path underneath (movement below), but its view is owned by the fight, so
	// it keeps pathing toward items/goals while shooting. Keep the aim frame in
	// sync when not fighting so the first shot on a new enemy starts from where
	// the bot is actually looking.
	qboolean engaging = SV_BotAI_Combat( clientNum, ps, time, cmd );
	if ( !engaging ) {
		VectorCopy( ps->viewangles, b->aimAngles );
	}

	// (Re)plan when we have no path, reached the goal, hit the deadline, or
	// have been wedged for a while.
	if ( b->pathLen < 2 || b->pathIndex >= b->pathLen ||
	     time >= b->repathTime || b->stuckCount >= 6 ) {
		if ( SV_BotAI_Replan( clientNum, time ) ) {
			b->stuckCount = 0;
		}
	}

	// Follow the navmesh path if we have one.
	if ( b->pathLen >= 2 && b->pathIndex < b->pathLen ) {
		const float *nodeOrg = SV_BotNav_NodeOrigin( b->path[b->pathIndex] );
		if ( nodeOrg ) {
			vec3_t target;
			VectorCopy( nodeOrg, target );

			// Advance to the next node once we're close in the horizontal
			// plane and roughly at its height.
			vec3_t flat;
			VectorSubtract( ps->origin, target, flat );
			float horiz = sqrt( flat[0] * flat[0] + flat[1] * flat[1] );
			if ( horiz < 40.0f && fabs( flat[2] ) < 64.0f ) {
				b->pathIndex++;
				if ( b->pathIndex >= b->pathLen ) {
					b->repathTime = time;	// arrived — replan next think
				}
			}

			// Jump when the edge into the next node needs it, or the node is
			// clearly above us.
			qboolean jump = qfalse;
			if ( b->pathIndex < b->pathLen ) {
				int flags = SV_BotNav_EdgeFlags( b->path[b->pathIndex - 1], b->path[b->pathIndex] );
				if ( flags == 2 /* NAV_EDGE_JUMP */ || target[2] - ps->origin[2] > 24.0f ) {
					jump = qtrue;
				}
			}
			if ( b->stuckCount >= 3 ) {
				jump = qtrue;	// unwedge
			}

			SV_BotAI_MoveToward( clientNum, ps, target, jump, engaging, b->aimAngles, cmd );
			return;
		}
	}

	// No usable path (navmesh not ready, or off-mesh): collision-aware roam so
	// the bot still moves and can walk back onto the mesh.
	if ( time >= b->repathTime || b->stuckCount >= 2 ||
	     !SV_BotAI_WalkableAhead( clientNum, ps->origin, b->moveYaw, 40.0f ) ) {
		float bias = b->moveYaw + crandom() * 45.0f;
		b->moveYaw = SV_BotAI_ChooseHeading( clientNum, ps->origin, bias );
		b->repathTime = time + 1000;
		b->stuckCount = 0;
	}
	{
		vec3_t target;
		target[0] = ps->origin[0] + cos( DEG2RAD( b->moveYaw ) ) * 64.0f;
		target[1] = ps->origin[1] + sin( DEG2RAD( b->moveYaw ) ) * 64.0f;
		target[2] = ps->origin[2];
		qboolean jump = ( b->stuckCount >= 3 ) ? qtrue : qfalse;
		SV_BotAI_MoveToward( clientNum, ps, target, jump, engaging, b->aimAngles, cmd );
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
	qboolean haveBot = qfalse;

	// Build the navmesh the first time a bot is actually present, so games
	// without bots pay nothing and the world/entities are fully spawned.
	for ( i = 0; i < sv.maxclients; i++ ) {
		if ( svs.clients[i].state == CS_ACTIVE &&
		     svs.clients[i].netchan.remoteAddress.type == NA_BOT ) {
			haveBot = qtrue;
			break;
		}
	}
	if ( haveBot && !SV_BotNav_Ready() ) {
		SV_BotNav_Generate();
	}

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
