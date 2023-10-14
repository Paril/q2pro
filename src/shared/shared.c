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

#include "shared/shared.h"

const vec3_t vec3_origin = { 0, 0, 0 };

void AngleVectors(const vec3_t angles, vec3_t forward, vec3_t right, vec3_t up)
{
    float        angle;
    float        sr, sp, sy, cr, cp, cy;

    angle = DEG2RAD(angles[YAW]);
    sy = sin(angle);
    cy = cos(angle);
    angle = DEG2RAD(angles[PITCH]);
    sp = sin(angle);
    cp = cos(angle);
    angle = DEG2RAD(angles[ROLL]);
    sr = sin(angle);
    cr = cos(angle);

    if (forward) {
        forward[0] = cp * cy;
        forward[1] = cp * sy;
        forward[2] = -sp;
    }
    if (right) {
        right[0] = (-1 * sr * sp * cy + -1 * cr * -sy);
        right[1] = (-1 * sr * sp * sy + -1 * cr * cy);
        right[2] = -1 * sr * cp;
    }
    if (up) {
        up[0] = (cr * sp * cy + -sr * -sy);
        up[1] = (cr * sp * sy + -sr * cy);
        up[2] = cr * cp;
    }
}

vec_t VectorNormalize(vec3_t v)
{
    float    length, ilength;

    length = VectorLength(v);

    if (length) {
        ilength = 1 / length;
        v[0] *= ilength;
        v[1] *= ilength;
        v[2] *= ilength;
    }

    return length;
}

vec_t VectorNormalize2(const vec3_t v, vec3_t out)
{
    VectorCopy(v, out);
    return VectorNormalize(out);
}

void ClearBounds(vec3_t mins, vec3_t maxs)
{
    mins[0] = mins[1] = mins[2] = 99999;
    maxs[0] = maxs[1] = maxs[2] = -99999;
}

void AddPointToBounds(const vec3_t v, vec3_t mins, vec3_t maxs)
{
    int        i;
    vec_t    val;

    for (i = 0; i < 3; i++) {
        val = v[i];
        mins[i] = min(mins[i], val);
        maxs[i] = max(maxs[i], val);
    }
}

void UnionBounds(const vec3_t a[2], const vec3_t b[2], vec3_t c[2])
{
    int        i;

    for (i = 0; i < 3; i++) {
        c[0][i] = min(a[0][i], b[0][i]);
        c[1][i] = max(a[1][i], b[1][i]);
    }
}

/*
=================
RadiusFromBounds
=================
*/
vec_t RadiusFromBounds(const vec3_t mins, const vec3_t maxs)
{
    int     i;
    vec3_t  corner;
    vec_t   a, b;

    for (i = 0; i < 3; i++) {
        a = fabsf(mins[i]);
        b = fabsf(maxs[i]);
        corner[i] = max(a, b);
    }

    return VectorLength(corner);
}

#if USE_MD5
#define X 0
#define Y 1
#define Z 2
#define W 3

void Quat_ComputeW (quat_t q)
{
	float t = 1.0f - (q[X] * q[X]) - (q[Y] * q[Y]) - (q[Z] * q[Z]);

	if (t < 0.0f) {
		q[W] = 0.0f;
    } else {
		q[W] = -sqrtf(t);
    }
}

#define QUAT_EPSILON 0.000001f

void Quat_SLerp (const quat_t qa, const quat_t qb, float backlerp, float frontlerp, quat_t out)
{
	if (backlerp <= 0.0) {
        Vector4Copy(qb, out);
		return;
    } else if (backlerp >= 1.0) {
        Vector4Copy(qa, out);
		return;
	}

    // compute "cosine of angle between quaternions" using dot product
	float cosOmega = Dot4Product (qa, qb);

	/* If negative dot, use -q1.  Two quaternions q and -q
	   represent the same rotation, but may produce
	   different slerp.  We chose q or -q to rotate using
	   the acute angle. */
	float q1w = qb[W];
	float q1x = qb[X];
	float q1y = qb[Y];
	float q1z = qb[Z];

	if (cosOmega < 0.0f) {
		q1w = -q1w;
		q1x = -q1x;
		q1y = -q1y;
		q1z = -q1z;
		cosOmega = -cosOmega;
	}

	// compute interpolation fraction
	float k0, k1;

	if (1.0f - cosOmega <= QUAT_EPSILON) {
        // very close - just use linear interpolation
		k0 = backlerp;
		k1 = frontlerp;
	} else {
        // compute the sin of the angle using the trig identity sin^2(omega) + cos^2(omega) = 1
		float sinOmega = sqrtf(1.0f - (cosOmega * cosOmega));

		// compute the angle from its sin and cosine
		float omega = atan2f(sinOmega, cosOmega);
		float oneOverSinOmega = 1.0f / sinOmega;

		k0 = sinf(backlerp * omega) * oneOverSinOmega;
		k1 = sinf(frontlerp * omega) * oneOverSinOmega;
	}

	out[W] = (k0 * qa[3]) + (k1 * q1w);
	out[X] = (k0 * qa[0]) + (k1 * q1x);
	out[Y] = (k0 * qa[1]) + (k1 * q1y);
	out[Z] = (k0 * qa[2]) + (k1 * q1z);
}

