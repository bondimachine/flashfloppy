OBJS += lfs.o

# Upstream littlefs, built as-is: relax the warning set FlashFloppy applies
# to its own sources. lfs_util.c is not built -- LFS_CONFIG (lfs_ff_config.h)
# supplies everything it would provide.
lfs.o: CFLAGS += -Wno-declaration-after-statement -Wno-redundant-decls
lfs.o: CFLAGS += -Wno-unused-function -Wno-unused-parameter
