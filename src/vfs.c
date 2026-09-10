/*
 * vfs.c
 *
 * Filesystem-backend abstraction: FatFS on removable media, and (RP2350
 * only) a read-only littlefs image in the internal QSPI flash.
 *
 * The littlefs volume is what a Pico 2 falls back to when no SD card is
 * fitted. It is mounted read-only: littlefs is copy-on-write, so writing a
 * sector of a disk image would rewrite whole 4kB flash blocks, and the flash
 * is soldered down. Writes are refused here and, more usefully, the volume
 * reports itself write-protected (volume_readonly()) so the layers above
 * drop writes exactly as they do for a write-protected SD card.
 *
 * Two differences from FatFS shape the code below:
 *  - littlefs has no notion of a current directory. We keep an absolute cwd
 *    string and build absolute paths from it.
 *  - littlefs filenames are case-sensitive, where FAT's are not. Paths are
 *    resolved case-insensitively so that FF.CFG, HXCSDFE.CFG and image
 *    filenames behave the way they do on an SD card.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

FATFS fatfs;

#if !HAS_LITTLEFS

FRESULT fs_mount(void)
{
    return f_mount(&fatfs, "", 1);
}

void fs_getcdir(fs_cdir_t *cdir) { cdir->fat = fatfs.cdir; }
void fs_setcdir(const fs_cdir_t *cdir) { fatfs.cdir = cdir->fat; }
void fs_short_slot_path(struct slot *slot, const fs_cdir_t *dir)
{
    (void)slot; (void)dir;
}

#else /* HAS_LITTLEFS */

/*
 * Internal-flash littlefs partition.
 *
 * The firmware image and its FF.CFG sector live below 1MB (see
 * scripts/rp2350.ld.S and flash_cfg.c); everything above is the image store.
 * block_count is left zero in the config so that the volume size is taken
 * from the superblock: an image smaller than the partition mounts fine.
 */
#define LFS_PART_OFF  0x00100000
#define LFS_PART_END  0x00400000 /* all Pico 2 boards carry 4MB of QSPI flash */
#define LFS_BLOCK_SIZE 4096      /* one flash sector */

static lfs_t lfs;
static bool_t lfs_mounted;

static uint8_t lfs_rbuf[LFS_CACHE_SIZE];
static uint8_t lfs_pbuf[LFS_CACHE_SIZE];
static uint8_t lfs_lookbuf[32];

/* Absolute current directory. */
static char lfs_cwd[FS_PATH_MAX] = "/";

/* Scratch for case-insensitive path resolution. Too big for the 1kB thread
 * stack, and resolution never nests. */
static struct {
    lfs_dir_t dir;
    struct lfs_info info;
} lfs_scan;

/* XIP reads: the firmware runs from SRAM, so flash is memory to us. */
static int lfs_bd_read(const struct lfs_config *c, lfs_block_t block,
                       lfs_off_t off, void *buffer, lfs_size_t size)
{
    uint32_t addr = LFS_PART_OFF + block * c->block_size + off;

    if ((addr < LFS_PART_OFF) || ((addr + size) > LFS_PART_END))
        return LFS_ERR_IO;

    memcpy(buffer, (const void *)(XIP_BASE + addr), size);
    return 0;
}

static const struct lfs_config lfs_cfg = {
    .read = lfs_bd_read,
    /* No prog/erase/sync: LFS_READONLY means littlefs never calls them. */
    .read_size = 1,             /* XIP: any alignment */
    .prog_size = LFS_CACHE_SIZE,
    .block_size = LFS_BLOCK_SIZE,
    .block_count = 0,           /* from the superblock */
    .block_cycles = -1,         /* no wear levelling: we never write */
    .cache_size = LFS_CACHE_SIZE,
    .lookahead_size = sizeof(lfs_lookbuf),
    .read_buffer = lfs_rbuf,
    .prog_buffer = lfs_pbuf,
    .lookahead_buffer = lfs_lookbuf,
};

