/*
===========================================================================
Portal System Integration with Quake3e-HD
This module bridges the portal system with the engine
===========================================================================
*/

#include "g_portal.h"
#include "../../../engine/common/q_shared.h"

// Stub functions for missing game module functions
// These would normally be provided by the game QVM

typedef struct gentity_s {
    entityState_t s;
    entityShared_t r;
    struct gclient_s *client;
    qboolean inuse;
    char *classname;
    int spawnflags;
    qboolean neverFree;
    int flags;
    char *model;
    char *model2;
    int freetime;
    int eventTime;
    qboolean freeAfterEvent;
    qboolean unlinkAfterEvent;
    qboolean physicsObject;
    float physicsBounce;
    int clipmask;
    char *target;
    char *targetname;
    char *team;
    char *targetShaderName;
    char *targetShaderNewName;
    struct gentity_s *target_ent;
    float speed;
    vec3_t movedir;
    int nextthink;
    void (*think)(struct gentity_s *ent);
    void (*reached)(struct gentity_s *ent);
    void (*blocked)(struct gentity_s *ent, struct gentity_s *other);
    void (*touch)(struct gentity_s *self, struct gentity_s *other, trace_t *trace);
    void (*use)(struct gentity_s *self, struct gentity_s *other, struct gentity_s *activator);
    void (*pain)(struct gentity_s *self, struct gentity_s *attacker, int damage);
    void (*die)(struct gentity_s *self, struct gentity_s *inflictor, struct gentity_s *attacker, int damage, int mod);
    int pain_debounce_time;
    int fly_sound_debounce_time;
    int last_move_time;
    int health;
    int takedamage;
    int damage;
    int splashDamage;
    int splashRadius;
    int methodOfDeath;
    int splashMethodOfDeath;
    int count;
    struct gentity_s *chain;
    struct gentity_s *enemy;
    struct gentity_s *activator;
    struct gentity_s *teamchain;
    struct gentity_s *teammaster;
    int watertype;
    int waterlevel;
    int noise_index;
    float wait;
    float random;
    gitem_t *item;
    int genericValue1;
    int genericValue2;
    int genericValue3;
    char *message;
    struct gentity_s *parent;
} gentity_t;

typedef struct gclient_s {
    playerState_t ps;
    int ping;
    int lastCmdTime;
    int buttons;
    int oldbuttons;
    int latched_buttons;
    vec3_t oldOrigin;
    int damage_armor;
    int damage_blood;
    int damage_knockback;
    vec3_t damage_from;
    qboolean damage_fromWorld;
    int accurateCount;
    int accuracy_shots;
    int accuracy_hits;
    int lastkilled_client;
    int lasthurt_client;
    int lasthurt_mod;
    int respawnTime;
    int inactivityTime;
    qboolean inactivityWarning;
    int rewardTime;
    int airOutTime;
    int lastKillTime;
    qboolean fireHeld;
    gentity_t *hook;
    int switchTeamTime;
    int switchClassTime;
    int timeResidual;
    char *areabits;
} gclient_t;

typedef struct {
    int time;
    int previousTime;
    int framenum;
    int startTime;
    int clientConnected[MAX_CLIENTS];
} level_locals_t;

// Global variables
static gentity_t g_entities_storage[MAX_GENTITIES];
gentity_t *g_entities = g_entities_storage;
static level_locals_t level_storage;
level_locals_t level = level_storage;
static int nextFreeEntity = 0;

// Initialize portal integration
void G_InitPortalIntegration(void) {
    memset(g_entities_storage, 0, sizeof(g_entities_storage));
    memset(&level_storage, 0, sizeof(level_storage));
    nextFreeEntity = MAX_CLIENTS;
    
    G_InitPortalSystem();
}

// Shutdown portal integration
void G_ShutdownPortalIntegration(void) {
    G_ShutdownPortalSystem();
}

// Entity management functions
gentity_t *G_Spawn(void) {
    int i;
    gentity_t *e;
    
    e = NULL;
    for (i = MAX_CLIENTS; i < MAX_GENTITIES; i++) {
        e = &g_entities[i];
        if (!e->inuse) {
            break;
        }
    }
    
    if (i == MAX_GENTITIES) {
        Com_Printf("G_Spawn: no free entities\n");
        return NULL;
    }
    
    memset(e, 0, sizeof(*e));
    e->inuse = qtrue;
    e->s.number = i;
    e->r.ownerNum = ENTITYNUM_NONE;
    e->classname = "noclass";
    e->s.eType = ET_GENERAL;
    e->s.eFlags = 0;
    
    return e;
}

