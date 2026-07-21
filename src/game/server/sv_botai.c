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

// What the bot is currently trying to do (top-level goal from the utility
// scorer). Kept as a label for the mover and for debug.
typedef enum {
	BGOAL_ROAM,			// wander the map
	BGOAL_HUNT,			// go to a believed enemy position
	BGOAL_ITEM,			// pick up a specific item
	BGOAL_RETREAT		// break contact toward safety/health
} botGoalType_t;

// Per-bot persistent AI state, indexed by client number.
typedef struct {
	qboolean	active;

	// Navmesh path being followed
	int			path[BOT_MAX_PATH];
	int			pathLen;
	int			pathIndex;		// next node in path to reach
	int			goalNode;		// destination node
	int			repathTime;		// hard re-plan deadline (ms)
	botGoalType_t goalType;

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
	float		aimWander[2];	// slowly drifting aim-error phase (pitch,yaw)

	// Belief: last thing the bot knew about an enemy (sight or sound). Lets it
	// pursue a target it can no longer see instead of forgetting instantly.
	int			beliefEnemy;	// client number the belief is about, or -1
	vec3_t		beliefPos;		// last known/heard position
	int			beliefTime;		// when the belief was last refreshed (ms)

	// Skill (1..5) resolved once per life from the difficulty cvar.
	int			skill;

	// Learning: running combat record used to nudge skill toward a target.
	int			kills;
	int			deaths;
	int			lastDeathCount;

	// Respawn pacing so a dead bot doesn't machine-gun the attack button
	int			respawnTime;
} sv_botai_t;

static sv_botai_t sv_botai[MAX_CLIENTS];

static cvar_t *sv_botDebug;
static cvar_t *sv_botSkill;		// 1..5 base difficulty
static cvar_t *sv_botLearn;		// adapt skill toward an even fight

// Difficulty curves, indexed by skill 1..5. These are what actually make a
// skill-1 bot feel like a fumbling novice and a skill-5 bot feel sharp.
typedef struct {
	int		reactionMs;		// delay before firing on a fresh target
	float	turnRate;		// max view turn per think (deg)
	float	aimError;		// steady-state aim wander amplitude (deg)
	float	fireCone;		// max aim error that still pulls the trigger (deg)
	float	visLead;		// projectile lead fraction (0 = none)
} botSkillParams_t;

static const botSkillParams_t bot_skillTable[6] = {
	// [0] unused
	{ 0,   0.0f,  0.0f, 0.0f,  0.0f },
	// 1: novice — slow to react, jerky aim, sprays
	{ 520, 9.0f,  9.0f, 18.0f, 0.0f },
	// 2
	{ 400, 13.0f, 6.5f, 15.0f, 0.15f },
	// 3: solid pubber
	{ 300, 18.0f, 4.5f, 12.0f, 0.35f },
	// 4
	{ 230, 24.0f, 2.8f, 9.0f,  0.6f },
	// 5: sharp — fast, steady, leads shots
	{ 160, 32.0f, 1.4f, 7.0f,  0.85f },
};

