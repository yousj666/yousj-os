#!/usr/bin/env python3
# M80: a dependency-free ISO 9660 + El Torito (UEFI) writer.
#
# Layout (2048-byte sectors):
#   0-15   system area (zeros)
#   16     primary volume descriptor
#   17     boot record volume descriptor (El Torito)
#   18     volume descriptor set terminator
#   19     boot catalog
#   20     L path table        21  M path table
#   22     root dir            23  /EFI dir
#   24     /EFI/BOOT dir       25  /EFI/YOUSJ dir
#   26..   file extents: BOOTX64.EFI (BOOT), BOOTX64.EFI
#          (YOUSJ), YOUSJ.KRN, EFIBOOT.IMG (the FAT image
#          the El Torito entry loads)
#
# The ISO also carries the same files as plain ISO files,
# so the disc is inspectable; firmware boots through the
# El Torito entry into the FAT image, exactly like the
# raw .img from `make image`.
import argparse, struct, sys

SECT = 2048


def both16(v):
    return struct.pack('<H', v) + struct.pack('>H', v)


def both32(v):
    return struct.pack('<I', v) + struct.pack('>I', v)


def dir_record(lba, size, is_dir, name, ts7):
    nb = name if isinstance(name, bytes) else name.encode()
    rec = bytearray()
    rec.append(0)                    # length, patched below
    rec.append(0)                    # ext attr length
    rec += both32(lba)
    rec += both32(size)
    rec += ts7                       # 7-byte recording date
    rec.append(2 if is_dir else 0)   # flags
    rec.append(0)                    # file unit size
    rec.append(0)                    # interleave gap
    rec += both16(1)                 # volume sequence number
    rec.append(len(nb))
    rec += nb
    if len(nb) % 2 == 0:
        rec.append(0)
    rec[0] = len(rec)
    return bytes(rec)


def dir_sector(entries):
    out = bytearray()
    for e in entries:
        out += e
    assert len(out) <= SECT, 'directory overflows one sector'
    return bytes(out) + bytes(SECT - len(out))


def path_table(big_endian):
    # order: root, EFI, BOOT, YOUSJ (breadth-first);
    # returns the unpadded content (its length is what the
    # PVD records as the path table size)
    dirs = [(b'\x00', LBA_ROOT, 1), (b'EFI', LBA_EFI, 1),
            (b'BOOT', LBA_BOOT, 2), (b'YOUSJ', LBA_YOUSJ, 2)]
    out = bytearray()
    for name, lba, parent in dirs:
        out.append(len(name))
        out.append(0)
        if big_endian:
            out += struct.pack('>I', lba) + struct.pack('>H', parent)
        else:
            out += struct.pack('<I', lba) + struct.pack('<H', parent)
        out += name
        if len(name) % 2 == 1:
            out.append(0)
    return bytes(out)


def pvd(total_sectors, pt_size, ts17):
    v = bytearray(SECT)
    v[0] = 1
    v[1:6] = b'CD001'
    v[6] = 1
    v[8:40] = b'YOUSJ OS'.ljust(32, b' ')
    v[40:72] = VOLID.ljust(32, b' ')
    v[80:88] = both32(total_sectors)
    v[120:124] = both16(1)           # volume set size
    v[124:128] = both16(1)           # volume sequence number
    v[128:132] = both16(SECT)        # logical block size
    v[132:140] = both32(pt_size)     # path table size
    v[140:144] = struct.pack('<I', LBA_PT_L)
    v[148:152] = struct.pack('>I', LBA_PT_M)
    v[156:190] = dir_record(LBA_ROOT, SECT, True, b'\x00', TS7)
    v[574:702] = b'YOUSJ OS'.ljust(128, b' ')
    v[702:830] = b'YOUSJ OS BUILD (MKISO)'.ljust(128, b' ')
    v[813:830] = ts17                # creation date/time
    v[830:847] = ts17                # modification
    v[847:864] = b'0000000000000000\x00'  # expiration: none
    v[864:881] = ts17                # effective
    v[881] = 1                       # file structure version
    return bytes(v)


def boot_record(catalog_lba):
    v = bytearray(SECT)
    v[0] = 0
    v[1:6] = b'CD001'
    v[6] = 1
    v[7:39] = b'EL TORITO SPECIFICATION'.ljust(32, b'\x00')
    v[71:75] = struct.pack('<I', catalog_lba)
    return bytes(v)


def terminator():
    v = bytearray(SECT)
    v[0] = 255
    v[1:6] = b'CD001'
    v[6] = 1
    return bytes(v)


