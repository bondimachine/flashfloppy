/*
 * vfs.h
 *
 * Filesystem-backend abstraction.
 *
 * FlashFloppy's mass storage is FatFS on a USB stick or SD card. The RP2350
 * build can additionally mount a read-only littlefs image held in the
 * internal QSPI flash, used when no SD card is present. This header presents
 * the file and directory operations the rest of the firmware needs,
 * dispatched to whichever backend is mounted.
 *
 * On builds without littlefs (HAS_LITTLEFS == 0) FS_FILE is FIL and FS_DIR
 * is DIR, and every file/directory operation is a static inline forwarding
 * to the FatFS call it wraps, so those builds behave exactly as they did
 * before. The remaining helpers are out of line in vfs.c.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#if TARGET != TARGET_bootloader
extern FATFS fatfs;
#endif

#if HAS_LITTLEFS

#include "../src/littlefs/lfs.h"

/* Per-file read cache. Must divide the littlefs block size. */
#define LFS_CACHE_SIZE 256

/* Which filesystem a file/directory handle belongs to. */
enum {
    FS_BE_fat = 0,   /* FatFS: USB stick or SD card */
    FS_BE_lfs,       /* littlefs: internal QSPI flash */
    FS_BE_dummy      /* no backing file (empty HxC slot 0) */
};

struct ff_lfs_file {
    lfs_file_t f;
    struct lfs_file_config cfg;
    uint8_t buf[LFS_CACHE_SIZE];
};

struct ff_lfs_dir {
    lfs_dir_t d;
    struct lfs_info info;
    const char *pat;   /* fs_findfirst() pattern; NULL for fs_readdir() */
};

struct v_file {
    uint8_t backend;
    union {
        FIL fat;
        struct ff_lfs_file lfs;
    } u;
};

struct v_dir {
    uint8_t backend;
    union {
        DIR fat;
        struct ff_lfs_dir lfs;
    } u;
};

#define FS_FILE struct v_file
#define FS_DIR struct v_dir
#define FS_FAT(p) (&(p)->u.fat)
#define FS_LFS(p) (&(p)->u.lfs)

/* Saved "current directory". FatFS identifies one by start cluster; littlefs
 * has no cwd concept at all, so we carry the absolute path. */
typedef struct {
    uint32_t fat;
    char path[FS_PATH_MAX];
} fs_cdir_t;

/* f_size/f_tell/f_eof are FatFS macros reaching into FIL. Redirect them
 * through the dispatcher; call sites are unchanged. */
#undef f_size
#undef f_tell
#undef f_eof
#define f_size(fp) fs_file_size(fp)
#define f_tell(fp) fs_file_tell(fp)
#define f_eof(fp) fs_file_eof(fp)

FSIZE_t fs_file_size(const FS_FILE *fp);
FSIZE_t fs_file_tell(const FS_FILE *fp);
int fs_file_eof(const FS_FILE *fp);

/* Raw operations, dispatched on the handle's backend. These return an
 * FRESULT; fs.c wraps them in the firmware's cancellation-based error
 * handling. */
FRESULT fs_open(FS_FILE *fp, const TCHAR *path, BYTE mode);
FRESULT fs_close(FS_FILE *fp);
FRESULT fs_read(FS_FILE *fp, void *buff, UINT btr, UINT *br);
FRESULT fs_write(FS_FILE *fp, const void *buff, UINT btw, UINT *bw);
FRESULT fs_sync(FS_FILE *fp);
FRESULT fs_truncate(FS_FILE *fp);
FRESULT fs_lseek(FS_FILE *fp, FSIZE_t ofs);
FRESULT fs_opendir(FS_DIR *dp, const TCHAR *path);
FRESULT fs_closedir(FS_DIR *dp);
FRESULT fs_readdir(FS_DIR *dp, FILINFO *fno);
FRESULT fs_findfirst(FS_DIR *dp, FILINFO *fno, const TCHAR *path,
                     const TCHAR *pattern);
FRESULT fs_findnext(FS_DIR *dp, FILINFO *fno);
FRESULT fs_chdir(const TCHAR *path);
FRESULT fs_stat(const TCHAR *path, FILINFO *fno);
FRESULT fs_unlink(const TCHAR *path);

#else /* !HAS_LITTLEFS */

#define FS_FILE FIL
#define FS_DIR DIR
#define FS_FAT(p) (p)

typedef struct {
    uint32_t fat;
} fs_cdir_t;