static FRESULT lfs_to_fresult(int err)
{
    switch (err) {
    case LFS_ERR_OK:     return FR_OK;
    case LFS_ERR_NOENT:  return FR_NO_FILE;
    case LFS_ERR_NOTDIR: return FR_NO_PATH;
    case LFS_ERR_EXIST:  return FR_EXIST;
    case LFS_ERR_ISDIR:  return FR_DENIED;
    case LFS_ERR_NOSPC:  return FR_DISK_FULL;
    case LFS_ERR_NOMEM:  return FR_NOT_ENOUGH_CORE;
    case LFS_ERR_NAMETOOLONG: return FR_INVALID_NAME;
    case LFS_ERR_INVAL:  return FR_INVALID_PARAMETER;
    case LFS_ERR_CORRUPT:
    case LFS_ERR_IO:     return FR_DISK_ERR;
    default:             return FR_INT_ERR;
    }
}

bool_t fs_is_lfs(void)
{
    return lfs_mounted;
}

/* Build an absolute path for `rel` relative to `base`. */
static FRESULT lfs_abspath(char *dst, const char *base, const char *rel)
{
    int len;

    if ((rel == NULL) || (*rel == '\0')) {
        len = snprintf(dst, FS_PATH_MAX, "%s", base);
    } else if (*rel == '/') {
        len = snprintf(dst, FS_PATH_MAX, "%s", rel);
    } else if (!strcmp(rel, ".")) {
        len = snprintf(dst, FS_PATH_MAX, "%s", base);
    } else if (!strcmp(rel, "..")) {
        char *p;
        len = snprintf(dst, FS_PATH_MAX, "%s", base);
        if ((len < FS_PATH_MAX) && ((p = strrchr(dst, '/')) != NULL))
            *(p == dst ? p+1 : p) = '\0';
    } else {
        len = snprintf(dst, FS_PATH_MAX, "%s%s%s", base,
                       base[1] == '\0' ? "" : "/", rel);
    }

    return (len >= FS_PATH_MAX) ? FR_INVALID_NAME : FR_OK;
}

/* littlefs filenames are case-sensitive; FAT's are not, and FlashFloppy and
 * its config files assume they are not. Walk `path` component by component,
 * replacing any component that does not exist verbatim with the entry that
 * matches it ignoring case. Rewrites in place: a case-insensitive match is
 * always the same length. */
static void lfs_resolve_ci(char *path)
{
    char *seg = path;
    struct lfs_info *info = &lfs_scan.info;

    for (;;) {
        char *end, sep;
        int err;

        while (*seg == '/')
            seg++;
        if (*seg == '\0')
            return;
        for (end = seg; *end && (*end != '/'); end++)
            continue;
        sep = *end;
        *end = '\0';

        if (lfs_stat(&lfs, path, info) < 0) {
            /* No exact match: scan the parent directory. */
            char save = seg[-1];
            seg[-1] = '\0';
            err = lfs_dir_open(&lfs, &lfs_scan.dir,
                               (seg-1 == path) ? "/" : path);
            seg[-1] = save;
            if (err >= 0) {
                while (lfs_dir_read(&lfs, &lfs_scan.dir, info) > 0) {
                    if (!strcmp_ci(info->name, seg)) {
                        memcpy(seg, info->name, strlen(seg));
                        break;
                    }
                }
                lfs_dir_close(&lfs, &lfs_scan.dir);
            }
        }

        *end = sep;
        if (sep == '\0')
            return;
        seg = end;
    }
}

/* Resolve `rel` against the current directory into `dst`. */
static FRESULT lfs_path(char *dst, const char *rel)
{
    FRESULT fr = lfs_abspath(dst, lfs_cwd, rel);
    if (fr == FR_OK)
        lfs_resolve_ci(dst);
    return fr;
}

