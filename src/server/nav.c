/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
// nav.c -- Kex navigation node support

#include "server.h"
#include "server/nav.h"
#include "common/error.h"
#if USE_REF
#include "refresh/refresh.h"
// ugly but necessary to hook into nav system without
// exposing this into a mess of spaghetti
#include "../refresh/gl.h"

static cvar_t *nav_debug;
static cvar_t *nav_debug_range;
#endif

static struct {
    bool	loaded;
    char	filename[MAX_QPATH];

    int32_t     num_nodes;
    int32_t     num_links;
    int32_t     num_traversals;
    int32_t     num_edicts;
    float       heuristic;

    // for quick node lookups
    int32_t     node_link_bitmap_size;
    byte        *node_link_bitmap;
    
    nav_node_t      *nodes;
    nav_link_t      *links;
    nav_traversal_t *traversals;
    nav_edict_t     *edicts;

    // built-in context
    nav_ctx_t   *ctx;
} nav_data;

// invalid value used for most of the system
const int32_t INVALID_ID = -1;

// magic file header
const int32_t NAV_MAGIC = MakeLittleLong('N', 'A', 'V', '3');

// last nav version we support
const int32_t NAV_VERSION = 6;

#define NAV_VERIFY(condition, error) \
    if (!(condition)) { Com_SetLastError(error); goto fail; }

#define NAV_VERIFY_READ(v) \
    NAV_VERIFY(FS_Read(&v, sizeof(v), f) == sizeof(v), "bad data")

/**
HEADER

i32 magic (NAV3)
i32 version (6)

V6 NAV DATA

i32 num_nodes
i32 num_links
i32 num_traversals
f32 heuristic

struct node
{
    u16 flags
    i16 num_links
    i16 first_link
    i16 radius
}

node[num_nodes] nodes
vec3[num_nodes] node_origins

struct link
{
    i16 target
    u8 type
    u8 flags
    i16 traversal
}

link[num_links] links

struct traversal
{
    vec3 funnel
    vec3 start
    vec3 end
    vec3 ladder_plane
}

traversal[num_traversals] traversals

i32 num_edicts

struct edict
{
    i16 link
    i32 model
    vec3 mins
    vec3 maxs
}

edict[num_edicts] edicts
*/

typedef struct nav_ctx_s {
    // TODO: min-heap or priority queue ordered by f_score
    int16_t     open_set[1024];

    // TODO: figure out a way to get rid of "came_from"
    // and track start -> end off the bat
    int16_t     *came_from, *went_to;
    float       *g_score, *f_score;
} nav_ctx_t;

nav_ctx_t *Nav_AllocCtx(void)
{
    size_t size = sizeof(nav_ctx_t) +
        (sizeof(float) * nav_data.num_nodes) +
        (sizeof(float) * nav_data.num_nodes) +
        sizeof(int16_t) * nav_data.num_nodes +
        sizeof(int16_t) * nav_data.num_nodes;
    nav_ctx_t *ctx = Z_TagMalloc(size, TAG_NAV);
    ctx->g_score = (float *) (ctx + 1);
    ctx->f_score = (float *) (ctx->g_score + nav_data.num_nodes);
    ctx->came_from = (int16_t *) (ctx->f_score + nav_data.num_nodes);
    ctx->went_to = (int16_t *) (ctx->came_from + nav_data.num_nodes);

    return ctx;
}

void Nav_FreeCtx(nav_ctx_t *ctx)
{
    Z_Free(ctx);
}

// built-in path functions
static float Nav_Heuristic(const nav_path_t *path, const nav_node_t *node)
{
    return VectorDistanceSquared(path->goal->origin, node->origin);
}

static float Nav_Weight(const nav_path_t *path, const nav_node_t *node, const nav_link_t *link)
{
    if (link->type == NavLinkType_Teleport)
        return 1.0f;

    return VectorDistanceSquared(node->origin, link->target->origin);
}

static bool Nav_NodeAccessible(const nav_node_t *node)
{
    return !(node->flags & NodeFlag_Disabled);
}

static bool Nav_LinkAccessible(const nav_path_t *path, const nav_node_t *node, const nav_link_t *link)
{
    return Nav_NodeAccessible(link->target);
}

static nav_node_t *Nav_ClosestNodeTo(const vec3_t p)
{
    float w = INFINITY;
    nav_node_t *c = NULL;

    for (int i = 0; i < nav_data.num_nodes; i++) {
        float l = VectorDistanceSquared(nav_data.nodes[i].origin, p);

        if (l < w) {
            w = l;
            c = &nav_data.nodes[i];
        }
    }

    return c;
}

