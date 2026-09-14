/*
 * vfs.c
 *
 * Helpers over FatFS: volume mount, slot <-> file conversion, and the odd
 * poke into FatFS internals. Every volume is FAT (USB stick, SD card, or the
 * RP2350's internal-flash image store via flash_vol.c), so the per-handle
 * operations are static inline forwards in vfs.h; only the helpers with a
 * body live here.
 *
 * This is free and unencumbered software released into the public domain.
 * See the file COPYING for more details, or visit <http://unlicense.org>.
 */

FATFS fatfs;

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

void fs_volume_space(uint32_t *p_free, uint32_t *p_total)
{
    *p_total = (fatfs.n_fatent*fatfs.csize+1953/2)/1953 + 100/2;
    *p_free = (fatfs.free_clst < (fatfs.n_fatent-2))
        ? (fatfs.free_clst*fatfs.csize+1953/2)/1953 + 100/2
        : *p_total;
}

void fs_from_slot(FS_FILE *file, const struct slot *slot, BYTE mode)
{
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

void fs_file_set_attr(FS_FILE *fp, uint8_t attr)
{
    FS_FAT(fp)->obj.attr = attr;
}

bool_t fs_file_is_dummy(const FS_FILE *fp)
{
    return (FS_FAT((FS_FILE *)fp)->obj.sclust == ~0u) && (f_size(fp) == 0);
}

bool_t fs_file_resizable(const FS_FILE *fp)
{
    return FS_FAT((FS_FILE *)fp)->dir_ptr != NULL;
}

void fs_file_freeze(FS_FILE *fp)
{
    FS_FAT(fp)->dir_ptr = NULL;
    FS_FAT(fp)->dir_sect = 0;
}

void fs_set_cltbl(FS_FILE *fp, DWORD *cltbl)
{
    FS_FAT(fp)->cltbl = cltbl;
}

bool_t fs_fastseek_init(FS_FILE *fp, DWORD *cltbl)
{
    FRESULT fr;

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
