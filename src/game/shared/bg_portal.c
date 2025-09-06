/*
===========================================================================
Portal Physics - Shared between client and server
Handles portal physics calculations for prediction
===========================================================================
*/

#include "bg_public.h"
#include "../server/portal/g_portal.h"

#define PORTAL_TOUCH_DISTANCE 64.0f
#define PORTAL_VELOCITY_SCALE 1.0f

/*
==================
BG_PortalTouchCheck

Check if a bounding box touches a portal plane
==================
*/
qboolean BG_PortalTouchCheck(vec3_t origin, vec3_t mins, vec3_t maxs, 
                             vec3_t portalOrigin, vec3_t portalNormal, float portalRadius) {
    vec3_t delta;
    float dist;
    vec3_t closest;
    float radius;
    
    // Calculate distance from origin to portal plane
    VectorSubtract(origin, portalOrigin, delta);
    dist = DotProduct(delta, portalNormal);
    
    // Check if we're within touch distance of the portal plane
    if (fabs(dist) > PORTAL_TOUCH_DISTANCE) {
        return qfalse;
    }
    
    // Project the origin onto the portal plane
    VectorMA(origin, -dist, portalNormal, closest);
    
    // Check if the projected point is within the portal radius
    VectorSubtract(closest, portalOrigin, delta);
    radius = VectorLength(delta);
    
    if (radius > portalRadius) {
        return qfalse;
    }
    
    return qtrue;
}

/*
==================
BG_TransformVelocityThroughPortal

Transform velocity vector when passing through a portal
Preserves speed but changes direction based on portal orientations
==================
*/
void BG_TransformVelocityThroughPortal(vec3_t velocity, 
                                       vec3_t enterNormal, vec3_t enterUp,
                                       vec3_t exitNormal, vec3_t exitUp) {
    vec3_t enterForward, enterRight;
    vec3_t exitForward, exitRight;
    vec3_t localVel;
    float speed;
    
    // Save the original speed
    speed = VectorLength(velocity);
    if (speed < 1.0f) {
        return;
    }
    
    // Calculate portal coordinate systems
    VectorNegate(enterNormal, enterForward);
    CrossProduct(enterUp, enterForward, enterRight);
    
    VectorNegate(exitNormal, exitForward);
    CrossProduct(exitUp, exitForward, exitRight);
    
    // Convert velocity to enter portal's local space
    localVel[0] = DotProduct(velocity, enterForward);
    localVel[1] = DotProduct(velocity, enterRight);
    localVel[2] = DotProduct(velocity, enterUp);
    
    // Flip forward component (entering becomes exiting)
    localVel[0] = -localVel[0];
    
    // Convert from local space to exit portal's world space
    velocity[0] = localVel[0] * exitForward[0] + 
                 localVel[1] * exitRight[0] + 
                 localVel[2] * exitUp[0];
    velocity[1] = localVel[0] * exitForward[1] + 
                 localVel[1] * exitRight[1] + 
                 localVel[2] * exitUp[1];
    velocity[2] = localVel[0] * exitForward[2] + 
                 localVel[1] * exitRight[2] + 
                 localVel[2] * exitUp[2];
    
    // Maintain original speed with scaling factor
    VectorNormalize(velocity);
    VectorScale(velocity, speed * PORTAL_VELOCITY_SCALE, velocity);
}

/*
==================
BG_TransformPointThroughPortal

Transform a point from one portal to another
==================
*/
void BG_TransformPointThroughPortal(vec3_t point, 
                                    vec3_t enterOrigin, vec3_t enterNormal, vec3_t enterUp,
                                    vec3_t exitOrigin, vec3_t exitNormal, vec3_t exitUp,
                                    vec3_t result) {
    vec3_t enterForward, enterRight;
    vec3_t exitForward, exitRight;
    vec3_t offset, localOffset;
    
    // Calculate portal coordinate systems
    VectorNegate(enterNormal, enterForward);
    CrossProduct(enterUp, enterForward, enterRight);
    
    VectorNegate(exitNormal, exitForward);
    CrossProduct(exitUp, exitForward, exitRight);
    
    // Get offset from enter portal origin
    VectorSubtract(point, enterOrigin, offset);
    
    // Convert to enter portal's local space
    localOffset[0] = DotProduct(offset, enterForward);
    localOffset[1] = DotProduct(offset, enterRight);
    localOffset[2] = DotProduct(offset, enterUp);
    
    // Flip forward component and add exit offset
    localOffset[0] = -localOffset[0] - 5.0f; // Small offset to prevent getting stuck
    
    // Convert to world space relative to exit portal
    result[0] = exitOrigin[0] + 
               localOffset[0] * exitForward[0] + 
               localOffset[1] * exitRight[0] + 
               localOffset[2] * exitUp[0];
    result[1] = exitOrigin[1] + 
               localOffset[0] * exitForward[1] + 
               localOffset[1] * exitRight[1] + 
               localOffset[2] * exitUp[1];
    result[2] = exitOrigin[2] + 
               localOffset[0] * exitForward[2] + 
               localOffset[1] * exitRight[2] + 
               localOffset[2] * exitUp[2];
}