const float PATH_POINT_TOO_CLOSE = 64.f * 64.f;

static const nav_link_t *Nav_GetLink(const nav_node_t *a, const nav_node_t *b)
{
    for (const nav_link_t *link = a->links; link != a->links + a->num_links; link++)
        if (link->target == b)
            return link;

    Q_assert(false);
    return NULL;
}

static bool Nav_TouchingNode(const vec3_t pos, float move_dist, const nav_node_t *node)
{
    float touch_radius = node->radius + move_dist;
    return fabsf(pos[0] - node->origin[0]) < touch_radius &&
           fabsf(pos[1] - node->origin[1]) < touch_radius &&
           fabsf(pos[2] - node->origin[2]) < touch_radius * 4;
}

static PathInfo Nav_Path_(nav_path_t *path)
{
    PathInfo info = { 0 };

    if (!nav_data.loaded) {
        info.returnCode = PathReturnCode_NoNavAvailable;
        return info;
    }

    const PathRequest *request = path->request;

    path->start = Nav_ClosestNodeTo(request->start);

    if (!path->start) {
        info.returnCode = PathReturnCode_NoStartNode;
        return info;
    }

    path->goal = Nav_ClosestNodeTo(request->goal);

    if (!path->goal) {
        info.returnCode = PathReturnCode_NoGoalNode;
        return info;
    }

    if (path->start == path->goal) {
        info.returnCode = PathReturnCode_ReachedGoal;
        return info;
    }

    int16_t start_id = path->start->id;
    int16_t goal_id = path->goal->id;
    
    nav_weight_func_t weight_func = path->weight ? path->weight : Nav_Weight;
    nav_heuristic_func_t heuristic_func = path->heuristic ? path->heuristic : Nav_Heuristic;
    nav_link_accessible_func_t link_accessible_func = path->link_accessible ? path->link_accessible : Nav_LinkAccessible;

    nav_ctx_t *ctx = path->context ? path->context : nav_data.ctx;

    memset(&ctx->open_set, 0xFF, sizeof(ctx->open_set));

    for (int i = 0; i < nav_data.num_nodes; i++)
        ctx->g_score[i] = ctx->f_score[i] = INFINITY;
    
    ctx->open_set[0] = start_id;
    ctx->came_from[start_id] = -1;
    ctx->g_score[start_id] = 0;
    ctx->f_score[start_id] = heuristic_func(path, path->start);

    while (true) {
        int16_t current_id = -1;

        for (int i = 0; i < 1024; i++) {
            if (ctx->open_set[i] == -1) {
                continue;
            } else if (current_id == -1) {
                current_id = i;
            } else if (ctx->f_score[ctx->open_set[i]] < ctx->f_score[ctx->open_set[current_id]]) {
                current_id = i;
            }
        }

        if (current_id == -1)
            break;

        int16_t current = ctx->open_set[current_id];

        if (current == goal_id) {
            int64_t num_points = 0;

            // reverse the order of came_from into went_to
            // to make stuff below a bit easier to work with
            int16_t n = current;
            while (ctx->came_from[n] != -1) {
                num_points++;
                n = ctx->came_from[n];
            }

            n = current;
            int64_t p = 0;
            while (ctx->came_from[n] != -1) {
                n = ctx->went_to[num_points - p - 1] = ctx->came_from[n];
                p++;
            }

            // num_points now contains points between start
            // and current; it will be at least 2, since start can't
            // be the same as end, but may be less once we start clipping.
            Q_assert(num_points >= 2);
            Q_assert(ctx->went_to[0] != -1);

            int64_t first_point = 0;

            // if the node isn't a traversal, we may want
            // to skip the first node if we're either past it
            // or touching it
            const nav_link_t *link = Nav_GetLink(&nav_data.nodes[ctx->went_to[0]], &nav_data.nodes[ctx->went_to[1]]);

            if (link->type == NavLinkType_Walk || link->type == NavLinkType_Crouch) {
                if (Nav_TouchingNode(request->start, request->moveDist, &nav_data.nodes[ctx->went_to[0]])) {
                    first_point++;
                }
            }

            // store resulting path for compass, etc
            if (request->pathPoints.count) {
                // if we're too far from the first node, add in our current position.
                float dist = VectorDistanceSquared(request->start, nav_data.nodes[ctx->went_to[first_point]].origin);

                if (dist > PATH_POINT_TOO_CLOSE) {
                    if (info.numPathPoints < request->pathPoints.count)
                        VectorCopy(request->start, request->pathPoints.posArray[info.numPathPoints]);
                    info.numPathPoints++;
                }

                // crawl forwards and add nodes
                for (p = first_point; p < num_points; p++) {

                    if (info.numPathPoints < request->pathPoints.count)
                        VectorCopy(nav_data.nodes[ctx->went_to[p]].origin, request->pathPoints.posArray[info.numPathPoints]);

                    info.numPathPoints++;
                }

                // add the end point if we have room
                dist = VectorDistanceSquared(request->goal, nav_data.nodes[ctx->went_to[current]].origin);

                if (dist > PATH_POINT_TOO_CLOSE) {
                    if (info.numPathPoints < request->pathPoints.count)
                        VectorCopy(request->goal, request->pathPoints.posArray[info.numPathPoints]);
                    info.numPathPoints++;
                }
            }

            // store move point info
            if (link->traversal != NULL) {
                VectorCopy(link->traversal->start, info.firstMovePoint);
                VectorCopy(link->traversal->end, info.secondMovePoint);
                info.returnCode = PathReturnCode_TraversalPending;
            } else {
                VectorCopy(nav_data.nodes[ctx->went_to[first_point]].origin, info.firstMovePoint);
                Q_assert(first_point + 1 < num_points);
                VectorCopy(nav_data.nodes[ctx->went_to[first_point + 1]].origin, info.secondMovePoint);
                info.returnCode = PathReturnCode_InProgress;
            }

            return info;
        }

        ctx->open_set[current_id] = -1;

        const nav_node_t *current_node = &nav_data.nodes[current];

        for (const nav_link_t *link = current_node->links; link != current_node->links + current_node->num_links; link++) {
            if (!link_accessible_func(path, current_node, link))
                continue;

            int16_t target_id = link->target->id;

            float temp_g_score = ctx->g_score[current] + weight_func(path, current_node, link);

            if (temp_g_score >= ctx->g_score[target_id])
                continue;

            ctx->came_from[target_id] = current;
            ctx->g_score[target_id] = temp_g_score;
            ctx->f_score[target_id] = temp_g_score + heuristic_func(path, link->target);

            int i;

            for (i = 0; i < 1024; i++)
                if (ctx->open_set[i] == target_id)
                    break;

            if (i == 1024) {
                for (i = 0; i < 1024; i++) {
                    if (ctx->open_set[i] == -1) {
                        ctx->open_set[i] = target_id;
                        break;
                    }
                }
            }
        }
    }
    
    info.returnCode = PathReturnCode_NoPathFound;
    return info;
}

