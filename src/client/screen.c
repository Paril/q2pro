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
// cl_scrn.c -- master for refresh, status bar, console, chat, notify, etc

#include "client.h"

static cvar_t   *scr_viewsize;
static cvar_t   *scr_showpause;
#if USE_DEBUG
static cvar_t   *scr_showstats;
static cvar_t   *scr_showpmove;
#endif
static cvar_t   *scr_showturtle;

static cvar_t   *scr_netgraph;
static cvar_t   *scr_timegraph;
static cvar_t   *scr_debuggraph;
static cvar_t   *scr_graphheight;
static cvar_t   *scr_graphscale;
static cvar_t   *scr_graphshift;

static cvar_t   *scr_draw2d;
static cvar_t   *scr_lag_x;
static cvar_t   *scr_lag_y;
static cvar_t   *scr_lag_draw;
static cvar_t   *scr_lag_min;
static cvar_t   *scr_lag_max;
static cvar_t   *scr_alpha;

static cvar_t   *scr_demobar;
static cvar_t   *scr_font;
static cvar_t   *scr_scale;

static cvar_t   *scr_crosshair;

static cvar_t   *scr_chathud;
static cvar_t   *scr_chathud_lines;
static cvar_t   *scr_chathud_time;
static cvar_t   *scr_chathud_x;
static cvar_t   *scr_chathud_y;

static cvar_t   *ch_health;
static cvar_t   *ch_red;
static cvar_t   *ch_green;
static cvar_t   *ch_blue;
static cvar_t   *ch_alpha;

static cvar_t   *ch_scale;
static cvar_t   *ch_x;
static cvar_t   *ch_y;

static cvar_t   *scr_hit_marker_time;

static cvar_t   *scr_damage_indicators;
static cvar_t   *scr_damage_indicator_time;

static cvar_t   *scr_pois;
static cvar_t   *scr_poi_edge_frac;
static cvar_t   *scr_poi_max_scale;

static cvar_t   *scr_safe_zone;

// nb: this is dumb but C doesn't allow
// `(T) { }` to count as a constant
const color_t colorTable[8] = {
    { .u32 = COLOR_U32_BLACK }, { .u32 = COLOR_U32_RED }, { .u32 = COLOR_U32_GREEN }, { .u32 = COLOR_U32_YELLOW },
    { .u32 = COLOR_U32_BLUE }, { .u32 = COLOR_U32_CYAN }, { .u32 = COLOR_U32_MAGENTA }, { .u32 = COLOR_U32_WHITE }
};

cl_scr_t scr;

/*
===============================================================================

UTILS

===============================================================================
*/

/*
==============
SCR_DrawStringStretch
==============
*/
int SCR_DrawStringStretch(int x, int y, int scale, int flags, size_t maxlen,
                          const char *s, color_t color, qhandle_t font)
{
    size_t len = strlen(s);

    if (len > maxlen) {
        len = maxlen;
    }

    if ((flags & UI_CENTER) == UI_CENTER) {
        x -= (len * CONCHAR_WIDTH * scale) / 2;
    } else if (flags & UI_RIGHT) {
        x -= len * CONCHAR_WIDTH * scale;
    }

    return R_DrawStringStretch(x, y, scale, flags, maxlen, s, color, font);
}


/*
==============
SCR_DrawStringMultiStretch
==============
*/
void SCR_DrawStringMultiStretch(int x, int y, int scale, int flags, size_t maxlen,
                                const char *s, color_t color, qhandle_t font)
{
    char    *p;
    size_t  len;
    int     last_x = x;
    int     last_y = y;

    while (*s && maxlen) {
        p = strchr(s, '\n');
        if (!p) {
            last_x = SCR_DrawStringStretch(x, y, scale, flags, maxlen, s, color, font);
            last_y = y;
            break;
        }

        len = min(p - s, maxlen);
        last_x = SCR_DrawStringStretch(x, y, scale, flags, len, s, color, font);
        last_y = y;
        maxlen -= len;

        y += CONCHAR_HEIGHT * scale;
        s = p + 1;
    }

    if (flags & UI_DRAWCURSOR && com_localTime & BIT(8))
        R_DrawStretchChar(last_x, last_y, CONCHAR_WIDTH * scale, CONCHAR_HEIGHT * scale, flags, 11, color, font);
}

static int SCR_DrawKStringStretch(int x, int y, int scale, int flags, size_t maxlen, const char *s, color_t color, const kfont_t *kfont)
{
    while (*s && maxlen--) {
        x += R_DrawKFontChar(x, y, scale, flags, *s++, color, kfont);
    }
    return x;
}

void SCR_DrawKStringMultiStretch(int x, int y, int scale, int flags, size_t maxlen, const char *s, color_t color, const kfont_t *kfont)
{
    int     last_x = x;
    int     last_y = y;

    while (*s && maxlen) {
        const char *p = strchr(s, '\n');
        if (!p) {
            last_x = SCR_DrawKStringStretch(x, y, scale, flags, maxlen, s, color, kfont);
            last_y = y;
            break;
        }

        int len = min(p - s, maxlen);
        last_x = SCR_DrawKStringStretch(x, y, scale, flags, len, s, color, kfont);
        last_y = y;
        maxlen -= len;

        y += CONCHAR_HEIGHT * scale;
        s = p + 1;
    }

    if (flags & UI_DRAWCURSOR && com_localTime & BIT(8))
        R_DrawKFontChar(last_x, last_y, scale, flags, 11, color, kfont);
}


/*
=================
SCR_FadeAlpha
=================
*/
float SCR_FadeAlpha(unsigned startTime, unsigned visTime, unsigned fadeTime)
{
    float alpha;
    unsigned timeLeft, delta = cls.realtime - startTime;

    if (delta >= visTime) {
        return 0;
    }

    if (fadeTime > visTime) {
        fadeTime = visTime;
    }

    alpha = 1;
    timeLeft = visTime - delta;
    if (timeLeft < fadeTime) {
        alpha = (float)timeLeft / fadeTime;
    }

    return alpha;
}

bool SCR_ParseColor(const char *s, color_t *color)
{
    int i;
    int c[8];

    // parse generic color
    if (*s == '#') {
        s++;
        for (i = 0; s[i]; i++) {
            if (i == 8) {
                return false;
            }
            c[i] = Q_charhex(s[i]);
            if (c[i] == -1) {
                return false;
            }
        }

        switch (i) {
        case 3:
            color->r = c[0] | (c[0] << 4);
            color->g = c[1] | (c[1] << 4);
            color->b = c[2] | (c[2] << 4);
            color->a = 255;
            break;
        case 6:
            color->r = c[1] | (c[0] << 4);
            color->g = c[3] | (c[2] << 4);
            color->b = c[5] | (c[4] << 4);
            color->a = 255;
            break;
        case 8:
            color->r = c[1] | (c[0] << 4);
            color->g = c[3] | (c[2] << 4);
            color->b = c[5] | (c[4] << 4);
            color->a = c[7] | (c[6] << 4);
            break;
        default:
            return false;
        }

        return true;
    }

    // parse name or index
    i = Com_ParseColor(s);
    if (i >= q_countof(colorTable)) {
        return false;
    }

    *color = colorTable[i];
    return true;
}

/*
===============================================================================

BAR GRAPHS

===============================================================================
*/