float Quat_Normalize(quat_t q)
{
	float length = sqrtf(Dot4Product(q, q));

	if (length) {
		float ilength = 1 / length;
		q[X] *= ilength;
		q[Y] *= ilength;
		q[Z] *= ilength;
		q[W] *= ilength;
	}

    return length;
}

void Quat_MultiplyQuat (const quat_t qa, const quat_t qb, quat_t out)
{
	out[W] = (qa[W] * qb[W]) - (qa[X] * qb[X]) - (qa[Y] * qb[Y]) - (qa[Z] * qb[Z]);
	out[X] = (qa[X] * qb[W]) + (qa[W] * qb[X]) + (qa[Y] * qb[Z]) - (qa[Z] * qb[Y]);
	out[Y] = (qa[Y] * qb[W]) + (qa[W] * qb[Y]) + (qa[Z] * qb[X]) - (qa[X] * qb[Z]);
	out[Z] = (qa[Z] * qb[W]) + (qa[W] * qb[Z]) + (qa[X] * qb[Y]) - (qa[Y] * qb[X]);
}

void Quat_MultiplyVector (const quat_t q, const vec3_t v, quat_t out)
{
	out[W] = -(q[X] * v[X]) - (q[Y] * v[Y]) - (q[Z] * v[Z]);
	out[X] =  (q[W] * v[X]) + (q[Y] * v[Z]) - (q[Z] * v[Y]);
	out[Y] =  (q[W] * v[Y]) + (q[Z] * v[X]) - (q[X] * v[Z]);
	out[Z] =  (q[W] * v[Z]) + (q[X] * v[Y]) - (q[Y] * v[X]);
}

void Quat_Invert(const quat_t in, quat_t out)
{
    out[X] = -in[X];
    out[Y] = -in[Y];
    out[Z] = -in[Z];
    out[W] = in[W];
}

void Quat_RotatePoint (const quat_t q, const vec3_t in, vec3_t out)
{
	quat_t tmp, inv, output;

    Quat_Invert(q, inv);
	Quat_Normalize (inv);
	Quat_MultiplyVector (q, in, tmp);
	Quat_MultiplyQuat (tmp, inv, output);

	out[X] = output[X];
	out[Y] = output[Y];
	out[Z] = output[Z];
}
#endif

//====================================================================================

/*
============
COM_SkipPath
============
*/
char *COM_SkipPath(const char *pathname)
{
    char    *last;

    Q_assert(pathname);

    last = (char *)pathname;
    while (*pathname) {
        if (*pathname == '/')
            last = (char *)pathname + 1;
        pathname++;
    }
    return last;
}

/*
============
COM_StripExtension
============
*/
size_t COM_StripExtension(char *out, const char *in, size_t size)
{
    size_t ret = COM_FileExtension(in) - in;

    if (size) {
        size_t len = min(ret, size - 1);
        memcpy(out, in, len);
        out[len] = 0;
    }

    return ret;
}

/*
============
COM_FileExtension
============
*/
char *COM_FileExtension(const char *in)
{
    const char *last, *s;

    Q_assert(in);

    for (last = s = in + strlen(in); s != in; s--) {
        if (*s == '/') {
            break;
        }
        if (*s == '.') {
            return (char *)s;
        }
    }

    return (char *)last;
}

/*
==================
COM_DefaultExtension

if path doesn't have .EXT, append extension
(extension should include the .)
==================
*/
size_t COM_DefaultExtension(char *path, const char *ext, size_t size)
{
    if (*COM_FileExtension(path))
        return strlen(path);
    else
        return Q_strlcat(path, ext, size);
}

/*
==================
COM_IsFloat

Returns true if the given string is valid representation
of floating point number.
==================
*/
bool COM_IsFloat(const char *s)
{
    int c, dot = '.';

    if (*s == '-') {
        s++;
    }
    if (!*s) {
        return false;
    }

    do {
        c = *s++;
        if (c == dot) {
            dot = 0;
        } else if (!Q_isdigit(c)) {
            return false;
        }
    } while (*s);

    return true;
}