#if USE_REF
static inline color_t ColorFromU32(uint32_t c)
{
    return (color_t) { .u32 = c };
}

static inline color_t ColorFromU32A(uint32_t c, uint8_t alpha)
{
    color_t color = { .u32 = c };
    color.u8[3] = alpha;
    return color;
}

static void Nav_DebugPath(const PathInfo *path, const PathRequest *request)
{
    GL_ClearDebugLines();

    int time = (request->debugging.drawTime * 1000) + 6000;

    R_AddDebugSphere(request->start, 8.0f, ColorFromU32A(U32_RED, 64), time, false);
    R_AddDebugSphere(request->goal, 8.0f, ColorFromU32A(U32_RED, 64), time, false);

    if (request->pathPoints.count) {
        R_AddDebugArrow(request->start, request->pathPoints.posArray[0], 8.0f, ColorFromU32A(U32_YELLOW, 64), ColorFromU32A(U32_YELLOW, 64), time, false);

        for (int64_t i = 0; i < request->pathPoints.count - 1; i++)
            R_AddDebugArrow(request->pathPoints.posArray[i], request->pathPoints.posArray[i + 1], 8.0f, ColorFromU32A(U32_YELLOW, 64), ColorFromU32A(U32_YELLOW, 64), time, false);

        R_AddDebugArrow(request->pathPoints.posArray[request->pathPoints.count - 1], request->goal, 8.0f, ColorFromU32A(U32_YELLOW, 64), ColorFromU32A(U32_YELLOW, 64), time, false);
    } else {
        R_AddDebugArrow(request->start, request->goal, 8.0f, ColorFromU32A(U32_YELLOW, 64), ColorFromU32A(U32_YELLOW, 64), time, false);
    }

    R_AddDebugSphere(path->firstMovePoint, 16.0f, ColorFromU32A(U32_RED, 64), time, false);
    R_AddDebugArrow(path->firstMovePoint, path->secondMovePoint, 16.0f, ColorFromU32A(U32_RED, 64), ColorFromU32A(U32_RED, 64), time, false);
}
#endif