/*
==============
SCR_AddNetgraph

A new packet was just parsed
==============
*/
void SCR_AddNetgraph(void)
{
    int         i, color;
    unsigned    ping;

    // if using the debuggraph for something else, don't
    // add the net lines
    if (scr_debuggraph->integer || scr_timegraph->integer)
        return;

    for (i = 0; i < cls.netchan.dropped; i++)
        SCR_DebugGraph(30, 0x40);

    for (i = 0; i < cl.suppress_count; i++)
        SCR_DebugGraph(30, 0xdf);

    if (scr_netgraph->integer > 1) {
        ping = msg_read.cursize;
        if (ping < 200)
            color = 61;
        else if (ping < 500)
            color = 59;
        else if (ping < 800)
            color = 57;
        else if (ping < 1200)
            color = 224;
        else
            color = 242;
        ping /= 40;
    } else {
        // see what the latency was on this packet
        i = cls.netchan.incoming_acknowledged & CMD_MASK;
        ping = (cls.realtime - cl.history[i].sent) / 30;
        color = 0xd0;
    }

    SCR_DebugGraph(min(ping, 30), color);
}

#define GRAPH_SAMPLES   4096
#define GRAPH_MASK      (GRAPH_SAMPLES - 1)

static struct {
    float       values[GRAPH_SAMPLES];
    byte        colors[GRAPH_SAMPLES];
    unsigned    current;
} graph;

/*
==============
SCR_DebugGraph
==============
*/
void SCR_DebugGraph(float value, int color)
{
    graph.values[graph.current & GRAPH_MASK] = value;
    graph.colors[graph.current & GRAPH_MASK] = color;
    graph.current++;
}

/*
==============
SCR_DrawDebugGraph
==============
*/
static void SCR_DrawDebugGraph(void)
{
    int     a, y, w, i, h, height;
    float   v, scale, shift;

    scale = scr_graphscale->value;
    shift = scr_graphshift->value;
    height = scr_graphheight->integer;
    if (height < 1)
        return;

    w = scr.hud_width;
    y = scr.hud_height;

    for (a = 0; a < w; a++) {
        i = (graph.current - 1 - a) & GRAPH_MASK;
        v = graph.values[i] * scale + shift;

        if (v < 0)
            v += height * (1 + (int)(-v / height));

        h = (int)v % height;
        R_DrawFill8(w - 1 - a, y - h, 1, h, graph.colors[i]);
    }
}

/*
===============================================================================

DEMO BAR

===============================================================================
*/

static void draw_progress_bar(float progress, bool paused, int framenum)
{
    char buffer[16];
    int x, w, h;
    size_t len;

    w = Q_rint(scr.hud_width * progress);
    h = Q_rint(CONCHAR_HEIGHT / scr.hud_scale);

    scr.hud_height -= h;

    R_DrawFill8(0, scr.hud_height, w, h, 4);
    R_DrawFill8(w, scr.hud_height, scr.hud_width - w, h, 0);

    R_SetScale(scr.hud_scale);

    w = Q_rint(scr.hud_width * scr.hud_scale);
    h = Q_rint(scr.hud_height * scr.hud_scale);

    len = Q_scnprintf(buffer, sizeof(buffer), "%.f%%", progress * 100);
    x = (w - len * CONCHAR_WIDTH) / 2;
    R_DrawString(x, h, 0, MAX_STRING_CHARS, buffer, COLOR_WHITE, scr.font_pic);

    if (scr_demobar->integer > 1) {
        int sec = framenum / BASE_FRAMERATE;
        int min = sec / 60; sec %= 60;

        Q_scnprintf(buffer, sizeof(buffer), "%d:%02d.%d", min, sec, framenum % BASE_FRAMERATE);
        R_DrawString(0, h, 0, MAX_STRING_CHARS, buffer, COLOR_WHITE, scr.font_pic);
    }

    if (paused) {
        SCR_DrawString(w, h, UI_RIGHT, COLOR_WHITE, "[PAUSED]");
    }

    R_SetScale(1.0f);
}

static void SCR_DrawDemo(void)
{
#if USE_MVD_CLIENT
    float progress;
    bool paused;
    int framenum;
#endif

    if (!scr_demobar->integer) {
        return;
    }

    if (cls.demo.playback) {
        if (cls.demo.file_size && !cls.demo.compat) {
            draw_progress_bar(
                cls.demo.file_progress,
                sv_paused->integer &&
                cl_paused->integer &&
                scr_showpause->integer == 2,
                cls.demo.frames_read);
        }
        return;
    }

#if USE_MVD_CLIENT
    if (sv_running->integer != ss_broadcast) {
        return;
    }

    if (!MVD_GetDemoStatus(&progress, &paused, &framenum)) {
        return;
    }

    if (sv_paused->integer && cl_paused->integer && scr_showpause->integer == 2) {
        paused = true;
    }

    draw_progress_bar(progress, paused, framenum);
#endif
}

/*
===============================================================================

LAGOMETER

===============================================================================
*/

#define LAG_WIDTH   48
#define LAG_HEIGHT  48

#define LAG_WARN_BIT    BIT(30)
#define LAG_CRIT_BIT    BIT(31)

#define LAG_BASE    0xD5
#define LAG_WARN    0xDC
#define LAG_CRIT    0xF2

static struct {
    unsigned samples[LAG_WIDTH];
    unsigned head;
} lag;

void SCR_LagClear(void)
{
    lag.head = 0;
}

void SCR_LagSample(void)
{
    int i = cls.netchan.incoming_acknowledged & CMD_MASK;
    client_history_t *h = &cl.history[i];
    unsigned ping;

    h->rcvd = cls.realtime;
    if (!h->cmdNumber || h->rcvd < h->sent) {
        return;
    }

    ping = h->rcvd - h->sent;
    for (i = 0; i < cls.netchan.dropped; i++) {
        lag.samples[lag.head % LAG_WIDTH] = ping | LAG_CRIT_BIT;
        lag.head++;
    }

    if (cl.frameflags & FF_SUPPRESSED) {
        ping |= LAG_WARN_BIT;
    }
    lag.samples[lag.head % LAG_WIDTH] = ping;
    lag.head++;
}

static void SCR_LagDraw(int x, int y)
{
    int i, j, v, c, v_min, v_max, v_range;

    v_min = Cvar_ClampInteger(scr_lag_min, 0, LAG_HEIGHT * 10);
    v_max = Cvar_ClampInteger(scr_lag_max, 0, LAG_HEIGHT * 10);

    v_range = v_max - v_min;
    if (v_range < 1)
        return;

    for (i = 0; i < LAG_WIDTH; i++) {
        j = lag.head - i - 1;
        if (j < 0) {
            break;
        }

        v = lag.samples[j % LAG_WIDTH];

        if (v & LAG_CRIT_BIT) {
            c = LAG_CRIT;
        } else if (v & LAG_WARN_BIT) {
            c = LAG_WARN;
        } else {
            c = LAG_BASE;
        }

        v &= ~(LAG_WARN_BIT | LAG_CRIT_BIT);
        v = Q_clip((v - v_min) * LAG_HEIGHT / v_range, 0, LAG_HEIGHT);

        R_DrawFill8(x + LAG_WIDTH - i - 1, y + LAG_HEIGHT - v, 1, v, c);
    }
}

static void SCR_DrawNet(color_t base_color)
{
    int x = scr_lag_x->integer;
    int y = scr_lag_y->integer;

    if (x < 0) {
        x += scr.hud_width - LAG_WIDTH + 1;
    }
    if (y < 0) {
        y += scr.hud_height - LAG_HEIGHT + 1;
    }

    // draw ping graph
    if (scr_lag_draw->integer) {
        if (scr_lag_draw->integer > 1) {
            R_DrawFill8(x, y, LAG_WIDTH, LAG_HEIGHT, 4);
        }
        SCR_LagDraw(x, y);
    }

    // draw phone jack
    if (cls.netchan.outgoing_sequence - cls.netchan.incoming_acknowledged >= CMD_BACKUP) {
        if ((cls.realtime >> 8) & 3) {
            R_DrawStretchPic(x, y, LAG_WIDTH, LAG_HEIGHT, base_color, scr.net_pic);
        }
    }
}