static void lfs_to_filinfo(FILINFO *fno, const struct lfs_info *info)
{
    memset(fno, 0, sizeof(*fno));
    fno->fsize = info->size;
    fno->fattrib = ((info->type == LFS_TYPE_DIR) ? AM_DIR : 0) | AM_RDO;
    snprintf(fno->fname, sizeof(fno->fname), "%s", info->name);
}

/* Read the next real entry (littlefs synthesises "." and ".."; FatFS does
 * not return them, and nor do we). */
static int lfs_next_entry(struct ff_lfs_dir *d)
{
    int res;
    while ((res = lfs_dir_read(&lfs, &d->d, &d->info)) > 0) {
        if (strcmp(d->info.name, ".") && strcmp(d->info.name, ".."))
            break;
    }
    return res;
}

/* FatFS-style glob: '*' matches any run of characters, '?' matches one.
 * Case-insensitive, as FatFS's matcher is. */
static bool_t lfs_pat_match(const char *pat, const char *nam)
{
    const char *star = NULL, *ss = nam;

    while (*nam != '\0') {
        if ((*pat == '?') || (tolower(*pat) == tolower(*nam))) {
            pat++; nam++;
        } else if (*pat == '*') {
            star = pat++;
            ss = nam;
        } else if (star != NULL) {
            pat = star + 1;
            nam = ++ss;
        } else {
            return FALSE;
        }
    }

    while (*pat == '*')
        pat++;
    return *pat == '\0';
}

FRESULT fs_mount(void)
{
    FRESULT fr;

    fs_release_all();
    lfs_mounted = FALSE;

    /* Removable media first: an inserted SD card (or USB stick, on boards
     * that have one) takes precedence over the internal image store. */
    fr = f_mount(&fatfs, "", 1);
    if (fr == FR_OK)
        return fr;

    if (lfs_mount(&lfs, &lfs_cfg) < 0)
        return fr; /* report the removable-media error, not ours */

    lfs_mounted = TRUE;
    strcpy(lfs_cwd, "/");
    printk("Volume: littlefs in QSPI flash (read-only)\n");

    return FR_OK;
}

/* Drop every open littlefs handle. Called from arena_init(), the one place
 * where the memory holding open handles is recycled: with the volume
 * read-only there is nothing to flush, and this keeps littlefs's list of
 * open files from retaining pointers into freed memory. */
void fs_release_all(void)
{
    lfs.mlist = NULL;
}

void fs_release(FS_FILE *fp)
{
    if (fp->backend != FS_BE_lfs)
        return;
    if (lfs_mounted)
        lfs_file_close(&lfs, &FS_LFS(fp)->f);
    fp->backend = FS_BE_fat;
}

FSIZE_t fs_file_size(const FS_FILE *fp)
{
    switch (fp->backend) {
    case FS_BE_lfs:
        return ((struct ff_lfs_file *)&fp->u.lfs)->f.ctz.size;
    case FS_BE_dummy:
        return 0;
    default:
        return fp->u.fat.obj.objsize;
    }
}

FSIZE_t fs_file_tell(const FS_FILE *fp)
{
    switch (fp->backend) {
    case FS_BE_lfs:
        return ((struct ff_lfs_file *)&fp->u.lfs)->f.pos;
    case FS_BE_dummy:
        return 0;
    default:
        return fp->u.fat.fptr;
    }
}

int fs_file_eof(const FS_FILE *fp)
{
    return fs_file_tell(fp) == fs_file_size(fp);
}

FRESULT fs_open(FS_FILE *fp, const TCHAR *path, BYTE mode)
{
    char abspath[FS_PATH_MAX];
    struct ff_lfs_file *f;
    FRESULT fr;
    int err;

    if (!lfs_mounted) {
        fp->backend = FS_BE_fat;
        return f_open(FS_FAT(fp), path, mode);
    }

    /* Release any handle this object already holds (image_open() retries a
     * slot against several handlers with the same FS_FILE). */
    fs_release(fp);

    if (mode & (FA_WRITE|FA_CREATE_ALWAYS|FA_CREATE_NEW|FA_OPEN_APPEND))
        return FR_DENIED;

    if ((fr = lfs_path(abspath, path)) != FR_OK)
        return fr;

    f = FS_LFS(fp);
    memset(f, 0, sizeof(*f));
    f->cfg.buffer = f->buf;

    err = lfs_file_opencfg(&lfs, &f->f, abspath, LFS_O_RDONLY, &f->cfg);
    if (err < 0)
        return lfs_to_fresult(err);

    fp->backend = FS_BE_lfs;
    return FR_OK;
}