bool COM_IsUint(const char *s)
{
    int c;

    if (!*s) {
        return false;
    }

    do {
        c = *s++;
        if (!Q_isdigit(c)) {
            return false;
        }
    } while (*s);

    return true;
}

bool COM_IsPath(const char *s)
{
    int c;

    if (!*s) {
        return false;
    }

    do {
        c = *s++;
        if (!Q_ispath(c)) {
            return false;
        }
    } while (*s);

    return true;
}

bool COM_IsWhite(const char *s)
{
    int c;

    while (*s) {
        c = *s++;
        if (Q_isgraph(c)) {
            return false;
        }
    }

    return true;
}

int SortStrcmp(const void *p1, const void *p2)
{
    return strcmp(*(const char **)p1, *(const char **)p2);
}

int SortStricmp(const void *p1, const void *p2)
{
    return Q_stricmp(*(const char **)p1, *(const char **)p2);
}

/*
================
COM_strclr

Operates inplace, normalizing high-bit and removing unprintable characters.
Returns final number of characters, not including the NUL character.
================
*/
size_t COM_strclr(char *s)
{
    char *p;
    int c;
    size_t len;

    p = s;
    len = 0;
    while (*s) {
        c = *s++;
        c &= 127;
        if (Q_isprint(c)) {
            *p++ = c;
            len++;
        }
    }

    *p = 0;

    return len;
}

char *COM_StripQuotes(char *s)
{
    if (*s == '"') {
        size_t p = strlen(s) - 1;

        if (s[p] == '"') {
            s[p] = 0;
            return s + 1;
        }
    }

    return s;
}

char *COM_TrimSpace(char *s)
{
    size_t len;

    while (*s && *s <= ' ')
        s++;

    len = strlen(s);
    while (len > 0 && s[len - 1] <= ' ')
        len--;

    s[len] = 0;
    return s;
}

/*
============
va

does a varargs printf into a temp buffer, so I don't need to have
varargs versions of all text functions.
============
*/
char *va(const char *format, ...)
{
    va_list         argptr;
    static char     buffers[4][MAX_STRING_CHARS];
    static int      index;

    index = (index + 1) & 3;

    va_start(argptr, format);
    Q_vsnprintf(buffers[index], sizeof(buffers[0]), format, argptr);
    va_end(argptr);

    return buffers[index];
}

/*
=============
vtos

This is just a convenience function for printing vectors.
=============
*/
char *vtos(const vec3_t v)
{
    static char str[8][32];
    static int  index;

    index = (index + 1) & 7;

    Q_snprintf(str[index], sizeof(str[0]), "(%.f %.f %.f)", v[0], v[1], v[2]);

    return str[index];
}

static char     com_token[4][MAX_TOKEN_CHARS];
static int      com_tokidx;

/*
==============
COM_Parse

Parse a token out of a string.
Handles C and C++ comments.
==============
*/
char *COM_ParseEx(const char **data_p, int32_t flags, char *output, size_t output_length)
{
    int         c;
    int         len;
    const char  *data;
    
    if (!output) {
        output = com_token[com_tokidx];
        output_length = sizeof(com_token[0]);
        com_tokidx = (com_tokidx + 1) & 3;
    }

    Q_assert(output_length);
        
    char        *s = output;

    data = *data_p;
    s[0] = 0;

    if (!data) {
        *data_p = NULL;
        return s;
    }

// skip whitespace
skipwhite:
    while ((c = *data) <= ' ') {
        if (c == 0) {
            *data_p = NULL;
            return s;
        }
        data++;
    }

// skip // comments
    if (c == '/' && data[1] == '/') {
        data += 2;
        while (*data && *data != '\n')
            data++;
        goto skipwhite;
    }

// skip /* */ comments
    if (c == '/' && data[1] == '*') {
        data += 2;
        while (*data) {
            if (data[0] == '*' && data[1] == '/') {
                data += 2;
                break;
            }
            data++;
        }
        goto skipwhite;
    }

    len = 0;

// handle quoted strings specially
    if (c == '\"') {
        data++;
        while (1) {
            c = *data++;
            if (c == '\"' || !c) {
                goto finish;
            }

            if (c == '\\' && (flags & PARSE_FLAG_ESCAPE)) {
                c = *data++;

                if (c == 'n') {
                    c = '\n';
                } else if (c == 't') {
                    c = '\t';
                } else if (c == '\0') {
                    goto finish;
                }
            }

            if (len < output_length - 1) {
                s[len++] = c;
            }
        }
    }

// parse a regular word
    do {

        if (c == '\\' && (flags & PARSE_FLAG_ESCAPE)) {
            c = *data++;

            if (c == 'n') {
                c = '\n';
            } else if (c == 't') {
                c = '\t';
            } else if (c == 'r') {
                c = '\r';
            } else if (c == '\0') {
                break;
            }
        }

        if (len < output_length - 1) {
            s[len++] = c;
        }
        data++;
        c = *data;
    } while (c > 32);

finish:
    s[len] = 0;

    *data_p = data;
    return s;
}

