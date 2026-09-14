/*
 * vfs.h
 *
 * Thin filesystem layer over FatFS.
 *
 * FlashFloppy's mass storage is FatFS on a USB stick, an SD card, or (RP2350)
 * a FAT image in the internal QSPI flash presented as a block device by
 * flash_vol.c. Every volume is FAT, so FS_FILE is FIL, FS_DIR is DIR, and
 * the file/directory operations are static inline forwards to FatFS. The
 * remaining helpers are out of line in vfs.c.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

#if TARGET != TARGET_bootloader
extern FATFS fatfs;
#endif

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

/* Mount the first available volume: USB stick, SD card, or (RP2350 only) the
 * FAT image in internal flash. Returns FR_OK on success. */
FRESULT fs_mount(void);

/* Volume free and total size, in MB. */
void fs_volume_space(uint32_t *p_free, uint32_t *p_total);

/* Save and restore the current directory. */
void fs_getcdir(fs_cdir_t *cdir);
void fs_setcdir(const fs_cdir_t *cdir);

/* Slot <-> open file. A slot records enough to re-open an image file without
 * walking the directory tree again (start cluster and dirent address). */
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
 * does not fill it in). */
void fs_file_set_attr(FS_FILE *fp, uint8_t attr);

/* TRUE if this is an empty slot with no backing file (HxC slot 0). */
bool_t fs_file_is_dummy(const FS_FILE *fp);

/* TRUE if the file's directory entry may still be updated, i.e. the file can
 * be extended. Cleared by fs_file_freeze() once an image is mounted. */
bool_t fs_file_resizable(const FS_FILE *fp);
void fs_file_freeze(FS_FILE *fp);

/* FatFS fast seek: build a cluster link map for the open file. Returns FALSE
 * if unavailable (not enough memory), in which case the caller keeps no
 * cluster table. */
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
