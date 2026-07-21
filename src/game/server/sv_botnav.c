/*
===========================================================================
Copyright (C) 2026 Quake3e-HD Project

Bot navigation mesh (Path B).

The removed AAS system was replaced with a navmesh built directly from the
map's real collision, using the server's own trace queries. Generation is a
flood fill over the walkable floor: starting from the map's spawn/item
entities, it drops player-sized probes onto the floor on a fixed grid and
connects neighbouring stand points that a player could actually walk, drop or
jump between (verified with box sweeps against the real world). The result is
a directed graph that A* routes over. No precompiled .aas data is needed and
nothing is stubbed — every node and edge is proven walkable by a real trace.

Generation runs once, lazily, the first time a bot needs to navigate, and is
cleared on each new world (SV_BotAI_Init).
===========================================================================
*/

#include "server.h"

#define NAV_MAX_NODES		8192
#define NAV_GRID			48.0f		// horizontal spacing between stand points
#define NAV_STEP			18.0f		// max step-up a walk edge may climb
#define NAV_MAXFALL			384.0f		// max survivable drop for a drop edge
#define NAV_JUMPUP			56.0f		// max rise a jump edge may gain
#define NAV_HASH_SIZE		4096
#define NAV_MAX_NODE_EDGES	8			// one per grid neighbour

#define NAV_EDGE_WALK		0
#define NAV_EDGE_DROP		1
#define NAV_EDGE_JUMP		2

typedef struct {
	int			to;
	float		cost;
	int			flags;
} navedge_t;

typedef struct {
	vec3_t		origin;			// stand point (player origin when standing here)
	navedge_t	edges[NAV_MAX_NODE_EDGES];
	int			numEdges;
	qboolean	hazard;			// lava/slime at this point
	int			hashNext;		// spatial-hash chain
} navnode_t;

static navnode_t	*nav_nodes;		// [NAV_MAX_NODES], heap (large)
static int			nav_numNodes;
static int			nav_hash[NAV_HASH_SIZE];
static qboolean		nav_generated;
static qboolean		nav_failed;

static const vec3_t nav_mins = { -15, -15, -24 };
static const vec3_t nav_maxs = {  15,  15,  32 };

// Navmesh generation must see only the static world geometry. SV_Trace also
// clips against entities (players, items, the bot's own body), which makes a
// downward floor probe stop on a nearby body instead of the floor. CM_BoxTrace
// against model 0 is the world hull alone. (Doors/movers are inline submodels,
// intentionally ignored here; they become traversal links later.)
static void SV_BotNav_WorldTrace( trace_t *tr, const vec3_t start, const vec3_t end ) {
	CM_BoxTrace( tr, start, end, nav_mins, nav_maxs, 0, MASK_PLAYERSOLID, qfalse );
}

/*
==================
SV_BotNav_Clear
==================
*/
void SV_BotNav_Clear( void ) {
	nav_numNodes = 0;
	nav_generated = qfalse;
	nav_failed = qfalse;
	for ( int i = 0; i < NAV_HASH_SIZE; i++ ) {
		nav_hash[i] = -1;
	}
}

static unsigned SV_BotNav_HashCell( int cx, int cy ) {
	unsigned h = (unsigned)( cx * 73856093 ) ^ (unsigned)( cy * 19349663 );
	return h & ( NAV_HASH_SIZE - 1 );
}

/*
==================
SV_BotNav_FindNodeAtCell

Return an existing node whose grid cell matches (cx,cy) and whose height is
within one storey of z, or -1. Keeps the flood from creating duplicate nodes
on the same lattice cell while still allowing stacked walkways.
==================
*/
static int SV_BotNav_FindNodeAtCell( int cx, int cy, float z ) {
	unsigned h = SV_BotNav_HashCell( cx, cy );
	for ( int n = nav_hash[h]; n != -1; n = nav_nodes[n].hashNext ) {
		int ncx = (int)floor( nav_nodes[n].origin[0] / NAV_GRID );
		int ncy = (int)floor( nav_nodes[n].origin[1] / NAV_GRID );
		if ( ncx == cx && ncy == cy && fabs( nav_nodes[n].origin[2] - z ) < 72.0f ) {
			return n;
		}
	}
	return -1;
}