/*
==============
COM_Compress

Operates in place, removing excess whitespace and comments.
Non-contiguous line feeds are preserved.

Returns resulting data length.
==============
*/
size_t COM_Compress(char *data)
{
    int     c, n = 0;
    char    *s = data, *d = data;

    while (*s) {
        // skip whitespace
        if (*s <= ' ') {
            if (n == 0) {
                n = ' ';
            }
            do {
                c = *s++;
                if (c == '\n') {
                    n = '\n';
                }
                if (!c) {
                    goto finish;
                }
            } while (*s <= ' ');
        }

        // skip // comments
        if (s[0] == '/' && s[1] == '/') {
            n = ' ';
            s += 2;
            while (*s && *s != '\n') {
                s++;
            }
            continue;
        }

        // skip /* */ comments
        if (s[0] == '/' && s[1] == '*') {
            n = ' ';
            s += 2;
            while (*s) {
                if (s[0] == '*' && s[1] == '/') {
                    s += 2;
                    break;
                }
                if (*s == '\n') {
                    n = '\n';
                }
                s++;
            }
            continue;
        }

        // add whitespace character
        if (n) {
            *d++ = n;
            n = 0;
        }

        // handle quoted strings specially
        if (*s == '\"') {
            s++;
            *d++ = '\"';
            do {
                c = *s++;
                if (!c) {
                    goto finish;
                }
                *d++ = c;
            } while (c != '\"');
            continue;
        }

        // handle line feed escape
        if (*s == '\\' && s[1] == '\n') {
            s += 2;
            continue;
        }
        if (*s == '\\' && s[1] == '\r' && s[2] == '\n') {
            s += 3;
            continue;
        }

        // parse a regular word
        do {
            *d++ = *s++;
        } while (*s > ' ');
    }

finish:
    *d = 0;

    return d - data;
}

/*
============================================================================

                    LIBRARY REPLACEMENT FUNCTIONS

============================================================================
*/

int Q_strncasecmp(const char *s1, const char *s2, size_t n)
{
    int        c1, c2;

    do {
        c1 = *s1++;
        c2 = *s2++;

        if (!n--)
            return 0;        /* strings are equal until end point */

        if (c1 != c2) {
            c1 = Q_tolower(c1);
            c2 = Q_tolower(c2);
            if (c1 < c2)
                return -1;
            if (c1 > c2)
                return 1;        /* strings not equal */
        }
    } while (c1);

    return 0;        /* strings are equal */
}

int Q_strcasecmp(const char *s1, const char *s2)
{
    int        c1, c2;

    do {
        c1 = *s1++;
        c2 = *s2++;

        if (c1 != c2) {
            c1 = Q_tolower(c1);
            c2 = Q_tolower(c2);
            if (c1 < c2)
                return -1;
            if (c1 > c2)
                return 1;        /* strings not equal */
        }
    } while (c1);

    return 0;        /* strings are equal */
}

char *Q_strcasestr(const char *s1, const char *s2)
{
    size_t l1, l2;

    l2 = strlen(s2);
    if (!l2) {
        return (char *)s1;
    }

    l1 = strlen(s1);
    while (l1 >= l2) {
        l1--;
        if (!Q_strncasecmp(s1, s2, l2)) {
            return (char *)s1;
        }
        s1++;
    }

    return NULL;
}

/*
===============
Q_strlcpy

Returns length of the source string.
===============
*/
size_t Q_strlcpy(char *dst, const char *src, size_t size)
{
    size_t ret = strlen(src);

    if (size) {
        size_t len = min(ret, size - 1);
        memcpy(dst, src, len);
        dst[len] = 0;
    }

    return ret;
}

/*
===============
Q_strnlcpy

Returns `count`.
===============
*/
size_t Q_strnlcpy(char *dst, const char *src, size_t count, size_t size)
{
    size_t ret = min(count, strlen(src));

    if (size) {
        size_t len = min(ret, size - 1);
        memcpy(dst, src, len);
        dst[len] = 0;
    }

    return ret;
}

/*
===============
Q_strlcat

Returns length of the source and destinations strings combined.
===============
*/
size_t Q_strlcat(char *dst, const char *src, size_t size)
{
    size_t len = strlen(dst);

    Q_assert(len < size);

    return len + Q_strlcpy(dst + len, src, size - len);
}

