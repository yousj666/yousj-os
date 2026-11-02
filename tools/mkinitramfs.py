#!/usr/bin/env python3
"""Build the Yousj OS initramfs (newc-format cpio archive).

Layout of the archive:
    bin/  etc/                      directories
    init                            build/yousj-init.elf
    bin/<name>                      the userland programs
    etc/motd (and any other file   tracked rootfs/ tree,
    under rootfs/)                  mirrored at its path

Deterministic: fixed mtimes/owners and sequential inodes, so
the kernel image is reproducible byte-for-byte.
"""
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = (os.path.abspath(sys.argv[1]) if len(sys.argv) > 1
         else os.path.join(REPO, "build"))
ROOTFS = os.path.join(REPO, "rootfs")
OUT = os.path.join(BUILD, "initramfs.cpio")

PROGRAMS = ["hello", "sh", "sigtest", "fxtest", "fstest",
            "cat", "pipetest", "mkdir", "ls", "rm", "rmdir", "mv", "bigfile", "mmtest", "polltest", "ln", "readlink"]

DIR_MODE = 0o040755
FILE_MODE = 0o100644
EXEC_MODE = 0o100755


def pad4(b):
    return b + b"\0" * ((4 - len(b) % 4) % 4)


class Archive:
    def __init__(self):
        self.ino = 1
        self.parts = []

    def add(self, name, mode, data=b""):
        fields = [self.ino, mode, 0, 0, 1, 0, len(data),
                  0, 0, 0, 0, len(name) + 1, 0]
        self.ino += 1
        hdr = b"070701" + b"".join(b"%08x" % f for f in fields)
        self.parts.append(pad4(hdr + name.encode() + b"\0"))
        self.parts.append(pad4(data))

    def finish(self):
        self.add("TRAILER!!!", 0)
        blob = b"".join(self.parts)
        return blob + b"\0" * ((512 - len(blob) % 512) % 512)


def main():
    arc = Archive()
    arc.add("bin", DIR_MODE)
    arc.add("etc", DIR_MODE)

    with open(os.path.join(BUILD, "yousj-init.elf"), "rb") as f:
        arc.add("init", EXEC_MODE, f.read())
    for prog in PROGRAMS:
        with open(os.path.join(BUILD, prog + ".elf"), "rb") as f:
            arc.add("bin/" + prog, EXEC_MODE, f.read())

    seen_dirs = {"bin", "etc"}
    for dirpath, dirnames, filenames in os.walk(ROOTFS):
        dirnames.sort()
        for fn in sorted(filenames):
            full = os.path.join(dirpath, fn)
            rel = os.path.relpath(full, ROOTFS).replace(os.sep, "/")
            parent = os.path.dirname(rel)
            if parent and parent not in seen_dirs:
                arc.add(parent, DIR_MODE)
                seen_dirs.add(parent)
            with open(full, "rb") as f:
                arc.add(rel, FILE_MODE, f.read())

    with open(OUT, "wb") as f:
        f.write(arc.finish())
    print("initramfs: %s (%d bytes)" % (OUT, os.path.getsize(OUT)))


if __name__ == "__main__":
    sys.exit(main())
