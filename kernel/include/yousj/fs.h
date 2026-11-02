/* Yousj OS - filesystem layer (M9): a ramfs plus the open-file
 * object model the fd syscalls sit on.
 *
 * Deliberately small, deliberately honest: one in-memory tree
 * of nodes (directories and regular files), file contents held
 * in PMM pages, path resolution over absolute paths plus a
 * per-task cwd string. There is no block layer and no on-disk
 * format yet -- this milestone's job is the *shape*: nodes,
 * open file descriptions with shared offsets (fork semantics),
 * and execve loading from the tree instead of the retired
 * embedded-image registry. A persistent backend can later sit
 * under the same fs_* interface.
 *
 * The console is a special file object (node == NULL): reads
 * and writes to it are routed to the serial port by the syscall
 * layer, which owns the UART's blocking semantics.
 */
#ifndef YOUSJ_FS_H
#define YOUSJ_FS_H

#include <yousj/types.h>

/* open() flags (Linux values). */
#define YOUSJ_O_RDONLY    0
#define YOUSJ_O_WRONLY    1
#define YOUSJ_O_RDWR      2
#define YOUSJ_O_ACCMODE   3
#define YOUSJ_O_CREAT     0x40
#define YOUSJ_O_TRUNC     0x200
#define YOUSJ_O_APPEND    0x400
#define YOUSJ_O_DIRECTORY 0x10000
#define YOUSJ_O_NONBLOCK  0x800   /* M10 */
#define YOUSJ_O_CLOEXEC   0x80000 /* M10 */

#define YOUSJ_DT_DIR 4
#define YOUSJ_DT_REG 8

#define YOUSJ_AT_FDCWD (-100)

struct yousj_ramfs_node;

/* M10: an anonymous pipe. One object, two file ends (a
 * YOUSJ_O_RDONLY end and a YOUSJ_O_WRONLY end). readers and
 * writers count open FILE OBJECTS (not fds -- dup/fork share
 * the object), which is what EOF/EPIPE hinge on. */
#define YOUSJ_PIPE_CAP 4096
struct yousj_pipe {
    uint8_t buf[YOUSJ_PIPE_CAP];
    uint32_t head;    /* read position */
    uint32_t tail;    /* write position */
    uint32_t count;   /* bytes buffered */
    int readers;
    int writers;
    bool in_use;
};

struct yousj_epoll; /* M18: defined in fs.c */

struct yousj_file {
    struct yousj_ramfs_node *node; /* NULL and pipe NULL: console */
    struct yousj_pipe *pipe;       /* non-NULL: a pipe end (M10) */
    struct yousj_epoll *epoll;     /* non-NULL: an epoll set (M18) */
    int backend;                   /* 0 ramfs, 1 yfs (M12) */
    uint32_t yfs_ino;              /* valid when backend == 1 */
    uint64_t offset;               /* shared across fork (the
                                      object is shared, not the
                                      fd number); unused for pipes */
    long flags;                    /* access mode + O_APPEND etc. */
    int refcount;
    bool in_use;
};

/* Boot: build the empty tree (root "/"), then install the
 * embedded program images as files ("/init" and "/bin/..."),
 * retiring the M5 execve registry. Order: fs_init() before
 * fs_populate_embedded(), both before the first user task. */
void fs_init(void);
/* M11: unpack the embedded newc cpio initramfs into the
 * ramfs. Returns the number of files installed, or negative
 * on a corrupt archive. */
long fs_load_initramfs(void);

/* Absolute-path lookup; NULL when any component is missing. */
struct yousj_ramfs_node *fs_lookup(const char *path);
bool fs_node_is_dir(const struct yousj_ramfs_node *n);
uint64_t fs_node_size(const struct yousj_ramfs_node *n);

/* Create a directory at an absolute path (parents must exist).
 * 0 or a negative errno. */
long fs_mkdir_path(const char *path);

/* Open the file at an absolute path with Linux-ish flag
 * semantics (O_CREAT creates a regular file, O_TRUNC truncates
 * on a writing open, directories are read-only). On success
 * *out holds a referenced file object; release with file_put. */