/*
===============
Q_strnlcat

Returns length of the source and destinations strings combined.
===============
*/
size_t Q_strnlcat(char *dst, const char *src, size_t count, size_t size)
{
    size_t len = strlen(dst);

    Q_assert(len < size);

    return len + Q_strnlcpy(dst + len, src, count, size - len);
}

/*
===============
Q_concat_array

Returns number of characters that would be written into the buffer,
excluding trailing '\0'. If the returned value is equal to or greater than
buffer size, resulting string is truncated.
===============
*/
size_t Q_concat_array(char *dest, size_t size, const char **arr)
{
    size_t total = 0;

    while (*arr) {
        const char *s = *arr++;
        size_t len = strlen(s);
        if (total < size) {
            size_t l = min(size - total - 1, len);
            memcpy(dest, s, l);
            dest += l;
        }
        total += len;
    }

    if (size) {
        *dest = 0;
    }

    return total;
}

/*
===============
Q_vsnprintf

Returns number of characters that would be written into the buffer,
excluding trailing '\0'. If the returned value is equal to or greater than
buffer size, resulting string is truncated.
===============
*/
size_t Q_vsnprintf(char *dest, size_t size, const char *fmt, va_list argptr)
{
    int ret;

    Q_assert(size <= INT_MAX);
    ret = vsnprintf(dest, size, fmt, argptr);
    Q_assert(ret >= 0);

    return ret;
}

/*
===============
Q_vscnprintf

Returns number of characters actually written into the buffer,
excluding trailing '\0'. If buffer size is 0, this function does nothing
and returns 0.
===============
*/
size_t Q_vscnprintf(char *dest, size_t size, const char *fmt, va_list argptr)
{
    if (size) {
        size_t ret = Q_vsnprintf(dest, size, fmt, argptr);
        return min(ret, size - 1);
    }

    return 0;
}

/*
===============
Q_snprintf

Returns number of characters that would be written into the buffer,
excluding trailing '\0'. If the returned value is equal to or greater than
buffer size, resulting string is truncated.
===============
*/
size_t Q_snprintf(char *dest, size_t size, const char *fmt, ...)
{
    va_list argptr;
    size_t  ret;

    va_start(argptr, fmt);
    ret = Q_vsnprintf(dest, size, fmt, argptr);
    va_end(argptr);

    return ret;
}

/*
===============
Q_scnprintf

Returns number of characters actually written into the buffer,
excluding trailing '\0'. If buffer size is 0, this function does nothing
and returns 0.
===============
*/
size_t Q_scnprintf(char *dest, size_t size, const char *fmt, ...)
{
    va_list argptr;
    size_t  ret;

    va_start(argptr, fmt);
    ret = Q_vscnprintf(dest, size, fmt, argptr);
    va_end(argptr);

    return ret;
}

char *Q_strchrnul(const char *s, int c)
{
    while (*s && *s != c) {
        s++;
    }
    return (char *)s;
}

/*
===============
Q_memccpy

Copies no more than 'size' bytes stopping when 'c' character is found.
Returns pointer to next byte after 'c' in 'dst', or NULL if 'c' was not found.
===============
*/
void *Q_memccpy(void *dst, const void *src, int c, size_t size)
{
    byte *d = dst;
    const byte *s = src;

    while (size--) {
        if ((*d++ = *s++) == c) {
            return d;
        }
    }

    return NULL;
}

size_t Q_strnlen(const char *s, size_t maxlen)
{
    char *p = memchr(s, 0, maxlen);
    return p ? p - s : maxlen;
}

/*
=====================================================================

  MT19337 PRNG

=====================================================================
*/

#define N 624
#define M 397

static uint32_t mt_state[N];
static uint32_t mt_index;

/*
==================
Q_srand

Seed PRNG with initial value
==================
*/
void Q_srand(uint32_t seed)
{
    mt_index = N;
    mt_state[0] = seed;
    for (int i = 1; i < N; i++)
        mt_state[i] = seed = 1812433253 * (seed ^ seed >> 30) + i;
}

/*
==================
Q_rand

Generate random integer in range [0, 2^32)
==================
*/
uint32_t Q_rand(void)
{
    uint32_t x, y;
    int i;

    if (mt_index >= N) {
        mt_index = 0;

#define STEP(j, k) do {                 \
        x  = mt_state[i] & 0x80000000;  \
        x |= mt_state[j] & 0x7FFFFFFF;  \
        y  = x >> 1;                    \
        y ^= 0x9908B0DF & -(x & 1);     \
        mt_state[i] = mt_state[k] ^ y;  \
    } while (0)

        for (i = 0; i < N - M; i++)
            STEP(i + 1, i + M);
        for (     ; i < N - 1; i++)
            STEP(i + 1, i - N + M);
        STEP(0, M - 1);
    }

    y = mt_state[mt_index++];
    y ^= y >> 11;
    y ^= y <<  7 & 0x9D2C5680;
    y ^= y << 15 & 0xEFC60000;
    y ^= y >> 18;

    return y;
}