/*
==================
SV_BotNav_DropToFloor

Find a standable floor point near (x,y) at roughly height zref. Traces down a
player box from a little above zref; succeeds only if it lands on solid, is
not embedded in a wall, and the stance box is clear. Returns the stand origin
in out, or qfalse.
==================
*/
static qboolean SV_BotNav_DropToFloor( float x, float y, float zref, vec3_t out ) {
	trace_t tr;
	vec3_t start, end;

	// SV_Trace positions the box by its CENTRE, and the stance box extends 24
	// below centre. Start high enough that the box bottom clears the highest
	// standable floor (zref + step); otherwise a same-level floor puts the box
	// bottom below the surface and the trace reports startsolid.
	VectorSet( start, x, y, zref + NAV_STEP + 40.0f );
	VectorSet( end,   x, y, zref - NAV_MAXFALL );

	SV_BotNav_WorldTrace( &tr, start, end );

	if ( tr.startsolid || tr.allsolid ) {
		return qfalse;
	}
	if ( tr.fraction >= 1.0f ) {
		return qfalse;	// no floor within fall range
	}
	if ( tr.plane.normal[2] < 0.7f ) {
		return qfalse;	// too steep to stand
	}

	VectorCopy( tr.endpos, out );
	return qtrue;
}

/*
==================
SV_BotNav_WalkEdge

Classify traversal from a to b for a walking player. Returns one of
NAV_EDGE_* and fills cost, or -1 if not traversable.
==================
*/
static int SV_BotNav_WalkEdge( const vec3_t a, const vec3_t b, float *cost ) {
	trace_t tr;
	vec3_t start, end;
	float rise = b[2] - a[2];
	float dist;

	// Sweep the stance box from a to b, lifted by step height so small steps
	// and curb heights do not read as walls.
	VectorCopy( a, start );
	start[2] += NAV_STEP;
	VectorCopy( b, end );
	end[2] += NAV_STEP;

	SV_BotNav_WorldTrace( &tr, start, end );

	dist = Distance( a, b );
	if ( dist < 1.0f ) {
		return -1;
	}

	if ( tr.fraction >= 1.0f ) {
		// Unobstructed horizontally. Distinguish level walk / step from a drop.
		if ( rise <= NAV_STEP && rise >= -NAV_STEP ) {
			*cost = dist;
			return NAV_EDGE_WALK;
		}
		if ( rise < -NAV_STEP && rise >= -NAV_MAXFALL ) {
			// Falls off the far edge — a drop. Cost scaled by fall height.
			*cost = dist + ( -rise ) * 0.5f;
			return NAV_EDGE_DROP;
		}
		if ( rise > NAV_STEP && rise <= NAV_JUMPUP ) {
			// Needs a jump to gain the height.
			*cost = dist + rise * 2.0f;
			return NAV_EDGE_JUMP;
		}
		return -1;
	}

	// Blocked at floor+step; a jump may clear a low obstacle or short gap if
	// the destination is not higher than a jump can reach.
	if ( rise <= NAV_JUMPUP && dist <= NAV_GRID * 1.6f ) {
		vec3_t js, je;
		VectorCopy( a, js );
		js[2] += NAV_STEP + 40.0f;		// apex-ish clearance
		VectorCopy( b, je );
		je[2] += NAV_STEP + 40.0f;
		SV_BotNav_WorldTrace( &tr, js, je );
		if ( tr.fraction >= 1.0f ) {
			*cost = dist + 64.0f;		// jumps are more expensive than walking
			return NAV_EDGE_JUMP;
		}
	}

	return -1;
}

static int SV_BotNav_AddNode( const vec3_t origin ) {
	if ( nav_numNodes >= NAV_MAX_NODES ) {
		return -1;
	}
	navnode_t *n = &nav_nodes[nav_numNodes];
	VectorCopy( origin, n->origin );
	n->numEdges = 0;
	n->hazard = ( SV_PointContents( origin, -1 ) & ( CONTENTS_LAVA | CONTENTS_SLIME ) ) ? qtrue : qfalse;

	int cx = (int)floor( origin[0] / NAV_GRID );
	int cy = (int)floor( origin[1] / NAV_GRID );
	unsigned h = SV_BotNav_HashCell( cx, cy );
	n->hashNext = nav_hash[h];
	nav_hash[h] = nav_numNodes;

	return nav_numNodes++;
}

static void SV_BotNav_AddEdge( int from, int to, float cost, int flags ) {
	navnode_t *n = &nav_nodes[from];
	// Each node stores its own edges inline (bounded by the 8 grid
	// neighbours), so nodes and edges can be created interleaved during the
	// flood without corrupting a shared edge array.
	for ( int e = 0; e < n->numEdges; e++ ) {
		if ( n->edges[e].to == to ) {
			return;		// already linked
		}
	}
	if ( n->numEdges >= NAV_MAX_NODE_EDGES ) {
		return;
	}
	navedge_t *e = &n->edges[n->numEdges++];
	e->to = to;
	e->cost = cost;
	e->flags = flags;
}