void G_FreeEntity(gentity_t *ent) {
    if (!ent || !ent->inuse) {
        return;
    }
    
    memset(ent, 0, sizeof(*ent));
    ent->classname = "freed";
    ent->freetime = level.time;
    ent->inuse = qfalse;
}

// Stub for plasma projectile - creates a portal effect projectile
gentity_t *fire_plasma(gentity_t *self, vec3_t start, vec3_t dir) {
    gentity_t *bolt;
    
    bolt = G_Spawn();
    if (!bolt) {
        return NULL;
    }
    
    bolt->classname = "plasma";
    bolt->s.eType = ET_MISSILE;
    bolt->s.weapon = WP_PLASMAGUN;
    bolt->parent = self;
    bolt->damage = 20;
    bolt->splashDamage = 15;
    bolt->splashRadius = 20;
    bolt->methodOfDeath = MOD_PLASMA;
    bolt->splashMethodOfDeath = MOD_PLASMA_SPLASH;
    bolt->clipmask = MASK_SHOT;
    
    bolt->s.pos.trType = TR_LINEAR;
    bolt->s.pos.trTime = level.time - 50;
    VectorCopy(start, bolt->s.pos.trBase);
    VectorScale(dir, 2000, bolt->s.pos.trDelta);
    SnapVector(bolt->s.pos.trDelta);
    
    VectorCopy(start, bolt->s.origin);
    
    return bolt;
}

// Teleport player function
void TeleportPlayer(gentity_t *player, vec3_t origin, vec3_t angles) {
    if (!player || !player->client) {
        return;
    }
    
    VectorCopy(origin, player->client->ps.origin);
    player->client->ps.origin[2] += 1;
    
    VectorClear(player->client->ps.velocity);
    
    SetClientViewAngle(player, angles);
    
    if (player->client->sess.sessionTeam != TEAM_SPECTATOR) {
        player->s.eFlags ^= EF_TELEPORT_BIT;
        player->client->ps.eFlags ^= EF_TELEPORT_BIT;
    }
}

// Set client view angle
void SetClientViewAngle(gentity_t *ent, vec3_t angle) {
    int i;
    
    if (!ent || !ent->client) {
        return;
    }
    
    for (i = 0; i < 3; i++) {
        int cmdAngle = ANGLE2SHORT(angle[i]);
        ent->client->ps.delta_angles[i] = cmdAngle - ent->client->pers.cmd.angles[i];
    }
    
    VectorCopy(angle, ent->s.angles);
    VectorCopy(ent->s.angles, ent->client->ps.viewangles);
}

// Process portal commands from client input
void G_ProcessPortalCommands(gentity_t *ent, usercmd_t *ucmd) {
    if (!ent || !ent->client || !ucmd) {
        return;
    }
    
    // Check for portal firing commands
    if (ucmd->buttons & BUTTON_PORTAL_ORANGE) {
        if (!(ent->client->oldbuttons & BUTTON_PORTAL_ORANGE)) {
            G_FirePortal(ent, PORTAL_ORANGE);
        }
    }
    
    if (ucmd->buttons & BUTTON_PORTAL_BLUE) {
        if (!(ent->client->oldbuttons & BUTTON_PORTAL_BLUE)) {
            G_FirePortal(ent, PORTAL_BLUE);
        }
    }
    
    if (ucmd->buttons & BUTTON_PORTAL_CLOSE) {
        if (!(ent->client->oldbuttons & BUTTON_PORTAL_CLOSE)) {
            G_ClosePlayerPortals(ent);
        }
    }
    
    ent->client->oldbuttons = ucmd->buttons;
}

// Check for fall damage immunity
qboolean G_CheckPortalFallDamageImmunity(gentity_t *ent) {
    int clientNum;
    
    if (!ent || !ent->client) {
        return qfalse;
    }
    
    clientNum = ent->client - level.clients;
    if (clientNum < 0 || clientNum >= MAX_CLIENTS) {
        return qfalse;
    }
    
    if (g_playerPortalStates[clientNum].fallDamageImmunityEndTime > level.time) {
        return qtrue;
    }
    
    return qfalse;
}

// Utility functions
float vectoyaw(const vec3_t vec) {
    float yaw;
    
    if (vec[YAW] == 0 && vec[PITCH] == 0) {
        yaw = 0;
    } else {
        if (vec[PITCH]) {
            yaw = (atan2(vec[YAW], vec[PITCH]) * 180 / M_PI);
        } else if (vec[YAW] > 0) {
            yaw = 90;
        } else {
            yaw = 270;
        }
        
        if (yaw < 0) {
            yaw += 360;
        }
    }
    
    return yaw;
}