PathInfo Nav_Path(nav_path_t *path)
{
    PathInfo result = Nav_Path_(path);
    
#if USE_REF
    if (path->request->debugging.drawTime)
        Nav_DebugPath(&result, path->request);
#endif

    return result;
}

void Nav_Load(const char *map_name)
{
    Q_assert(!nav_data.loaded);

    nav_data.loaded = true;

    Q_snprintf(nav_data.filename, sizeof(nav_data.filename), "bots/navigation/%s.nav", map_name);

    qhandle_t f;
    int64_t l = FS_OpenFile(nav_data.filename, &f, FS_MODE_READ);

    if (l < 0)
        return;

    int v;

    NAV_VERIFY_READ(v);
    NAV_VERIFY(v == NAV_MAGIC, "bad magic");

    NAV_VERIFY_READ(v);
    NAV_VERIFY(v == NAV_VERSION, "bad version");

    // TODO: support versions 5 to 1 which we may have used in some earlier maps
    NAV_VERIFY_READ(nav_data.num_nodes);
    NAV_VERIFY_READ(nav_data.num_links);
    NAV_VERIFY_READ(nav_data.num_traversals);
    NAV_VERIFY_READ(nav_data.heuristic);

    NAV_VERIFY(nav_data.nodes = Z_TagMalloc(sizeof(nav_node_t) * nav_data.num_nodes, TAG_NAV), "out of memory");
    NAV_VERIFY(nav_data.links = Z_TagMalloc(sizeof(nav_link_t) * nav_data.num_links, TAG_NAV), "out of memory");
    NAV_VERIFY(nav_data.traversals = Z_TagMalloc(sizeof(nav_traversal_t) * nav_data.num_traversals, TAG_NAV), "out of memory");
    NAV_VERIFY(nav_data.edicts = Z_TagMalloc(sizeof(nav_traversal_t) * nav_data.num_traversals, TAG_NAV), "out of memory");

    for (int i = 0; i < nav_data.num_nodes; i++) {
        nav_node_t *node = nav_data.nodes + i;
        
        node->id = i;
        NAV_VERIFY_READ(node->flags);
        NAV_VERIFY_READ(node->num_links);
        int16_t first_link;
        NAV_VERIFY_READ(first_link);
        NAV_VERIFY(first_link >= 0 && first_link + node->num_links <= nav_data.num_links, "bad node link extents");
        node->links = &nav_data.links[first_link];
        NAV_VERIFY_READ(node->radius);
    }

    for (int i = 0; i < nav_data.num_nodes; i++) {
        nav_node_t *node = nav_data.nodes + i;

        NAV_VERIFY_READ(node->origin);
    }

    for (int i = 0; i < nav_data.num_links; i++) {
        nav_link_t *link = nav_data.links + i;
        
        int16_t target;
        NAV_VERIFY_READ(target);
        NAV_VERIFY(target >= 0 && target < nav_data.num_nodes, "bad link target");
        link->target = &nav_data.nodes[target];
        NAV_VERIFY_READ(link->type);
        NAV_VERIFY_READ(link->flags);
        int16_t traversal;
        NAV_VERIFY_READ(traversal);
        link->traversal = NULL;

        if (traversal != -1) {
            NAV_VERIFY(traversal < nav_data.num_traversals, "bad link traversal");
            link->traversal = &nav_data.traversals[traversal];
        }
    }

    for (int i = 0; i < nav_data.num_traversals; i++) {
        nav_traversal_t *traversal = nav_data.traversals + i;
        
        NAV_VERIFY_READ(traversal->funnel);
        NAV_VERIFY_READ(traversal->start);
        NAV_VERIFY_READ(traversal->end);
        NAV_VERIFY_READ(traversal->ladder_plane);
    }
    
    NAV_VERIFY_READ(nav_data.num_edicts);

    for (int i = 0; i < nav_data.num_edicts; i++) {
        nav_edict_t *edict = nav_data.edicts + i;
        
        int16_t link;
        NAV_VERIFY_READ(link);
        NAV_VERIFY(link >= 0 && link < nav_data.num_links, "bad edict link");
        edict->link = &nav_data.links[link];
        NAV_VERIFY_READ(edict->model);
        NAV_VERIFY_READ(edict->mins);
        NAV_VERIFY_READ(edict->maxs);
    }

    nav_data.node_link_bitmap_size = nav_data.num_nodes / CHAR_BIT;
    NAV_VERIFY(nav_data.node_link_bitmap = Z_TagMallocz(nav_data.node_link_bitmap_size * nav_data.num_nodes, TAG_NAV), "out of memory");

    for (int i = 0; i < nav_data.num_nodes; i++) {
        nav_node_t *node = nav_data.nodes + i;
        byte *bits = nav_data.node_link_bitmap + (nav_data.node_link_bitmap_size * i);

        for (nav_link_t *link = node->links; link != node->links + node->num_links; link++) {
            Q_SetBit(bits, link->target->id);
        }
    }

    Com_DPrintf("Bot navigation file (%s) loaded:\n %i nodes\n %i links\n %i traversals\n %i edicts\n",
        nav_data.filename, nav_data.num_nodes, nav_data.num_links, nav_data.num_traversals, nav_data.num_edicts);

    nav_data.ctx = Nav_AllocCtx();

    return;

fail:
    Com_EPrintf("Couldn't load bot navigation file (%s): %s\n", nav_data.filename, Com_GetLastError());
    Nav_Unload();
}