/*
==================
SV_BotNav_SeedsFromEntities

Collect candidate seed points from the map entity string: any entity with an
origin (spawn points, items, spots). These are guaranteed to sit in reachable
places, which lets the flood start inside the playable space.
==================
*/
static int SV_BotNav_SeedsFromEntities( vec3_t *seeds, int maxSeeds ) {
	const char *p = CM_EntityString();
	int count = 0;
	char key[MAX_TOKEN_CHARS];
	vec3_t origin;
	qboolean haveOrigin;

	if ( !p ) {
		return 0;
	}

	while ( count < maxSeeds ) {
		const char *tok = COM_Parse( &p );
		if ( !tok[0] ) {
			break;			// end of string
		}
		if ( tok[0] != '{' ) {
			continue;
		}
		haveOrigin = qfalse;
		VectorClear( origin );
		// Read key/value pairs until '}'
		while ( 1 ) {
			tok = COM_Parse( &p );
			if ( !tok[0] || tok[0] == '}' ) {
				break;
			}
			Q_strncpyz( key, tok, sizeof( key ) );
			tok = COM_Parse( &p );
			if ( !tok[0] ) {
				break;
			}
			if ( !Q_stricmp( key, "origin" ) ) {
				// Parse three floats with strtod (advances past each), rather
				// than sscanf("%f"), which does not reliably fill all three
				// components in this build's CRT.
				char *s = (char *)tok;
				origin[0] = (float)strtod( s, &s );
				origin[1] = (float)strtod( s, &s );
				origin[2] = (float)strtod( s, &s );
				haveOrigin = qtrue;
			}
		}
		if ( haveOrigin ) {
			// VectorCopy is a macro (destination expanded 3x): index must be
			// side-effect free.
			VectorCopy( origin, seeds[count] );
			count++;
		}
	}
	return count;
}

/*
==================
SV_BotNav_Generate

Flood the walkable floor from the entity seeds and build the node graph.
==================
*/
void SV_BotNav_Generate( void ) {
	static const int dx[8] = { 1, -1, 0, 0, 1, 1, -1, -1 };
	static const int dy[8] = { 0, 0, 1, -1, 1, -1, 1, -1 };
	vec3_t *seeds;
	int numSeeds;
	int *queue;
	int qhead, qtail;
	int startMs;

	if ( nav_generated || nav_failed ) {
		return;
	}
	if ( !nav_nodes ) {
		nav_nodes = Z_Malloc( sizeof( navnode_t ) * NAV_MAX_NODES );
	}
	SV_BotNav_Clear();
	startMs = Sys_Milliseconds();

	seeds = Z_Malloc( sizeof( vec3_t ) * 1024 );
	numSeeds = 0;

	// Seed first from the live players: whoever is in the map is, by
	// definition, standing on reachable floor, so these are the most reliable
	// seeds and guarantee the main walkable area is flooded.
	for ( int c = 0; c < sv.maxclients && numSeeds < 1024; c++ ) {
		if ( svs.clients[c].state == CS_ACTIVE ) {
			playerState_t *ps = SV_GameClientNum( c );
			if ( ps->pm_type != PM_DEAD ) {
				// NB: VectorCopy is a macro that expands its destination three
				// times, so the index must not have a side effect here.
				VectorCopy( ps->origin, seeds[numSeeds] );
				numSeeds++;
			}
		}
	}

	// Then the map's spawn/item entities, to cover areas no player has visited
	// yet (isolated ledges reached only by jump pads, etc.).
	numSeeds += SV_BotNav_SeedsFromEntities( seeds + numSeeds, 1024 - numSeeds );

	queue = Z_Malloc( sizeof( int ) * NAV_MAX_NODES );
	qhead = qtail = 0;

	// Seed the flood: snap each seed to the floor and add a node.
	for ( int s = 0; s < numSeeds; s++ ) {
		vec3_t stand;
		if ( !SV_BotNav_DropToFloor( seeds[s][0], seeds[s][1], seeds[s][2], stand ) ) {
			continue;
		}
		int cx = (int)floor( stand[0] / NAV_GRID );
		int cy = (int)floor( stand[1] / NAV_GRID );
		if ( SV_BotNav_FindNodeAtCell( cx, cy, stand[2] ) != -1 ) {
			continue;
		}
		int node = SV_BotNav_AddNode( stand );
		if ( node >= 0 ) {
			queue[qtail++] = node;
		}
	}

	// Breadth-first flood over the grid neighbours of each node.
	while ( qhead < qtail && nav_numNodes < NAV_MAX_NODES ) {
		int cur = queue[qhead++];
		vec3_t org;
		VectorCopy( nav_nodes[cur].origin, org );

		for ( int d = 0; d < 8; d++ ) {
			float nx = org[0] + dx[d] * NAV_GRID;
			float ny = org[1] + dy[d] * NAV_GRID;
			vec3_t stand;

			if ( !SV_BotNav_DropToFloor( nx, ny, org[2], stand ) ) {
				continue;
			}

			float cost;
			int etype = SV_BotNav_WalkEdge( org, stand, &cost );
			if ( etype < 0 ) {
				continue;
			}

			int ncx = (int)floor( stand[0] / NAV_GRID );
			int ncy = (int)floor( stand[1] / NAV_GRID );
			int nb = SV_BotNav_FindNodeAtCell( ncx, ncy, stand[2] );
			qboolean isNew = qfalse;
			if ( nb == -1 ) {
				nb = SV_BotNav_AddNode( stand );
				if ( nb < 0 ) {
					continue;
				}
				isNew = qtrue;
			}

			float c = cost;
			if ( nav_nodes[nb].hazard ) {
				c += 4000.0f;	// route around lava/slime unless unavoidable
			}
			SV_BotNav_AddEdge( cur, nb, c, etype );

			if ( isNew && qtail < NAV_MAX_NODES ) {
				queue[qtail++] = nb;
			}
		}
	}

	Z_Free( seeds );
	Z_Free( queue );

	nav_generated = qtrue;
	if ( nav_numNodes < 2 ) {
		nav_failed = qtrue;
	}

	int totalEdges = 0;
	for ( int i = 0; i < nav_numNodes; i++ ) {
		totalEdges += nav_nodes[i].numEdges;
	}
	Com_Printf( "SV_BotNav: %d nodes, %d edges from %d seeds (%d ms)\n",
		nav_numNodes, totalEdges, numSeeds, Sys_Milliseconds() - startMs );
}

