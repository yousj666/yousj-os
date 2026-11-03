/* Yousj OS - yfs: the on-disk filesystem (M12).
 *
 * A deliberately small write-through filesystem on the ATA
 * driver. Format version 4 (M29; the superblock carries
 * the version, and older-version disks are refused, never
 * silently reformatted). Fixed geometry (sector = 512 B):
 *
 *   sector 0        superblock ("YOUSJFS1", version 4)
 *   sectors 1..32   inode table: 128 inodes x 128 B
 *   sectors 33..40  data-block bitmap (32768 blocks max)
 *   sector 41       journal header (M19)
 *   sectors 42..89  journal payloads (48 full-sector images)
 *   sector 90..     data blocks of 4 KiB (8 sectors each)
 *
 * Inodes carry 11 direct block pointers plus one indirect
 * block (about 4 MiB max file) and, since v4, independent
 * atime/mtime/ctime fields. Directories are files whose
 * content is 32-byte entries {u32 ino; char name[28]}; an
 * entry is empty iff name[0]==0. Every mutation is written
 * to the disk synchronously (no cache): what the syscall
 * returned from is what survives a power cut. Paths here
 * are relative to the mount root ("" or "/" is the root
 * directory).
 */
#ifndef YOUSJ_YFS_H
#define YOUSJ_YFS_H

#include <yousj/types.h>

#define YFS_MAX_NAME 27

/* Probe the ATA buses, mount an existing yfs or format a
 * blank disk. Returns: 1 mounted existing, 2 formatted new,
 * 3 a yfs disk of an unsupported (older) format version --
 * refused and left untouched, 0 no usable disk, negative
 * on I/O error. */
int yfs_mount(void);
bool yfs_mounted(void);
uint32_t yfs_disk_sectors(void);

/* Path/inode operations. Inodes are small ints; -1 (with an
 * errno-style negative from the long-returning ones) on
 * failure. */
long yfs_lookup(const char *rel);          /* inode or -errno */
bool yfs_is_dir(uint32_t ino);
uint32_t yfs_size(uint32_t ino);
uint32_t yfs_mtime(uint32_t ino); /* M23 */
/* M29: the v4 inode's real timestamp fields */
void yfs_times(uint32_t ino, uint32_t *a, uint32_t *m,
               uint32_t *c);
void yfs_set_times(uint32_t ino, long asec, long msec);
/* M36: stored permission bits (with legacy defaults) */
uint32_t yfs_mode(uint32_t ino);
void yfs_set_mode(uint32_t ino, uint32_t bits);
long yfs_mknod_t(const char *parent_rel, const char *name,
                 uint8_t type);
long yfs_mknod(const char *parent_rel, const char *name,
               bool is_dir);
/* M16: inode types are 0 free, 1 file, 2 dir, 3 symlink. */
int yfs_type_of(uint32_t ino);
long yfs_symlink(const char *parent_rel, const char *name,
                 const char *target);               /* new inode or -errno */
long yfs_pread(uint32_t ino, uint32_t off, void *dst,
               uint32_t count);
long yfs_pwrite(uint32_t ino, uint32_t off, const void *src,
                uint32_t count);           /* extends the file */
int yfs_truncate(uint32_t ino, uint32_t size);
/* Directory scan: finds the entry at or after *slot (a raw
 * entry index), fills name/ino/type, advances *slot past it.
 * Returns 1 when an entry was found, 0 at the end. */
int yfs_readdir(uint32_t ino, uint32_t *slot, char *name_out,
                uint32_t *ino_out, int *is_dir_out);
/* M13: want_dir=false is unlink, true is rmdir. */
long yfs_link(const char *old_rel, const char *new_rel);
uint32_t yfs_nlink(uint32_t ino);
long yfs_unlink(const char *parent_rel, const char *name,
                bool want_dir);
long yfs_rename(const char *old_rel, const char *new_rel);
long yfs_exchange(const char *a_rel, const char *b_rel);
void yfs_note_open(uint32_t ino);
void yfs_note_close(uint32_t ino);

#endif /* YOUSJ_YFS_H */