static const botSkillParams_t *SV_BotAI_Skill( int clientNum ) {
	int s = sv_botai[clientNum].skill;
	if ( s < 1 ) s = 1;
	else if ( s > 5 ) s = 5;
	return &bot_skillTable[s];
}

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
	if ( !sv_botSkill ) {
		sv_botSkill = Cvar_Get( "sv_botSkill", "4", CVAR_ARCHIVE );
		Cvar_CheckRange( sv_botSkill, "1", "5", CV_INTEGER );
	}
	if ( !sv_botLearn ) {
		sv_botLearn = Cvar_Get( "sv_botLearn", "0", CVAR_ARCHIVE );
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
	b->goalType = BGOAL_ROAM;
	b->repathTime = 0;
	b->stuckCheckTime = 0;
	b->stuckCount = 0;
	VectorCopy( ps->origin, b->lastOrigin );
	b->enemy = -1;
	b->enemySeenTime = 0;
	b->reactionTime = 0;
	b->fireHoldTime = 0;
	b->aimWander[0] = random() * 6.28f;
	b->aimWander[1] = random() * 6.28f;
	VectorCopy( ps->viewangles, b->aimAngles );
	b->beliefEnemy = -1;
	b->beliefTime = 0;
	// Resolve skill for this life. sv_botLearn nudges it toward an even K/D.
	b->skill = sv_botSkill ? sv_botSkill->integer : 4;
	if ( sv_botLearn && sv_botLearn->integer && ( b->kills + b->deaths ) >= 6 ) {
		if ( b->kills > b->deaths + 3 && b->skill > 1 ) b->skill--;
		else if ( b->deaths > b->kills + 3 && b->skill < 5 ) b->skill++;
	}
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
	const botSkillParams_t *sk = SV_BotAI_Skill( clientNum );

	int enemy = SV_BotAI_FindEnemy( clientNum, ps );
	if ( enemy != b->enemy ) {
		// New target: the bot needs a moment (skill-scaled) to react before it
		// can fire.
		if ( enemy >= 0 ) {
			b->reactionTime = time + sk->reactionMs;
		}
		b->enemy = enemy;
	}

	// Refresh the belief store whenever we can see the enemy — this is what
	// lets the bot pursue a target that later breaks line of sight.
	if ( enemy >= 0 ) {
		sharedEntity_t *ee = SV_GentityNum( enemy );
		b->beliefEnemy = enemy;
		VectorCopy( ee->r.currentOrigin, b->beliefPos );
		b->beliefTime = time;
	}

	if ( enemy < 0 ) {
		// No visible enemy — keep firing for a short beat if we just lost one
		// (so a target ducking behind cover isn't instantly forgotten), then
		// hand movement back to navigation/hunting.
		return ( time < b->fireHoldTime ) ? qtrue : qfalse;
	}

	b->enemySeenTime = time;
	b->fireHoldTime = time + 400;

	// Aim point: enemy eye level, led by the enemy's velocity (higher skill
	// leads more accurately).
	vec3_t eye, target, dir, want;
	VectorCopy( ps->origin, eye );
	eye[2] += ps->viewheight;
	sharedEntity_t *ent = SV_GentityNum( enemy );
	playerState_t *eps = SV_GameClientNum( enemy );
	VectorCopy( ent->r.currentOrigin, target );
	target[2] += eps->viewheight;

	float dist = Distance( eye, target );
	if ( sk->visLead > 0.0f ) {
		float leadTime = ( dist / 900.0f ) * sk->visLead;	// ~rocket-speed lead
		VectorMA( target, leadTime, eps->velocity, target );
	}

	VectorSubtract( target, eye, dir );
	vectoangles( dir, want );

	// Humanized aim error: a smoothly drifting offset (two out-of-phase sines,
	// NOT per-frame white noise, which looks robotic). Amplitude comes from
	// skill and grows a little at range.
	b->aimWander[0] += 0.11f;
	b->aimWander[1] += 0.079f;
	float amp = sk->aimError * ( 1.0f + dist / 2000.0f );
	want[PITCH] += sin( b->aimWander[0] ) * amp * 0.6f;
	want[YAW]   += sin( b->aimWander[1] ) * amp;

	// Turn-rate-limited aim (skill-scaled; snappier at short range).
	float turn = sk->turnRate * ( ( dist < 400.0f ) ? 1.3f : 1.0f );
	float err = SV_BotAI_TurnTowards( b->aimAngles, want, turn );
	SV_BotAI_SetViewAngles( cmd, b->aimAngles );

	// Fire once the reaction delay has passed and the aim is inside the
	// skill-scaled fire cone.
	if ( time >= b->reactionTime && err < sk->fireCone ) {
		cmd->buttons |= BUTTON_ATTACK;
	}
	return qtrue;
}

