/*
Copyright (C) 2003-2012 Andrey Nazarov

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

#pragma once

#if USE_CLIENT
extern const char com_env_suf[6][3];
#endif

typedef enum {
    COLOR_INDEX_BLACK,
    COLOR_INDEX_RED,
    COLOR_INDEX_GREEN,
    COLOR_INDEX_YELLOW,
    COLOR_INDEX_BLUE,
    COLOR_INDEX_CYAN,
    COLOR_INDEX_MAGENTA,
    COLOR_INDEX_WHITE,

    COLOR_INDEX_ALT,
    COLOR_INDEX_NONE,

    COLOR_INDEX_COUNT
} color_index_t;

extern const char *const colorNames[COLOR_INDEX_COUNT];

bool Com_WildCmpEx(const char *filter, const char *string, int term, bool ignorecase);
#define Com_WildCmp(filter, string)  Com_WildCmpEx(filter, string, 0, false)

#if USE_CLIENT || USE_MVD_CLIENT
bool Com_ParseTimespec(const char *s, int *frames);
#endif

void Com_PlayerToEntityState(const player_state_t *ps, entity_state_t *es);

// only begin attenuating sound volumes when outside the FULLVOLUME range
#define SOUND_FULLVOLUME            80
#define SOUND_LOOPATTENUATE         (ATTN_STATIC * 0.001f)
#define SOUND_LOOPATTENUATE_MULT    0.0006f

float Com_GetEntityLoopDistMult(float attenuation);

bool Com_ParseMapName(char *out, const char *in, size_t size);

unsigned Com_HashString(const char *s, unsigned size);
unsigned Com_HashStringLen(const char *s, size_t len, unsigned size);

size_t Com_FormatLocalTime(char *buffer, size_t size, const char *fmt);

size_t Com_FormatTime(char *buffer, size_t size, time_t t);
size_t Com_FormatTimeLong(char *buffer, size_t size, time_t t);
size_t Com_TimeDiff(char *buffer, size_t size, time_t *p, time_t now);
size_t Com_TimeDiffLong(char *buffer, size_t size, time_t *p, time_t now);

size_t Com_FormatSize(char *dest, size_t destsize, int64_t bytes);
size_t Com_FormatSizeLong(char *dest, size_t destsize, int64_t bytes);

void Com_PageInMemory(void *buffer, size_t size);

color_index_t Com_ParseColor(const char *s);

#if USE_REF
unsigned Com_ParseExtensionString(const char *s, const char *const extnames[]);
#endif

extern const char com_hexchars[16];

size_t Com_EscapeString(char *dst, const char *src, size_t size);

char *Com_MakePrintable(const char *s);

#if USE_CLIENT
uint32_t Com_SlowRand(void);
#define Com_SlowFrand()  ((int32_t)Com_SlowRand() * 0x1p-32f + 0.5f)
#define Com_SlowCrand()  ((int32_t)Com_SlowRand() * 0x1p-31f)
#endif

#if USE_CLIENT
#define UNICODE_UNKNOWN     0xFFFD
#define UNICODE_MAX         0x10FFFF
uint32_t UTF8_ReadCodePoint(const char **src);
size_t UTF8_TranslitBuffer(char *dst, const char *src, size_t size);
char *UTF8_TranslitString(const char *src);
#endif

// Bitmap chunks (for sparse bitmaps)
#define BC_BITS         (sizeof(size_t) * CHAR_BIT)
#define BC_COUNT(n)     (((n) + BC_BITS - 1) / BC_BITS)

// Some mods actually exploit CS_STATUSBAR to take space up to CS_AIRACCEL
static inline size_t Com_ConfigstringSize(const cs_remap_t *csr, int cs)
{
    if (cs >= CS_STATUSBAR && cs < csr->airaccel)
        return csr->configstring_size * (csr->airaccel - cs);

    if (cs >= csr->general && cs < csr->end)
        return csr->configstring_size * (csr->end - cs);

    return csr->configstring_size;
}

typedef struct {
    int         time;      // variable server frame time
    int         div;       // BASE_FRAMETIME/frametime
} frametime_t;

// Compute frametime based on requested frame rate
static inline frametime_t Com_ComputeFrametime(int rate)
{
    int framediv = Q_clip(rate / BASE_FRAMERATE, 1, MAX_FRAMEDIV);
    return (frametime_t){ .time = BASE_FRAMETIME / framediv, .div = framediv };
}