void AxisToAngles(vec3_t axis, vec3_t angles) {
    angles[0] = asin(axis[2]) * 180 / M_PI;
    angles[1] = atan2(axis[1], axis[0]) * 180 / M_PI;
    angles[2] = 0;
}

void RotatePointAroundVector(vec3_t dst, const vec3_t dir, const vec3_t point, float degrees) {
    float m[3][3];
    float im[3][3];
    float zrot[3][3];
    float tmpmat[3][3];
    float rot[3][3];
    float angle;
    vec3_t vr, vup, vf;
    
    angle = DEG2RAD(degrees);
    
    vf[0] = dir[0];
    vf[1] = dir[1];
    vf[2] = dir[2];
    
    PerpendicularVector(vr, dir);
    CrossProduct(vr, vf, vup);
    
    m[0][0] = vr[0];
    m[1][0] = vr[1];
    m[2][0] = vr[2];
    
    m[0][1] = vup[0];
    m[1][1] = vup[1];
    m[2][1] = vup[2];
    
    m[0][2] = vf[0];
    m[1][2] = vf[1];
    m[2][2] = vf[2];
    
    memcpy(im, m, sizeof(im));
    
    im[0][1] = m[1][0];
    im[0][2] = m[2][0];
    im[1][0] = m[0][1];
    im[1][2] = m[2][1];
    im[2][0] = m[0][2];
    im[2][1] = m[1][2];
    
    memset(zrot, 0, sizeof(zrot));
    zrot[0][0] = cos(angle);
    zrot[0][1] = sin(angle);
    zrot[1][0] = -sin(angle);
    zrot[1][1] = cos(angle);
    zrot[2][2] = 1.0F;
    
    MatrixMultiply(m, zrot, tmpmat);
    MatrixMultiply(tmpmat, im, rot);
    
    dst[0] = rot[0][0] * point[0] + rot[0][1] * point[1] + rot[0][2] * point[2];
    dst[1] = rot[1][0] * point[0] + rot[1][1] * point[1] + rot[1][2] * point[2];
    dst[2] = rot[2][0] * point[0] + rot[2][1] * point[1] + rot[2][2] * point[2];
}

void MatrixMultiply(float in1[3][3], float in2[3][3], float out[3][3]) {
    out[0][0] = in1[0][0] * in2[0][0] + in1[0][1] * in2[1][0] + in1[0][2] * in2[2][0];
    out[0][1] = in1[0][0] * in2[0][1] + in1[0][1] * in2[1][1] + in1[0][2] * in2[2][1];
    out[0][2] = in1[0][0] * in2[0][2] + in1[0][1] * in2[1][2] + in1[0][2] * in2[2][2];
    out[1][0] = in1[1][0] * in2[0][0] + in1[1][1] * in2[1][0] + in1[1][2] * in2[2][0];
    out[1][1] = in1[1][0] * in2[0][1] + in1[1][1] * in2[1][1] + in1[1][2] * in2[2][1];
    out[1][2] = in1[1][0] * in2[0][2] + in1[1][1] * in2[1][2] + in1[1][2] * in2[2][2];
    out[2][0] = in1[2][0] * in2[0][0] + in1[2][1] * in2[1][0] + in1[2][2] * in2[2][0];
    out[2][1] = in1[2][0] * in2[0][1] + in1[2][1] * in2[1][1] + in1[2][2] * in2[2][1];
    out[2][2] = in1[2][0] * in2[0][2] + in1[2][1] * in2[1][2] + in1[2][2] * in2[2][2];
}

void PerpendicularVector(vec3_t dst, const vec3_t src) {
    int pos;
    int i;
    float minelem = 1.0F;
    vec3_t tempvec;
    
    for (pos = 0, i = 0; i < 3; i++) {
        if (fabs(src[i]) < minelem) {
            pos = i;
            minelem = fabs(src[i]);
        }
    }
    
    tempvec[0] = tempvec[1] = tempvec[2] = 0.0F;
    tempvec[pos] = 1.0F;
    
    ProjectPointOnPlane(dst, tempvec, src);
    VectorNormalize(dst);
}

void ProjectPointOnPlane(vec3_t dst, const vec3_t p, const vec3_t normal) {
    float d;
    vec3_t n;
    float inv_denom;
    
    inv_denom = 1.0F / DotProduct(normal, normal);
    
    d = DotProduct(normal, p) * inv_denom;
    
    n[0] = normal[0] * inv_denom;
    n[1] = normal[1] * inv_denom;
    n[2] = normal[2] * inv_denom;
    
    dst[0] = p[0] - d * n[0];
    dst[1] = p[1] - d * n[1];
    dst[2] = p[2] - d * n[2];
}