void Nav_Unload(void)
{
    if (!nav_data.loaded)
        return;

    Z_FreeTags(TAG_NAV);

    memset(&nav_data, 0, sizeof(nav_data));
}

#if USE_REF
static void Nav_Debug(void)
{
    if (!nav_debug->integer) {
        return;
    }

    for (int i = 0; i < nav_data.num_nodes; i++) {
        const nav_node_t *node = &nav_data.nodes[i];
        float len;
        vec3_t d;
        VectorSubtract(node->origin, glr.fd.vieworg, d);
        len = VectorNormalize(d);

        if (len > nav_debug_range->value) {
            continue;
        }

        uint8_t alpha = constclamp((1.0f - ((len - 32.f) / (nav_debug_range->value - 32.f))), 0.0f, 1.0f) * 255.f;

        R_AddDebugCircle(node->origin, node->radius, ColorFromU32A(U32_CYAN, alpha), SV_FRAMETIME, true);

        vec3_t mins = { -16, -16, -24 }, maxs = { 16, 16, 32 };

        if (node->flags & NodeFlag_Crouch) {
            maxs[2] = 4.0f;
        }
        
        VectorAdd(mins, node->origin, mins);
        VectorAdd(maxs, node->origin, maxs);
        mins[2] += 24.f;
        maxs[2] += 24.f;

        R_AddDebugBounds(mins, maxs, ColorFromU32A(U32_YELLOW, alpha), SV_FRAMETIME, true);

        vec3_t s;
        VectorCopy(node->origin, s);
        s[2] += 24;

        R_AddDebugLine(node->origin, s, ColorFromU32A(U32_CYAN, alpha), SV_FRAMETIME, true);

        vec3_t t;
        VectorCopy(node->origin, t);
        t[2] += 48;

        R_AddDebugText(t, va("%i", node - nav_data.nodes), 0.25f, NULL, ColorFromU32A(U32_CYAN, alpha), SV_FRAMETIME, true);
        
        for (const nav_link_t *link = node->links; link != node->links + node->num_links; link++) {
            const byte *bits = nav_data.node_link_bitmap + (nav_data.node_link_bitmap_size * i);

            vec3_t e;
            VectorCopy(link->target->origin, e);
            e[2] += 24;

            // two-way link
            const byte *target_bits = nav_data.node_link_bitmap + (nav_data.node_link_bitmap_size * link->target->id);

            if (Q_IsBitSet(target_bits, i)) {
                if (i < link->target->id) {
                    continue;
                }

                R_AddDebugLine(s, e, ColorFromU32A(U32_WHITE, alpha), SV_FRAMETIME, true);
            } else {
                R_AddDebugArrow(s, e, 8.0f, ColorFromU32A(U32_CYAN, alpha), ColorFromU32A(U32_RED, alpha), SV_FRAMETIME, true);
            }
        }
    }

}
#endif

void Nav_Frame(void)
{
#if USE_REF
    Nav_Debug();
#endif
}

void Nav_Init(void)
{
#if USE_REF
    nav_debug = Cvar_Get("nav_debug", "0", 0);
    nav_debug_range = Cvar_Get("nav_debug_range", "512", 0);
#endif
}

void Nav_Shutdown(void)
{
}