long fs_open_path(const char *path, long flags,
                  struct yousj_file **out);

/* The pinned console file object (never freed; file_put on it
 * is a no-op). */
struct yousj_file *fs_console_file(void);

/* M10: create a pipe and its two end objects (each refcount 1).
 * 0 or a negative errno. */
long fs_pipe_create(struct yousj_file **rd_out,
                    struct yousj_file **wr_out);

/* M18: epoll instances. ctl ops mirror Linux: ADD 1, DEL 2,
 * MOD 3. Entries pin their target file (a reference is
 * held), so closing the target fd does not drop the
 * interest -- exactly like Linux. */
struct yousj_epoll_entry {
    struct yousj_file *file;
    int fd;
    uint32_t events;
    uint64_t data;
};
long fs_epoll_create(struct yousj_file **out);
long fs_epoll_ctl(struct yousj_file *epf, long op, int fd,
                  struct yousj_file *target, uint32_t events,
                  uint64_t data);
int  fs_epoll_count(struct yousj_file *epf);
bool fs_epoll_entry(struct yousj_file *epf, int idx,
                    struct yousj_epoll_entry *out);

/* M12: mount the yfs data disk at /data when one is present.
 * Returns yfs_mount's code (1 mounted, 2 formatted, 0 none,
 * negative error). */
int fs_mount_data(void);
/* Read a whole file by path (either backend) into dst.
 * Returns its length, -EISDIR for a directory, or another
 * negative errno. Used by execve. */
long fs_read_all_path(const char *path, uint8_t *dst,
                      uint64_t cap);
/* M13: unlink (files), rmdir (empty dirs), rename (same
 * backend only, -EXDEV across). */
long fs_unlink_path(const char *path);
long fs_rmdir_path(const char *path);
long fs_rename_path(const char *old_path, const char *new_path);
/* M16: symlinks. fs_symlink_path creates a link at linkpath
 * holding target (stored verbatim, absolute or relative);
 * fs_readlink_path returns the target's length, or -EINVAL
 * when the path is not a link. */
long fs_symlink_path(const char *target, const char *linkpath);
long fs_readlink_path(const char *path, char *buf,
                      uint64_t cap);
bool fs_file_is_dir(struct yousj_file *f);
/* M14: positional I/O on an open file object (the mmap
 * layer's read-in / write-back path; the object's own
 * offset is untouched and access-mode checks are the
 * caller's job, done once at mmap time). */
long fs_pread_file(struct yousj_file *f, uint64_t off,
                   uint8_t *dst, uint64_t count);
long fs_pwrite_file(struct yousj_file *f, uint64_t off,
                    const uint8_t *src, uint64_t count);
/* Whether path exists; when it does, also reports whether
 * it is a directory and its size (either backend). For
 * chdir's validation and execve's staging. */
bool fs_path_stat(const char *path, bool *is_dir_out,
                  uint64_t *size_out);

/* M20: the stat family's view of a file. mode carries the
 * full Linux type+permission bits. */
struct yousj_stat_info {
    uint32_t mode;
    uint64_t size;
    uint64_t ino;
};
bool fs_path_stat2(const char *path, bool follow,
                   struct yousj_stat_info *info);
bool fs_file_stat(struct yousj_file *f,
                  struct yousj_stat_info *info);

void file_get(struct yousj_file *f);
void file_put(struct yousj_file *f);

/* Ramfs file I/O. `dst`/`src`/`buf` are addresses in the
 * CALLER's address space (the syscall layer validates ranges;
 * the console never reaches these). Return bytes moved or a
 * negative errno. */
long file_read(struct yousj_file *f, uint8_t *dst, uint64_t count);
long file_write(struct yousj_file *f, const uint8_t *src,
                uint64_t count);
long file_seek(struct yousj_file *f, long off, long whence);
long file_getdents(struct yousj_file *f, uint8_t *buf,
                   uint64_t count);

/* Copy a whole node's contents into a contiguous kernel
 * buffer (execve staging). Returns bytes or a negative errno. */
long fs_node_read_all(struct yousj_ramfs_node *n, uint8_t *dst,
                      uint64_t cap);

#endif /* YOUSJ_FS_H */
