/*
===========================================================================
Portal System for Quake3e-HD
Portal-style gameplay implementation
===========================================================================
*/

#ifndef G_PORTAL_H
#define G_PORTAL_H

#include "../../../engine/common/q_shared.h"

#define MAX_PORTAL_PAIRS 8
#define PORTAL_RADIUS 32.0f
#define PORTAL_ACTIVATION_TIME 500
#define PORTAL_CLOSE_TIME 300
#define FALL_DAMAGE_IMMUNITY_TIME 5000

typedef enum {
    PORTAL_ORANGE = 0,
    PORTAL_BLUE = 1,
    PORTAL_MAX_TYPES
} portalType_t;

typedef enum {
    PORTAL_STATE_INACTIVE = 0,
    PORTAL_STATE_OPENING,
    PORTAL_STATE_ACTIVE,
    PORTAL_STATE_CLOSING,
    PORTAL_STATE_CLOSED
} portalState_t;

typedef struct portalInfo_s {
    qboolean        inUse;
    portalType_t    type;
    portalState_t   state;
    
    int             entityNum;
    int             linkedPortalNum;
    int             ownerNum;
    
    vec3_t          origin;
    vec3_t          surfaceNormal;
    vec3_t          portalForward;
    vec3_t          portalRight;
    vec3_t          portalUp;
    
    float           radius;
    int             creationTime;
    int             stateChangeTime;
    
    matrix3_t       rotationMatrix;
    vec3_t          viewOffset;
} portalInfo_t;

typedef struct playerPortalState_s {
    int             lastPortalExitTime;
    int             fallDamageImmunityEndTime;
    vec3_t          lastPortalExitVelocity;
    int             activeOrangePortal;
    int             activeBluePortal;
} playerPortalState_t;

extern portalInfo_t g_portals[MAX_PORTAL_PAIRS * 2];
extern playerPortalState_t g_playerPortalStates[MAX_CLIENTS];

void G_InitPortalSystem(void);
void G_ShutdownPortalSystem(void);
void G_FirePortal(gentity_t *player, portalType_t type);
void G_ClosePlayerPortals(gentity_t *player);
void G_CreatePortal(vec3_t origin, vec3_t normal, gentity_t *owner, portalType_t type);
void G_RemovePortal(int portalNum);
void G_UpdatePortal(gentity_t *portal);
void G_PortalThink(gentity_t *portal);
qboolean G_CheckPortalTeleport(gentity_t *ent, gentity_t *portal);
void G_TeleportThroughPortal(gentity_t *ent, gentity_t *enterPortal, gentity_t *exitPortal);
void G_TransformVelocityThroughPortal(vec3_t velocity, vec3_t enterNormal, vec3_t exitNormal);
qboolean G_TracePortalSurface(vec3_t start, vec3_t end, vec3_t outOrigin, vec3_t outNormal);
qboolean G_IsValidPortalSurface(trace_t *trace);
void G_PortalTouch(gentity_t *portal, gentity_t *other, trace_t *trace);
void G_PortalProjectileImpact(gentity_t *projectile, trace_t *trace, portalType_t type);
qboolean G_TraceThroughPortals(vec3_t start, vec3_t end, trace_t *trace, int passEntityNum);
void G_AddPortalCommands(void);

#endif