int SV_BotNav_NumNodes( void ) {
	return nav_numNodes;
}

qboolean SV_BotNav_Ready( void ) {
	return ( nav_generated && !nav_failed && nav_numNodes >= 2 ) ? qtrue : qfalse;
}

const float *SV_BotNav_NodeOrigin( int node ) {
	if ( node < 0 || node >= nav_numNodes ) {
		return NULL;
	}
	return nav_nodes[node].origin;
}

/*
==================
SV_BotNav_NearestNode

Nearest node to a point, searching the point's grid cell and its neighbours
first, then falling back to a linear scan if the local cells are empty.
==================
*/
int SV_BotNav_NearestNode( const vec3_t p ) {
	int best = -1;
	float bestDist = 1e30f;
	int cx = (int)floor( p[0] / NAV_GRID );
	int cy = (int)floor( p[1] / NAV_GRID );

	for ( int oy = -2; oy <= 2; oy++ ) {
		for ( int ox = -2; ox <= 2; ox++ ) {
			unsigned h = SV_BotNav_HashCell( cx + ox, cy + oy );
			for ( int n = nav_hash[h]; n != -1; n = nav_nodes[n].hashNext ) {
				float d = DistanceSquared( p, nav_nodes[n].origin );
				if ( d < bestDist ) {
					bestDist = d;
					best = n;
				}
			}
		}
	}

	if ( best != -1 ) {
		return best;
	}

	// Local cells empty — linear fallback (rare; e.g. bot knocked out of the
	// meshed area).
	for ( int n = 0; n < nav_numNodes; n++ ) {
		float d = DistanceSquared( p, nav_nodes[n].origin );
		if ( d < bestDist ) {
			bestDist = d;
			best = n;
		}
	}
	return best;
}

int SV_BotNav_RandomNode( void ) {
	if ( nav_numNodes < 1 ) {
		return -1;
	}
	return (int)( random() * ( nav_numNodes - 1 ) );
}

/*
==================
SV_BotNav_EdgeFlags

Traversal type of the edge from a to b (NAV_EDGE_*), or -1 if not adjacent.
Used by the mover to know when to jump.
==================
*/
int SV_BotNav_EdgeFlags( int from, int to ) {
	if ( from < 0 || from >= nav_numNodes ) {
		return -1;
	}
	navnode_t *n = &nav_nodes[from];
	for ( int e = 0; e < n->numEdges; e++ ) {
		if ( n->edges[e].to == to ) {
			return n->edges[e].flags;
		}
	}
	return -1;
}

