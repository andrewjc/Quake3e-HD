/*
===========================================================================
Client-side Portal Effects and Rendering
===========================================================================
*/

#include "../../../engine/common/q_shared.h"
#include "../../server/portal/g_portal.h"

#define PORTAL_EFFECT_TIME 500
#define PORTAL_RING_SEGMENTS 32
#define PORTAL_PARTICLE_COUNT 64

typedef struct {
    vec3_t origin;
    vec3_t normal;
    vec3_t up;
    vec3_t right;
    float radius;
    int type;
    int state;
    int stateChangeTime;
    qboolean active;
    int linkedPortal;
} clientPortalInfo_t;

static clientPortalInfo_t cg_portals[MAX_PORTAL_PAIRS * 2];

/*
==================
CG_InitPortals

Initialize client-side portal system
==================
*/
void CG_InitPortals(void) {
    memset(cg_portals, 0, sizeof(cg_portals));
}

/*
==================
CG_AddPortalToScene

Add a portal entity to the scene
==================
*/
void CG_AddPortalToScene(centity_t *cent) {
    refEntity_t ent;
    vec3_t angles;
    int portalNum;
    clientPortalInfo_t *portal;
    float scale;
    int timeDiff;
    vec4_t color;
    polyVert_t verts[PORTAL_RING_SEGMENTS + 1];
    int i;
    float angle;
    vec3_t point;
    
    // Get portal number from entity
    portalNum = cent->currentState.generic1;
    if (portalNum < 0 || portalNum >= MAX_PORTAL_PAIRS * 2) {
        return;
    }
    
    portal = &cg_portals[portalNum];
    
    // Update portal info from entity state
    VectorCopy(cent->currentState.origin, portal->origin);
    VectorCopy(cent->currentState.origin2, portal->normal);
    portal->type = cent->currentState.modelindex;
    portal->linkedPortal = cent->currentState.otherEntityNum;
    
    // Calculate portal orientation
    CG_SetupPortalOrientation(portal);
    
    // Determine portal state and animation
    timeDiff = cg.time - cent->currentState.time;
    if (timeDiff < PORTAL_ACTIVATION_TIME) {
        portal->state = PORTAL_STATE_OPENING;
        scale = (float)timeDiff / PORTAL_ACTIVATION_TIME;
    } else {
        portal->state = PORTAL_STATE_ACTIVE;
        scale = 1.0f;
    }
    
    portal->radius = PORTAL_RADIUS * scale;
    
    // Set portal color based on type
    if (portal->type == PORTAL_ORANGE) {
        color[0] = 1.0f;
        color[1] = 0.5f;
        color[2] = 0.0f;
        color[3] = 0.8f;
    } else {
        color[0] = 0.0f;
        color[1] = 0.5f;
        color[2] = 1.0f;
        color[3] = 0.8f;
    }
    
    // Create portal ring geometry
    CG_DrawPortalRing(portal, color, scale);
    
    // Add portal particles
    if (portal->state == PORTAL_STATE_OPENING) {
        CG_AddPortalOpeningParticles(portal, scale);
    } else if (portal->state == PORTAL_STATE_ACTIVE) {
        CG_AddPortalActiveParticles(portal);
    }
    
    // Add portal glow effect
    CG_AddPortalGlow(portal, color);
    
    // If both portals are active, set up remote view
    if (portal->state == PORTAL_STATE_ACTIVE && portal->linkedPortal != ENTITYNUM_NONE) {
        CG_SetupPortalView(portal);
    }
}

/*
==================
CG_SetupPortalOrientation

Calculate portal orientation vectors
==================
*/
void CG_SetupPortalOrientation(clientPortalInfo_t *portal) {
    vec3_t up;
    
    // Generate up vector
    if (fabs(portal->normal[2]) > 0.9f) {
        VectorSet(up, 1, 0, 0);
    } else {
        VectorSet(up, 0, 0, 1);
    }
    
    // Calculate right and up vectors
    CrossProduct(up, portal->normal, portal->right);
    VectorNormalize(portal->right);
    CrossProduct(portal->normal, portal->right, portal->up);
    VectorNormalize(portal->up);
}