def boot_catalog(bootimg_lba, bootimg_bytes):
    cat = bytearray(SECT)
    val = bytearray(32)
    val[0] = 1                       # header: validation entry
    val[1] = 0xEF                    # platform: UEFI
    struct.pack_into('<H', val, 28, 0)     # checksum placeholder
    val[30] = 0x55
    val[31] = 0xAA
    s = sum(struct.unpack('<16H', bytes(val)))
    struct.pack_into('<H', val, 28, (-s) & 0xFFFF)
    cat[0:32] = val
    ent = bytearray(32)
    ent[0] = 0x88                    # bootable
    ent[1] = 0                       # no emulation
    struct.pack_into('<H', ent, 2, 0)      # load segment (default)
    ent[4] = 0xEF                    # system type: UEFI
    struct.pack_into('<H', ent, 6, bootimg_bytes // 512)
    struct.pack_into('<I', ent, 8, bootimg_lba)
    cat[32:64] = ent
    return bytes(cat)


import datetime
NOW = datetime.datetime(2026, 11, 4, 12, 0, 0)
TS7 = bytes([NOW.year - 1900, NOW.month, NOW.day, NOW.hour,
             NOW.minute, NOW.second, 0])
TS17 = NOW.strftime('%Y%m%d%H%M%S').encode() + b'00\x00'
VOLID = b'YOUSJ_0_1_0'

LBA_PVD, LBA_BR, LBA_TERM, LBA_CAT = 16, 17, 18, 19
LBA_PT_L, LBA_PT_M = 20, 21
LBA_ROOT, LBA_EFI, LBA_BOOT, LBA_YOUSJ = 22, 23, 24, 25
LBA_FILES = 26


def pad2048(data):
    return data + bytes((-len(data)) % SECT)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--efi', required=True)
    ap.add_argument('--kernel', required=True)
    ap.add_argument('--bootimg', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    efi = open(a.efi, 'rb').read()
    krn = open(a.kernel, 'rb').read()
    img = open(a.bootimg, 'rb').read()
    assert len(img) % 512 == 0 and len(img) // 512 <= 0xFFFF

    lba_efi_boot = LBA_FILES
    lba_efi_yousj = lba_efi_boot + (len(pad2048(efi)) // SECT)
    lba_krn = lba_efi_yousj + (len(pad2048(efi)) // SECT)
    lba_img = lba_krn + (len(pad2048(krn)) // SECT)
    total = lba_img + (len(pad2048(img)) // SECT)

    dot = lambda lba: dir_record(lba, SECT, True, b'\x00', TS7)
    dotdot = lambda lba: dir_record(lba, SECT, True, b'\x01', TS7)
    pt_l = path_table(False)
    pt_m = path_table(True)

    root = dir_sector([
        dot(LBA_ROOT), dotdot(LBA_ROOT),
        dir_record(LBA_EFI, SECT, True, 'EFI', TS7),
        dir_record(lba_img, len(img), False, 'EFIBOOT.IMG;1', TS7),
    ])
    efi_dir = dir_sector([
        dot(LBA_EFI), dotdot(LBA_ROOT),
        dir_record(LBA_BOOT, SECT, True, 'BOOT', TS7),
        dir_record(LBA_YOUSJ, SECT, True, 'YOUSJ', TS7),
    ])
    boot_dir = dir_sector([
        dot(LBA_BOOT), dotdot(LBA_EFI),
        dir_record(lba_efi_boot, len(efi), False,
                   'BOOTX64.EFI;1', TS7),
    ])
    yousj_dir = dir_sector([
        dot(LBA_YOUSJ), dotdot(LBA_EFI),
        dir_record(lba_efi_yousj, len(efi), False,
                   'BOOTX64.EFI;1', TS7),
        dir_record(lba_krn, len(krn), False, 'YOUSJ.KRN;1', TS7),
    ])

    out = bytearray()
    out += bytes(16 * SECT)                       # system area
    out += pvd(total, len(pt_l), TS17)
    out += boot_record(LBA_CAT)
    out += terminator()
    out += boot_catalog(lba_img, len(img))
    out += pad2048(pt_l)
    out += pad2048(pt_m)
    out += root
    out += efi_dir
    out += boot_dir
    out += yousj_dir
    out += pad2048(efi)
    out += pad2048(efi)
    out += pad2048(krn)
    out += pad2048(img)
    assert len(out) == total * SECT, (len(out), total * SECT)
    open(a.out, 'wb').write(bytes(out))
    print('ISO written: %s (%d sectors, %d bytes)' %
          (a.out, total, len(out)))
    verify(a.out, len(img))


def verify(path, img_bytes):
    d = open(path, 'rb').read()
    assert d[16 * SECT + 1:16 * SECT + 6] == b'CD001', 'no PVD'
    assert d[17 * SECT + 7:17 * SECT + 30] == \
        b'EL TORITO SPECIFICATION', 'no El Torito record'
    cat = d[19 * SECT:20 * SECT]
    assert cat[0] == 1 and cat[1] == 0xEF, 'bad validation entry'
    assert cat[30] == 0x55 and cat[31] == 0xAA, 'bad key bytes'
    assert sum(struct.unpack('<16H', cat[0:32])) % 65536 == 0, \
        'catalog checksum'
    ent = cat[32:64]
    assert ent[0] == 0x88 and ent[4] == 0xEF, 'bad boot entry'
    lba = struct.unpack('<I', ent[8:12])[0]
    cnt = struct.unpack('<H', ent[6:8])[0]
    assert cnt * 512 == img_bytes, 'boot image size mismatch'
    img = d[lba * SECT:lba * SECT + img_bytes]
    assert img[510:512] == b'\x55\xaa', 'boot image not a FAT MBR'
    print('verify OK: PVD + El Torito catalog + FAT boot image')


if __name__ == '__main__':
    main()