/*
==================
SV_BotAI_Hear

Passive hearing: an enemy firing within earshot reveals its position as a
belief, even without line of sight, so bots converge on fights instead of
only reacting to what is directly visible. Firing is read from the client's
last usercmd (BUTTON_ATTACK).
==================
*/
static void SV_BotAI_Hear( int clientNum, const playerState_t *ps, int time ) {
	sv_botai_t *b = &sv_botai[clientNum];
	int myTeam = ps->persistant[PERS_TEAM];

	for ( int i = 0; i < sv.maxclients; i++ ) {
		if ( i == clientNum || svs.clients[i].state != CS_ACTIVE ) {
			continue;
		}
		if ( !( svs.clients[i].lastUsercmd.buttons & BUTTON_ATTACK ) ) {
			continue;
		}
		playerState_t *eps = SV_GameClientNum( i );
		if ( eps->pm_type == PM_DEAD || eps->stats[STAT_HEALTH] <= 0 ) {
			continue;
		}
		if ( myTeam != TEAM_FREE && eps->persistant[PERS_TEAM] == myTeam ) {
			continue;
		}
		sharedEntity_t *ent = SV_GentityNum( i );
		float dist = Distance( ps->origin, ent->r.currentOrigin );
		if ( dist > 1400.0f ) {
			continue;	// out of earshot
		}
		// Only overwrite an older/further belief.
		if ( b->beliefEnemy < 0 || time - b->beliefTime > 500 ) {
			b->beliefEnemy = i;
			VectorCopy( ent->r.currentOrigin, b->beliefPos );
			b->beliefTime = time;
		}
	}
}

/*
==================
SV_BotAI_Replan

Find the nearest visible, unclaimed pickup item (spawned entity of type
ET_ITEM that is currently drawn). Returns its world position via out and
qtrue, or qfalse if none.
==================
*/
static qboolean SV_BotAI_NearestItem( int clientNum, const playerState_t *ps, vec3_t out ) {
	vec3_t eye;
	VectorCopy( ps->origin, eye );
	eye[2] += ps->viewheight;

	float bestDist = 1e30f;
	qboolean found = qfalse;

	// Item entities live above the client range. A picked-up item is hidden
	// with EF_NODRAW until it respawns, so skip those.
	for ( int e = sv.maxclients; e < MAX_GENTITIES; e++ ) {
		sharedEntity_t *ent = SV_GentityNum( e );
		if ( !ent->r.linked || ent->s.eType != ET_ITEM ) {
			continue;
		}
		if ( ent->s.eFlags & EF_NODRAW ) {
			continue;
		}
		float dist = Distance( eye, ent->r.currentOrigin );
		if ( dist < bestDist ) {
			bestDist = dist;
			VectorCopy( ent->r.currentOrigin, out );
			found = qtrue;
		}
	}
	return found;
}

/*
==================
SV_BotAI_RouteTo

Plan a route from the bot to the nav node nearest a world point. Returns qtrue
on success and records the goal type.
==================
*/
static qboolean SV_BotAI_RouteTo( int clientNum, int startNode, const vec3_t dest,
                                  botGoalType_t type, int time ) {
	sv_botai_t *b = &sv_botai[clientNum];
	int goal = SV_BotNav_NearestNode( dest );
	if ( goal < 0 || goal == startNode ) {
		return qfalse;
	}
	int len = SV_BotNav_FindPath( startNode, goal, b->path, BOT_MAX_PATH );
	if ( len < 2 ) {
		return qfalse;
	}
	b->pathLen = len;
	b->pathIndex = 1;
	b->goalNode = goal;
	b->goalType = type;
	b->repathTime = time + 8000;
	return qtrue;
}