FRESULT fs_close(FS_FILE *fp)
{
    if (fp->backend == FS_BE_dummy)
        return FR_OK;
    if (fp->backend != FS_BE_lfs)
        return f_close(FS_FAT(fp));
    fs_release(fp);
    return FR_OK;
}

FRESULT fs_read(FS_FILE *fp, void *buff, UINT btr, UINT *br)
{
    lfs_ssize_t nr;

    if (fp->backend == FS_BE_dummy) {
        *br = 0;
        return FR_OK;
    }
    if (fp->backend != FS_BE_lfs)
        return f_read(FS_FAT(fp), buff, btr, br);

    nr = lfs_file_read(&lfs, &FS_LFS(fp)->f, buff, btr);
    if (nr < 0) {
        *br = 0;
        return lfs_to_fresult(nr);
    }

    *br = nr;
    return FR_OK;
}

FRESULT fs_write(FS_FILE *fp, const void *buff, UINT btw, UINT *bw)
{
    if ((fp->backend == FS_BE_lfs) || (fp->backend == FS_BE_dummy)) {
        *bw = 0;
        return FR_DENIED;
    }
    return f_write(FS_FAT(fp), buff, btw, bw);
}

FRESULT fs_sync(FS_FILE *fp)
{
    if ((fp->backend == FS_BE_lfs) || (fp->backend == FS_BE_dummy))
        return FR_OK;
    return f_sync(FS_FAT(fp));
}

FRESULT fs_truncate(FS_FILE *fp)
{
    if ((fp->backend == FS_BE_lfs) || (fp->backend == FS_BE_dummy))
        return FR_DENIED;
    return f_truncate(FS_FAT(fp));
}

FRESULT fs_lseek(FS_FILE *fp, FSIZE_t ofs)
{
    lfs_soff_t pos;

    if (fp->backend == FS_BE_dummy)
        return FR_OK;
    if (fp->backend != FS_BE_lfs)
        return f_lseek(FS_FAT(fp), ofs);

    pos = lfs_file_seek(&lfs, &FS_LFS(fp)->f, ofs, LFS_SEEK_SET);
    return (pos < 0) ? lfs_to_fresult(pos) : FR_OK;
}

FRESULT fs_opendir(FS_DIR *dp, const TCHAR *path)
{
    char abspath[FS_PATH_MAX];
    FRESULT fr;
    int err;

    if (!lfs_mounted) {
        dp->backend = FS_BE_fat;
        return f_opendir(FS_FAT(dp), path);
    }

    /* Not every caller closes its directory (f_closedir() is a no-op in this
     * FatFS configuration). Release first so we cannot leak a handle. */
    if (dp->backend == FS_BE_lfs) {
        lfs_dir_close(&lfs, &FS_LFS(dp)->d);
        dp->backend = FS_BE_fat;
    }

    if ((fr = lfs_path(abspath, path)) != FR_OK)
        return fr;

    memset(FS_LFS(dp), 0, sizeof(struct ff_lfs_dir));
    err = lfs_dir_open(&lfs, &FS_LFS(dp)->d, abspath);
    if (err < 0)
        return lfs_to_fresult(err);

    dp->backend = FS_BE_lfs;
    return FR_OK;
}

