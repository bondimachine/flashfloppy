# mk_littlefs.py [--size=BYTES] <srcdir> <output>
#
# Build a littlefs image from a directory tree, for the RP2350 build's
# internal-flash image store. The output is written as a raw binary and, if
# <output> ends in .uf2, as a UF2 targeting the store's flash offset so it can
# be dropped onto the RP2350 BOOTSEL drive.
#
# The geometry must match the firmware's (see src/vfs.c): 4kB blocks, 256-byte
# program size and cache. Nothing else about the host configuration is written
# to the image.
#
# Requires littlefs-python:  pip install littlefs-python
#
# This is free and unencumbered software released into the public domain.
# See the file COPYING for more details, or visit <http://unlicense.org>.

import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mk_uf2

# Must match src/vfs.c.
BLOCK_SIZE = 4096
PROG_SIZE = 256
READ_SIZE = 1
CACHE_SIZE = 256
LOOKAHEAD_SIZE = 32

# The image store starts 1MB into flash, above the firmware and its FF.CFG
# sector, and runs to the end of the 4MB QSPI flash fitted to every Pico 2.
PART_OFF = 0x00100000
PART_END = 0x00400000

XIP_BASE = 0x10000000


def add_tree(fs, srcdir):
    nr_files = nr_dirs = total = 0

    for dirpath, dirnames, filenames in os.walk(srcdir):
        dirnames.sort()
        filenames.sort()
        rel = os.path.relpath(dirpath, srcdir)
        prefix = "/" if rel == "." else "/" + rel.replace(os.sep, "/")
        if prefix != "/":
            fs.mkdir(prefix)
            nr_dirs += 1
        for name in filenames:
            if name.startswith("."):
                continue        # .DS_Store and friends
            src = os.path.join(dirpath, name)
            if not os.path.isfile(src):
                continue
            with open(src, "rb") as f:
                data = f.read()
            dst = prefix.rstrip("/") + "/" + name
            with fs.open(dst, "wb") as f:
                f.write(data)
            print("  %s (%u bytes)" % (dst, len(data)))
            nr_files += 1
            total += len(data)

    return nr_files, nr_dirs, total


def main(argv):
    size = PART_END - PART_OFF
    args = []

    for arg in argv[1:]:
        if arg.startswith("--size="):
            size = int(arg[len("--size="):], 0)
        elif arg.startswith("-"):
            args = []
            break
        else:
            args.append(arg)

    if len(args) != 2:
        print("Usage: %s [--size=BYTES] <srcdir> <output>" % argv[0])
        print("  --size=  Volume size in bytes (default %u)"
              % (PART_END - PART_OFF))
        return 1

    srcdir, output = args

    if size % BLOCK_SIZE:
        print("Error: size must be a multiple of %u" % BLOCK_SIZE)
        return 1
    if size > (PART_END - PART_OFF):
        print("Error: size exceeds the %u-byte image store"
              % (PART_END - PART_OFF))
        return 1
    if not os.path.isdir(srcdir):
        print("Error: '%s' is not a directory" % srcdir)
        return 1

    try:
        from littlefs import LittleFS
    except ImportError:
        print("Error: littlefs-python is not installed.")
        print("       pip install littlefs-python")
        return 1

    fs = LittleFS(block_size=BLOCK_SIZE,
                  block_count=size // BLOCK_SIZE,
                  read_size=READ_SIZE,
                  prog_size=PROG_SIZE,
                  cache_size=CACHE_SIZE,
                  lookahead_size=LOOKAHEAD_SIZE)

    try:
        nr_files, nr_dirs, total = add_tree(fs, srcdir)
    except Exception as e:
        print("Error: %s" % e)
        print("       (out of space? try a larger --size, or fewer files)")
        return 1

    image = bytes(fs.context.buffer)

    binfile = output
    if output.endswith(".uf2"):
        binfile = output[:-4] + ".bin"
    with open(binfile, "wb") as f:
        f.write(image)

    print("%s: %u files, %u dirs, %u bytes of data in a %u-byte volume"
          % (binfile, nr_files, nr_dirs, total, len(image)))

    if output.endswith(".uf2"):
        # The whole volume is written byte for byte: littlefs allocates
        # blocks across the volume rather than from one end, and the reader
        # must see exactly the bytes the writer produced. Pass a smaller
        # --size if the UF2 is inconveniently large.
        mk_uf2.write_uf2(image, output, XIP_BASE + PART_OFF)

    print("Flash it with:")
    print("  picotool load -o 0x%08x %s" % (XIP_BASE + PART_OFF, binfile))
    if output.endswith(".uf2"):
        print("or copy %s to the RP2350 BOOTSEL drive." % output)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