/*
==================
CG_DrawPortalRing

Draw the portal ring effect
==================
*/
void CG_DrawPortalRing(clientPortalInfo_t *portal, vec4_t color, float scale) {
    polyVert_t verts[PORTAL_RING_SEGMENTS * 2];
    int i;
    float angle;
    vec3_t inner, outer;
    float innerRadius = portal->radius * 0.9f;
    float outerRadius = portal->radius;
    qhandle_t shader;
    
    // Get portal ring shader
    shader = trap_R_RegisterShader("portal/ring");
    
    // Generate ring vertices
    for (i = 0; i < PORTAL_RING_SEGMENTS; i++) {
        angle = (float)i / PORTAL_RING_SEGMENTS * M_PI * 2;
        
        // Inner vertex
        VectorMA(portal->origin, cos(angle) * innerRadius, portal->right, inner);
        VectorMA(inner, sin(angle) * innerRadius, portal->up, verts[i * 2].xyz);
        verts[i * 2].st[0] = 0.0f;
        verts[i * 2].st[1] = (float)i / PORTAL_RING_SEGMENTS;
        verts[i * 2].modulate[0] = color[0] * 255;
        verts[i * 2].modulate[1] = color[1] * 255;
        verts[i * 2].modulate[2] = color[2] * 255;
        verts[i * 2].modulate[3] = color[3] * 255;
        
        // Outer vertex
        VectorMA(portal->origin, cos(angle) * outerRadius, portal->right, outer);
        VectorMA(outer, sin(angle) * outerRadius, portal->up, verts[i * 2 + 1].xyz);
        verts[i * 2 + 1].st[0] = 1.0f;
        verts[i * 2 + 1].st[1] = (float)i / PORTAL_RING_SEGMENTS;
        verts[i * 2 + 1].modulate[0] = color[0] * 255;
        verts[i * 2 + 1].modulate[1] = color[1] * 255;
        verts[i * 2 + 1].modulate[2] = color[2] * 255;
        verts[i * 2 + 1].modulate[3] = color[3] * 255;
    }
    
    // Draw the ring
    trap_R_AddPolyToScene(shader, PORTAL_RING_SEGMENTS * 2, verts, 1);
}

/*
==================
CG_AddPortalOpeningParticles

Add particles for portal opening effect
==================
*/
void CG_AddPortalOpeningParticles(clientPortalInfo_t *portal, float scale) {
    localEntity_t *le;
    vec3_t velocity;
    vec3_t origin;
    int i;
    float angle, speed;
    
    for (i = 0; i < PORTAL_PARTICLE_COUNT * scale; i++) {
        angle = random() * M_PI * 2;
        speed = 100 + random() * 200;
        
        // Start from portal center
        VectorCopy(portal->origin, origin);
        
        // Add some randomness
        VectorMA(origin, (random() - 0.5f) * portal->radius, portal->right, origin);
        VectorMA(origin, (random() - 0.5f) * portal->radius, portal->up, origin);
        
        // Velocity outward from center
        VectorScale(portal->normal, -speed, velocity);
        velocity[0] += (random() - 0.5f) * 100;
        velocity[1] += (random() - 0.5f) * 100;
        velocity[2] += (random() - 0.5f) * 100;
        
        // Create particle
        le = CG_AllocLocalEntity();
        if (!le) {
            break;
        }
        
        le->leType = LE_MOVE_SCALE_FADE;
        le->startTime = cg.time;
        le->endTime = cg.time + 1000;
        le->lifeRate = 1.0f / (le->endTime - le->startTime);
        
        VectorCopy(origin, le->pos.trBase);
        VectorCopy(velocity, le->pos.trDelta);
        le->pos.trType = TR_GRAVITY;
        le->pos.trTime = cg.time;
        
        le->color[0] = portal->type == PORTAL_ORANGE ? 1.0f : 0.0f;
        le->color[1] = 0.5f;
        le->color[2] = portal->type == PORTAL_ORANGE ? 0.0f : 1.0f;
        le->color[3] = 1.0f;
        
        le->radius = 2 + random() * 4;
    }
}