// A* working storage (single-threaded server AI, so static is fine).
static float	nav_g[NAV_MAX_NODES];
static int		nav_from[NAV_MAX_NODES];
static qboolean	nav_closed[NAV_MAX_NODES];
static int		nav_open[NAV_MAX_NODES];		// simple binary heap of node ids
static float	nav_openKey[NAV_MAX_NODES];
static int		nav_openCount;

static void SV_BotNav_HeapPush( int node, float key ) {
	int i = nav_openCount++;
	nav_open[i] = node;
	nav_openKey[i] = key;
	while ( i > 0 ) {
		int parent = ( i - 1 ) / 2;
		if ( nav_openKey[parent] <= nav_openKey[i] ) {
			break;
		}
		int tn = nav_open[i]; nav_open[i] = nav_open[parent]; nav_open[parent] = tn;
		float tk = nav_openKey[i]; nav_openKey[i] = nav_openKey[parent]; nav_openKey[parent] = tk;
		i = parent;
	}
}

static int SV_BotNav_HeapPop( void ) {
	int top = nav_open[0];
	nav_openCount--;
	if ( nav_openCount > 0 ) {
		nav_open[0] = nav_open[nav_openCount];
		nav_openKey[0] = nav_openKey[nav_openCount];
		int i = 0;
		while ( 1 ) {
			int l = 2 * i + 1, r = 2 * i + 2, small = i;
			if ( l < nav_openCount && nav_openKey[l] < nav_openKey[small] ) small = l;
			if ( r < nav_openCount && nav_openKey[r] < nav_openKey[small] ) small = r;
			if ( small == i ) break;
			int tn = nav_open[i]; nav_open[i] = nav_open[small]; nav_open[small] = tn;
			float tk = nav_openKey[i]; nav_openKey[i] = nav_openKey[small]; nav_openKey[small] = tk;
			i = small;
		}
	}
	return top;
}

/*
==================
SV_BotNav_FindPath

A* from start to goal over the node graph. Writes the node sequence
(start..goal inclusive) into outNodes and returns its length, or 0 if no
route exists. The path is not string-pulled; the mover advances node to node.
==================
*/
int SV_BotNav_FindPath( int start, int goal, int *outNodes, int maxNodes ) {
	if ( start < 0 || goal < 0 || start >= nav_numNodes || goal >= nav_numNodes ) {
		return 0;
	}
	if ( start == goal ) {
		if ( maxNodes >= 1 ) outNodes[0] = start;
		return ( maxNodes >= 1 ) ? 1 : 0;
	}

	for ( int i = 0; i < nav_numNodes; i++ ) {
		nav_g[i] = 1e30f;
		nav_from[i] = -1;
		nav_closed[i] = qfalse;
	}
	nav_openCount = 0;
	nav_g[start] = 0.0f;
	SV_BotNav_HeapPush( start, Distance( nav_nodes[start].origin, nav_nodes[goal].origin ) );

	while ( nav_openCount > 0 ) {
		int cur = SV_BotNav_HeapPop();
		if ( cur == goal ) {
			break;
		}
		if ( nav_closed[cur] ) {
			continue;
		}
		nav_closed[cur] = qtrue;

		navnode_t *n = &nav_nodes[cur];
		for ( int e = 0; e < n->numEdges; e++ ) {
			navedge_t *edge = &n->edges[e];
			int nb = edge->to;
			if ( nav_closed[nb] ) {
				continue;
			}
			float ng = nav_g[cur] + edge->cost;
			if ( ng < nav_g[nb] ) {
				nav_g[nb] = ng;
				nav_from[nb] = cur;
				float f = ng + Distance( nav_nodes[nb].origin, nav_nodes[goal].origin );
				SV_BotNav_HeapPush( nb, f );
			}
		}
	}

	if ( nav_from[goal] == -1 && start != goal ) {
		return 0;	// unreachable
	}

	// Reconstruct, then reverse into outNodes.
	int tmp[256];
	int len = 0;
	int c = goal;
	while ( c != -1 && len < 256 ) {
		tmp[len++] = c;
		if ( c == start ) {
			break;
		}
		c = nav_from[c];
	}
	if ( len == 0 || tmp[len - 1] != start ) {
		return 0;
	}

	int out = 0;
	for ( int i = len - 1; i >= 0 && out < maxNodes; i-- ) {
		outNodes[out++] = tmp[i];
	}
	return out;
}