static inline FRESULT fs_open(FS_FILE *fp, const TCHAR *path, BYTE mode)
{
    return f_open(fp, path, mode);
}
static inline FRESULT fs_close(FS_FILE *fp) { return f_close(fp); }
static inline FRESULT fs_read(FS_FILE *fp, void *buff, UINT btr, UINT *br)
{
    return f_read(fp, buff, btr, br);
}
static inline FRESULT fs_write(FS_FILE *fp, const void *buff, UINT btw,
                               UINT *bw)
{
    return f_write(fp, buff, btw, bw);
}
static inline FRESULT fs_sync(FS_FILE *fp) { return f_sync(fp); }
static inline FRESULT fs_truncate(FS_FILE *fp) { return f_truncate(fp); }
static inline FRESULT fs_lseek(FS_FILE *fp, FSIZE_t ofs)
{
    return f_lseek(fp, ofs);
}
static inline FRESULT fs_opendir(FS_DIR *dp, const TCHAR *path)
{
    return f_opendir(dp, path);
}
static inline FRESULT fs_closedir(FS_DIR *dp) { return f_closedir(dp); }
static inline FRESULT fs_readdir(FS_DIR *dp, FILINFO *fno)
{
    return f_readdir(dp, fno);
}
static inline FRESULT fs_findfirst(FS_DIR *dp, FILINFO *fno, const TCHAR *path,
                                   const TCHAR *pattern)
{
    return f_findfirst(dp, fno, path, pattern);
}
static inline FRESULT fs_findnext(FS_DIR *dp, FILINFO *fno)
{
    return f_findnext(dp, fno);
}
static inline FRESULT fs_chdir(const TCHAR *path) { return f_chdir(path); }
static inline FRESULT fs_stat(const TCHAR *path, FILINFO *fno)
{
    return f_stat(path, fno);
}
static inline FRESULT fs_unlink(const TCHAR *path) { return f_unlink(path); }

#endif /* HAS_LITTLEFS */

/* TRUE if the mounted volume is the internal-flash littlefs image. */
#if HAS_LITTLEFS
bool_t fs_is_lfs(void);
#else
static inline bool_t fs_is_lfs(void) { return FALSE; }
#endif

/* Mount the first available volume: USB stick, then SD card, then (RP2350
 * only) the internal-flash littlefs image. Returns FR_OK on success. */
FRESULT fs_mount(void);

/* Volume free and total size, in MB. */
void fs_volume_space(uint32_t *p_free, uint32_t *p_total);

/* Close an open handle, if the backend has one to close. Safe on a handle
 * that was merely zeroed. */
#if HAS_LITTLEFS
void fs_release(FS_FILE *fp);
#else
static inline void fs_release(FS_FILE *fp) { (void)fp; }
#endif

/* Drop every open littlefs handle without closing it. Called from
 * arena_init(), where the memory holding open handles is recycled. */
#if HAS_LITTLEFS
void fs_release_all(void);
#else
static inline void fs_release_all(void) { }
#endif

/* Save and restore the current directory. */
void fs_getcdir(fs_cdir_t *cdir);
void fs_setcdir(const fs_cdir_t *cdir);

/* Slot <-> open file. A slot records enough to re-open an image file without
 * walking the directory tree again (FatFS: start cluster and dirent address;
 * littlefs: the absolute path). */
void fs_from_slot(FS_FILE *file, const struct slot *slot, BYTE mode);
void fs_to_slot(struct slot *slot, FS_FILE *file, const char *name);

/* Populate a slot from a directory entry in the current directory, as cached
 * by native_read_and_sort_dir(). Returns FALSE if it can no longer be found,
 * TRUE otherwise, with *p_attrib set to the entry's attributes. */
bool_t fs_slot_from_dirent(struct slot *slot, FS_FILE *file, const char *name,
                           uint32_t dir_sect, uint32_t dir_off,
                           uint8_t *p_attrib);

/* Fill in the path of a slot naming a file in directory `dir`. Short slots
 * (IMG.CFG, HXCSDFE.CFG, AUTOBOOT.HFE, HxC slot records) carry no path of
 * their own, so the caller says which directory the file lives in. */
void fs_short_slot_path(struct slot *slot, const fs_cdir_t *dir);

/* Stamp a FatFS directory attribute onto a file opened by name (f_open()
 * does not fill it in). A no-op on littlefs, where fs_to_slot() takes the
 * attributes from the read-only volume instead. */
void fs_file_set_attr(FS_FILE *fp, uint8_t attr);

/* Present `fp` as an open, empty, read-only file. Used when a caller asks
 * for a file to be created on a volume that cannot be written to. */
void fs_open_null(FS_FILE *fp);

/* TRUE if this is an empty slot with no backing file (HxC slot 0). */
bool_t fs_file_is_dummy(const FS_FILE *fp);

/* TRUE if the file's directory entry may still be updated, i.e. the file can
 * be extended. Cleared by fs_file_freeze() once an image is mounted. */
bool_t fs_file_resizable(const FS_FILE *fp);
void fs_file_freeze(FS_FILE *fp);

/* FatFS fast seek: build a cluster link map for the open file. Returns FALSE
 * if unavailable (not enough memory, or a backend with no cluster chains),
 * in which case the caller keeps no cluster table. */
bool_t fs_fastseek_init(FS_FILE *fp, DWORD *cltbl);
void fs_set_cltbl(FS_FILE *fp, DWORD *cltbl);

#if LEVEL == LEVEL_logfile
/* Logfile management (see logfile.c). */
void logfile_flush(FS_FILE *file);
#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