FRESULT fs_closedir(FS_DIR *dp)
{
    int err;

    if (dp->backend != FS_BE_lfs)
        return f_closedir(FS_FAT(dp));

    err = lfs_dir_close(&lfs, &FS_LFS(dp)->d);
    dp->backend = FS_BE_fat;
    return lfs_to_fresult(err);
}

FRESULT fs_readdir(FS_DIR *dp, FILINFO *fno)
{
    struct ff_lfs_dir *d;
    int res;

    if (dp->backend != FS_BE_lfs)
        return f_readdir(FS_FAT(dp), fno);

    d = FS_LFS(dp);
    for (;;) {
        res = lfs_next_entry(d);
        if (res < 0)
            return lfs_to_fresult(res);
        if (res == 0) {
            /* End of directory: FatFS signals this with an empty name. */
            memset(fno, 0, sizeof(*fno));
            return FR_OK;
        }
        if ((d->pat == NULL) || lfs_pat_match(d->pat, d->info.name))
            break;
    }

    lfs_to_filinfo(fno, &d->info);
    return FR_OK;
}

FRESULT fs_findfirst(FS_DIR *dp, FILINFO *fno, const TCHAR *path,
                     const TCHAR *pattern)
{
    FRESULT fr;

    if (!lfs_mounted) {
        dp->backend = FS_BE_fat;
        return f_findfirst(FS_FAT(dp), fno, path, pattern);
    }

    if ((fr = fs_opendir(dp, path)) != FR_OK)
        return fr;

    /* As FatFS does, we keep the caller's pattern by reference. */
    FS_LFS(dp)->pat = pattern;
    return fs_readdir(dp, fno);
}

FRESULT fs_findnext(FS_DIR *dp, FILINFO *fno)
{
    if (dp->backend != FS_BE_lfs)
        return f_findnext(FS_FAT(dp), fno);
    return fs_readdir(dp, fno);
}

FRESULT fs_chdir(const TCHAR *path)
{
    char abspath[FS_PATH_MAX];
    struct lfs_info *info = &lfs_scan.info;
    FRESULT fr;

    if (!lfs_mounted)
        return f_chdir(path);

    if ((fr = lfs_path(abspath, path)) != FR_OK)
        return fr;

    if (lfs_stat(&lfs, abspath, info) < 0)
        return FR_NO_PATH;
    if (info->type != LFS_TYPE_DIR)
        return FR_NO_PATH;

    strcpy(lfs_cwd, abspath);
    return FR_OK;
}

FRESULT fs_stat(const TCHAR *path, FILINFO *fno)
{
    char abspath[FS_PATH_MAX];
    struct lfs_info *info = &lfs_scan.info;
    FRESULT fr;
    int err;

    if (!lfs_mounted)
        return f_stat(path, fno);

    if ((fr = lfs_path(abspath, path)) != FR_OK)
        return fr;

    if ((err = lfs_stat(&lfs, abspath, info)) < 0)
        return lfs_to_fresult(err);

    lfs_to_filinfo(fno, info);
    return FR_OK;
}

FRESULT fs_unlink(const TCHAR *path)
{
    if (!lfs_mounted)
        return f_unlink(path);
    return FR_DENIED;
}

void fs_getcdir(fs_cdir_t *cdir)
{
    cdir->fat = fatfs.cdir;
    strcpy(cdir->path, lfs_cwd);
}

void fs_setcdir(const fs_cdir_t *cdir)
{
    fatfs.cdir = cdir->fat;
    strcpy(lfs_cwd, cdir->path);
}

void fs_short_slot_path(struct slot *slot, const fs_cdir_t *dir)
{
    if (!lfs_mounted)
        return;
    snprintf(slot->path, sizeof(slot->path), "%s%s%s%s%s", dir->path,
             dir->path[1] == '\0' ? "" : "/", slot->name,
             slot->type[0] ? "." : "", slot->type);
}

#endif /* HAS_LITTLEFS */

/*
 * Backend-independent helpers.
 */