/*
==================
Q_rand_uniform

Generate random integer in range [0, n) avoiding modulo bias
==================
*/
uint32_t Q_rand_uniform(uint32_t n)
{
    uint32_t r, m;

    if (n < 2)
        return 0;

    m = -n % n; // m = 2^32 mod n
    do {
        r = Q_rand();
    } while (r < m);

    return r % n;
}

/*
=====================================================================

  INFO STRINGS

=====================================================================
*/

/*
===============
Info_ValueForKey

Searches the string for the given
key and returns the associated value, or an empty string.
===============
*/
char *Info_ValueForKey(const char *s, const char *key)
{
    // use 4 buffers so compares work without stomping on each other
    static char value[4][MAX_INFO_STRING];
    static int  valueindex;
    char        pkey[MAX_INFO_STRING];
    char        *o;

    valueindex = (valueindex + 1) & 3;
    if (*s == '\\')
        s++;
    while (1) {
        o = pkey;
        while (*s != '\\') {
            if (!*s)
                goto fail;
            *o++ = *s++;
        }
        *o = 0;
        s++;

        o = value[valueindex];
        while (*s != '\\' && *s) {
            *o++ = *s++;
        }
        *o = 0;

        if (!strcmp(key, pkey))
            return value[valueindex];

        if (!*s)
            goto fail;
        s++;
    }

fail:
    o = value[valueindex];
    *o = 0;
    return o;
}

/*
==================
Info_RemoveKey
==================
*/
bool Info_RemoveKey(char *s, const char *key)
{
    bool    found_one = false;
    char    *start;
    char    pkey[MAX_INFO_STRING];
    char    *o;

    while (1) {
        start = s;
        if (*s == '\\')
            s++;
        o = pkey;
        while (*s != '\\') {
            if (!*s)
                return found_one;
            *o++ = *s++;
        }
        *o = 0;
        s++;

        while (*s != '\\' && *s) {
            s++;
        }

        if (!strcmp(key, pkey)) {
            o = start; // remove this part
            while (*s) {
                *o++ = *s++;
            }
            *o = 0;
            s = start;
            found_one = true;
            continue; // search for duplicates
        }

        if (!*s)
            return found_one;
    }

    return found_one;
}


/*
==================
Info_Validate

Some characters are illegal in info strings because they
can mess up the server's parsing.
Also checks the length of keys/values and the whole string.
==================
*/
bool Info_Validate(const char *s)
{
    size_t len, total;
    int c;

    total = 0;
    while (1) {
        //
        // validate key
        //
        if (*s == '\\') {
            s++;
            if (++total == MAX_INFO_STRING) {
                return false;   // oversize infostring
            }
        }
        if (!*s) {
            return false;   // missing key
        }
        len = 0;
        while (*s != '\\') {
            c = *s++;
            if (!Q_isprint(c) || c == '\"' || c == ';') {
                return false;   // illegal characters
            }
            if (++len == MAX_INFO_KEY) {
                return false;   // oversize key
            }
            if (++total == MAX_INFO_STRING) {
                return false;   // oversize infostring
            }
            if (!*s) {
                return false;   // missing value
            }
        }

        //
        // validate value
        //
        s++;
        if (++total == MAX_INFO_STRING) {
            return false;   // oversize infostring
        }
        if (!*s) {
            return false;   // missing value
        }
        len = 0;
        while (*s != '\\') {
            c = *s++;
            if (!Q_isprint(c) || c == '\"' || c == ';') {
                return false;   // illegal characters
            }
            if (++len == MAX_INFO_VALUE) {
                return false;   // oversize value
            }
            if (++total == MAX_INFO_STRING) {
                return false;   // oversize infostring
            }
            if (!*s) {
                return true;    // end of string
            }
        }
    }

    return false; // quiet compiler warning
}

/*
============
Info_SubValidate
============
*/
size_t Info_SubValidate(const char *s)
{
    size_t len;
    int c;

    len = 0;
    while (*s) {
        c = *s++;
        c &= 127;       // strip high bits
        if (c == '\\' || c == '\"' || c == ';') {
            return SIZE_MAX;  // illegal characters
        }
        if (++len == MAX_QPATH) {
            return MAX_QPATH;  // oversize value
        }
    }

    return len;
}