/*
===============================================================================

DRAW OBJECTS

===============================================================================
*/

typedef struct {
    list_t          entry;
    int             x, y;
    cvar_t          *cvar;
    cmd_macro_t     *macro;
    int             flags;
    color_t         color;
} drawobj_t;

#define FOR_EACH_DRAWOBJ(obj) \
    LIST_FOR_EACH(drawobj_t, obj, &scr_objects, entry)
#define FOR_EACH_DRAWOBJ_SAFE(obj, next) \
    LIST_FOR_EACH_SAFE(drawobj_t, obj, next, &scr_objects, entry)

static LIST_DECL(scr_objects);

static void SCR_Color_g(genctx_t *ctx)
{
    int color;

    for (color = 0; color < COLOR_INDEX_COUNT; color++)
        Prompt_AddMatch(ctx, colorNames[color]);
}

static void SCR_Draw_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        Cvar_Variable_g(ctx);
        Cmd_Macro_g(ctx);
    } else if (argnum == 4) {
        SCR_Color_g(ctx);
    }
}

// draw cl_fps -1 80
static void SCR_Draw_f(void)
{
    int x, y;
    const char *s, *c;
    drawobj_t *obj;
    cmd_macro_t *macro;
    cvar_t *cvar;
    color_t color;
    int flags;
    int argc = Cmd_Argc();

    if (argc == 1) {
        if (LIST_EMPTY(&scr_objects)) {
            Com_Printf("No draw strings registered.\n");
            return;
        }
        Com_Printf("Name               X    Y\n"
                   "--------------- ---- ----\n");
        FOR_EACH_DRAWOBJ(obj) {
            s = obj->macro ? obj->macro->name : obj->cvar->name;
            Com_Printf("%-15s %4d %4d\n", s, obj->x, obj->y);
        }
        return;
    }

    if (argc < 4) {
        Com_Printf("Usage: %s <name> <x> <y> [color]\n", Cmd_Argv(0));
        return;
    }

    color = COLOR_BLACK;
    flags = UI_IGNORECOLOR;

    s = Cmd_Argv(1);
    x = Q_atoi(Cmd_Argv(2));
    y = Q_atoi(Cmd_Argv(3));

    if (x < 0) {
        flags |= UI_RIGHT;
    }

    if (argc > 4) {
        c = Cmd_Argv(4);
        if (!strcmp(c, "alt")) {
            flags |= UI_ALTCOLOR;
        } else if (strcmp(c, "none")) {
            if (!SCR_ParseColor(c, &color)) {
                Com_Printf("Unknown color '%s'\n", c);
                return;
            }
            flags &= ~UI_IGNORECOLOR;
        }
    }

    cvar = NULL;
    macro = Cmd_FindMacro(s);
    if (!macro) {
        cvar = Cvar_WeakGet(s);
    }

    FOR_EACH_DRAWOBJ(obj) {
        if (obj->macro == macro && obj->cvar == cvar) {
            obj->x = x;
            obj->y = y;
            obj->flags = flags;
            obj->color.u32 = color.u32;
            return;
        }
    }

    obj = Z_Malloc(sizeof(*obj));
    obj->x = x;
    obj->y = y;
    obj->cvar = cvar;
    obj->macro = macro;
    obj->flags = flags;
    obj->color.u32 = color.u32;

    List_Append(&scr_objects, &obj->entry);
}

static void SCR_Draw_g(genctx_t *ctx)
{
    drawobj_t *obj;
    const char *s;

    if (LIST_EMPTY(&scr_objects)) {
        return;
    }

    Prompt_AddMatch(ctx, "all");

    FOR_EACH_DRAWOBJ(obj) {
        s = obj->macro ? obj->macro->name : obj->cvar->name;
        Prompt_AddMatch(ctx, s);
    }
}

static void SCR_UnDraw_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        SCR_Draw_g(ctx);
    }
}

static void SCR_UnDraw_f(void)
{
    char *s;
    drawobj_t *obj, *next;
    cmd_macro_t *macro;
    cvar_t *cvar;

    if (Cmd_Argc() != 2) {
        Com_Printf("Usage: %s <name>\n", Cmd_Argv(0));
        return;
    }

    if (LIST_EMPTY(&scr_objects)) {
        Com_Printf("No draw strings registered.\n");
        return;
    }

    s = Cmd_Argv(1);
    if (!strcmp(s, "all")) {
        FOR_EACH_DRAWOBJ_SAFE(obj, next) {
            Z_Free(obj);
        }
        List_Init(&scr_objects);
        Com_Printf("Deleted all draw strings.\n");
        return;
    }

    cvar = NULL;
    macro = Cmd_FindMacro(s);
    if (!macro) {
        cvar = Cvar_WeakGet(s);
    }

    FOR_EACH_DRAWOBJ_SAFE(obj, next) {
        if (obj->macro == macro && obj->cvar == cvar) {
            List_Remove(&obj->entry);
            Z_Free(obj);
            return;
        }
    }

    Com_Printf("Draw string '%s' not found.\n", s);
}

static void SCR_DrawObjects(color_t base_color)
{
    char buffer[MAX_QPATH];
    int x, y;
    drawobj_t *obj;

    FOR_EACH_DRAWOBJ(obj) {
        x = obj->x;
        y = obj->y;
        if (x < 0) {
            x += scr.hud_width + 1;
        }
        if (y < 0) {
            y += scr.hud_height - CONCHAR_HEIGHT + 1;
        }

        color_t color = base_color;

        if (!(obj->flags & UI_IGNORECOLOR)) {
            color = obj->color;
        }

        if (obj->macro) {
            obj->macro->function(buffer, sizeof(buffer));
            SCR_DrawString(x, y, obj->flags, color, buffer);
        } else {
            SCR_DrawString(x, y, obj->flags, color, obj->cvar->string);
        }   
    }
}

/*
===============================================================================

CHAT HUD

===============================================================================
*/

#define MAX_CHAT_TEXT       150
#define MAX_CHAT_LINES      32
#define CHAT_LINE_MASK      (MAX_CHAT_LINES - 1)

typedef struct {
    char        text[MAX_CHAT_TEXT];
    unsigned    time;
} chatline_t;

static chatline_t   scr_chatlines[MAX_CHAT_LINES];
static unsigned     scr_chathead;

void SCR_ClearChatHUD_f(void)
{
    memset(scr_chatlines, 0, sizeof(scr_chatlines));
    scr_chathead = 0;
}

void SCR_AddToChatHUD(const char *text)
{
    chatline_t *line;
    char *p;

    line = &scr_chatlines[scr_chathead++ & CHAT_LINE_MASK];
    Q_strlcpy(line->text, text, sizeof(line->text));
    line->time = cls.realtime;

    p = strrchr(line->text, '\n');
    if (p)
        *p = 0;
}