/*
==================
CG_AddPortalActiveParticles

Add swirling particles for active portal
==================
*/
void CG_AddPortalActiveParticles(clientPortalInfo_t *portal) {
    localEntity_t *le;
    vec3_t velocity;
    vec3_t origin;
    float angle, radius;
    float time = cg.time * 0.001f;
    
    // Create swirling particles
    angle = time * 2;
    radius = portal->radius * 0.8f;
    
    VectorMA(portal->origin, cos(angle) * radius, portal->right, origin);
    VectorMA(origin, sin(angle) * radius, portal->up, origin);
    
    // Velocity tangent to circle
    VectorScale(portal->right, -sin(angle) * 50, velocity);
    VectorMA(velocity, cos(angle) * 50, portal->up, velocity);
    
    le = CG_AllocLocalEntity();
    if (le) {
        le->leType = LE_MOVE_SCALE_FADE;
        le->startTime = cg.time;
        le->endTime = cg.time + 500;
        le->lifeRate = 1.0f / (le->endTime - le->startTime);
        
        VectorCopy(origin, le->pos.trBase);
        VectorCopy(velocity, le->pos.trDelta);
        le->pos.trType = TR_LINEAR;
        le->pos.trTime = cg.time;
        
        le->color[0] = portal->type == PORTAL_ORANGE ? 1.0f : 0.0f;
        le->color[1] = 0.5f;
        le->color[2] = portal->type == PORTAL_ORANGE ? 0.0f : 1.0f;
        le->color[3] = 0.5f;
        
        le->radius = 3;
    }
}

/*
==================
CG_AddPortalGlow

Add glow effect around portal
==================
*/
void CG_AddPortalGlow(clientPortalInfo_t *portal, vec4_t color) {
    refEntity_t ent;
    
    memset(&ent, 0, sizeof(ent));
    
    VectorCopy(portal->origin, ent.origin);
    ent.reType = RT_SPRITE;
    ent.customShader = trap_R_RegisterShader("portal/glow");
    ent.radius = portal->radius * 2;
    ent.shaderRGBA[0] = color[0] * 255;
    ent.shaderRGBA[1] = color[1] * 255;
    ent.shaderRGBA[2] = color[2] * 255;
    ent.shaderRGBA[3] = color[3] * 128;
    
    trap_R_AddRefEntityToScene(&ent);
}

/*
==================
CG_SetupPortalView

Set up remote view through portal
==================
*/
void CG_SetupPortalView(clientPortalInfo_t *portal) {
    refdef_t refdef;
    clientPortalInfo_t *linkedPortal;
    
    if (portal->linkedPortal < 0 || portal->linkedPortal >= MAX_PORTAL_PAIRS * 2) {
        return;
    }
    
    linkedPortal = &cg_portals[portal->linkedPortal];
    if (!linkedPortal->active) {
        return;
    }
    
    // Copy current refdef
    memcpy(&refdef, &cg.refdef, sizeof(refdef));
    
    // Transform view through portal
    BG_TransformPointThroughPortal(refdef.vieworg,
                                  portal->origin, portal->normal, portal->up,
                                  linkedPortal->origin, linkedPortal->normal, linkedPortal->up,
                                  refdef.vieworg);
    
    BG_TransformAnglesThroughPortal(refdef.viewangles,
                                    portal->normal, portal->up,
                                    linkedPortal->normal, linkedPortal->up);
    
    // Render portal view
    trap_R_RenderScene(&refdef);
}

/*
==================
CG_AllocLocalEntity

Allocate a local entity for effects
==================
*/
localEntity_t *CG_AllocLocalEntity(void) {
    // This would normally be implemented in the cgame module
    // For now, return NULL as a stub
    return NULL;
}

/*
==================
CG_FirePortalGun

Handle portal gun firing effects
==================
*/
void CG_FirePortalGun(vec3_t origin, vec3_t dir, int portalType) {
    vec3_t end;
    trace_t trace;
    localEntity_t *le;
    
    // Calculate end point
    VectorMA(origin, 8192, dir, end);
    
    // Trace to find impact point
    CG_Trace(&trace, origin, NULL, NULL, end, cg.predictedPlayerState.clientNum, MASK_SHOT);
    
    // Create plasma-like projectile effect
    le = CG_AllocLocalEntity();
    if (le) {
        le->leType = LE_PROJECTILE;
        le->startTime = cg.time;
        le->endTime = cg.time + 10000;
        
        VectorCopy(origin, le->pos.trBase);
        VectorScale(dir, 2000, le->pos.trDelta);
        le->pos.trType = TR_LINEAR;
        le->pos.trTime = cg.time;
        
        le->color[0] = portalType == PORTAL_ORANGE ? 1.0f : 0.0f;
        le->color[1] = 0.5f;
        le->color[2] = portalType == PORTAL_ORANGE ? 0.0f : 1.0f;
        le->color[3] = 1.0f;
    }
    
    // Play firing sound
    trap_S_StartSound(origin, ENTITYNUM_NONE, CHAN_WEAPON, 
                     trap_S_RegisterSound("sound/weapons/plasma/plasma_fire.wav"));
}