void fs_volume_space(uint32_t *p_free, uint32_t *p_total)
{
#if HAS_LITTLEFS
    if (fs_is_lfs()) {
        /* Read-only: report the volume size and no free-space figure. */
        *p_free = *p_total =
            (lfs.block_count * (LFS_BLOCK_SIZE/1024)) / 1024 + 100/2;
        return;
    }
#endif
    *p_total = (fatfs.n_fatent*fatfs.csize+1953/2)/1953 + 100/2;
    *p_free = (fatfs.free_clst < (fatfs.n_fatent-2))
        ? (fatfs.free_clst*fatfs.csize+1953/2)/1953 + 100/2
        : *p_total;
}

void fs_from_slot(FS_FILE *file, const struct slot *slot, BYTE mode)
{
#if HAS_LITTLEFS
    if (fs_is_lfs()) {
        FRESULT fr;
        fs_release(file);
        if ((slot->firstCluster == ~0u) && (slot->size == 0)) {
            /* Dummy slot: no backing file (empty HxC slot 0). */
            memset(file, 0, sizeof(*file));
            file->backend = FS_BE_dummy;
            return;
        }
        fr = fs_open(file, slot->path, mode & ~(FA_WRITE|FA_OPEN_ALWAYS));
        if (fr != FR_OK)
            F_die(fr);
        return;
    }
#endif

    memset(file, 0, sizeof(*file));
    FS_FAT(file)->obj.fs = &fatfs;
    FS_FAT(file)->obj.id = fatfs.id;
    FS_FAT(file)->obj.attr = slot->attributes;
    FS_FAT(file)->obj.sclust = slot->firstCluster;
    FS_FAT(file)->obj.objsize = slot->size;
    FS_FAT(file)->flag = mode;
    FS_FAT(file)->dir_sect = slot->dir_sect;
    FS_FAT(file)->dir_ptr = (void *)slot->dir_ptr;
}

/* Split `name` into the slot's display name and lower-cased extension. */
static void slot_set_name(struct slot *slot, const char *name)
{
    unsigned int i;
    char *dot;

    snprintf(slot->name, sizeof(slot->name), "%s", name);
    if ((dot = strrchr(slot->name, '.')) != NULL) {
        snprintf(slot->type, sizeof(slot->type), "%s", dot+1);
        for (i = 0; i < sizeof(slot->type); i++)
            slot->type[i] = tolower(slot->type[i]);
        *dot = '\0';
    } else {
        memset(slot->type, 0, sizeof(slot->type));
    }
}

void fs_to_slot(struct slot *slot, FS_FILE *file, const char *name)
{
#if HAS_LITTLEFS
    if (fs_is_lfs()) {
        slot->attributes = AM_RDO;
        slot->firstCluster = 0;
        slot->size = f_size(file);
        slot->dir_sect = slot->dir_ptr = 0;
        snprintf(slot->path, sizeof(slot->path), "%s%s%s", lfs_cwd,
                 lfs_cwd[1] == '\0' ? "" : "/", name);
        slot_set_name(slot, name);
        return;
    }
#endif

    slot->attributes = FS_FAT(file)->obj.attr;
    slot->firstCluster = FS_FAT(file)->obj.sclust;
    slot->size = FS_FAT(file)->obj.objsize;
    slot->dir_sect = FS_FAT(file)->dir_sect;
    slot->dir_ptr = (uint32_t)FS_FAT(file)->dir_ptr;
    slot_set_name(slot, name);
}

/* Hack inside the guts of FatFS. */
void flashfloppy_fill_fileinfo(FIL *fp);