/*
==================
Info_SetValueForKey
==================
*/
bool Info_SetValueForKey(char *s, const char *key, const char *value)
{
    char    newi[MAX_INFO_STRING], *v;
    size_t  l, kl, vl;
    int     c;

    // validate key
    kl = Info_SubValidate(key);
    if (kl >= MAX_QPATH) {
        return false;
    }

    // validate value
    vl = Info_SubValidate(value);
    if (vl >= MAX_QPATH) {
        return false;
    }

    Info_RemoveKey(s, key);
    if (!vl) {
        return true;
    }

    l = strlen(s);
    if (l + kl + vl + 2 >= MAX_INFO_STRING) {
        return false;
    }

    newi[0] = '\\';
    memcpy(newi + 1, key, kl);
    newi[kl + 1] = '\\';
    memcpy(newi + kl + 2, value, vl + 1);

    // only copy ascii values
    s += l;
    v = newi;
    while (*v) {
        c = *v++;
        c &= 127;        // strip high bits
        if (Q_isprint(c))
            *s++ = c;
    }
    *s = 0;

    return true;
}

/*
==================
Info_NextPair
==================
*/
void Info_NextPair(const char **string, char *key, char *value)
{
    char        *o;
    const char  *s;

    *value = *key = 0;

    s = *string;
    if (!s) {
        return;
    }

    if (*s == '\\')
        s++;

    if (!*s) {
        *string = NULL;
        return;
    }

    o = key;
    while (*s && *s != '\\') {
        *o++ = *s++;
    }
    *o = 0;

    if (!*s) {
        *string = NULL;
        return;
    }

    o = value;
    s++;
    while (*s && *s != '\\') {
        *o++ = *s++;
    }
    *o = 0;

    *string = s;
}

/*
==================
Info_Print
==================
*/
void Info_Print(const char *infostring)
{
    char    key[MAX_INFO_STRING];
    char    value[MAX_INFO_STRING];

    while (1) {
        Info_NextPair(&infostring, key, value);
        if (!infostring)
            break;

        if (!key[0])
            strcpy(key, "<MISSING KEY>");

        if (!value[0])
            strcpy(value, "<MISSING VALUE>");

        Com_Printf("%-20s %s\n", key, value);
    }
}

/*
=====================================================================

  CONFIG STRING REMAPPING

=====================================================================
*/

const cs_remap_t cs_remap_old = {
    .extended    = false,

    .max_edicts  = MAX_EDICTS_OLD,
    .max_models  = MAX_MODELS_OLD,
    .max_sounds  = MAX_SOUNDS_OLD,
    .max_images  = MAX_IMAGES_OLD,
    .max_shadowlights = 0,
    .max_wheelitems = 0,

    .airaccel    = CS_AIRACCEL_OLD,
    .maxclients  = CS_MAXCLIENTS_OLD,
    .mapchecksum = CS_MAPCHECKSUM_OLD,

    .models      = CS_MODELS_OLD,
    .sounds      = CS_SOUNDS_OLD,
    .images      = CS_IMAGES_OLD,
    .lights      = CS_LIGHTS_OLD,
    .shadowlights = -1,
    .items       = CS_ITEMS_OLD,
    .playerskins = CS_PLAYERSKINS_OLD,
    .general     = CS_GENERAL_OLD,
    .wheelweapons = -1,
    .wheelammo   = -1,
    .wheelpowerups = -1,
    .cdloopcount = -1,
    .gamestyle   = -1,

    .end         = MAX_CONFIGSTRINGS_OLD
};

const cs_remap_t cs_remap_rerelease = {
    .extended    = true,

    .max_edicts  = MAX_EDICTS,
    .max_models  = MAX_MODELS,
    .max_sounds  = MAX_SOUNDS,
    .max_images  = MAX_IMAGES,
    .max_shadowlights = MAX_SHADOW_LIGHTS,
    .max_wheelitems = MAX_WHEEL_ITEMS,

    .airaccel    = CS_AIRACCEL,
    .maxclients  = CS_MAXCLIENTS,
    .mapchecksum = CS_MAPCHECKSUM,

    .models      = CS_MODELS,
    .sounds      = CS_SOUNDS,
    .images      = CS_IMAGES,
    .lights      = CS_LIGHTS,
    .shadowlights = CS_SHADOWLIGHTS,
    .items       = CS_ITEMS,
    .playerskins = CS_PLAYERSKINS,
    .general     = CS_GENERAL,
    .wheelweapons = CS_WHEEL_WEAPONS,
    .wheelammo   = CS_WHEEL_AMMO,
    .wheelpowerups = CS_WHEEL_POWERUPS,
    .cdloopcount = CS_CD_LOOP_COUNT,
    .gamestyle   = CS_GAME_STYLE,

    .end         = MAX_CONFIGSTRINGS
};