static void SCR_DrawChatHUD(color_t base_color)
{
    int x, y, i, lines, flags, step;
    float alpha;
    chatline_t *line;

    if (scr_chathud->integer == 0)
        return;

    x = scr_chathud_x->integer;
    y = scr_chathud_y->integer;

    if (scr_chathud->integer == 2)
        flags = UI_ALTCOLOR;
    else
        flags = 0;

    if (x < 0) {
        x += scr.hud_width + 1;
        flags |= UI_RIGHT;
    } else {
        flags |= UI_LEFT;
    }

    if (y < 0) {
        y += scr.hud_height - CONCHAR_HEIGHT + 1;
        step = -CONCHAR_HEIGHT;
    } else {
        step = CONCHAR_HEIGHT;
    }

    lines = scr_chathud_lines->integer;
    if (lines > scr_chathead)
        lines = scr_chathead;

    for (i = 0; i < lines; i++) {
        line = &scr_chatlines[(scr_chathead - i - 1) & CHAT_LINE_MASK];

        if (scr_chathud_time->integer) {
            alpha = SCR_FadeAlpha(line->time, scr_chathud_time->integer, 1000);
            if (!alpha)
                break;

            color_t color = base_color;
            color.a *= alpha;

            SCR_DrawString(x, y, flags, color, line->text);
        } else {
            SCR_DrawString(x, y, flags, base_color, line->text);
        }

        y += step;
    }
}

/*
===============================================================================

DEBUG STUFF

===============================================================================
*/