/*
==================
SV_BotAI_Replan

Two-tier utility goal selection: score the candidate goals (hunt a believed
enemy, grab health when hurt, otherwise roam for items/position) and route to
the winner. Replaces the old "random node" roaming.
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

	int health = ps->stats[STAT_HEALTH];

	// --- Strategic tier: score goals, highest wins. ---
	float huntScore = 0.0f, itemScore = 0.0f;
	qboolean haveBelief = ( b->beliefEnemy >= 0 && time - b->beliefTime < 6000 );
	if ( haveBelief ) {
		// Fresher belief = stronger pull; back off when badly hurt.
		float freshness = 1.0f - ( time - b->beliefTime ) / 6000.0f;
		huntScore = 0.55f + 0.35f * freshness;
		if ( health < 40 ) huntScore *= 0.4f;
	}

	vec3_t itemPos;
	qboolean haveItem = SV_BotAI_NearestItem( clientNum, ps, itemPos );
	if ( haveItem ) {
		itemScore = 0.35f;
		if ( health < 60 ) itemScore += 0.4f;	// want pickups (health/armor) when hurt
	}

	// Retreat bias: badly hurt with a live threat -> prefer an item (health)
	// over hunting.
	if ( health < 35 && haveItem ) {
		itemScore += 0.3f;
	}

	if ( haveBelief && huntScore >= itemScore ) {
		if ( SV_BotAI_RouteTo( clientNum, startNode, b->beliefPos, BGOAL_HUNT, time ) ) {
			return qtrue;
		}
	}
	if ( haveItem ) {
		if ( SV_BotAI_RouteTo( clientNum, startNode, itemPos, BGOAL_ITEM, time ) ) {
			return qtrue;
		}
	}

	// --- Fallback: roam to a random reachable node. ---
	for ( int attempt = 0; attempt < 8; attempt++ ) {
		int goal = SV_BotNav_RandomNode();
		if ( goal < 0 || goal == startNode ) {
			continue;
		}
		int len = SV_BotNav_FindPath( startNode, goal, b->path, BOT_MAX_PATH );
		if ( len >= 2 ) {
			b->pathLen = len;
			b->pathIndex = 1;
			b->goalNode = goal;
			b->goalType = BGOAL_ROAM;
			b->repathTime = time + 12000;
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

	// Dead: hold view and tap fire to respawn, paced so we don't spam it. This
	// is checked before per-life init so a corpse doesn't re-initialize every
	// frame (which would also miscount deaths).
	if ( ps->pm_type == PM_DEAD || ps->stats[STAT_HEALTH] <= 0 ) {
		if ( b->active ) {
			b->deaths++;			// count each death once (alive->dead edge)
			b->active = qfalse;		// next live frame re-inits the bot
		}
		SV_BotAI_SetViewAngles( cmd, ps->viewangles );
		if ( time >= b->respawnTime ) {
			cmd->buttons = BUTTON_ATTACK;
			b->respawnTime = time + 700;
		}
		return;
	}

	// Alive: (re)initialize on (re)spawn.
	if ( !b->active ) {
		SV_BotAI_ClientActive( clientNum, ps );
	}

	// Track frags from the score for the learning layer (FFA: score == frags).
	int score = ps->persistant[PERS_SCORE];
	if ( score > b->lastDeathCount ) {
		b->kills += score - b->lastDeathCount;
	}
	b->lastDeathCount = score;

	// Progress / stuck sampling.
	if ( time >= b->stuckCheckTime ) {
		float moved = Distance( ps->origin, b->lastOrigin );
		b->stuckCount = ( moved < 10.0f ) ? b->stuckCount + 1 : 0;
		VectorCopy( ps->origin, b->lastOrigin );
		b->stuckCheckTime = time + 250;
	}

	// Passive hearing feeds the belief store so bots converge on nearby fights.
	SV_BotAI_Hear( clientNum, ps, time );

	// Combat: if engaging, aim and fire. The bot still advances along its nav
	// path underneath (movement below), but its view is owned by the fight, so
	// it keeps pathing toward items/goals while shooting. Keep the aim frame in
	// sync when not fighting so the first shot on a new enemy starts from where
	// the bot is actually looking.
	qboolean engaging = SV_BotAI_Combat( clientNum, ps, time, cmd );
	if ( !engaging ) {
		VectorCopy( ps->viewangles, b->aimAngles );
	}

	// (Re)plan when we have no path, reached the goal, hit the deadline, are
	// wedged, or a fresh belief appeared that we're not already hunting (so the
	// bot promptly pursues a target it just saw/heard).
	qboolean freshBelief = ( b->beliefEnemy >= 0 && time - b->beliefTime < 1500 &&
	                         b->goalType != BGOAL_HUNT );
	if ( b->pathLen < 2 || b->pathIndex >= b->pathLen ||
	     time >= b->repathTime || b->stuckCount >= 6 || freshBelief ) {
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
			Com_Printf( "botai: %s org=(%.0f %.0f %.0f) hp=%d enemy=%d goal=%d belief=%d k=%d d=%d\n",
				cl->name, ps->origin[0], ps->origin[1], ps->origin[2],
				ps->stats[STAT_HEALTH], sv_botai[i].enemy, sv_botai[i].goalType,
				sv_botai[i].beliefEnemy, sv_botai[i].kills, sv_botai[i].deaths );
		}
	}
}
