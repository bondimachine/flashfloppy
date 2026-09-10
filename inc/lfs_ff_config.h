/*
 * lfs_ff_config.h
 *
 * LFS_CONFIG replacement for littlefs's lfs_util.h.
 *
 * FlashFloppy is built -nostdlib -ffreestanding, and already declares its
 * own memcpy/memset/&c in util.h. Pulling in <string.h> and <stdlib.h> via
 * the stock lfs_util.h would trip -Wredundant-decls, so we take littlefs's
 * documented escape hatch (-DLFS_CONFIG=lfs_ff_config.h) and supply the
 * handful of helpers it needs ourselves.
 *
 * The inline helpers below are derived from littlefs's lfs_util.h:
 *   Copyright (c) 2022, The littlefs authors.
 *   Copyright (c) 2017, Arm Limited. All rights reserved.
 *   SPDX-License-Identifier: BSD-3-Clause
 * See src/littlefs/LICENSE.md.
 */

#ifndef LFS_FF_CONFIG_H
#define LFS_FF_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* No malloc: every buffer is supplied by the caller. */
#define LFS_NO_MALLOC

/* Logging and assertions are compiled out: littlefs's messages are printf-
 * formatted with <inttypes.h> macros which we do not pull in. Errors are
 * reported to the user through the FRESULT returned by the vfs layer. */
#define LFS_TRACE(...)
#define LFS_DEBUG(...)
#define LFS_WARN(...)
#define LFS_ERROR(...)
#define LFS_ASSERT(test)

/* String helpers not provided by util.h. memcpy/memset/memcmp/strchr/strcpy
 * are declared by util.h, which decls.h includes ahead of us. */
static inline size_t lfs_ff_strspn(const char *s, const char *accept)
{
    const char *p, *a;
    for (p = s; *p != '\0'; p++) {
        for (a = accept; *a != '\0'; a++)
            if (*p == *a)
                break;
        if (*a == '\0')
            break;
    }
    return p - s;
}

static inline size_t lfs_ff_strcspn(const char *s, const char *reject)
{
    const char *p, *r;
    for (p = s; *p != '\0'; p++) {
        for (r = reject; *r != '\0'; r++)
            if (*p == *r)
                break;
        if (*r != '\0')
            break;
    }
    return p - s;
}

#define strspn(s,a) lfs_ff_strspn(s,a)
#define strcspn(s,r) lfs_ff_strcspn(s,r)

/* Builtins (verbatim from lfs_util.h, minus the portable fallbacks: we are
 * always little-endian GCC on ARM). */
static inline uint32_t lfs_max(uint32_t a, uint32_t b) { return (a > b) ? a : b; }
static inline uint32_t lfs_min(uint32_t a, uint32_t b) { return (a < b) ? a : b; }

static inline uint32_t lfs_aligndown(uint32_t a, uint32_t alignment)
{
    return a - (a % alignment);
}

static inline uint32_t lfs_alignup(uint32_t a, uint32_t alignment)
{
    return lfs_aligndown(a + alignment-1, alignment);
}

static inline uint32_t lfs_npw2(uint32_t a) { return 32 - __builtin_clz(a-1); }
static inline uint32_t lfs_ctz(uint32_t a) { return __builtin_ctz(a); }
/* __builtin_popcount() would call into libgcc, which we do not link. */
static inline uint32_t lfs_popc(uint32_t a)
{
    a = a - ((a >> 1) & 0x55555555);
    a = (a & 0x33333333) + ((a >> 2) & 0x33333333);
    return (((a + (a >> 4)) & 0xf0f0f0f) * 0x1010101) >> 24;
}

static inline int lfs_scmp(uint32_t a, uint32_t b)
{
    return (int)(unsigned)(a - b);
}

static inline uint32_t lfs_fromle32(uint32_t a) { return a; }
static inline uint32_t lfs_tole32(uint32_t a) { return a; }
static inline uint32_t lfs_frombe32(uint32_t a) { return __builtin_bswap32(a); }
static inline uint32_t lfs_tobe32(uint32_t a) { return __builtin_bswap32(a); }

/* CRC-32, polynomial 0x04c11db7 (verbatim from lfs_util.c). */
static inline uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size)
{
    static const uint32_t rtable[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *data = buffer;
    size_t i;

    for (i = 0; i < size; i++) {
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xf];
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xf];
    }

    return crc;
}

static inline void *lfs_malloc(size_t size) { (void)size; return NULL; }
static inline void lfs_free(void *p) { (void)p; }

#endif /* LFS_FF_CONFIG_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