bool_t fs_slot_from_dirent(struct slot *slot, FS_FILE *file, const char *name,
                           uint32_t dir_sect, uint32_t dir_off,
                           uint8_t *p_attrib)
{
#if HAS_LITTLEFS
    if (fs_is_lfs()) {
        struct lfs_info *info = &lfs_scan.info;
        char abspath[FS_PATH_MAX];
        if (lfs_abspath(abspath, lfs_cwd, name) != FR_OK)
            return FALSE;
        if (lfs_stat(&lfs, abspath, info) < 0)
            return FALSE;
        *p_attrib = (info->type == LFS_TYPE_DIR) ? AM_DIR : AM_RDO;
        if (*p_attrib & AM_DIR)
            return TRUE;
        slot->attributes = AM_RDO;
        slot->firstCluster = 0;
        slot->size = info->size;
        slot->dir_sect = slot->dir_ptr = 0;
        strcpy(slot->path, abspath);
        slot_set_name(slot, name);
        return TRUE;
    }
#endif

    memset(file, 0, sizeof(*file));
    FS_FAT(file)->obj.fs = &fatfs;
    FS_FAT(file)->dir_sect = dir_sect;
    FS_FAT(file)->dir_ptr = fatfs.win + dir_off;
    flashfloppy_fill_fileinfo(FS_FAT(file));
    *p_attrib = FS_FAT(file)->obj.attr;
    if (*p_attrib & AM_DIR)
        return TRUE;
    fs_to_slot(slot, file, name);
    return TRUE;
}

void fs_open_null(FS_FILE *fp)
{
    memset(fp, 0, sizeof(*fp));
#if HAS_LITTLEFS
    if (fs_is_lfs()) {
        fp->backend = FS_BE_dummy;
        return;
    }
#endif
    /* A zero-length file rooted at the mounted volume: reads return nothing,
     * and writes are dropped a layer above by volume_readonly(). */
    FS_FAT(fp)->obj.fs = &fatfs;
    FS_FAT(fp)->obj.id = fatfs.id;
    FS_FAT(fp)->flag = FA_READ;
}

void fs_file_set_attr(FS_FILE *fp, uint8_t attr)
{
#if HAS_LITTLEFS
    if (fp->backend != FS_BE_fat)
        return;
#endif
    FS_FAT(fp)->obj.attr = attr;
}

bool_t fs_file_is_dummy(const FS_FILE *fp)
{
#if HAS_LITTLEFS
    if (fp->backend == FS_BE_dummy)
        return TRUE;
    if (fp->backend == FS_BE_lfs)
        return FALSE;
#endif
    return (FS_FAT((FS_FILE *)fp)->obj.sclust == ~0u) && (f_size(fp) == 0);
}

bool_t fs_file_resizable(const FS_FILE *fp)
{
#if HAS_LITTLEFS
    if (fp->backend != FS_BE_fat)
        return FALSE;
#endif
    return FS_FAT((FS_FILE *)fp)->dir_ptr != NULL;
}

void fs_file_freeze(FS_FILE *fp)
{
#if HAS_LITTLEFS
    if (fp->backend != FS_BE_fat)
        return;
#endif
    FS_FAT(fp)->dir_ptr = NULL;
    FS_FAT(fp)->dir_sect = 0;
}

void fs_set_cltbl(FS_FILE *fp, DWORD *cltbl)
{
#if HAS_LITTLEFS
    if (fp->backend != FS_BE_fat)
        return;
#endif
    FS_FAT(fp)->cltbl = cltbl;
}

bool_t fs_fastseek_init(FS_FILE *fp, DWORD *cltbl)
{
    FRESULT fr;

#if HAS_LITTLEFS
    /* littlefs files are addressed by a CTZ skip list: seeking is already
     * cheap, and there is no cluster chain to flatten. */
    if (fp->backend != FS_BE_fat)
        return FALSE;
#endif

    FS_FAT(fp)->cltbl = cltbl;
    fr = f_lseek(FS_FAT(fp), CREATE_LINKMAP);
    printk("Fast Seek: %u frags\n", (*cltbl / 2) - 1);
    if (fr == FR_OK)
        return TRUE;
    if (fr != FR_NOT_ENOUGH_CORE)
        F_die(fr);
    printk("Fast Seek: FAILED\n");
    FS_FAT(fp)->cltbl = NULL;
    return FALSE;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "Linux"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