static void SCR_DrawTurtle(color_t base_color)
{
    int x, y;

    if (scr_showturtle->integer <= 0)
        return;

    if (!cl.frameflags)
        return;

    x = CONCHAR_WIDTH;
    y = scr.hud_height - 11 * CONCHAR_HEIGHT;

#define DF(f) \
    if (cl.frameflags & FF_##f) { \
        SCR_DrawString(x, y, UI_ALTCOLOR, base_color, #f); \
        y += CONCHAR_HEIGHT; \
    }

    if (scr_showturtle->integer > 1) {
        DF(SUPPRESSED)
    }
    DF(CLIENTPRED)
    if (scr_showturtle->integer > 1) {
        DF(CLIENTDROP)
        DF(SERVERDROP)
    }
    DF(BADFRAME)
    DF(OLDFRAME)
    DF(OLDENT)
    DF(NODELTA)

#undef DF
}

#if USE_DEBUG

static void SCR_DrawDebugStats(void)
{
    char buffer[MAX_QPATH];
    int i, j;
    int x, y;

    j = scr_showstats->integer;
    if (j <= 0)
        return;

    if (j > cl.max_stats)
        j = cl.max_stats;

    x = CONCHAR_WIDTH;
    y = (scr.hud_height - j * CONCHAR_HEIGHT) / 2;
    for (i = 0; i < j; i++) {
        Q_snprintf(buffer, sizeof(buffer), "%2d: %d", i, cl.frame.ps.stats[i]);
        color_t color = COLOR_WHITE;
        if (cl.oldframe.ps.stats[i] != cl.frame.ps.stats[i]) {
            color = COLOR_RED;
        }
        R_DrawString(x, y, 0, MAX_STRING_CHARS, buffer, color, scr.font_pic);
        y += CONCHAR_HEIGHT;
    }
}

static void SCR_DrawDebugPmove(void)
{
    static const char * const types[] = {
        "NORMAL", "SPECTATOR", "DEAD", "GIB", "FREEZE"
    };
    static const char * const flags[] = {
        "DUCKED", "JUMP_HELD", "ON_GROUND",
        "TIME_WATERJUMP", "TIME_LAND", "TIME_TELEPORT",
        "NO_PREDICTION", "TELEPORT_BIT"
    };
    unsigned i, j;
    int x, y;

    if (!scr_showpmove->integer)
        return;

    x = CONCHAR_WIDTH;
    y = (scr.hud_height - 2 * CONCHAR_HEIGHT) / 2;

    i = cl.frame.ps.pmove.pm_type;
    if (i > PM_FREEZE)
        i = PM_FREEZE;

    R_DrawString(x, y, 0, MAX_STRING_CHARS, types[i], COLOR_WHITE, scr.font_pic);
    y += CONCHAR_HEIGHT;

    j = cl.frame.ps.pmove.pm_flags;
    for (i = 0; i < 8; i++) {
        if (j & (1 << i)) {
            x = R_DrawString(x, y, 0, MAX_STRING_CHARS, flags[i], COLOR_WHITE, scr.font_pic);
            x += CONCHAR_WIDTH;
        }
    }
}

#endif

//============================================================================

// Sets scr_vrect, the coordinates of the rendered window
static void SCR_CalcVrect(void)
{
    int     size;

    // bound viewsize
    size = Cvar_ClampInteger(scr_viewsize, 40, 100);

    scr.vrect.width = scr.hud_width * size / 100;
    scr.vrect.height = scr.hud_height * size / 100;

    scr.vrect.x = (scr.hud_width - scr.vrect.width) / 2;
    scr.vrect.y = (scr.hud_height - scr.vrect.height) / 2;
}

/*
=================
SCR_SizeUp_f

Keybinding command
=================
*/
static void SCR_SizeUp_f(void)
{
    Cvar_SetInteger(scr_viewsize, scr_viewsize->integer + 10, FROM_CONSOLE);
}

/*
=================
SCR_SizeDown_f

Keybinding command
=================
*/
static void SCR_SizeDown_f(void)
{
    Cvar_SetInteger(scr_viewsize, scr_viewsize->integer - 10, FROM_CONSOLE);
}

/*
=================
SCR_Sky_f

Set a specific sky and rotation speed. If empty sky name is provided, falls
back to server defaults.
=================
*/
static void SCR_Sky_f(void)
{
    char    *name;
    float   rotate;
    vec3_t  axis;
    int     argc = Cmd_Argc();

    if (argc < 2) {
        Com_Printf("Usage: sky <basename> [rotate] [axis x y z]\n");
        return;
    }

    if (cls.state != ca_active) {
        Com_Printf("No map loaded.\n");
        return;
    }

    name = Cmd_Argv(1);
    if (!*name) {
        CL_SetSky();
        return;
    }

    if (argc > 2)
        rotate = Q_atof(Cmd_Argv(2));
    else
        rotate = 0;

    if (argc == 6) {
        axis[0] = Q_atof(Cmd_Argv(3));
        axis[1] = Q_atof(Cmd_Argv(4));
        axis[2] = Q_atof(Cmd_Argv(5));
    } else
        VectorSet(axis, 0, 0, 1);
    
    R_SetSky(name, rotate, true, axis);
}

/*
================
SCR_TimeRefresh_f
================
*/
static void SCR_TimeRefresh_f(void)
{
    int     i;
    unsigned    start, stop;
    float       time;

    if (cls.state != ca_active) {
        Com_Printf("No map loaded.\n");
        return;
    }

    start = Sys_Milliseconds();

    cl.refdef.frametime = 0.0f;
    if (Cmd_Argc() == 2) {
        // run without page flipping
        R_BeginFrame();
        for (i = 0; i < 128; i++) {
            cl.refdef.viewangles[1] = i / 128.0f * 360.0f;
            R_RenderFrame(&cl.refdef);
        }
        R_EndFrame();
    } else {
        for (i = 0; i < 128; i++) {
            cl.refdef.viewangles[1] = i / 128.0f * 360.0f;

            R_BeginFrame();
            R_RenderFrame(&cl.refdef);
            R_EndFrame();
        }
    }

    stop = Sys_Milliseconds();
    time = (stop - start) * 0.001f;
    Com_Printf("%f seconds (%f fps)\n", time, 128.0f / time);
}


//============================================================================

static void ch_scale_changed(cvar_t *self)
{
    int w, h;
    float scale;

    scale = Cvar_ClampValue(self, 0.1f, 9.0f);

    // prescale
    R_GetPicSize(&w, &h, scr.crosshair_pic);
    scr.crosshair_width = Q_rint(w * scale);
    scr.crosshair_height = Q_rint(h * scale);

    R_GetPicSize(&w, &h, scr.hit_marker_pic);
    scr.hit_marker_width = Q_rint(w * scale);
    scr.hit_marker_height = Q_rint(h * scale);
}

static void ch_color_changed(cvar_t *self)
{
    if (ch_health->integer) {
        SCR_SetCrosshairColor();
    } else {
        scr.crosshair_color.r = Cvar_ClampValue(ch_red, 0, 1) * 255;
        scr.crosshair_color.g = Cvar_ClampValue(ch_green, 0, 1) * 255;
        scr.crosshair_color.b = Cvar_ClampValue(ch_blue, 0, 1) * 255;
    }
    scr.crosshair_color.a = Cvar_ClampValue(ch_alpha, 0, 1) * 255;
}

static void scr_crosshair_changed(cvar_t *self)
{
    if (self->integer > 0) {
        scr.crosshair_pic = R_RegisterPic(va("ch%i", self->integer));
        ch_scale_changed(ch_scale);
    } else {
        scr.crosshair_pic = 0;
    }
}

void SCR_SetCrosshairColor(void)
{
    int health;

    if (!ch_health->integer) {
        return;
    }

    health = cl.frame.ps.stats[STAT_HEALTH];
    if (health <= 0) {
        VectorSet(scr.crosshair_color.u8, 0, 0, 0);
        return;
    }

    // red
    scr.crosshair_color.r = 255;

    // green
    if (health >= 66) {
        scr.crosshair_color.g = 255;
    } else if (health < 33) {
        scr.crosshair_color.g = 0;
    } else {
        scr.crosshair_color.g = (255 * (health - 33)) / 33;
    }

    // blue
    if (health >= 99) {
        scr.crosshair_color.b = 255;
    } else if (health < 66) {
        scr.crosshair_color.b = 0;
    } else {
        scr.crosshair_color.b = (255 * (health - 66)) / 33;
    }
}

void SCR_ModeChanged(void)
{
    IN_Activate();
    Con_CheckResize();
    UI_ModeChanged();
    cls.disable_screen = 0;
    if (scr.initialized)
        scr.hud_scale = R_ClampScale(scr_scale);
}

static void scr_font_changed(cvar_t *self)
{
    scr.font_pic = R_RegisterFont(self->string);
    if (!scr.font_pic && strcmp(self->string, self->default_string)) {
        Cvar_Reset(self);
        scr.font_pic = R_RegisterFont(self->default_string);
    }
}

/*
==================
SCR_Clear
==================
*/
void SCR_Clear(void)
{
    memset(scr.damage_entries, 0, sizeof(scr.damage_entries));
    memset(scr.pois, 0, sizeof(scr.pois));
}

/*
==================
SCR_RegisterMedia
==================
*/
void SCR_RegisterMedia(void)
{
    int     i;

    for (i = 0; i < STAT_MINUS; i++)
        scr.sb_pics[0][i] = R_RegisterPic(va("num_%d", i));
    scr.sb_pics[0][i] = R_RegisterPic("num_minus");

    for (i = 0; i < STAT_MINUS; i++)
        scr.sb_pics[1][i] = R_RegisterPic(va("anum_%d", i));
    scr.sb_pics[1][i] = R_RegisterPic("anum_minus");

    scr.inven_pic = R_RegisterPic("inventory");
    scr.field_pic = R_RegisterPic("field_3");
    scr.backtile_pic = R_RegisterImage("backtile", IT_PIC, IF_PERMANENT | IF_REPEAT);
    scr.pause_pic = R_RegisterPic("pause");
    scr.loading_pic = R_RegisterPic("loading");

    scr.damage_display_pic = R_RegisterPic("damage_indicator");
    R_GetPicSize(&scr.damage_display_width, &scr.damage_display_height, scr.damage_display_pic);

    scr.net_pic = R_RegisterPic("net");
    scr.hit_marker_pic = R_RegisterImage("marker", IT_PIC, IF_PERMANENT | IF_OPTIONAL);

    scr_crosshair_changed(scr_crosshair);

    if (cgame)
        cgame->TouchPics();

    SCR_LoadKFont(&scr.kfont, "fonts/qconfont.kfont");

    scr_font_changed(scr_font);
}

static void scr_scale_changed(cvar_t *self)
{
    scr.hud_scale = R_ClampScale(self);
}


//============================================================================

typedef struct stat_reg_s stat_reg_t;

typedef struct stat_reg_s {
    char        name[MAX_QPATH];
    xcommand_t  cb;

    stat_reg_t  *next;
} stat_reg_t;

static const stat_reg_t *stat_active;
static stat_reg_t *stat_head;

struct {
    int x, y;
    int key_width, value_width;
    int key_id;
} stat_state;

void SCR_RegisterStat(const char *name, xcommand_t cb)
{
    stat_reg_t *reg = Z_TagMalloc(sizeof(stat_reg_t), TAG_CMD);
    reg->next = stat_head;
    Q_strlcpy(reg->name, name, sizeof(reg->name));
    reg->cb = cb;
    stat_head = reg;
}

void SCR_UnregisterStat(const char *name)
{
    stat_reg_t **rover = &stat_head;

    while (*rover) {
        if (!strcmp((*rover)->name, name)) {
            stat_reg_t *s = *rover;
            *rover = s->next;

            if (stat_active == s)
                stat_active = NULL;

            Z_Free(s);
            return;
        }

        rover = &(*rover)->next;
    }

    Com_EPrintf("can't unregister missing stat \"%s\"\n", name);
}

static void SCR_ToggleStat(const char *name)
{
    stat_reg_t *rover = stat_head;

    while (rover) {
        if (!strcmp(rover->name, name)) {
            if (stat_active == rover) {
                stat_active = NULL;
            } else {
                stat_active = rover;
            }
            return;
        }

        rover = rover->next;
    }
}

void SCR_StatTableSize(int key_width, int value_width)
{
    stat_state.key_width = key_width;
    stat_state.value_width = value_width;
}

#define STAT_MARGIN 1

void SCR_StatKeyValue(const char *key, const char *value)
{
    int c = (stat_state.key_id & 1) ? 24 : 0;
    R_DrawFill32(stat_state.x, stat_state.y, CONCHAR_WIDTH * (stat_state.key_width + stat_state.value_width) + (STAT_MARGIN * 2), CONCHAR_HEIGHT + (STAT_MARGIN * 2), COLOR_RGBA(c, c, c, 127));
    SCR_DrawString(stat_state.x + STAT_MARGIN, stat_state.y + STAT_MARGIN, UI_DROPSHADOW, COLOR_WHITE, key);
    stat_state.x += CONCHAR_WIDTH * stat_state.key_width;
    SCR_DrawString(stat_state.x + STAT_MARGIN, stat_state.y + STAT_MARGIN, UI_DROPSHADOW, COLOR_WHITE, value);

    stat_state.x = 24;
    stat_state.y += CONCHAR_HEIGHT + (STAT_MARGIN * 2);
    stat_state.key_id++;
}

void SCR_DrawStats(void)
{
    if (!stat_active)
        return;

    stat_state.x = 24;
    stat_state.y = 24;
    stat_state.key_id = 0;

    SCR_StatTableSize(24, 32);

    stat_active->cb();
}

bool SCR_StatActive(void)
{
    return !!stat_active;
}

static void SCR_Stat_g(genctx_t *ctx)
{
    if (!stat_head)
        return;

    for (stat_reg_t *stat = stat_head; stat; stat = stat->next) {
        Prompt_AddMatch(ctx, stat->name);
    }
}

static void SCR_Stat_c(genctx_t *ctx, int argnum)
{
    if (argnum == 1) {
        SCR_Stat_g(ctx);
    }
}

static void SCR_Stat_f(void)
{
    if (Cmd_Argc() == 2) {
        SCR_ToggleStat(Cmd_Argv(1));
    } else {
        Com_Printf("Available stats:\n");

        stat_reg_t *rover = stat_head;

        while (rover) {
            Com_Printf(" * %s\n", rover->name);
            rover = rover->next;
        }
    }
}

static const cmdreg_t scr_cmds[] = {
    { "timerefresh", SCR_TimeRefresh_f },
    { "sizeup", SCR_SizeUp_f },
    { "sizedown", SCR_SizeDown_f },
    { "sky", SCR_Sky_f },
    { "draw", SCR_Draw_f, SCR_Draw_c },
    { "undraw", SCR_UnDraw_f, SCR_UnDraw_c },
    { "clearchathud", SCR_ClearChatHUD_f },
    { "stat", SCR_Stat_f, SCR_Stat_c },
    { NULL }
};

/*
==================
SCR_Init
==================
*/
void SCR_Init(void)
{
    scr_viewsize = Cvar_Get("viewsize", "100", CVAR_ARCHIVE);
    scr_showpause = Cvar_Get("scr_showpause", "1", 0);
    scr_demobar = Cvar_Get("scr_demobar", "1", 0);
    scr_font = Cvar_Get("scr_font", "conchars", 0);
    scr_font->changed = scr_font_changed;
    scr_scale = Cvar_Get("scr_scale", "0", CVAR_ARCHIVE);
    scr_scale->changed = scr_scale_changed;
    scr_crosshair = Cvar_Get("crosshair", "3", CVAR_ARCHIVE);
    scr_crosshair->changed = scr_crosshair_changed;

    scr_netgraph = Cvar_Get("netgraph", "0", 0);
    scr_timegraph = Cvar_Get("timegraph", "0", 0);
    scr_debuggraph = Cvar_Get("debuggraph", "0", 0);
    scr_graphheight = Cvar_Get("graphheight", "32", 0);
    scr_graphscale = Cvar_Get("graphscale", "1", 0);
    scr_graphshift = Cvar_Get("graphshift", "0", 0);

    scr_chathud = Cvar_Get("scr_chathud", "0", CVAR_ARCHIVE);
    scr_chathud_lines = Cvar_Get("scr_chathud_lines", "4", CVAR_ARCHIVE);
    scr_chathud_time = Cvar_Get("scr_chathud_time", "0", CVAR_ARCHIVE);
    scr_chathud_time->changed = cl_timeout_changed;
    scr_chathud_time->changed(scr_chathud_time);
    scr_chathud_x = Cvar_Get("scr_chathud_x", "8", CVAR_ARCHIVE);
    scr_chathud_y = Cvar_Get("scr_chathud_y", "-64", CVAR_ARCHIVE);

    ch_health = Cvar_Get("ch_health", "0", 0);
    ch_health->changed = ch_color_changed;
    ch_red = Cvar_Get("ch_red", "1", 0);
    ch_red->changed = ch_color_changed;
    ch_green = Cvar_Get("ch_green", "1", 0);
    ch_green->changed = ch_color_changed;
    ch_blue = Cvar_Get("ch_blue", "1", 0);
    ch_blue->changed = ch_color_changed;
    ch_alpha = Cvar_Get("ch_alpha", "1", 0);
    ch_alpha->changed = ch_color_changed;

    ch_scale = Cvar_Get("ch_scale", "1", 0);
    ch_scale->changed = ch_scale_changed;
    ch_x = Cvar_Get("ch_x", "0", 0);
    ch_y = Cvar_Get("ch_y", "0", 0);

    scr_draw2d = Cvar_Get("scr_draw2d", "2", 0);
    scr_showturtle = Cvar_Get("scr_showturtle", "1", 0);
    scr_lag_x = Cvar_Get("scr_lag_x", "-1", 0);
    scr_lag_y = Cvar_Get("scr_lag_y", "-1", 0);
    scr_lag_draw = Cvar_Get("scr_lag_draw", "0", 0);
    scr_lag_min = Cvar_Get("scr_lag_min", "0", 0);
    scr_lag_max = Cvar_Get("scr_lag_max", "200", 0);
    scr_alpha = Cvar_Get("scr_alpha", "1", 0);
#if USE_DEBUG
    scr_showstats = Cvar_Get("scr_showstats", "0", 0);
    scr_showpmove = Cvar_Get("scr_showpmove", "0", 0);
#endif

    scr_hit_marker_time = Cvar_Get("scr_hit_marker_time", "500", 0);
    
    scr_damage_indicators = Cvar_Get("scr_damage_indicators", "1", 0);
    scr_damage_indicator_time = Cvar_Get("scr_damage_indicator_time", "1000", 0);

    scr_pois = Cvar_Get("scr_pois", "1", 0);
    scr_poi_edge_frac = Cvar_Get("scr_poi_edge_frac", "0.15", 0);
    scr_poi_max_scale = Cvar_Get("scr_poi_max_scale", "1.0", 0);
    scr_safe_zone = Cvar_Get("scr_safe_zone", "0.02", 0);

    Cmd_Register(scr_cmds);

    scr_scale_changed(scr_scale);
    ch_color_changed(NULL);

    scr.initialized = true;
}

void SCR_Shutdown(void)
{
    Cmd_Deregister(scr_cmds);
    scr.initialized = false;
}

//=============================================================================

/*
================
SCR_BeginLoadingPlaque
================
*/
void SCR_BeginLoadingPlaque(void)
{
    if (!cls.state) {
        return;
    }

    S_StopAllSounds();
    OGG_Update();

    if (cls.disable_screen) {
        return;
    }

#if USE_DEBUG
    if (developer->integer) {
        return;
    }
#endif

    // if at console or menu, don't bring up the plaque
    if (cls.key_dest & (KEY_CONSOLE | KEY_MENU)) {
        return;
    }

    scr.draw_loading = true;
    SCR_UpdateScreen();

    cls.disable_screen = Sys_Milliseconds();
}

/*
================
SCR_EndLoadingPlaque
================
*/
void SCR_EndLoadingPlaque(void)
{
    if (!cls.state) {
        return;
    }
    cls.disable_screen = 0;
    Con_ClearNotify_f();
}

// Clear any parts of the tiled background that were drawn on last frame
static void SCR_TileClear(void)
{
    int top, bottom, left, right;

    //if (con.currentHeight == 1)
    //  return;     // full screen console

    if (scr_viewsize->integer == 100)
        return;     // full screen rendering

    top = scr.vrect.y;
    bottom = top + scr.vrect.height;
    left = scr.vrect.x;
    right = left + scr.vrect.width;

    // clear above view screen
    R_TileClear(0, 0, scr.hud_width, top, scr.backtile_pic);

    // clear below view screen
    R_TileClear(0, bottom, scr.hud_width,
                scr.hud_height - bottom, scr.backtile_pic);

    // clear left of view screen
    R_TileClear(0, top, left, scr.vrect.height, scr.backtile_pic);

    // clear right of view screen
    R_TileClear(right, top, scr.hud_width - right,
                scr.vrect.height, scr.backtile_pic);
}

//=============================================================================

static void SCR_DrawPause(color_t base_color)
{
    int x, y, w, h;

    if (!sv_paused->integer)
        return;
    if (!cl_paused->integer)
        return;
    if (scr_showpause->integer != 1)
        return;

    R_GetPicSize(&w, &h, scr.pause_pic);
    x = (scr.hud_width - w) / 2;
    y = (scr.hud_height - h) / 2;

    R_DrawPic(x, y, base_color, scr.pause_pic);
}

static void SCR_DrawLoading(void)
{
    int x, y, w, h;

    if (!scr.draw_loading)
        return;

    scr.draw_loading = false;

    R_SetScale(scr.hud_scale);

    R_GetPicSize(&w, &h, scr.loading_pic);
    x = (r_config.width * scr.hud_scale - w) / 2;
    y = (r_config.height * scr.hud_scale - h) / 2;

    R_DrawPic(x, y, COLOR_WHITE, scr.loading_pic);

    R_SetScale(1.0f);
}

static void SCR_DrawHitMarker(color_t base_color)
{
    if (!cl.hit_marker_count)
        return;
    if (!scr.hit_marker_pic || scr_hit_marker_time->integer <= 0 ||
        cls.realtime - cl.hit_marker_time > scr_hit_marker_time->integer) {
        cl.hit_marker_count = 0;
        return;
    }

    float frac = (float)(cls.realtime - cl.hit_marker_time) / scr_hit_marker_time->integer;
    float alpha = 1.0f - (frac * frac);

    float scale = max(1.0f, 1.5f * (1.f - frac));
    int w = scr.hit_marker_width * scale;
    int h = scr.hit_marker_height * scale;

    int x = (scr.hud_width - w) / 2;
    int y = (scr.hud_height - h) / 2;

    color_t color = COLOR_RGBA(255, 0, 0, base_color.a * alpha);

    R_DrawStretchPic(x + ch_x->integer,
                     y + ch_y->integer,
                     w,
                     h,
                     color,
                 scr.hit_marker_pic);
}

static scr_damage_entry_t *SCR_AllocDamageDisplay(const vec3_t dir)
{
    scr_damage_entry_t *entry = scr.damage_entries;

    for (int i = 0; i < MAX_DAMAGE_ENTRIES; i++, entry++) {
        if (entry->time <= cls.realtime) {
            goto new_entry;
        }

        float dot = DotProduct(entry->dir, dir);

        if (dot >= 0.95f) {
            return entry;
        }
    }

    entry = scr.damage_entries;

new_entry:
    entry->damage = 0;
    VectorClear(entry->color);
    return entry;
}

void SCR_AddToDamageDisplay(int damage, const vec3_t color, const vec3_t dir)
{
    if (!scr_damage_indicators->integer) {
        return;
    }

    scr_damage_entry_t *entry = SCR_AllocDamageDisplay(dir);

    entry->damage += damage;
    VectorAdd(entry->color, color, entry->color);
    VectorNormalize(entry->color);
    VectorCopy(dir, entry->dir);
    entry->time = cls.realtime + scr_damage_indicator_time->integer;
}

static void SCR_DrawDamageDisplays(color_t base_color)
{
    for (int i = 0; i < MAX_DAMAGE_ENTRIES; i++) {
        scr_damage_entry_t *entry = &scr.damage_entries[i];

        if (entry->time <= cls.realtime)
            continue;

        float frac = (entry->time - cls.realtime) / scr_damage_indicator_time->value;

        float my_yaw = cl.predicted_angles[YAW];
        vec3_t angles;
        vectoangles2(entry->dir, angles);
        float damage_yaw = angles[YAW];
        float yaw_diff = DEG2RAD((my_yaw - damage_yaw) - 180);

        color_t color = COLOR_RGBA(
            (int) (entry->color[0] * 255.f),
            (int) (entry->color[1] * 255.f),
            (int) (entry->color[2] * 255.f),
            (int) (frac * base_color.a)
        );

        int x = scr.hud_width / 2;
        int y = scr.hud_height / 2;

        int size = min(scr.damage_display_width, (DAMAGE_ENTRY_BASE_SIZE * entry->damage));

        R_DrawStretchRotatePic(x, y, size, scr.damage_display_height, color, yaw_diff,
            0, -(scr.crosshair_height + (scr.damage_display_height / 2)), scr.damage_display_pic);
    }
}

void SCR_RemovePOI(int id)
{
    if (!scr_pois->integer)
        return;

    if (id == 0) {
        Com_WPrintf("tried to remove unkeyed POI\n");
        return;
    }
    
    scr_poi_t *poi = &scr.pois[0];

    for (int i = 0; i < MAX_TRACKED_POIS; i++, poi++) {

        if (poi->id == id) {
            poi->id = 0;
            poi->time = 0;
            break;
        }
    }
}

void SCR_AddPOI(int id, int time, const vec3_t p, int image, int color, int flags)
{
    if (!scr_pois->integer)
        return;

    scr_poi_t *poi = NULL;

    if (id == 0) {
        // find any free non-key'd POI. we'll find
        // the oldest POI as a fallback to replace.
    
        scr_poi_t *oldest_poi = NULL, *poi_rover = &scr.pois[0];

        for (int i = 0; i < MAX_TRACKED_POIS; i++, poi_rover++) {
            // not expired
            if (poi_rover->time > cl.time) {
                // keyed
                if (poi_rover->id) {
                    continue;
                } else if (!oldest_poi || poi_rover->time < oldest_poi->time) {
                    oldest_poi = poi_rover;
                }
            } else {
                // expired
                poi = poi_rover;
                break;
            }
        }

        if (!poi) {
            poi = oldest_poi;
        }

    } else {
        // we must replace a matching POI with the ID
        // if one exists, otherwise we pick a free POI,
        // and finally we pick the oldest non-key'd POI.

        scr_poi_t *oldest_poi = NULL;
        scr_poi_t *free_poi = NULL;
        scr_poi_t *poi_rover = &scr.pois[0];

        for (int i = 0; i < MAX_TRACKED_POIS; i++, poi_rover++) {
            // found matching ID, just re-use that one
            if (poi_rover->id == id) {
                poi = poi_rover;
                break;
            }

            if (poi_rover->time <= cl.time) {
                // expired
                if (!free_poi) {
                    free_poi = poi_rover;
                }
            } else {
                // not expired; we should only ever replace non-key'd POIs
                if (!poi_rover->id) {
                    if (!oldest_poi || poi_rover->time < oldest_poi->time) {
                        oldest_poi = poi_rover;
                    }
                }
            }
        }

        if (!poi) {
            poi = free_poi ? free_poi : oldest_poi;
        }
    }

    if (!poi) {
        Com_WPrintf("couldn't add a POI\n");
    }

    poi->id = id;
    poi->time = cl.time + time;
    VectorCopy(p, poi->position);
    poi->image = cl.image_precache[image];
    R_GetPicSize(&poi->width, &poi->height, poi->image);
    poi->color = color;
    poi->flags = flags;
}

extern uint32_t d_8to24table[256];

typedef enum
{
    POI_FLAG_NONE = 0,
    POI_FLAG_HIDE_ON_AIM = 1, // hide the POI if we get close to it with our aim
} svc_poi_flags;

static void SCR_DrawPOIs(color_t base_color)
{
    if (!scr_pois->integer)
        return;

    float projection_matrix[16];
    Matrix_Frustum(cl.refdef.fov_x, cl.refdef.fov_y, 1.0f, 0.01f, 8192.f, projection_matrix);

    float view_matrix[16];
    vec3_t viewaxis[3];
    AnglesToAxis(cl.predicted_angles, viewaxis);
    Matrix_FromOriginAxis(cl.refdef.vieworg, viewaxis, view_matrix);

    Matrix_Multiply(projection_matrix, view_matrix, projection_matrix);
    
    scr_poi_t *poi = &scr.pois[0];

    float max_height = scr.hud_height * 0.75f;

    for (int i = 0; i < MAX_TRACKED_POIS; i++, poi++) {

        if (poi->time <= cl.time) {
            continue;
        }

        // https://www.khronos.org/opengl/wiki/GluProject_and_gluUnProject_code
        vec4_t sp = { poi->position[0], poi->position[1], poi->position[2], 1.0f };
        Matrix_TransformVec4(sp, projection_matrix, sp);

        bool behind = sp[3] < 0.f;

        if (sp[3]) {
            sp[3] = 1.0f / sp[3];
            VectorScale(sp, sp[3], sp);
        }

        sp[0] = ((sp[0] * 0.5f) + 0.5f) * scr.hud_width;
        sp[1] = ((-sp[1] * 0.5f) + 0.5f) * scr.hud_height;

        if (behind) {
            sp[0] = scr.hud_width - sp[0];
            sp[1] = scr.hud_height - sp[1];

            if (sp[1] > 0) {
                if (sp[0] < scr.hud_width / 2)
                    sp[0] = 0;
                else
                    sp[0] = scr.hud_width - 1;

                sp[1] = min(sp[1], max_height);
            }
        }

        // scale the icon if they are closer to the edges of the screen
        float scale = 1.0f;

        if (scr_poi_max_scale->value != 1.0f) {
            float edge_dist = min(scr.hud_width, scr.hud_height) * scr_poi_edge_frac->value;

            for (int x = 0; x < 2; x++) {
                float extent = ((x == 0) ? scr.hud_width : scr.hud_height);
                float frac;

                if (sp[x] < edge_dist) {
                    frac = (sp[x] / edge_dist);
                } else if (sp[x] > extent - edge_dist) {
                    frac = (extent - sp[x]) / edge_dist;
                } else {
                    continue;
                }

                scale = Q_clipf(1.0f + (1.0f - frac) * (scr_poi_max_scale->value - 1.f), scale, scr_poi_max_scale->value);
            }
        }

        // center & clamp
        int hw = (poi->width * scale) / 2;
        int hh = (poi->height * scale) / 2;
        
        sp[0] -= hw;
        sp[1] -= hh;
        
        sp[0] = Q_clipf(sp[0], 0, scr.hud_width - hw);
        sp[1] = Q_clipf(sp[1], 0, scr.hud_height - hh);

        color_t c = { .u32 = d_8to24table[poi->color] };

        // calculate alpha if necessary
        if (poi->flags & POI_FLAG_HIDE_ON_AIM) {
            vec3_t centered = { (scr.hud_width / 2) - sp[0], (scr.hud_height / 2) - sp[1], 0.f };
            sp[2] = 0.f;
            float len = VectorLength(centered);
            c.a = base_color.a * Q_clipf(len / (hw * 6), 0.25f, 1.0f);
        } else {
            c.a = base_color.a;
        }

        R_DrawStretchPic(sp[0], sp[1], hw, hh, c, poi->image);
    }
}

static void SCR_DrawCrosshair(color_t base_color)
{
    int x, y;

    if (!scr_crosshair->integer)
        return;
    if (cl.frame.ps.stats[STAT_LAYOUTS] & (LAYOUTS_HIDE_HUD | LAYOUTS_HIDE_CROSSHAIR))
        return;

    SCR_DrawPOIs(base_color);

    x = (scr.hud_width - scr.crosshair_width) / 2;
    y = (scr.hud_height - scr.crosshair_height) / 2;

    color_t crosshair_color = scr.crosshair_color;
    crosshair_color.a = (crosshair_color.a * base_color.a) / 255;

    R_DrawStretchPic(x + ch_x->integer,
                     y + ch_y->integer,
                     scr.crosshair_width,
                     scr.crosshair_height,
                     crosshair_color,
                     scr.crosshair_pic);

    SCR_DrawHitMarker(crosshair_color);

    SCR_DrawDamageDisplays(crosshair_color);
}

static void SCR_Draw2D(void)
{
    if (scr_draw2d->integer <= 0)
        return;     // turn off for screenshots

    if (cls.key_dest & KEY_MENU)
        return;

    R_SetScale(scr.hud_scale);

    scr.hud_height = Q_rint(scr.hud_height * scr.hud_scale);
    scr.hud_width = Q_rint(scr.hud_width * scr.hud_scale);
    
    // the rest of 2D elements share common alpha
    color_t color = COLOR_SETA_F(COLOR_WHITE, Cvar_ClampValue(scr_alpha, 0, 1));

    // crosshair has its own color and alpha
    SCR_DrawCrosshair(color);

    if (scr_timegraph->integer)
        SCR_DebugGraph(cls.frametime * 300, 0xdc);

    if (scr_debuggraph->integer || scr_timegraph->integer || scr_netgraph->integer)
        SCR_DrawDebugGraph();

    /* Draw cgame HUD elements
     * Note: a scaling factor of 1 is fine, we're passing a "pre-scale" HUD rect
     * and the drawing functions do the scaling */
    vrect_t hud_rect = {0, 0, scr.hud_width, scr.hud_height};
    vrect_t hud_safe = {scr.hud_width * scr_safe_zone->value, scr.hud_height * scr_safe_zone->value};
    cgame->DrawHUD(0, &cl.cgame_data, hud_rect, hud_safe, 1, 0, &cl.frame.ps);

    CL_Carousel_Draw();

    CL_Wheel_Draw();

    SCR_DrawNet(color);

    SCR_DrawObjects(color);

    SCR_DrawChatHUD(color);

    SCR_DrawTurtle(color);

    SCR_DrawPause(color);

    // debug stats have no alpha

#if USE_DEBUG
    SCR_DrawDebugStats();
    SCR_DrawDebugPmove();
#endif

    R_SetScale(1.0f);
}

static void SCR_DrawActive(void)
{
    // if full screen menu is up, do nothing at all
    if (!UI_IsTransparent())
        return;

    // draw black background if not active
    if (cls.state < ca_active) {
        R_DrawFill8(0, 0, r_config.width, r_config.height, 0);
        return;
    }

    if (cls.state == ca_cinematic) {
        SCR_DrawCinematic();
        return;
    }

    // start with full screen HUD
    scr.hud_height = r_config.height;
    scr.hud_width = r_config.width;

    SCR_DrawDemo();

    SCR_CalcVrect();

    // clear any dirty part of the background
    SCR_TileClear();

    // draw 3D game view
    V_RenderView();

    // draw all 2D elements
    SCR_Draw2D();
}

//=======================================================

/*
==================
SCR_UpdateScreen

This is called every frame, and can also be called explicitly to flush
text to the screen.
==================
*/
void SCR_UpdateScreen(void)
{
    static int recursive;

    if (!scr.initialized) {
        return;             // not initialized yet
    }

    // if the screen is disabled (loading plaque is up), do nothing at all
    if (cls.disable_screen) {
        unsigned delta = Sys_Milliseconds() - cls.disable_screen;

        if (delta < 120 * 1000) {
            return;
        }

        cls.disable_screen = 0;
        Com_Printf("Loading plaque timed out.\n");
    }

    if (recursive > 1) {
        Com_Error(ERR_FATAL, "%s: recursively called", __func__);
    }

    recursive++;

    R_BeginFrame();

    // do 3D refresh drawing
    SCR_DrawActive();

    // draw main menu
    UI_Draw(cls.realtime);

    // draw console
    Con_DrawConsole();

    // draw loading plaque
    SCR_DrawLoading();

    R_EndFrame();

    recursive--;
}