/*
==================
BG_TransformAnglesThroughPortal

Transform view angles when passing through a portal
==================
*/
void BG_TransformAnglesThroughPortal(vec3_t angles,
                                     vec3_t enterNormal, vec3_t enterUp,
                                     vec3_t exitNormal, vec3_t exitUp) {
    vec3_t forward, right, up;
    vec3_t enterForward, enterRight;
    vec3_t exitForward, exitRight;
    vec3_t localDir;
    vec3_t newForward;
    
    // Get view direction from angles
    AngleVectors(angles, forward, right, up);
    
    // Calculate portal coordinate systems
    VectorNegate(enterNormal, enterForward);
    CrossProduct(enterUp, enterForward, enterRight);
    
    VectorNegate(exitNormal, exitForward);
    CrossProduct(exitUp, exitForward, exitRight);
    
    // Convert forward vector to enter portal's local space
    localDir[0] = DotProduct(forward, enterForward);
    localDir[1] = DotProduct(forward, enterRight);
    localDir[2] = DotProduct(forward, enterUp);
    
    // Flip forward component
    localDir[0] = -localDir[0];
    
    // Convert to exit portal's world space
    newForward[0] = localDir[0] * exitForward[0] + 
                   localDir[1] * exitRight[0] + 
                   localDir[2] * exitUp[0];
    newForward[1] = localDir[0] * exitForward[1] + 
                   localDir[1] * exitRight[1] + 
                   localDir[2] * exitUp[1];
    newForward[2] = localDir[0] * exitForward[2] + 
                   localDir[1] * exitRight[2] + 
                   localDir[2] * exitUp[2];
    
    // Convert back to angles
    vectoangles(newForward, angles);
}

/*
==================
BG_PredictPortalTeleport

Client-side prediction of portal teleportation
==================
*/
void BG_PredictPortalTeleport(playerState_t *ps, 
                              vec3_t enterOrigin, vec3_t enterNormal, vec3_t enterUp,
                              vec3_t exitOrigin, vec3_t exitNormal, vec3_t exitUp) {
    vec3_t newOrigin;
    vec3_t newVelocity;
    vec3_t newAngles;
    
    // Transform position
    BG_TransformPointThroughPortal(ps->origin, 
                                  enterOrigin, enterNormal, enterUp,
                                  exitOrigin, exitNormal, exitUp,
                                  newOrigin);
    
    // Transform velocity
    VectorCopy(ps->velocity, newVelocity);
    BG_TransformVelocityThroughPortal(newVelocity, enterNormal, enterUp, exitNormal, exitUp);
    
    // Transform view angles
    VectorCopy(ps->viewangles, newAngles);
    BG_TransformAnglesThroughPortal(newAngles, enterNormal, enterUp, exitNormal, exitUp);
    
    // Apply changes
    VectorCopy(newOrigin, ps->origin);
    VectorCopy(newVelocity, ps->velocity);
    VectorCopy(newAngles, ps->viewangles);
    
    // Set teleport flag for smooth interpolation
    ps->eFlags ^= EF_TELEPORT_BIT;
}

/*
==================
BG_CheckPortalImmunity

Check if player has fall damage immunity from recent portal use
==================
*/
qboolean BG_CheckPortalImmunity(playerState_t *ps, int currentTime) {
    // Use stats field to track portal immunity
    // stats[STAT_PORTAL_IMMUNITY] stores the time when immunity ends
    if (ps->stats[15] > currentTime) {  // Using unused stat slot
        return qtrue;
    }
    
    return qfalse;
}

/*
==================
BG_SetPortalImmunity

Set fall damage immunity after portal teleportation
==================
*/
void BG_SetPortalImmunity(playerState_t *ps, int currentTime) {
    // Use stats field to track portal immunity
    ps->stats[15] = currentTime + FALL_DAMAGE_IMMUNITY_TIME;  // Using unused stat slot
}