#define MAX_IMAGES_EX           2048
#define CS_AIRACCEL_EX          CS_AIRACCEL
#define CS_MAXCLIENTS_EX        CS_MAXCLIENTS
#define CS_MAPCHECKSUM_EX       CS_MAPCHECKSUM
#define CS_MODELS_EX            CS_MODELS
#define CS_SOUNDS_EX            CS_SOUNDS
#define CS_IMAGES_EX            CS_IMAGES
#define CS_LIGHTS_EX            (CS_IMAGES_EX + MAX_IMAGES_EX)
#define CS_ITEMS_EX             (CS_LIGHTS_EX + MAX_LIGHTSTYLES)
#define CS_PLAYERSKINS_EX       (CS_ITEMS_EX + MAX_ITEMS)
#define CS_GENERAL_EX           (CS_PLAYERSKINS_EX + MAX_CLIENTS)
#define MAX_CONFIGSTRINGS_EX    (CS_GENERAL_EX + MAX_GENERAL)

const cs_remap_t cs_remap_q2pro_new = {
    .extended    = true,

    .max_edicts  = MAX_EDICTS,
    .max_models  = MAX_MODELS,
    .max_sounds  = MAX_SOUNDS,
    .max_images  = MAX_IMAGES_EX,
    .max_shadowlights = 0,
    .max_wheelitems = 0,

    .airaccel    = CS_AIRACCEL_EX,
    .maxclients  = CS_MAXCLIENTS_EX,
    .mapchecksum = CS_MAPCHECKSUM_EX,

    .models      = CS_MODELS_EX,
    .sounds      = CS_SOUNDS_EX,
    .images      = CS_IMAGES_EX,
    .lights      = CS_LIGHTS_EX,
    .shadowlights = -1,
    .items       = CS_ITEMS_EX,
    .playerskins = CS_PLAYERSKINS_EX,
    .general     = CS_GENERAL_EX,
    .wheelweapons = -1,
    .wheelammo   = -1,
    .wheelpowerups = -1,
    .cdloopcount = -1,
    .gamestyle   = -1,

    .end         = MAX_CONFIGSTRINGS_EX
};

static int index_remap(int index, int old_start, int new_start, int max_indices)
{
    int num = index - old_start;
    if (num >= max_indices)
        return -1;
    return new_start + num;
}

int remap_cs_index(int index, const cs_remap_t *from, const cs_remap_t *to)
{
    if (index < from->airaccel)
        return index; // no change
    else if (index == from->airaccel)
        return to->airaccel;
    else if (index == from->maxclients)
        return to->maxclients;
    else if (index == from->mapchecksum)
        return to->mapchecksum;
    else if (index >= from->models && index < from->models + from->max_models)
        return index_remap(index, from->models, to->models, to->max_models);
    else if (index >= from->sounds && index < from->sounds + from->max_sounds)
        return index_remap(index, from->sounds, to->sounds, to->max_sounds);
    else if (index >= from->images && index < from->images + from->max_images)
        return index_remap(index, from->images, to->images, to->max_images);
    else if (index >= from->lights && index < from->lights + MAX_LIGHTSTYLES)
        return index_remap(index, from->lights, to->lights, MAX_LIGHTSTYLES);
    else if (index >= from->shadowlights && index < from->max_shadowlights)
        return index_remap(index, from->shadowlights, to->shadowlights, to->max_shadowlights);
    else if (index >= from->items && index < from->items + MAX_ITEMS)
        return index_remap(index, from->items, to->items, MAX_ITEMS);
    else if (index >= from->playerskins && index < from->playerskins + MAX_CLIENTS)
        return index_remap(index, from->playerskins, to->playerskins, MAX_CLIENTS);
    else if (index >= from->general && index < from->playerskins + MAX_GENERAL)
        return index_remap(index, from->general, to->general, MAX_GENERAL);
    else if (index >= from->wheelweapons && index < from->wheelweapons + from->max_wheelitems)
        return index_remap(index, from->wheelweapons, to->wheelweapons, from->max_wheelitems);
    else if (index >= from->wheelammo && index < from->wheelammo + from->max_wheelitems)
        return index_remap(index, from->wheelammo, to->wheelammo, from->max_wheelitems);
    else if (index >= from->wheelpowerups && index < from->wheelpowerups + from->max_wheelitems)
        return index_remap(index, from->wheelpowerups, to->wheelpowerups, from->max_wheelitems);
    else if (index == from->cdloopcount)
        return to->cdloopcount;
    else if (index == from->gamestyle)
        return to->gamestyle;
    return -1;
}
