/* Yousj OS - ramfs + open-file objects (M9). See <yousj/fs.h>
 * for the model. Written by Muse (coordinator).
 *
 * Single-threaded-by-preemption note: syscalls can be preempted
 * by the timer IRQ, and another task may run fs code in between.
 * Like the rest of this kernel's early layers, the structures
 * here are manipulated without locks; the operations are short
 * and the failure mode of a race is a torn read, acceptable for
 * this milestone and documented rather than hidden.
 */
#include <yousj/types.h>
#include <yousj/fs.h>
#include <yousj/time.h>
#include <yousj/yfs.h>
#include <yousj/pmm.h>
#include <yousj/sched.h> /* M25: tick counters for /proc */
#include <yousj/vmm.h> /* errno magnitudes + phys_to_virt */
#include <yousj/user.h> /* M39: user window constants for maps */
#include <yousj/signal.h> /* M48: blocking inotify reads */

#define YOUSJ_EBADF   9
#define YOUSJ_EEXIST_L 17
#define YOUSJ_ENOTDIR 20
#define YOUSJ_EISDIR  21
#define YOUSJ_ENOSPC  28
#define YOUSJ_EMFILE_L 24
#define YOUSJ_ENOMEM_L 12
#define YOUSJ_ENFILE  23
#define YOUSJ_ENAMETOOLONG_L 36
#define YOUSJ_EXDEV_L 18
#define YOUSJ_ENOTEMPTY_L 39
#define YOUSJ_ELOOP_L 40
#define YOUSJ_EACCES_L 13
#define YOUSJ_ENOENT_L 2

static uint64_t str_len(const char *s); /* defined below */

/* --- M12: /data dispatch to the yfs backend ------------------- */
static bool is_data_path(const char *path)
{
    if (!yfs_mounted())
        return false;
    return path[0] == '/' && path[1] == 'd' && path[2] == 'a' &&
           path[3] == 't' && path[4] == 'a' &&
           (path[5] == '\0' || path[5] == '/');
}

/* "/data" -> "" ; "/data/a/b" -> "a/b" */
static const char *data_rel(const char *path)
{
    const char *rel = path + 5;
    return rel[0] == '/' ? rel + 1 : rel;
}

/* Split "a/b/c" into parent "a/b" and name "c" (buffers in the
 * caller). No slash: parent is "" (the yfs root). */
static void split_parent_name(const char *rel, char *parent,
                              char *name)
{
    uint64_t i, last = 0xFFFFFFFFFFFFFFFFULL, n = str_len(rel);
    for (i = 0; i < n; i++)
        if (rel[i] == '/')
            last = i;
    if (last == 0xFFFFFFFFFFFFFFFFULL) {
        parent[0] = '\0';
        for (i = 0; i <= n; i++)
            name[i] = rel[i];
    } else {
        for (i = 0; i < last; i++)
            parent[i] = rel[i];
        parent[last] = '\0';
        for (i = last + 1; i <= n; i++)
            name[i - last - 1] = rel[i];
    }
}

#define MAX_NODES      128
#define NAME_MAX_LEN   31
#define MAX_FILE_PAGES 64 /* 256 KiB per ramfs file */
#define MAX_FILES      96
#define PATH_MAX_LEN   255

/* M44: names live in directory entries, not in the
 * node -- one node can now be named from several
 * directories (hard links). A directory node owns a
 * list of dents; every non-root node is named by
 * nlink dents. `parent` stays meaningful for
 * directories only (they cannot be hard-linked). */
struct yousj_dent {
    bool in_use;
    char name[NAME_MAX_LEN + 1];
    struct yousj_ramfs_node *node;
    struct yousj_ramfs_node *dir;
    struct yousj_dent *next;
};

#define MAX_DENTS 192
static struct yousj_dent g_dents[MAX_DENTS];

struct yousj_ramfs_node {
    bool in_use;
    bool is_dir;
    bool is_link;                       /* M16: content = target */
    struct yousj_ramfs_node *parent;    /* directories: the
                                           containing dir */
    struct yousj_dent *dents;           /* directories: entries */
    uint32_t nlink;                     /* M44: dent count */
    uint32_t dev;                       /* M47: character
                                           device id, 0 = none */
    uint32_t size;                      /* regular files */
    uint32_t mtime;                     /* M23: epoch seconds */
    uint32_t atime;                     /* M26: independent */
    uint32_t ctime;                     /* M26: independent */
    uint32_t mode;                      /* M36: permission
                                           bits (07777) */
    uint32_t pages[MAX_FILE_PAGES];     /* phys frames, 0 = hole */
    uint16_t open_refs;                 /* M13: live file objects */
    bool unlinked;                      /* M13: name removed; the
                                           storage dies with the
                                           last close */
};

static struct yousj_ramfs_node g_nodes[MAX_NODES];
static struct yousj_ramfs_node *g_root;
static struct yousj_file g_files[MAX_FILES];
static struct yousj_file g_console_file; /* node == NULL */

static uint64_t str_len(const char *s)
{
    uint64_t n = 0;
    while (s[n])
        n++;
    return n;
}

static void mem_copy(uint8_t *dst, const uint8_t *src, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = src[i];
}

static void mem_zero(uint8_t *dst, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++)
        dst[i] = 0;
}

/* --- nodes -------------------------------------------------------- */

static struct yousj_ramfs_node *node_alloc(void)
{
    int i;
    for (i = 0; i < MAX_NODES; i++) {
        if (!g_nodes[i].in_use) {
            mem_zero((uint8_t *)&g_nodes[i], sizeof(g_nodes[i]));
            g_nodes[i].in_use = true;
            g_nodes[i].mode = 0644; /* M36: file default */
            return &g_nodes[i];
        }
    }
    return NULL;
}

static int mem_cmp_bytes(const char *a, const char *b, uint64_t len)
{
    uint64_t i;
    for (i = 0; i < len; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static struct yousj_dent *dent_alloc(void)
{
    int i;
    for (i = 0; i < MAX_DENTS; i++) {
        if (!g_dents[i].in_use) {
            mem_zero((uint8_t *)&g_dents[i],
                     sizeof(g_dents[i]));
            g_dents[i].in_use = true;
            return &g_dents[i];
        }
    }
    return NULL;
}

static void dent_attach(struct yousj_ramfs_node *dir,
                        struct yousj_dent *d)
{
    d->dir = dir;
    d->next = dir->dents;
    dir->dents = d;
}

static void dent_detach(struct yousj_dent *d)
{
    struct yousj_dent **link = &d->dir->dents;
    while (*link != NULL && *link != d)
        link = &(*link)->next;
    if (*link == d) {
        *link = d->next;
        d->next = NULL;
    }
}

/* Unlink-style removal frees the dent for reuse;
 * rename keeps its dent (it is re-attached), so the
 * two steps are separate on purpose (M44 debug:
 * conflating them recycled a live dent). */
static void dent_free(struct yousj_dent *d)
{
    d->in_use = false;
}

static struct yousj_dent *node_find_dent(
    struct yousj_ramfs_node *dir, const char *name, uint64_t len)
{
    struct yousj_dent *d;
    for (d = dir->dents; d != NULL; d = d->next) {
        if (str_len(d->name) == len &&
            mem_cmp_bytes(d->name, name, len))
            return d;
    }
    return NULL;
}

static struct yousj_ramfs_node *node_find_child(
    struct yousj_ramfs_node *dir, const char *name, uint64_t len)
{
    struct yousj_dent *d = node_find_dent(dir, name, len);
    return d != NULL ? d->node : NULL;
}

static struct yousj_ramfs_node *node_create(
    struct yousj_ramfs_node *parent, const char *name, uint64_t len,
    bool is_dir)
{
    struct yousj_ramfs_node *n = node_alloc();
    struct yousj_dent *d;
    if (n == NULL)
        return NULL;
    d = dent_alloc();
    if (d == NULL) {
        n->in_use = false;
        return NULL;
    }
    if (len > NAME_MAX_LEN)
        len = NAME_MAX_LEN;
    mem_copy((uint8_t *)d->name, (const uint8_t *)name, len);
    d->name[len] = '\0';
    d->node = n;
    n->is_dir = is_dir;
    n->mode = is_dir ? 0755u : 0644u; /* M36 */
    n->mtime = (uint32_t)yousj_now_unix();
    n->atime = n->mtime;
    n->ctime = n->mtime;
    n->parent = parent;
    n->nlink = 1;
    dent_attach(parent, d);
    return n;
}

/* Split path into components and walk. `path` must be absolute.
 * When `parent_out` is set, returns the PARENT directory of the
 * final component instead and copies the final component into
 * `base_out` (used by creation paths). */
static struct yousj_ramfs_node *walk(const char *path, bool want_parent,
                                     char *base_out)
{
    struct yousj_ramfs_node *cur = g_root;
    const char *p = path;
    if (p == NULL || p[0] != '/')
        return NULL;
    p++;
    if (*p == '\0')
        return want_parent ? NULL : cur; /* the root itself */
    for (;;) {
        const char *slash = p;
        uint64_t len;
        bool last;
        struct yousj_ramfs_node *next;
        while (*slash && *slash != '/')
            slash++;
        len = (uint64_t)(slash - p);
        last = (*slash == '\0');
        if (len == 0)
            return NULL; /* "//" or trailing "/" */
        if (!cur->is_dir)
            return NULL;
        if (last && want_parent) {
            if (base_out != NULL) {
                uint64_t i;
                if (len > NAME_MAX_LEN)
                    return NULL;
                for (i = 0; i < len; i++)
                    base_out[i] = p[i];
                base_out[len] = '\0';
            }
            return cur;
        }
        next = node_find_child(cur, p, len);
        if (next == NULL)
            return NULL;
        if (last)
            return next;
        cur = next;
        p = slash + 1;
    }
}

struct yousj_ramfs_node *fs_lookup(const char *path)
{
    return walk(path, false, NULL);
}

bool fs_node_is_dir(const struct yousj_ramfs_node *n)
{
    return n != NULL && n->is_dir;
}

uint64_t fs_node_size(const struct yousj_ramfs_node *n)
{
    return n != NULL ? n->size : 0;
}

static long ramfs_mkdir_path(const char *path)
{
    char base[NAME_MAX_LEN + 1];
    struct yousj_ramfs_node *parent = walk(path, true, base);
    if (parent == NULL)
        return -YOUSJ_ENOENT;
    if (node_find_child(parent, base, str_len(base)) != NULL)
        return -YOUSJ_EEXIST_L;
    return node_create(parent, base, str_len(base), true) != NULL
               ? 0
               : -YOUSJ_ENOSPC;
}

/* --- file contents ------------------------------------------------- */

static int node_ensure_page(struct yousj_ramfs_node *n, uint32_t idx)
{
    uint64_t phys;
    if (idx >= MAX_FILE_PAGES)
        return -YOUSJ_ENOSPC;
    if (n->pages[idx] != 0)
        return 0;
    phys = pmm_alloc_page();
    if (phys == 0)
        return -YOUSJ_ENOSPC;
    n->pages[idx] = (uint32_t)phys;
    mem_zero((uint8_t *)yousj_phys_to_virt(phys), YOUSJ_PAGE_SIZE);
    return 0;
}

static long node_pread(struct yousj_ramfs_node *n, uint64_t off,
                       uint8_t *dst, uint64_t count)
{
    uint64_t done = 0;
    if (off >= n->size)
        return 0;
    if (count > n->size - off)
        count = n->size - off;
    while (done < count) {
        uint64_t pos = off + done;
        uint32_t idx = (uint32_t)(pos / YOUSJ_PAGE_SIZE);
        uint64_t within = pos % YOUSJ_PAGE_SIZE;
        uint64_t chunk = YOUSJ_PAGE_SIZE - within;
        if (chunk > count - done)
            chunk = count - done;
        if (n->pages[idx] == 0) {
            mem_zero(dst + done, chunk); /* sparse hole */
        } else {
            mem_copy(dst + done,
                     (const uint8_t *)yousj_phys_to_virt(n->pages[idx]) +
                         within,
                     chunk);
        }
        done += chunk;
    }
    return (long)done;
}

static long node_pwrite(struct yousj_ramfs_node *n, uint64_t off,
                        const uint8_t *src, uint64_t count)
{
    uint64_t done = 0;
    while (done < count) {
        uint64_t pos = off + done;
        uint32_t idx = (uint32_t)(pos / YOUSJ_PAGE_SIZE);
        uint64_t within = pos % YOUSJ_PAGE_SIZE;
        uint64_t chunk = YOUSJ_PAGE_SIZE - within;
        int rc;
        if (chunk > count - done)
            chunk = count - done;
        rc = node_ensure_page(n, idx);
        if (rc != 0)
            break; /* partial write counts, like Linux */
        mem_copy((uint8_t *)yousj_phys_to_virt(n->pages[idx]) + within,
                 src + done, chunk);
        done += chunk;
    }
    if (done > 0) {
        if (off + done > n->size)
            n->size = (uint32_t)(off + done);
        n->mtime = (uint32_t)yousj_now_unix();
        n->ctime = n->mtime;
    }
    return (long)done;
}

static void node_truncate(struct yousj_ramfs_node *n)
{
    uint32_t i;
    for (i = 0; i < MAX_FILE_PAGES; i++) {
        if (n->pages[i] != 0) {
            pmm_free_page(n->pages[i]);
            n->pages[i] = 0;
        }
    }
    n->size = 0;
    n->mtime = (uint32_t)yousj_now_unix();
    n->ctime = n->mtime;
}

/* --- M13: detach / deferred free -------------------------------- */

/* M44: name removal is dent_detach() plus a nlink
 * drop; the node dies when the count reaches zero
 * AND the last open reference drains (node_maybe_
 * free via the unlinked flag, as since M13). */
static void node_maybe_free(struct yousj_ramfs_node *n);

static void node_unname(struct yousj_ramfs_node *n)
{
    if (n->nlink > 0)
        n->nlink--;
    if (n->nlink == 0) {
        n->unlinked = true;
        node_maybe_free(n);
    }
}

static void node_maybe_free(struct yousj_ramfs_node *n)
{
    if (n->unlinked && n->open_refs == 0) {
        node_truncate(n);
        n->in_use = false;
    }
}

static void node_ref_dec(struct yousj_ramfs_node *n)
{
    if (n->open_refs > 0)
        n->open_refs--;
    node_maybe_free(n);
}

long fs_node_read_all(struct yousj_ramfs_node *n, uint8_t *dst,
                      uint64_t cap)
{
    if (n == NULL || n->is_dir)
        return -YOUSJ_EISDIR;
    if (n->size > cap)
        return -YOUSJ_ENOMEM_L;
    return node_pread(n, 0, dst, n->size);
}

/* --- file objects --------------------------------------------------- */

struct yousj_file *fs_console_file(void)
{
    return &g_console_file;
}

static struct yousj_file *file_alloc(void)
{
    int i;
    for (i = 0; i < MAX_FILES; i++) {
        if (!g_files[i].in_use) {
            mem_zero((uint8_t *)&g_files[i], sizeof(g_files[i]));
            g_files[i].in_use = true;
            g_files[i].refcount = 1;
            return &g_files[i];
        }
    }
    return NULL;
}

void file_get(struct yousj_file *f)
{
    if (f != NULL && f != &g_console_file)
        f->refcount++;
}

static void epoll_release(struct yousj_epoll *ep);
static void inotify_release(struct yousj_inotify *in);

void file_put(struct yousj_file *f)
{
    if (f == NULL || f == &g_console_file)
        return;
    if (--f->refcount <= 0) {
        /* M10: releasing the last reference to a pipe end drops
         * the pipe's end count; the pipe itself dies when both
         * counts reach zero. Blocked peers poll these counts,
         * so no explicit wakeup is needed at this stage. */
        if (f->pipe != NULL) {
            struct yousj_pipe *p = f->pipe;
            if ((f->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_RDONLY)
                p->readers--;
            else if ((f->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_WRONLY)
                p->writers--;
            if (p->readers <= 0 && p->writers <= 0)
                p->in_use = false;
            f->pipe = NULL;
        }
        if (f->node != NULL) {
            node_ref_dec(f->node);
            f->node = NULL;
        }
        if (f->epoll != NULL) {
            epoll_release(f->epoll);
            f->epoll = NULL;
        }
        if (f->inotify != NULL) {
            inotify_release(f->inotify);
            f->inotify = NULL;
        }
        if (f->backend == 1)
            yfs_note_close(f->yfs_ino);
        f->in_use = false;
    }
}

#define MAX_PIPES 16
static struct yousj_pipe g_pipes[MAX_PIPES];

long fs_pipe_create(struct yousj_file **rd_out,
                    struct yousj_file **wr_out)
{
    struct yousj_pipe *p = NULL;
    struct yousj_file *rd, *wr;
    int i;

    for (i = 0; i < MAX_PIPES; i++) {
        if (!g_pipes[i].in_use) {
            p = &g_pipes[i];
            break;
        }
    }
    if (p == NULL)
        return -YOUSJ_ENFILE;
    mem_zero((uint8_t *)p, sizeof(*p));
    p->in_use = true;
    p->readers = 1;
    p->writers = 1;
    rd = file_alloc();
    wr = file_alloc();
    if (rd == NULL || wr == NULL) {
        if (rd != NULL)
            rd->in_use = false;
        if (wr != NULL)
            wr->in_use = false;
        p->in_use = false;
        return -YOUSJ_ENFILE;
    }
    rd->pipe = p;
    rd->flags = YOUSJ_O_RDONLY;
    wr->pipe = p;
    wr->flags = YOUSJ_O_WRONLY;
    *rd_out = rd;
    *wr_out = wr;
    return 0;
}

/* ------------------------------------------------------------------ */
/* M18: epoll instances. An instance is a small interest
 * table; each entry pins its target file object. The wait
 * loop lives in syscall.c (it owns the readiness probe and
 * the sleep/signal protocol); fs.c owns storage and the
 * file-lifecycle hook (file_put -> epoll_release). */

#define MAX_EPOLLS      8
#define EPOLL_MAX_ENTRIES 32

struct yousj_epoll {
    bool in_use;
    int count;
    struct yousj_epoll_entry entries[EPOLL_MAX_ENTRIES];
};

static struct yousj_epoll g_epolls[MAX_EPOLLS];

static void epoll_release(struct yousj_epoll *ep)
{
    int i;
    for (i = 0; i < ep->count; i++)
        file_put(ep->entries[i].file);
    ep->count = 0;
    ep->in_use = false;
}

long fs_epoll_create(struct yousj_file **out)
{
    int i;
    struct yousj_epoll *ep = NULL;
    struct yousj_file *f;
    for (i = 0; i < MAX_EPOLLS; i++) {
        if (!g_epolls[i].in_use) {
            ep = &g_epolls[i];
            break;
        }
    }
    if (ep == NULL)
        return -YOUSJ_ENFILE;
    f = file_alloc();
    if (f == NULL)
        return -YOUSJ_ENFILE;
    ep->in_use = true;
    ep->count = 0;
    f->epoll = ep;
    f->flags = YOUSJ_O_RDONLY;
    *out = f;
    return 0;
}

long fs_epoll_ctl(struct yousj_file *epf, long op, int fd,
                  struct yousj_file *target, uint32_t events,
                  uint64_t data)
{
    struct yousj_epoll *ep;
    int i, at = -1;
    if (epf == NULL || epf->epoll == NULL)
        return -YOUSJ_EINVAL;
    ep = epf->epoll;
    for (i = 0; i < ep->count; i++) {
        if (ep->entries[i].file == target &&
            ep->entries[i].fd == fd) {
            at = i;
            break;
        }
    }
    switch (op) {
    case 1: /* EPOLL_CTL_ADD */
        if (at >= 0)
            return -YOUSJ_EEXIST_L;
        if (ep->count >= EPOLL_MAX_ENTRIES)
            return -YOUSJ_ENOSPC;
        file_get(target);
        ep->entries[ep->count].file = target;
        ep->entries[ep->count].fd = fd;
        ep->entries[ep->count].events = events;
        ep->entries[ep->count].data = data;
        ep->count++;
        return 0;
    case 2: /* EPOLL_CTL_DEL */
        if (at < 0)
            return -YOUSJ_ENOENT;
        file_put(ep->entries[at].file);
        for (i = at + 1; i < ep->count; i++)
            ep->entries[i - 1] = ep->entries[i];
        ep->count--;
        return 0;
    case 3: /* EPOLL_CTL_MOD */
        if (at < 0)
            return -YOUSJ_ENOENT;
        ep->entries[at].events = events;
        ep->entries[at].data = data;
        return 0;
    default:
        return -YOUSJ_EINVAL;
    }
}

int fs_epoll_count(struct yousj_file *epf)
{
    if (epf == NULL || epf->epoll == NULL)
        return 0;
    return epf->epoll->count;
}

bool fs_epoll_entry(struct yousj_file *epf, int idx,
                    struct yousj_epoll_entry *out)
{
    if (epf == NULL || epf->epoll == NULL || idx < 0 ||
        idx >= epf->epoll->count)
        return false;
    *out = epf->epoll->entries[idx];
    return true;
}

/* --- M48: inotify -------------------------------------------------
 * Instances mirror the epoll pattern: a pool, a watch
 * table keyed by target identity (ramfs node pointer /
 * yfs inode number), and a fixed-size ring of pending
 * event records. Mutation entry points in this file
 * call the notify helpers; delivery filters by each
 * watch's mask (DELETE_SELF / MOVE_SELF / IGNORED /
 * Q_OVERFLOW are always delivered, as in Linux). A
 * deleted object's watches get DELETE_SELF + IGNORED
 * and are dropped. Blocking reads poll on a 2-tick
 * sleep like the pipe code. */

#define IN_MODIFY_K        0x00000002u
#define IN_ATTRIB_K        0x00000004u
#define IN_CLOSE_WRITE_K   0x00000008u
#define IN_CLOSE_NOWRITE_K 0x00000010u
#define IN_MOVED_FROM_K    0x00000040u
#define IN_MOVED_TO_K      0x00000080u
#define IN_CREATE_K        0x00000100u
#define IN_DELETE_K        0x00000200u
#define IN_DELETE_SELF_K   0x00000400u
#define IN_MOVE_SELF_K     0x00000800u
#define IN_Q_OVERFLOW_K    0x00004000u
#define IN_IGNORED_K       0x00008000u
#define IN_ISDIR_K         0x40000000u

#define MAX_INOTIFY         8
#define INOTIFY_MAX_WATCHES 32
#define INOTIFY_QUEUE       64

struct yousj_inotify_watch {
    bool in_use;
    int wd;
    int backend; /* 0 ramfs, 1 yfs */
    struct yousj_ramfs_node *node;
    uint32_t ino;
    bool is_dir;
    uint32_t mask;
};

struct yousj_inotify {
    bool in_use;
    int next_wd;
    struct yousj_inotify_watch watches[INOTIFY_MAX_WATCHES];
    struct yousj_inotify_event queue[INOTIFY_QUEUE];
    int q_head;
    int q_count;
    bool overflow;
};

static struct yousj_inotify g_inotifies[MAX_INOTIFY];
static uint32_t g_inotify_cookie = 1;

/* Defined further down (the M16 resolution layer). */
static long fs_resolve_links(const char *path, char *out,
                             bool follow);
static bool is_proc_path(const char *path);

static void inotify_enqueue(struct yousj_inotify *in,
                            const struct yousj_inotify_event *ev)
{
    if (in->q_count >= INOTIFY_QUEUE) {
        in->overflow = true;
        return;
    }
    in->queue[(in->q_head + in->q_count) % INOTIFY_QUEUE] =
        *ev;
    in->q_count++;
}

/* Deliver one event to one watch with Linux filtering:
 * the watch mask gates ordinary events; the self-
 * destruction family always lands. subject_is_dir
 * adds IN_ISDIR. A DELETE_SELF also lands IN_IGNORED
 * and retires the watch. */
static void inotify_deliver(struct yousj_inotify *in,
                            struct yousj_inotify_watch *w,
                            uint32_t events, uint32_t cookie,
                            const char *name,
                            bool subject_is_dir)
{
    struct yousj_inotify_event ev;
    uint32_t m = events & w->mask;
    if (events & (IN_DELETE_SELF_K | IN_MOVE_SELF_K))
        m = events;
    if (m == 0)
        return;
    if (subject_is_dir)
        m |= IN_ISDIR_K;
    mem_zero((uint8_t *)&ev, sizeof(ev));
    ev.wd = w->wd;
    ev.mask = m;
    ev.cookie = cookie;
    if (name != NULL) {
        uint64_t nl = str_len(name) + 1;
        if (nl > 28)
            nl = 28;
        mem_copy((uint8_t *)ev.name, (const uint8_t *)name,
                 nl);
        ev.len = (uint32_t)((nl + 3) & ~3u);
    }
    inotify_enqueue(in, &ev);
    if (events & IN_DELETE_SELF_K) {
        mem_zero((uint8_t *)&ev, sizeof(ev));
        ev.wd = w->wd;
        ev.mask = IN_IGNORED_K;
        inotify_enqueue(in, &ev);
        w->in_use = false;
    }
}

static bool watch_matches(struct yousj_inotify_watch *w,
                          int backend,
                          struct yousj_ramfs_node *node,
                          uint32_t ino)
{
    if (!w->in_use || w->backend != backend)
        return false;
    return backend == 0 ? w->node == node : w->ino == ino;
}

/* Child event: delivered to watches on the PARENT. */
static void inotify_child_id(int backend,
                             struct yousj_ramfs_node *pnode,
                             uint32_t pino, const char *name,
                             uint32_t events, uint32_t cookie,
                             bool subject_is_dir)
{
    int i, j;
    for (i = 0; i < MAX_INOTIFY; i++) {
        struct yousj_inotify *in = &g_inotifies[i];
        if (!in->in_use)
            continue;
        for (j = 0; j < INOTIFY_MAX_WATCHES; j++) {
            struct yousj_inotify_watch *w = &in->watches[j];
            if (watch_matches(w, backend, pnode, pino))
                inotify_deliver(in, w, events, cookie, name,
                                subject_is_dir);
        }
    }
}

/* Self event: delivered to watches on the object. */
static void inotify_self_id(int backend,
                            struct yousj_ramfs_node *node,
                            uint32_t ino, uint32_t events,
                            bool subject_is_dir)
{
    int i, j;
    for (i = 0; i < MAX_INOTIFY; i++) {
        struct yousj_inotify *in = &g_inotifies[i];
        if (!in->in_use)
            continue;
        for (j = 0; j < INOTIFY_MAX_WATCHES; j++) {
            struct yousj_inotify_watch *w = &in->watches[j];
            if (watch_matches(w, backend, node, ino))
                inotify_deliver(in, w, events, 0, NULL,
                                subject_is_dir);
        }
    }
}

/* Path-level wrappers for the fs_* entry points.
 * `resolved` is a link-resolved absolute path. */
struct target_id {
    int backend;
    struct yousj_ramfs_node *node;
    uint32_t ino;
    bool is_dir;
    bool ok;
};

static void inotify_capture(const char *resolved,
                            struct target_id *id)
{
    id->ok = false;
    id->node = NULL;
    id->ino = 0;
    id->is_dir = false;
    if (is_data_path(resolved)) {
        struct yousj_stat_info info;
        id->backend = 1;
        if (fs_path_stat2(resolved, false, &info)) {
            id->ino = (uint32_t)(info.ino - 1000000u);
            id->is_dir =
                (info.mode & 0170000u) == 0040000u;
            id->ok = true;
        }
    } else {
        struct yousj_ramfs_node *n = fs_lookup(resolved);
        id->backend = 0;
        if (n != NULL) {
            id->node = n;
            id->is_dir = n->is_dir;
            id->ok = true;
        }
    }
}

static void inotify_child_path(const char *resolved,
                               uint32_t events,
                               uint32_t cookie,
                               bool subject_is_dir)
{
    if (is_data_path(resolved)) {
        const char *rel = data_rel(resolved);
        char parent[128];
        const char *slash = NULL;
        const char *q;
        long pino;
        uint64_t i;
        for (q = rel; *q != '\0'; q++)
            if (*q == '/')
                slash = q;
        if (slash == NULL || slash == rel) {
            /* parent is the yfs root */
            inotify_child_id(1, NULL, 0, rel, events,
                             cookie, subject_is_dir);
            return;
        }
        for (i = 0; rel + i < slash && i < 120; i++)
            parent[i] = rel[i];
        parent[i] = '\0';
        pino = yfs_lookup(parent);
        if (pino < 0)
            return;
        inotify_child_id(1, NULL, (uint32_t)pino, slash + 1,
                         events, cookie, subject_is_dir);
    } else {
        char base[NAME_MAX_LEN + 1];
        struct yousj_ramfs_node *parent =
            walk(resolved, true, base);
        if (parent == NULL)
            return;
        inotify_child_id(0, parent, 0, base, events, cookie,
                         subject_is_dir);
    }
}

static void inotify_self_path(const char *resolved,
                              uint32_t events)
{
    struct target_id id;
    inotify_capture(resolved, &id);
    if (id.ok)
        inotify_self_id(id.backend, id.node, id.ino, events,
                        id.is_dir);
}

long fs_inotify_create(struct yousj_file **out)
{
    int i;
    struct yousj_file *f;
    for (i = 0; i < MAX_INOTIFY; i++)
        if (!g_inotifies[i].in_use)
            break;
    if (i == MAX_INOTIFY)
        return -YOUSJ_ENOSPC;
    f = file_alloc();
    if (f == NULL)
        return -YOUSJ_ENFILE;
    mem_zero((uint8_t *)&g_inotifies[i],
             sizeof(g_inotifies[i]));
    g_inotifies[i].in_use = true;
    g_inotifies[i].next_wd = 1;
    f->inotify = &g_inotifies[i];
    *out = f;
    return 0;
}

static void inotify_release(struct yousj_inotify *in)
{
    in->in_use = false;
}

long fs_inotify_add_watch(struct yousj_file *f,
                          const char *path, uint32_t mask,
                          int *wd_out)
{
    char resolved[PATH_MAX_LEN + 1];
    struct yousj_inotify *in = f->inotify;
    struct target_id id;
    int j;
    long rc;
    if (in == NULL)
        return -YOUSJ_EINVAL;
    rc = fs_resolve_links(path, resolved, true);
    if (rc != 0)
        return rc;
    if (is_proc_path(resolved))
        return -YOUSJ_EINVAL;
    inotify_capture(resolved, &id);
    if (!id.ok)
        return -YOUSJ_ENOENT_L;
    for (j = 0; j < INOTIFY_MAX_WATCHES; j++) {
        struct yousj_inotify_watch *w = &in->watches[j];
        if (watch_matches(w, id.backend, id.node, id.ino)) {
            w->mask = mask;
            *wd_out = w->wd;
            return 0;
        }
    }
    for (j = 0; j < INOTIFY_MAX_WATCHES; j++) {
        struct yousj_inotify_watch *w = &in->watches[j];
        if (!w->in_use) {
            w->in_use = true;
            w->wd = in->next_wd++;
            w->backend = id.backend;
            w->node = id.node;
            w->ino = id.ino;
            w->is_dir = id.is_dir;
            w->mask = mask;
            *wd_out = w->wd;
            return 0;
        }
    }
    return -YOUSJ_ENOSPC;
}

long fs_inotify_rm_watch(struct yousj_file *f, int wd)
{
    struct yousj_inotify *in = f->inotify;
    int j;
    if (in == NULL)
        return -YOUSJ_EINVAL;
    for (j = 0; j < INOTIFY_MAX_WATCHES; j++) {
        struct yousj_inotify_watch *w = &in->watches[j];
        if (w->in_use && w->wd == wd) {
            struct yousj_inotify_event ev;
            mem_zero((uint8_t *)&ev, sizeof(ev));
            ev.wd = wd;
            ev.mask = IN_IGNORED_K;
            inotify_enqueue(in, &ev);
            w->in_use = false;
            return 0;
        }
    }
    return -YOUSJ_EINVAL;
}

uint64_t fs_inotify_pending(struct yousj_file *f)
{
    struct yousj_inotify *in = f->inotify;
    uint64_t bytes = 0;
    int i;
    if (in == NULL)
        return 0;
    for (i = 0; i < in->q_count; i++) {
        struct yousj_inotify_event *ev =
            &in->queue[(in->q_head + i) % INOTIFY_QUEUE];
        bytes += 16 + ev->len;
    }
    return bytes;
}

long fs_inotify_read(struct yousj_file *f, uint8_t *dst,
                     uint64_t count)
{
    struct yousj_inotify *in = f->inotify;
    struct task *me = sched_current();
    if (in == NULL)
        return -YOUSJ_EINVAL;
    if (count < 16)
        return -YOUSJ_EINVAL;
    for (;;) {
        if (in->q_count > 0) {
            uint64_t used = 0;
            while (in->q_count > 0) {
                struct yousj_inotify_event *ev =
                    &in->queue[in->q_head];
                uint64_t rec = 16 + ev->len;
                if (used + rec > count)
                    break;
                mem_copy(dst + used, (const uint8_t *)ev,
                         16);
                if (ev->len > 0)
                    mem_copy(dst + used + 16,
                             (const uint8_t *)ev->name,
                             ev->len);
                used += rec;
                in->q_head =
                    (in->q_head + 1) % INOTIFY_QUEUE;
                in->q_count--;
            }
            if (in->overflow && used + 16 <= count) {
                struct yousj_inotify_event ev;
                mem_zero((uint8_t *)&ev, sizeof(ev));
                ev.wd = -1;
                ev.mask = IN_Q_OVERFLOW_K;
                mem_copy(dst + used, (const uint8_t *)&ev,
                         16);
                used += 16;
                in->overflow = false;
            }
            return (long)used;
        }
        if (f->flags & YOUSJ_O_NONBLOCK)
            return -11; /* EAGAIN */
        if (signal_deliverable_pending(me)) {
            me->sig_wake = false;
            return -512; /* ERESTARTSYS */
        }
        sched_sleep_ticks(2);
    }
}

void fs_inotify_note_close(struct yousj_file *f)
{
    if (f == NULL || f->pipe != NULL || f->epoll != NULL ||
        f->inotify != NULL || f->backend == 2)
        return;
    if (f->backend == 1)
        inotify_self_id(1, NULL, f->yfs_ino,
                        f->written ? IN_CLOSE_WRITE_K
                                   : IN_CLOSE_NOWRITE_K,
                        yfs_is_dir(f->yfs_ino));
    else if (f->node != NULL)
        inotify_self_id(0, f->node, 0,
                        f->written ? IN_CLOSE_WRITE_K
                                   : IN_CLOSE_NOWRITE_K,
                        f->node->is_dir);
}


/* ------------------------------------------------------------------ */
/* M16: symbolic-link resolution. Every path-taking entry
 * point runs its path through fs_resolve_links first: any
 * component that is a symlink is spliced out (targets may
 * be absolute, or relative to the link's own directory),
 * "." and ".." collapse, and chains are capped (ELOOP).
 * Operations that act ON a name (mkdir, unlink, rmdir,
 * rename, readlink) pass follow_final=false so the last
 * component itself is never followed; readers pass true.
 * Resolution happens purely in path space, so a link may
 * cross the /data boundary in either direction. */

static void path_copy(char *dst, const char *src)
{
    uint64_t i = 0;
    while (src[i] != '\0' && i < PATH_MAX_LEN) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* Collapse "//", "." and ".." in place (absolute paths). */
static void path_normalize(char *p)
{
    uint16_t starts[128];
    int ncomp = 0;
    uint64_t i = 0, w = 0;
    while (p[i] != '\0') {
        uint64_t j = i, len;
        while (p[j] != '\0' && p[j] != '/')
            j++;
        len = j - i;
        if (len == 1 && p[i] == '.') {
            /* skip */
        } else if (len == 2 && p[i] == '.' && p[i + 1] == '.') {
            if (ncomp > 0)
                w = starts[--ncomp];
        } else if (len > 0) {
            uint64_t k;
            if (w == 0 || p[w - 1] != '/')
                p[w++] = '/';
            starts[ncomp++] = (uint16_t)w;
            for (k = 0; k < len; k++)
                p[w++] = p[i + k];
        }
        i = (p[j] == '/') ? j + 1 : j;
    }
    if (w == 0)
        p[w++] = '/';
    p[w] = '\0';
}

static void parent_of(const char *path, char *out)
{
    uint64_t n = str_len(path), i;
    path_copy(out, path);
    if (n <= 1) {
        out[0] = '/';
        out[1] = '\0';
        return;
    }
    for (i = n - 1; i > 0; i--) {
        if (out[i] == '/') {
            if (i == 0) {
                out[0] = '/';
                out[1] = '\0';
            } else {
                out[i] = '\0';
            }
            return;
        }
    }
    out[0] = '/';
    out[1] = '\0';
}

/* 0 + is_link (+target text) when the path exists; ENOENT
 * when it does not. */
static long fs_link_check(const char *path, bool *is_link,
                          char *target, uint64_t cap)
{
    *is_link = false;
    if (is_data_path(path)) {
        long ino = yfs_lookup(data_rel(path));
        if (ino < 0)
            return -YOUSJ_ENOENT;
        if (yfs_type_of((uint32_t)ino) == 3) {
            long n = yfs_pread((uint32_t)ino, 0,
                               (uint8_t *)target,
                               (uint32_t)(cap - 1));
            if (n < 0)
                return n;
            target[n] = '\0';
            *is_link = true;
        }
        return 0;
    }
    {
        struct yousj_ramfs_node *n = fs_lookup(path);
        if (n == NULL)
            return -YOUSJ_ENOENT;
        if (n->is_link) {
            long got = node_pread(n, 0, (uint8_t *)target,
                                  cap - 1);
            if (got < 0)
                return got;
            target[got] = '\0';
            *is_link = true;
        }
        return 0;
    }
}

static long fs_resolve_links(const char *path, char *out,
                             bool follow_final)
{
    char cur[PATH_MAX_LEN + 1];
    int depth;
    path_copy(cur, path);
    path_normalize(cur);
    for (depth = 0; ; depth++) {
        bool spliced = false;
        uint64_t i = 1; /* components follow the root '/' */
        char prefix[PATH_MAX_LEN + 1];
        prefix[0] = '/';
        prefix[1] = '\0';
        while (cur[i] != '\0') {
            uint64_t j = i, plen;
            bool final_comp, is_link;
            char target[PATH_MAX_LEN + 1];
            while (cur[j] != '\0' && cur[j] != '/')
                j++;
            final_comp = (cur[j] == '\0');
            plen = str_len(prefix);
            if (plen > 1)
                prefix[plen++] = '/';
            mem_copy((uint8_t *)prefix + plen,
                     (const uint8_t *)cur + i, j - i);
            prefix[plen + (j - i)] = '\0';
            if ((!final_comp || follow_final) &&
                fs_link_check(prefix, &is_link, target,
                              sizeof target) == 0 && is_link) {
                char next[PATH_MAX_LEN + 1];
                uint64_t w;
                if (depth >= 8)
                    return -YOUSJ_ELOOP_L;
                if (target[0] == '/') {
                    path_copy(next, target);
                } else {
                    char parent[PATH_MAX_LEN + 1];
                    parent_of(prefix, parent);
                    path_copy(next, parent);
                    w = str_len(next);
                    if (w > 1)
                        next[w++] = '/';
                    next[w] = '\0';
                    path_copy(next + w, target);
                }
                w = str_len(next);
                if (cur[j] != '\0')
                    path_copy(next + w, cur + j);
                path_copy(cur, next);
                path_normalize(cur);
                spliced = true;
                break;
            }
            i = (cur[j] == '/') ? j + 1 : j;
        }
        if (!spliced) {
            path_copy(out, cur);
            return 0;
        }
    }
}

static long ramfs_open_path(const char *path, long flags,
                            uint32_t mode_bits,
                            struct yousj_file **out)
{
    struct yousj_ramfs_node *n = fs_lookup(path);
    long acc = flags & YOUSJ_O_ACCMODE;
    struct yousj_file *f;

    if (n == NULL) {
        char base[NAME_MAX_LEN + 1];
        struct yousj_ramfs_node *parent;
        if (!(flags & YOUSJ_O_CREAT))
            return -YOUSJ_ENOENT;
        if (flags & YOUSJ_O_DIRECTORY)
            return -YOUSJ_EINVAL;
        parent = walk(path, true, base);
        if (parent == NULL)
            return -YOUSJ_ENOENT;
        n = node_create(parent, base, str_len(base), false);
        if (n == NULL)
            return -YOUSJ_ENOSPC;
        if (mode_bits != 0)
            n->mode = mode_bits; /* M37: O_CREAT mode */
        inotify_child_id(0, parent, 0, base, IN_CREATE_K, 0,
                         false); /* M48 */
    }
    if (n->is_link)
        return -YOUSJ_ELOOP_L; /* resolution missed one */
    if (n->is_dir) {
        if (acc != YOUSJ_O_RDONLY)
            return -YOUSJ_EISDIR;
    } else {
        if (flags & YOUSJ_O_DIRECTORY)
            return -YOUSJ_ENOTDIR;
        if ((flags & YOUSJ_O_TRUNC) && acc != YOUSJ_O_RDONLY)
            node_truncate(n);
    }
    f = file_alloc();
    if (f == NULL)
        return -YOUSJ_ENFILE;
    f->node = n;
    n->open_refs++;
    f->flags = flags;
    f->offset = 0;
    *out = f;
    return 0;
}

static long yfs_open_path_fs(const char *rel, long flags,
                             uint32_t mode_bits,
                             struct yousj_file **out)
{
    long ino;
    long acc = flags & YOUSJ_O_ACCMODE;
    struct yousj_file *f;
    bool is_dir;
    ino = yfs_lookup(rel);

    if (ino < 0) {
        if (ino != -YOUSJ_ENOENT || !(flags & YOUSJ_O_CREAT))
            return ino;
        if (flags & YOUSJ_O_DIRECTORY)
            return -YOUSJ_EINVAL;
        {
            char parent[128], name[40];
            split_parent_name(rel, parent, name);
            ino = yfs_mknod(parent, name, false);
            if (ino < 0)
                return ino;
            if (mode_bits != 0)
                yfs_set_mode((uint32_t)ino, mode_bits);
            {
                long pino = yfs_lookup(parent);
                if (pino >= 0)
                    inotify_child_id(1, NULL,
                                     (uint32_t)pino, name,
                                     IN_CREATE_K, 0,
                                     false); /* M48 */
            }
        }
    }
    if (yfs_type_of((uint32_t)ino) == 3)
        return -YOUSJ_ELOOP_L; /* resolution missed one */
    is_dir = yfs_is_dir((uint32_t)ino);
    if (is_dir) {
        if (acc != YOUSJ_O_RDONLY)
            return -YOUSJ_EISDIR;
    } else {
        if (flags & YOUSJ_O_DIRECTORY)
            return -YOUSJ_ENOTDIR;
        if ((flags & YOUSJ_O_TRUNC) && acc != YOUSJ_O_RDONLY)
            yfs_truncate((uint32_t)ino, 0);
    }
    f = file_alloc();
    if (f == NULL)
        return -YOUSJ_ENFILE;
    f->backend = 1;
    f->yfs_ino = (uint32_t)ino;
    yfs_note_open((uint32_t)ino);
    f->flags = flags;
    f->offset = 0;
    *out = f;
    return 0;
}

/* --- M25: /proc -- a generated, read-only filesystem -----
 * Three flat files whose contents are formatted from live
 * kernel counters. The content is snapshotted into the
 * file object at open: a reader sees one consistent
 * generation, and re-opening refreshes it (Linux readers
 * like cat open per read anyway). Writes and mutations
 * are refused with EACCES; the directory itself can be
 * listed and chdir'd into like any other.
 *
 * M28: per-task subdirectories. /proc/<pid> exists for
 * every live task (zombies included, until reaped) and
 * carries two generated leaves, status and stat. A pid
 * that names no live task is ENOENT everywhere, exactly
 * like a name that was never there. Generation resolves
 * the task afresh at open/stat time; a task that dies
 * mid-read simply leaves the opener with its snapshot. */
#define PROC_ID_UPTIME  1
#define PROC_ID_MEMINFO 2
#define PROC_ID_VERSION 3
#define PROC_ID_PIDDIR  4 /* M28 */
#define PROC_ID_PSTATUS 5 /* M28 */
#define PROC_ID_PSTAT   6 /* M28 */
#define PROC_ID_PCMDLINE 7 /* M31 */
#define PROC_ID_PENVIRON 8 /* M34 */
#define PROC_ID_PCWD     9 /* M35 */
#define PROC_ID_PEXE     10 /* M35 */
#define PROC_ID_PMAPS    11 /* M39 */
#define PROC_ID_PFDDIR   12 /* M41 */
#define PROC_ID_PFD      13 /* M41 */
#define PROC_ID_PFDINFODIR 14 /* M45 */
#define PROC_ID_PFDINFO  15 /* M45 */
static const char *const g_proc_names[3] = {
    "uptime", "meminfo", "version"
};

static bool is_proc_path(const char *path)
{
    return path[0] == '/' && path[1] == 'p' && path[2] == 'r' &&
           path[3] == 'o' && path[4] == 'c' &&
           (path[5] == '\0' || path[5] == '/');
}

static bool proc_is_dir_id(int id)
{
    return id == 0 || id == PROC_ID_PIDDIR ||
           id == PROC_ID_PFDDIR ||
           id == PROC_ID_PFDINFODIR;
}

/* Inode numbers: the flat files keep 9000+id; a pid entry
 * gets 10000 + pid*8 + leaf (0 dir, 1 status, 2 stat,
 * 3 cmdline, 4 environ). The stride grew at M34; the
 * numbers are generated per lookup, nothing persists. */
static uint64_t proc_ino_for(int id, uint32_t pid)
{
    if (id == PROC_ID_PIDDIR)
        return 10000 + (uint64_t)pid * 8;
    if (id == PROC_ID_PSTATUS)
        return 10000 + (uint64_t)pid * 8 + 1;
    if (id == PROC_ID_PSTAT)
        return 10000 + (uint64_t)pid * 8 + 2;
    if (id == PROC_ID_PCMDLINE)
        return 10000 + (uint64_t)pid * 8 + 3;
    if (id == PROC_ID_PENVIRON)
        return 10000 + (uint64_t)pid * 8 + 4;
    if (id == PROC_ID_PCWD)
        return 10000 + (uint64_t)pid * 8 + 5;
    if (id == PROC_ID_PEXE)
        return 10000 + (uint64_t)pid * 8 + 6;
    if (id == PROC_ID_PMAPS)
        return 10000 + (uint64_t)pid * 8 + 7;
    if (id == PROC_ID_PFDDIR)
        return 30000 + (uint64_t)pid;
    return 9000 + (uint64_t)id;
}

/* "" -> 0 (the directory), a known name -> its id,
 * "<pid>" / "<pid>/status" / "<pid>/stat" -> the M28 ids
 * (with *pid_out set), else -1. A numeric head that names
 * no live task is -1: from the outside it does not exist. */
static int proc_find2(const char *rel, uint32_t *pid_out,
                      int *fd_out);

static int proc_find(const char *rel, uint32_t *pid_out)
{
    return proc_find2(rel, pid_out, NULL);
}

static int proc_find2(const char *rel, uint32_t *pid_out,
                      int *fd_out)
{
    int i;
    *pid_out = 0;
    if (fd_out != NULL)
        *fd_out = -1;
    if (rel[0] == '\0')
        return 0;
    for (i = 0; i < 3; i++) {
        const char *n = g_proc_names[i];
        uint64_t j = 0;
        while (n[j] != '\0' && rel[j] == n[j])
            j++;
        if (n[j] == '\0' && rel[j] == '\0')
            return i + 1;
    }
    {
        uint64_t v = 0;
        uint64_t j = 0;
        while (rel[j] >= '0' && rel[j] <= '9') {
            v = v * 10 + (uint64_t)(rel[j] - '0');
            if (v > 0xFFFFFFu)
                return -1;
            j++;
        }
        if (j == 0)
            return -1;
        if (sched_find_task((int)v) == NULL)
            return -1;
        *pid_out = (uint32_t)v;
        if (rel[j] == '\0')
            return PROC_ID_PIDDIR;
        if (rel[j] != '/')
            return -1;
        {
            const char *leaf = rel + j + 1;
            if (leaf[0] == 's' && leaf[1] == 't' &&
                leaf[2] == 'a' && leaf[3] == 't' &&
                leaf[4] == 'u' && leaf[5] == 's' &&
                leaf[6] == '\0')
                return PROC_ID_PSTATUS;
            if (leaf[0] == 's' && leaf[1] == 't' &&
                leaf[2] == 'a' && leaf[3] == 't' &&
                leaf[4] == '\0')
                return PROC_ID_PSTAT;
            if (leaf[0] == 'c' && leaf[1] == 'm' &&
                leaf[2] == 'd' && leaf[3] == 'l' &&
                leaf[4] == 'i' && leaf[5] == 'n' &&
                leaf[6] == 'e' && leaf[7] == '\0')
                return PROC_ID_PCMDLINE;
            if (leaf[0] == 'e' && leaf[1] == 'n' &&
                leaf[2] == 'v' && leaf[3] == 'i' &&
                leaf[4] == 'r' && leaf[5] == 'o' &&
                leaf[6] == 'n' && leaf[7] == '\0')
                return PROC_ID_PENVIRON;
            if (leaf[0] == 'c' && leaf[1] == 'w' &&
                leaf[2] == 'd' && leaf[3] == '\0')
                return PROC_ID_PCWD;
            if (leaf[0] == 'e' && leaf[1] == 'x' &&
                leaf[2] == 'e' && leaf[3] == '\0')
                return PROC_ID_PEXE;
            if (leaf[0] == 'm' && leaf[1] == 'a' &&
                leaf[2] == 'p' && leaf[3] == 's' &&
                leaf[4] == '\0')
                return PROC_ID_PMAPS;
            if (leaf[0] == 'f' && leaf[1] == 'd' &&
                leaf[2] == '\0')
                return PROC_ID_PFDDIR;
            if (leaf[0] == 'f' && leaf[1] == 'd' &&
                leaf[2] == 'i' && leaf[3] == 'n' &&
                leaf[4] == 'f' && leaf[5] == 'o' &&
                leaf[6] == '\0')
                return PROC_ID_PFDINFODIR;
            if (leaf[0] == 'f' && leaf[1] == 'd' &&
                leaf[2] == 'i' && leaf[3] == 'n' &&
                leaf[4] == 'f' && leaf[5] == 'o' &&
                leaf[6] == '/') {
                const char *d = leaf + 7;
                uint64_t v = 0, j = 0;
                struct task *t;
                if (d[0] == '\0')
                    return -1;
                while (d[j] >= '0' && d[j] <= '9') {
                    v = v * 10 + (uint64_t)(d[j] - '0');
                    if (v >= YOUSJ_NR_OPEN)
                        return -1;
                    j++;
                }
                if (d[j] != '\0')
                    return -1;
                t = sched_find_task((int)*pid_out);
                if (t == NULL || t->fd_file[v] == NULL)
                    return -1;
                if (fd_out != NULL)
                    *fd_out = (int)v;
                return PROC_ID_PFDINFO;
            }
            if (leaf[0] == 'f' && leaf[1] == 'd' &&
                leaf[2] == '/') {
                /* M41: fd/<n> -- only while that fd is
                 * open in the target task. */
                const char *d = leaf + 3;
                uint64_t v = 0, j = 0;
                struct task *t;
                if (d[0] == '\0')
                    return -1;
                while (d[j] >= '0' && d[j] <= '9') {
                    v = v * 10 + (uint64_t)(d[j] - '0');
                    if (v >= YOUSJ_NR_OPEN)
                        return -1;
                    j++;
                }
                if (d[j] != '\0')
                    return -1;
                t = sched_find_task((int)*pid_out);
                if (t == NULL || t->fd_file[v] == NULL)
                    return -1;
                if (fd_out != NULL)
                    *fd_out = (int)v;
                return PROC_ID_PFD;
            }
        }
        return -1;
    }
}

/* M35: cwd and exe are magic links, not generated
 * files. Resolve one to its target path; -1 when the
 * task is gone or (for exe) never exec'd. The cwd of a
 * task that never chdir'd is "/". */
static long proc_link_target(int id, uint32_t pid,
                             char *buf, uint64_t cap)
{
    struct task *t = sched_find_task((int)pid);
    const char *src;
    uint64_t i = 0;
    if (t == NULL)
        return -1;
    if (id == PROC_ID_PCWD)
        src = t->cwd_path[0] != '\0' ? t->cwd_path : "/";
    else if (id == PROC_ID_PEXE) {
        if (t->exe_path[0] == '\0')
            return -1;
        src = t->exe_path;
    } else
        return -1;
    while (src[i] != '\0' && i + 1 < cap) {
        buf[i] = src[i];
        i++;
    }
    buf[i] = '\0';
    return (long)i;
}

/* M41: the target text of a /proc/<pid>/fd/<n> link.
 * -1 when the task or the fd is gone. */
static void str_cat(char *dst, const char *src)
{
    uint64_t i = 0, j = 0;
    while (dst[i] != '\0')
        i++;
    while (src[j] != '\0')
        dst[i++] = src[j++];
    dst[i] = '\0';
}

static void u32_dec(char *dst, uint32_t v)
{
    char tmp[12];
    int n = 0;
    if (v == 0) {
        dst[0] = '0';
        dst[1] = '\0';
        return;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    {
        int i;
        for (i = 0; i < n; i++)
            dst[i] = tmp[n - 1 - i];
        dst[n] = '\0';
    }
}

static long proc_fd_target(uint32_t pid, int fd, char *buf,
                           uint64_t cap)
{
    struct task *t = sched_find_task((int)pid);
    struct yousj_file *f;
    char num[12];
    uint64_t i;
    if (t == NULL || fd < 0 || fd >= YOUSJ_NR_OPEN)
        return -1;
    f = t->fd_file[fd];
    if (f == NULL)
        return -1;
    buf[0] = '\0';
    if (f->pipe != NULL) {
        str_cat(buf, "pipe:[");
        u32_dec(num, (uint32_t)(f->pipe - g_pipes));
        str_cat(buf, num);
        str_cat(buf, "]");
    } else if (f->epoll != NULL) {
        str_cat(buf, "anon_inode:[eventpoll]");
    } else if (f->backend == 2) {
        str_cat(buf, "/proc");
        if (f->proc_id == PROC_ID_PIDDIR ||
            (f->proc_id >= PROC_ID_PSTATUS &&
             f->proc_id <= PROC_ID_PFDDIR)) {
            str_cat(buf, "/");
            u32_dec(num, f->proc_pid);
            str_cat(buf, num);
            if (f->proc_id == PROC_ID_PSTATUS)
                str_cat(buf, "/status");
            else if (f->proc_id == PROC_ID_PSTAT)
                str_cat(buf, "/stat");
            else if (f->proc_id == PROC_ID_PCMDLINE)
                str_cat(buf, "/cmdline");
            else if (f->proc_id == PROC_ID_PENVIRON)
                str_cat(buf, "/environ");
            else if (f->proc_id == PROC_ID_PCWD)
                str_cat(buf, "/cwd");
            else if (f->proc_id == PROC_ID_PEXE)
                str_cat(buf, "/exe");
            else if (f->proc_id == PROC_ID_PMAPS)
                str_cat(buf, "/maps");
            else if (f->proc_id == PROC_ID_PFDDIR)
                str_cat(buf, "/fd");
        } else if (f->proc_id >= 1 && f->proc_id <= 3) {
            str_cat(buf, "/");
            str_cat(buf, g_proc_names[f->proc_id - 1]);
        }
    } else if (f->backend == 0 && f->node == NULL) {
        str_cat(buf, "/dev/console");
    } else {
        str_cat(buf, f->open_path);
    }
    if (buf[0] == '\0')
        return -1;
    (void)cap; /* targets are bounded by open_path's 256 */
    i = 0;
    while (buf[i] != '\0')
        i++;
    return (long)i;
}

struct pbuf {
    char *buf;
    uint32_t cap;
    uint32_t len;
};
static void pb_puts(struct pbuf *b, const char *s)
{
    while (*s != '\0' && b->len < b->cap) {
        b->buf[b->len] = *s;
        b->len++;
        s++;
    }
}
static void pb_putu(struct pbuf *b, uint64_t v)
{
    char tmp[20];
    int n = 0;
    if (v == 0) {
        pb_puts(b, "0");
        return;
    }
    while (v > 0) {
        tmp[n] = (char)('0' + (v % 10));
        n++;
        v /= 10;
    }
    while (n > 0) {
        n--;
        if (b->len < b->cap) {
            b->buf[b->len] = tmp[n];
            b->len++;
        }
    }
}

/* M39: hex for maps (fixed-width, zero-padded). */
static void pb_hex(struct pbuf *b, uint64_t v, int width)
{
    char tmp[16];
    int i;
    for (i = 0; i < width; i++) {
        uint32_t d = (uint32_t)(v & 0xF);
        tmp[width - 1 - i] = (char)(d < 10 ? '0' + d
                                          : 'a' + (d - 10));
        v >>= 4;
    }
    for (i = 0; i < width; i++) {
        char one[2];
        one[0] = tmp[i];
        one[1] = '\0';
        pb_puts(b, one);
    }
}

static void pb_maps_line(struct pbuf *b, uint64_t lo,
                         uint64_t hi, const char *perms,
                         uint64_t off, const char *path)
{
    pb_hex(b, lo, 12);
    pb_puts(b, "-");
    pb_hex(b, hi, 12);
    pb_puts(b, " ");
    pb_puts(b, perms);
    pb_puts(b, " ");
    pb_hex(b, off, 8);
    pb_puts(b, " 00:00 0");
    if (path != NULL && path[0] != '\0') {
        pb_puts(b, " ");
        pb_puts(b, path);
    }
    pb_puts(b, "\n");
}
/* "<seconds>.<centiseconds>" for a tick count */
static void pb_put_ticks(struct pbuf *b, uint64_t ticks)
{
    uint64_t cs = ticks % 100; /* 100 Hz: one tick is 1 cs */
    pb_putu(b, ticks / 100);
    pb_puts(b, ".");
    if (cs < 10)
        pb_puts(b, "0");
    pb_putu(b, cs);
}

/* Same letters the ps snapshot prints (sched.c). */
static char proc_state_char(const struct task *t)
{
    if (t->sig_stopped)
        return 'T';
    switch (t->state) {
    case TASK_RUNNING: return 'R';
    case TASK_READY:   return 'r';
    case TASK_BLOCKED: return 'S';
    case TASK_ZOMBIE:  return 'Z';
    case TASK_DEAD:    return 'D';
    }
    return '?';
}

static const char *proc_state_word(char c)
{
    switch (c) {
    case 'R': return "Running";
    case 'r': return "Ready";
    case 'S': return "Sleeping";
    case 'Z': return "Zombie";
    case 'D': return "Dead";
    case 'T': return "Stopped";
    default:  return "Unknown";
    }
}

static void pb_oct(struct pbuf *b, uint32_t v)
{
    char tmp[12];
    int n = 0, i;
    if (v == 0) {
        pb_puts(b, "0");
        return;
    }
    while (v > 0) {
        tmp[n++] = (char)('0' + (v & 7u));
        v >>= 3;
    }
    for (i = n - 1; i >= 0; i--) {
        char one[2];
        one[0] = tmp[i];
        one[1] = '\0';
        pb_puts(b, one);
    }
}

static uint32_t proc_generate(int id, uint32_t pid, int pfd,
                              char *buf, uint32_t cap)
{
    struct pbuf b;
    b.buf = buf;
    b.cap = cap;
    b.len = 0;
    switch (id) {
    case PROC_ID_UPTIME:
        pb_put_ticks(&b, sched_uptime_ticks());
        pb_puts(&b, " ");
        pb_put_ticks(&b, sched_idle_ticks());
        pb_puts(&b, "\n");
        break;
    case PROC_ID_MEMINFO:
        pb_puts(&b, "MemTotal:       ");
        pb_putu(&b, pmm_total_page_count() * 4);
        pb_puts(&b, " kB\nMemFree:        ");
        pb_putu(&b, pmm_free_page_count() * 4);
        pb_puts(&b, " kB\nMemAvailable:   ");
        pb_putu(&b, pmm_free_page_count() * 4);
        pb_puts(&b, " kB\n");
        break;
    case PROC_ID_VERSION:
        pb_puts(&b, "Yousj OS version 0.9 (M45) x86_64 #1\n");
        break;
    case PROC_ID_PCMDLINE: {
        /* Raw bytes, not text: the argv is NUL-joined, so
         * this case copies instead of pb_puts-ing. */
        struct task *t = sched_find_task((int)pid);
        uint32_t i, n;
        if (t == NULL)
            break;
        n = t->cmdline_len;
        if (n > b.cap)
            n = b.cap;
        for (i = 0; i < n; i++)
            b.buf[b.len++] = t->cmdline[i];
        break;
    }
    case PROC_ID_PENVIRON: {
        /* M34: same raw-bytes treatment for the envp. */
        struct task *t = sched_find_task((int)pid);
        uint32_t i, n;
        if (t == NULL)
            break;
        n = t->env_len;
        if (n > b.cap)
            n = b.cap;
        for (i = 0; i < n; i++)
            b.buf[b.len++] = t->env[i];
        break;
    }
    case PROC_ID_PFDINFO: {
        struct task *t = sched_find_task((int)pid);
        struct yousj_file *tf =
            (t != NULL && pfd >= 0 && pfd < YOUSJ_NR_OPEN)
                ? t->fd_file[pfd]
                : NULL;
        struct yousj_stat_info si;
        if (tf == NULL)
            break;
        pb_puts(&b, "pos:\t");
        pb_putu(&b, tf->offset);
        pb_puts(&b, "\nflags:\t");
        pb_oct(&b, (uint32_t)tf->flags);
        pb_puts(&b, "\nmnt_id:\t");
        pb_putu(&b, tf->backend == 1 ? 2
                                     : (tf->backend == 0 &&
                                                tf->node != NULL
                                            ? 1
                                            : 0));
        pb_puts(&b, "\nino:\t");
        pb_putu(&b, fs_file_stat(tf, &si) ? si.ino : 0);
        pb_puts(&b, "\n");
        break;
    }
    case PROC_ID_PMAPS: {
        /* M39: the address space in Linux maps shape.
         * The image is one line (its segments are not
         * retained separately, so the writable tail
         * rides in the r-xp range -- documented); heap
         * only when the break has moved; the stack is
         * the four pages exec committed; then the VMA
         * list in slot order, with a file path when the
         * mapping is file-backed. */
        struct task *t = sched_find_task((int)pid);
        int i;
        if (t == NULL || t->mm == NULL ||
            t->exe_path[0] == '\0')
            break;
        pb_maps_line(&b, YOUSJ_USER_BASE, t->brk_start,
                     "r-xp", 0, t->exe_path);
        if (t->brk_end > t->brk_start)
            pb_maps_line(&b, t->brk_start, t->brk_end,
                         "rw-p", 0, "[heap]");
        pb_maps_line(&b,
                     YOUSJ_INIT_STACK_TOP - 4 * 4096,
                     YOUSJ_INIT_STACK_TOP, "rw-p", 0,
                     "[stack]");
        for (i = 0; i < YOUSJ_MAX_VMA; i++) {
            struct yousj_vma *v = &t->vmas[i];
            char perm[5];
            if (!v->used)
                continue;
            perm[0] = (v->prot & 1) ? 'r' : '-';
            perm[1] = (v->prot & 2) ? 'w' : '-';
            perm[2] = (v->prot & 4) ? 'x' : '-';
            perm[3] = v->shared ? 's' : 'p';
            perm[4] = '\0';
            pb_maps_line(&b, v->base, v->base + v->len,
                         perm, v->file_off,
                         v->file != NULL
                             ? v->file->open_path
                             : "");
        }
        break;
    }
    case PROC_ID_PSTATUS: {
        struct task *t = sched_find_task((int)pid);
        char c;
        if (t == NULL)
            break;
        c = proc_state_char(t);
        pb_puts(&b, "Name:\t");
        pb_puts(&b, t->name);
        pb_puts(&b, "\nPid:\t");
        pb_putu(&b, pid);
        pb_puts(&b, "\nPPid:\t");
        pb_putu(&b, t->parent_tid);
        pb_puts(&b, "\nState:\t");
        {
            char cs[2];
            cs[0] = c;
            cs[1] = '\0';
            pb_puts(&b, cs);
        }
        pb_puts(&b, " (");
        pb_puts(&b, proc_state_word(c));
        pb_puts(&b, ")\n");
        break;
    }
    case PROC_ID_PSTAT: {
        /* Linux field order; the four fields we know are
         * real, the rest are zero placeholders. */
        struct task *t = sched_find_task((int)pid);
        int i;
        if (t == NULL)
            break;
        pb_putu(&b, pid);
        pb_puts(&b, " (");
        pb_puts(&b, t->name);
        pb_puts(&b, ") ");
        {
            char cs[2];
            cs[0] = proc_state_char(t);
            cs[1] = '\0';
            pb_puts(&b, cs);
        }
        pb_puts(&b, " ");
        pb_putu(&b, t->parent_tid);
        for (i = 0; i < 20; i++)
            pb_puts(&b, " 0");
        pb_puts(&b, "\n");
        break;
    }
    default:
        break;
    }
    return b.len;
}

static long proc_open(const char *path, long flags,
                      struct yousj_file **out)
{
    const char *rel = path + 5;
    long acc = flags & YOUSJ_O_ACCMODE;
    struct yousj_file *f;
    int id;
    int fdn = -1;
    uint32_t pid = 0;
    if (rel[0] == '/')
        rel++;
    id = proc_find2(rel, &pid, &fdn);
    if (id < 0)
        return -YOUSJ_ENOENT_L;
    if (proc_is_dir_id(id)) {
        if (acc != YOUSJ_O_RDONLY)
            return -YOUSJ_EISDIR;
    } else {
        if (acc != YOUSJ_O_RDONLY)
            return -YOUSJ_EACCES_L;
    }
    f = file_alloc();
    if (f == NULL)
        return -YOUSJ_ENFILE;
    f->backend = 2;
    f->proc_id = id;
    f->proc_pid = pid;
    f->proc_fd = fdn;
    f->flags = flags;
    f->offset = 0;
    if (!proc_is_dir_id(id))
        f->proc_len = proc_generate(id, pid, fdn,
                                    f->proc_buf,
                                    sizeof(f->proc_buf));
    *out = f;
    return 0;
}

long fs_open_path(const char *path, long flags,
                  uint32_t mode_bits, struct yousj_file **out)
{
    char resolved[PATH_MAX_LEN + 1];
    long rc;
    /* M35: opening a magic link opens its target. */
    if (is_proc_path(path)) {
        const char *rel = path + 5;
        uint32_t pid = 0;
        int id;
        if (rel[0] == '/')
            rel++;
        {
            int fdn = -1;
            id = proc_find2(rel, &pid, &fdn);
            if (id == PROC_ID_PCWD || id == PROC_ID_PEXE) {
                char target[PATH_MAX_LEN + 1];
                if (proc_link_target(id, pid, target,
                                      sizeof(target)) < 0)
                    return -YOUSJ_ENOENT_L;
                return fs_open_path(target, flags, mode_bits,
                                    out);
            }
            if (id == PROC_ID_PFD) {
                /* M41: opening an fd link re-opens its
                 * target path (fresh offset, like Linux
                 * for regular files). Non-path targets
                 * (pipes, the console, epoll) cannot be
                 * re-opened -- EINVAL. */
                char target[PATH_MAX_LEN + 1];
                if (proc_fd_target(pid, fdn, target,
                                    sizeof(target)) < 0)
                    return -YOUSJ_ENOENT_L;
                if (target[0] != '/')
                    return -YOUSJ_EINVAL;
                return fs_open_path(target, flags, mode_bits,
                                    out);
            }
        }
    }
    rc = fs_resolve_links(path, resolved, true);
    if (rc != 0)
        return rc;
    path = resolved;
    if (is_data_path(path))
        return yfs_open_path_fs(data_rel(path), flags,
                                mode_bits, out);
    if (is_proc_path(path))
        return proc_open(path, flags, out);
    return ramfs_open_path(path, flags, mode_bits, out);
}

long fs_mkdir_path(const char *path, uint32_t mode_bits)
{
    char resolved[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(path, resolved, false);
    if (rc != 0)
        return rc;
    path = resolved;
    if (is_proc_path(path))
        return -YOUSJ_EACCES_L;
    if (is_data_path(path)) {
        char parent[128], name[40];
        long ino;
        split_parent_name(data_rel(path), parent, name);
        ino = yfs_mknod(parent, name, true);
        if (ino >= 0 && mode_bits != 0)
            yfs_set_mode((uint32_t)ino, mode_bits);
        if (ino >= 0)
            inotify_child_path(path, IN_CREATE_K, 0,
                               true); /* M48 */
        return ino >= 0 ? 0 : ino;
    }
    rc = ramfs_mkdir_path(path);
    if (rc == 0 && mode_bits != 0) {
        struct yousj_ramfs_node *n = fs_lookup(path);
        if (n != NULL)
            n->mode = mode_bits;
    }
    if (rc == 0)
        inotify_child_path(path, IN_CREATE_K, 0,
                           true); /* M48 */
    return rc;
}

long fs_unlink_path(const char *path)
{
    char resolved[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(path, resolved, false);
    if (rc != 0)
        return rc;
    path = resolved;
    if (is_proc_path(path))
        return -YOUSJ_EACCES_L;
    if (is_data_path(path)) {
        char parent[128], name[40];
        struct target_id self;
        long urc;
        inotify_capture(path, &self);
        split_parent_name(data_rel(path), parent, name);
        {
            bool last = self.ok &&
                        yfs_nlink(self.ino) <= 1;
            urc = yfs_unlink(parent, name, false);
            if (urc == 0) {
                inotify_child_path(path, IN_DELETE_K, 0,
                                   false); /* M48 */
                if (last)
                    inotify_self_id(1, NULL, self.ino,
                                    IN_DELETE_SELF_K, false);
            }
        }
        return urc;
    }
    {
        char base[NAME_MAX_LEN + 1];
        struct yousj_ramfs_node *parent = walk(path, true, base);
        struct yousj_ramfs_node *child;
        if (parent == NULL)
            return -YOUSJ_ENOENT;
        {
            struct yousj_dent *dent = node_find_dent(
                parent, base, str_len(base));
            if (dent == NULL)
                return -YOUSJ_ENOENT;
            child = dent->node;
            if (child->is_dir)
                return -YOUSJ_EISDIR;
            {
                bool last = child->nlink <= 1;
                dent_detach(dent);
                dent_free(dent);
                node_unname(child);
                inotify_child_id(0, parent, 0, base,
                                 IN_DELETE_K, 0,
                                 false); /* M48 */
                if (last)
                    inotify_self_id(0, child, 0,
                                    IN_DELETE_SELF_K, false);
            }
        }
        return 0;
    }
}

long fs_rmdir_path(const char *path)
{
    char resolved[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(path, resolved, false);
    if (rc != 0)
        return rc;
    path = resolved;
    if (is_proc_path(path))
        return -YOUSJ_EACCES_L;
    if (is_data_path(path)) {
        char parent[128], name[40];
        struct target_id self;
        long urc;
        inotify_capture(path, &self);
        split_parent_name(data_rel(path), parent, name);
        urc = yfs_unlink(parent, name, true);
        if (urc == 0) {
            inotify_child_path(path, IN_DELETE_K, 0,
                               true); /* M48 */
            if (self.ok)
                inotify_self_id(1, NULL, self.ino,
                                IN_DELETE_SELF_K, true);
        }
        return urc;
    }
    {
        char base[NAME_MAX_LEN + 1];
        struct yousj_ramfs_node *parent = walk(path, true, base);
        struct yousj_ramfs_node *child;
        if (parent == NULL)
            return -YOUSJ_ENOENT;
        {
            struct yousj_dent *dent = node_find_dent(
                parent, base, str_len(base));
            if (dent == NULL)
                return -YOUSJ_ENOENT;
            child = dent->node;
            if (!child->is_dir)
                return -YOUSJ_ENOTDIR;
            if (child == g_root)
                return -YOUSJ_EINVAL;
            if (child->dents != NULL)
                return -YOUSJ_ENOTEMPTY_L;
            dent_detach(dent);
            dent_free(dent);
            node_unname(child);
            inotify_child_id(0, parent, 0, base, IN_DELETE_K,
                             0, true); /* M48 */
            inotify_self_id(0, child, 0, IN_DELETE_SELF_K,
                            true);
        }
        return 0;
    }
}

long fs_rename_path(const char *old_path, const char *new_path)
{
    char ro[PATH_MAX_LEN + 1], rn[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(old_path, ro, false);
    if (rc != 0)
        return rc;
    rc = fs_resolve_links(new_path, rn, false);
    if (rc != 0)
        return rc;
    old_path = ro;
    new_path = rn;
    if (is_proc_path(old_path) || is_proc_path(new_path))
        return -YOUSJ_EACCES_L;
    bool od = is_data_path(old_path), nd = is_data_path(new_path);
    char obase[NAME_MAX_LEN + 1], nbase[NAME_MAX_LEN + 1];
    struct yousj_ramfs_node *oparent, *nparent, *src, *dst, *a;
    uint64_t nlen;

    if (od != nd)
        return -YOUSJ_EXDEV_L;
    if (od) {
        struct target_id srcid, dstid;
        long rrc;
        inotify_capture(old_path, &srcid);
        inotify_capture(new_path, &dstid);
        rrc = yfs_rename(data_rel(old_path),
                         data_rel(new_path));
        if (rrc == 0) {
            uint32_t cookie = ++g_inotify_cookie;
            bool isdir = srcid.ok && srcid.is_dir;
            inotify_child_path(old_path, IN_MOVED_FROM_K,
                               cookie, isdir); /* M48 */
            inotify_child_path(new_path, IN_MOVED_TO_K,
                               cookie, isdir);
            if (srcid.ok)
                inotify_self_id(1, NULL, srcid.ino,
                                IN_MOVE_SELF_K, isdir);
            if (dstid.ok &&
                !(srcid.ok && dstid.ino == srcid.ino))
                inotify_self_id(1, NULL, dstid.ino,
                                IN_DELETE_SELF_K,
                                dstid.is_dir);
        }
        return rrc;
    }
    oparent = walk(old_path, true, obase);
    if (oparent == NULL)
        return -YOUSJ_ENOENT;
    src = node_find_child(oparent, obase, str_len(obase));
    if (src == NULL)
        return -YOUSJ_ENOENT;
    if (src == g_root)
        return -YOUSJ_EINVAL;
    nlen = str_len(new_path);
    (void)nlen;
    nparent = walk(new_path, true, nbase);
    if (nparent == NULL)
        return -YOUSJ_ENOENT;
    if (str_len(nbase) > NAME_MAX_LEN)
        return -YOUSJ_ENAMETOOLONG_L;
    if (src->is_dir) {
        /* the new parent may not be src or inside src */
        for (a = nparent; a != NULL; a = a->parent) {
            if (a == src)
                return -YOUSJ_EINVAL;
            if (a->parent == a)
                break;
        }
    }
    {
        struct yousj_dent *sdent = node_find_dent(
            oparent, obase, str_len(obase));
        struct yousj_dent *ddent = node_find_dent(
            nparent, nbase, str_len(nbase));
        struct yousj_ramfs_node *victim = NULL;
        bool victim_last = false;
        if (ddent != NULL && ddent->node == src)
            return 0;
        if (ddent != NULL) {
            dst = ddent->node;
            if (dst->is_dir) {
                if (!src->is_dir)
                    return -YOUSJ_EISDIR;
                if (dst->dents != NULL)
                    return -YOUSJ_ENOTEMPTY_L;
            } else if (src->is_dir) {
                return -YOUSJ_ENOTDIR;
            }
            victim = dst;
            victim_last = dst->nlink <= 1;
            dent_detach(ddent);
            dent_free(ddent);
            node_unname(dst);
        }
        /* M44: the dent itself moves and takes the new
         * name; the node (and its other names) never
         * notices. */
        dent_detach(sdent);
        mem_copy((uint8_t *)sdent->name,
                 (const uint8_t *)nbase, str_len(nbase) + 1);
        dent_attach(nparent, sdent);
        if (src->is_dir)
            src->parent = nparent;
        {
            uint32_t cookie = ++g_inotify_cookie;
            inotify_child_id(0, oparent, 0, obase,
                             IN_MOVED_FROM_K, cookie,
                             src->is_dir); /* M48 */
            inotify_child_id(0, nparent, 0, nbase,
                             IN_MOVED_TO_K, cookie,
                             src->is_dir);
            inotify_self_id(0, src, 0, IN_MOVE_SELF_K,
                            src->is_dir);
            if (victim != NULL && victim_last)
                inotify_self_id(0, victim, 0,
                                IN_DELETE_SELF_K,
                                victim->is_dir);
        }
    }
    src->ctime = (uint32_t)yousj_now_unix(); /* M26 */
    return 0;
}

/* M44: hard links. The source is resolved with the
 * caller's follow choice (linkat's AT_SYMLINK_FOLLOW);
 * without it a symlink source links the symlink
 * itself. Directories refuse with EPERM on both
 * backends; cross-backend is EXDEV. */
long fs_link_path(const char *old_path, const char *new_path,
                  bool follow)
{
    char ro[PATH_MAX_LEN + 1], rn[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(old_path, ro, follow);
    if (rc != 0)
        return rc;
    rc = fs_resolve_links(new_path, rn, false);
    if (rc != 0)
        return rc;
    if (is_proc_path(ro) || is_proc_path(rn))
        return -YOUSJ_EACCES_L;
    {
        bool od = is_data_path(ro), nd = is_data_path(rn);
        long lrc;
        if (od != nd)
            return -YOUSJ_EXDEV_L;
        if (od) {
            lrc = yfs_link(data_rel(ro), data_rel(rn));
            if (lrc == 0)
                inotify_child_path(rn, IN_CREATE_K, 0,
                                   false); /* M48 */
            return lrc;
        }
    }
    {
        struct yousj_ramfs_node *src = fs_lookup(ro);
        char base[NAME_MAX_LEN + 1];
        struct yousj_ramfs_node *parent;
        struct yousj_dent *d;
        if (src == NULL)
            return -YOUSJ_ENOENT_L;
        if (src->is_dir)
            return -1; /* EPERM */
        parent = walk(rn, true, base);
        if (parent == NULL)
            return -YOUSJ_ENOENT_L;
        if (node_find_child(parent, base, str_len(base)) !=
            NULL)
            return -YOUSJ_EEXIST_L;
        d = dent_alloc();
        if (d == NULL)
            return -YOUSJ_ENOSPC;
        mem_copy((uint8_t *)d->name, (const uint8_t *)base,
                 str_len(base) + 1);
        d->node = src;
        src->nlink++;
        src->ctime = (uint32_t)yousj_now_unix();
        dent_attach(parent, d);
        inotify_child_id(0, parent, 0, base, IN_CREATE_K, 0,
                         false); /* M48 */
        return 0;
    }
}

/* M46: RENAME_EXCHANGE. Both paths must exist; the
 * names trade objects. Exchanging a directory with
 * its own descendant (or vice versa) is EINVAL,
 * checked on the resolved path strings; a directory
 * also may not trade with the root. */
static bool path_is_ancestor(const char *a, const char *b)
{
    uint64_t i = 0;
    if (a[0] == '/' && a[1] == '\0')
        return b[0] == '/' && b[1] != '\0';
    while (a[i] != '\0' && a[i] == b[i])
        i++;
    return a[i] == '\0' && b[i] == '/';
}

long fs_exchange_path(const char *a_path, const char *b_path)
{
    char ra[PATH_MAX_LEN + 1], rb[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(a_path, ra, false);
    if (rc != 0)
        return rc;
    rc = fs_resolve_links(b_path, rb, false);
    if (rc != 0)
        return rc;
    if (is_proc_path(ra) || is_proc_path(rb))
        return -YOUSJ_EACCES_L;
    {
        bool ad = is_data_path(ra), bd = is_data_path(rb);
        uint64_t i = 0;
        bool same = true;
        while (ra[i] != '\0' || rb[i] != '\0') {
            if (ra[i] != rb[i]) {
                same = false;
                break;
            }
            i++;
        }
        if (same)
            return 0;
        if (path_is_ancestor(ra, rb) ||
            path_is_ancestor(rb, ra))
            return -YOUSJ_EINVAL;
        if (ad != bd)
            return -YOUSJ_EXDEV_L;
        if (ad)
            return yfs_exchange(data_rel(ra), data_rel(rb));
    }
    {
        char abase[NAME_MAX_LEN + 1], bbase[NAME_MAX_LEN + 1];
        struct yousj_ramfs_node *ap = walk(ra, true, abase);
        struct yousj_ramfs_node *bp = walk(rb, true, bbase);
        struct yousj_dent *da, *db;
        struct yousj_ramfs_node *na, *nb;
        if (ap == NULL || bp == NULL)
            return -YOUSJ_ENOENT_L;
        da = node_find_dent(ap, abase, str_len(abase));
        db = node_find_dent(bp, bbase, str_len(bbase));
        if (da == NULL || db == NULL)
            return -YOUSJ_ENOENT_L;
        na = da->node;
        nb = db->node;
        if (na == g_root || nb == g_root)
            return -YOUSJ_EINVAL;
        da->node = nb;
        db->node = na;
        if (na->is_dir)
            na->parent = bp;
        if (nb->is_dir)
            nb->parent = ap;
        na->ctime = (uint32_t)yousj_now_unix();
        nb->ctime = na->ctime;
        return 0;
    }
}

long fs_symlink_path(const char *target, const char *linkpath)
{
    char resolved[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(linkpath, resolved, false);
    if (rc != 0)
        return rc;
    if (str_len(target) > PATH_MAX_LEN)
        return -YOUSJ_ENAMETOOLONG_L;
    if (is_proc_path(resolved))
        return -YOUSJ_EACCES_L;
    if (is_data_path(resolved)) {
        char parent[128], name[40];
        split_parent_name(data_rel(resolved), parent, name);
        rc = yfs_symlink(parent, name, target);
        if (rc >= 0)
            inotify_child_path(resolved, IN_CREATE_K, 0,
                               false); /* M48 */
        return rc >= 0 ? 0 : rc;
    }
    {
        char base[NAME_MAX_LEN + 1];
        struct yousj_ramfs_node *parent = walk(resolved, true,
                                               base);
        struct yousj_ramfs_node *n;
        if (parent == NULL)
            return -YOUSJ_ENOENT;
        if (node_find_child(parent, base, str_len(base)) != NULL)
            return -YOUSJ_EEXIST_L;
        n = node_create(parent, base, str_len(base), false);
        if (n == NULL)
            return -YOUSJ_ENOSPC;
        n->is_link = true;
        n->mode = 0777; /* M36 */
        if (node_pwrite(n, 0, (const uint8_t *)target,
                        str_len(target)) < 0)
            return -YOUSJ_ENOSPC;
        inotify_child_id(0, parent, 0, base, IN_CREATE_K, 0,
                         false); /* M48 */
        return 0;
    }
}

long fs_readlink_path(const char *path, char *buf, uint64_t cap)
{
    char resolved[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(path, resolved, false);
    if (rc != 0)
        return rc;
    /* M35: /proc magic links; every other /proc entry is
     * not a link at all (EINVAL, as on Linux). */
    if (is_proc_path(resolved)) {
        const char *rel = resolved + 5;
        uint32_t pid = 0;
        int id;
        if (rel[0] == '/')
            rel++;
        {
            int fdn = -1;
            id = proc_find2(rel, &pid, &fdn);
            if (id == PROC_ID_PFD) {
                char target[PATH_MAX_LEN + 1];
                long tl = proc_fd_target(pid, fdn, target,
                                          sizeof(target));
                uint64_t i, n;
                if (tl < 0)
                    return -YOUSJ_ENOENT_L;
                n = (uint64_t)tl < cap ? (uint64_t)tl : cap;
                for (i = 0; i < n; i++)
                    buf[i] = target[i];
                return (long)n;
            }
        }
        if (id < 0)
            return -YOUSJ_ENOENT_L;
        if (id == PROC_ID_PCWD || id == PROC_ID_PEXE) {
            char target[PATH_MAX_LEN + 1];
            long tl = proc_link_target(id, pid, target,
                                        sizeof(target));
            uint64_t i, n;
            if (tl < 0)
                return -YOUSJ_ENOENT_L;
            n = (uint64_t)tl < cap ? (uint64_t)tl : cap;
            for (i = 0; i < n; i++)
                buf[i] = target[i];
            return (long)n;
        }
        return -YOUSJ_EINVAL;
    }
    if (is_data_path(resolved)) {
        long ino = yfs_lookup(data_rel(resolved));
        if (ino < 0)
            return ino;
        if (yfs_type_of((uint32_t)ino) != 3)
            return -YOUSJ_EINVAL;
        return yfs_pread((uint32_t)ino, 0, (uint8_t *)buf,
                         (uint32_t)cap);
    }
    {
        struct yousj_ramfs_node *n = fs_lookup(resolved);
        if (n == NULL)
            return -YOUSJ_ENOENT;
        if (!n->is_link)
            return -YOUSJ_EINVAL;
        return node_pread(n, 0, (uint8_t *)buf, cap);
    }
}

long fs_read_all_path(const char *path, uint8_t *dst, uint64_t cap)
{
    char resolved[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(path, resolved, true);
    if (rc != 0)
        return rc;
    path = resolved;
    if (is_data_path(path)) {
        long ino = yfs_lookup(data_rel(path));
        uint32_t size;
        if (ino < 0)
            return ino;
        if (yfs_is_dir((uint32_t)ino))
            return -YOUSJ_EISDIR;
        size = yfs_size((uint32_t)ino);
        if (size > cap)
            return -YOUSJ_ENOMEM_L;
        if (yfs_pread((uint32_t)ino, 0, dst, size) != (long)size)
            return -1;
        return (long)size;
    }
    {
        struct yousj_ramfs_node *n = fs_lookup(path);
        if (n == NULL)
            return -YOUSJ_ENOENT;
        if (n->is_dir)
            return -YOUSJ_EISDIR;
        if (n->size > cap)
            return -YOUSJ_ENOMEM_L;
        return fs_node_read_all(n, dst, n->size);
    }
}

/* --- M20: stat information -------------------------------- */

#define S_IFREG_K 0100000u
#define S_IFDIR_K 0040000u
#define S_IFLNK_K 0120000u
#define S_IFIFO_K 0010000u
#define S_IFCHR_K 0020000u

static void stat_from_ramfs_node(struct yousj_ramfs_node *n,
                                 struct yousj_stat_info *info)
{
    info->mtime = n->mtime;
    info->atime = n->atime;
    info->ctime = n->ctime;
    if (n->dev != 0) { /* M47 */
        info->mode = S_IFCHR_K | (n->mode & 07777u);
        info->size = 0;
    } else if (n->is_link) {
        info->mode = S_IFLNK_K | (n->mode & 07777u);
        info->size = n->size;
    } else if (n->is_dir) {
        info->mode = S_IFDIR_K | (n->mode & 07777u);
        info->size = 0;
    } else {
        info->mode = S_IFREG_K | (n->mode & 07777u);
        info->size = n->size;
    }
    info->ino = (uint64_t)(n - g_nodes) + 2;
    if (n->is_dir) {
        /* Linux convention: 2 + subdirectories */
        uint32_t subs = 0;
        struct yousj_dent *d;
        for (d = n->dents; d != NULL; d = d->next)
            if (d->node->is_dir)
                subs++;
        info->nlink = 2 + subs;
    } else {
        info->nlink = n->nlink;
    }
}

static bool stat_from_yfs_ino(uint32_t ino,
                              struct yousj_stat_info *info)
{
    int t = yfs_type_of(ino);
    /* M29: format v4 stores all three independently.
     * Fetch through uint32_t locals: info's fields are
     * uint64_t, and taking their addresses as uint32_t*
     * would store only the low halves, leaving the high
     * halves as stack garbage (that exact bug cost a
     * debugging round -- the values "looked right" in
     * every uint32-truncated debug print). */
    {
        uint32_t a, m, c;
        yfs_times(ino, &a, &m, &c);
        info->atime = a;
        info->mtime = m;
        info->ctime = c;
    }
    if (t == 3) {
        info->mode = S_IFLNK_K | yfs_mode(ino);
        info->size = yfs_size(ino);
    } else if (t == 2) {
        info->mode = S_IFDIR_K | yfs_mode(ino);
        info->size = 0;
    } else if (t == 1) {
        info->mode = S_IFREG_K | yfs_mode(ino);
        info->size = yfs_size(ino);
    } else {
        return false;
    }
    info->ino = (uint64_t)ino + 1000000u;
    info->nlink = t == 2 ? 2 : yfs_nlink(ino);
    return true;
}

bool fs_path_stat2(const char *path, bool follow,
                   struct yousj_stat_info *info)
{
    char resolved[PATH_MAX_LEN + 1];
    if (fs_resolve_links(path, resolved, follow) != 0)
        return false;
    path = resolved;
    if (is_data_path(path)) {
        long ino = yfs_lookup(data_rel(path));
        if (ino < 0)
            return false;
        return stat_from_yfs_ino((uint32_t)ino, info);
    }
    if (is_proc_path(path)) {
        const char *rel = path + 5;
        int id;
        int pfdn = -1;
        uint32_t pid = 0;
        if (rel[0] == '/')
            rel++;
        id = proc_find2(rel, &pid, &pfdn);
        if (id < 0)
            return false;
        /* M35: the magic links either report themselves
         * (lstat) or the target's stat (follow). M41:
         * fd links join them, with non-path targets
         * reported by kind when followed. */
        if (id == PROC_ID_PCWD || id == PROC_ID_PEXE ||
            id == PROC_ID_PFD) {
            char target[PATH_MAX_LEN + 1];
            long tl;
            int fdn = -1;
            if (id == PROC_ID_PFD) {
                proc_find2(rel, &pid, &fdn);
                tl = proc_fd_target(pid, fdn, target,
                                     sizeof(target));
            } else {
                tl = proc_link_target(id, pid, target,
                                      sizeof(target));
            }
            if (tl < 0)
                return false;
            if (follow) {
                if (target[0] == '/' &&
                    fs_path_stat2(target, true, info))
                    return true;
                if (target[0] == '/' &&
                    !(target[1] == 'd' && target[2] == 'e' &&
                      target[3] == 'v'))
                    return false; /* a real path that is
                                     gone: no stat to give */
                /* pipes, the console, anon inodes */
                info->mtime = (uint32_t)yousj_now_unix();
                info->atime = info->mtime;
                info->ctime = info->mtime;
                info->ino = 20000 + (uint64_t)pid * 16 +
                            (uint64_t)(fdn < 0 ? 0 : fdn);
                info->mode = target[0] == 'p'
                                 ? (S_IFIFO_K | 0644u)
                                 : target[1] == 'd'
                                       ? (S_IFCHR_K | 0666u)
                                       : (S_IFREG_K | 0644u);
                info->size = 0;
                return true;
            }
            info->mtime = (uint32_t)yousj_now_unix();
            info->atime = info->mtime;
            info->ctime = info->mtime;
            info->ino = id == PROC_ID_PFD
                            ? 20000 + (uint64_t)pid * 16 +
                                  (uint64_t)(fdn < 0 ? 0 : fdn)
                            : proc_ino_for(id, pid);
            info->mode = S_IFLNK_K | 0777u;
            info->size = (uint32_t)tl;
            return true;
        }
        info->mtime = (uint32_t)yousj_now_unix();
        info->atime = info->mtime;
        info->ctime = info->mtime;
        info->ino = proc_ino_for(id, pid);
        if (id == PROC_ID_PFDINFODIR)
            info->ino = 40000 + (uint64_t)pid;
        if (id == PROC_ID_PFDINFO)
            info->ino = 50000 + (uint64_t)pid * 16 +
                        (uint64_t)(pfdn < 0 ? 0 : pfdn);
        if (proc_is_dir_id(id)) {
            info->mode = S_IFDIR_K | 0555u;
            info->size = 0;
        } else {
            char scratch[640]; /* M34/35: fit a full environ */
            info->mode = S_IFREG_K | 0444u;
            info->size = proc_generate(id, pid, pfdn,
                                       scratch,
                                       sizeof(scratch));
        }
        return true;
    }
    {
        struct yousj_ramfs_node *n = fs_lookup(path);
        if (n == NULL)
            return false;
        stat_from_ramfs_node(n, info);
        return true;
    }
}

bool fs_file_stat(struct yousj_file *f,
                  struct yousj_stat_info *info)
{
    if (f == NULL)
        return false;
    info->nlink = 1; /* M44: special files */
    if (f->epoll != NULL) {
        info->mode = S_IFREG_K | 0600u;
        info->mtime = 0;
        info->atime = 0;
        info->ctime = 0;
        info->size = 0;
        info->ino = 0;
        return true;
    }
    if (f->pipe != NULL) {
        info->mode = S_IFIFO_K | 0644u;
        info->mtime = 0;
        info->atime = 0;
        info->ctime = 0;
        info->size = 0;
        info->ino = 0;
        return true;
    }
    if (f->backend == 2) {
        info->mtime = (uint32_t)yousj_now_unix();
        info->atime = info->mtime;
        info->ctime = info->mtime;
        info->ino = proc_ino_for(f->proc_id, f->proc_pid);
        if (proc_is_dir_id(f->proc_id)) {
            info->mode = S_IFDIR_K | 0555u;
            info->size = 0;
        } else {
            info->mode = S_IFREG_K | 0444u;
            info->size = f->proc_len;
        }
        return true;
    }
    if (f->backend == 1)
        return stat_from_yfs_ino(f->yfs_ino, info);
    if (f->node != NULL) {
        stat_from_ramfs_node(f->node, info);
        return true;
    }
    /* the console */
    info->mode = S_IFCHR_K | 0666u;
    info->mtime = 0;
    info->atime = 0;
    info->ctime = 0;
    info->size = 0;
    info->ino = 1;
    return true;
}

/* --- M26: utimensat / futimens ------------------------- */

static uint32_t utime_value(const struct yousj_utime_op *op,
                            uint32_t now, uint32_t old)
{
    if (op->kind == YOUSJ_UTIME_SET)
        return (uint32_t)op->sec;
    if (op->kind == YOUSJ_UTIME_NOW)
        return now;
    return old;
}

static long utimens_ramfs_node(struct yousj_ramfs_node *n,
                               const struct yousj_utime_op *aop,
                               const struct yousj_utime_op *mop)
{
    uint32_t now = (uint32_t)yousj_now_unix();
    n->atime = utime_value(aop, now, n->atime);
    n->mtime = utime_value(mop, now, n->mtime);
    n->ctime = now; /* a successful utimens always bumps ctime */
    return 0;
}

/* M29: yfs v4 stores the stamps independently, so the
 * operands finally apply one-to-one (M26 aliased them
 * onto the single stamp the 64-byte inode could hold).
 * Both omitted: nothing to store, ctime untouched. */
static long utimens_yfs_ino(uint32_t ino,
                            const struct yousj_utime_op *aop,
                            const struct yousj_utime_op *mop)
{
    uint32_t now = (uint32_t)yousj_now_unix();
    long asec = aop->kind == YOUSJ_UTIME_OMIT
                    ? -1
                    : (long)utime_value(aop, now, 0);
    long msec = mop->kind == YOUSJ_UTIME_OMIT
                    ? -1
                    : (long)utime_value(mop, now, 0);
    yfs_set_times(ino, asec, msec);
    return 0;
}

long fs_utimens_path(const char *path, bool follow,
                     const struct yousj_utime_op *aop,
                     const struct yousj_utime_op *mop)
{
    char resolved[PATH_MAX_LEN + 1];
    if (fs_resolve_links(path, resolved, follow) != 0)
        return -YOUSJ_ENOENT_L;
    path = resolved;
    if (is_data_path(path)) {
        long ino = yfs_lookup(data_rel(path));
        if (ino < 0)
            return -YOUSJ_ENOENT_L;
        return utimens_yfs_ino((uint32_t)ino, aop, mop);
    }
    if (is_proc_path(path))
        return -YOUSJ_EACCES_L; /* generated files: read-only */
    {
        struct yousj_ramfs_node *n = fs_lookup(path);
        if (n == NULL)
            return -YOUSJ_ENOENT_L;
        return utimens_ramfs_node(n, aop, mop);
    }
}

long fs_utimens_file(struct yousj_file *f,
                     const struct yousj_utime_op *aop,
                     const struct yousj_utime_op *mop)
{
    if (f == NULL)
        return -YOUSJ_EBADF;
    if (f->backend == 2)
        return -YOUSJ_EACCES_L;
    if (f->backend == 1)
        return utimens_yfs_ino(f->yfs_ino, aop, mop);
    if (f->node != NULL)
        return utimens_ramfs_node(f->node, aop, mop);
    return -YOUSJ_EINVAL; /* pipes, epoll, console: no stamps */
}

/* --- M36: chmod / chown --------------------------------
 * Modes are stored per node/inode (see node_create and
 * yfs mode). chown has no ownership model to change --
 * every file is uid/gid 0 -- so it validates existence
 * and succeeds, which is the honest subset. */
long fs_chmod_path(const char *path, uint32_t bits,
                   bool follow)
{
    char resolved[PATH_MAX_LEN + 1];
    if (fs_resolve_links(path, resolved, follow) != 0)
        return -YOUSJ_ENOENT_L;
    path = resolved;
    if (is_data_path(path)) {
        long ino = yfs_lookup(data_rel(path));
        if (ino < 0)
            return -YOUSJ_ENOENT_L;
        yfs_set_mode((uint32_t)ino, bits);
        inotify_self_id(1, NULL, (uint32_t)ino, IN_ATTRIB_K,
                        yfs_is_dir((uint32_t)ino)); /* M48 */
        return 0;
    }
    if (is_proc_path(path))
        return -YOUSJ_EACCES_L;
    {
        struct yousj_ramfs_node *n = fs_lookup(path);
        if (n == NULL)
            return -YOUSJ_ENOENT_L;
        n->mode = bits & 07777u;
        n->ctime = (uint32_t)yousj_now_unix();
        inotify_self_id(0, n, 0, IN_ATTRIB_K,
                        n->is_dir); /* M48 */
        return 0;
    }
}

long fs_chmod_file(struct yousj_file *f, uint32_t bits)
{
    if (f == NULL)
        return -YOUSJ_EBADF;
    if (f->backend == 2)
        return -YOUSJ_EACCES_L;
    if (f->backend == 1) {
        yfs_set_mode(f->yfs_ino, bits);
        inotify_self_id(1, NULL, f->yfs_ino, IN_ATTRIB_K,
                        yfs_is_dir(f->yfs_ino)); /* M48 */
        return 0;
    }
    if (f->node != NULL) {
        f->node->mode = bits & 07777u;
        f->node->ctime = (uint32_t)yousj_now_unix();
        inotify_self_id(0, f->node, 0, IN_ATTRIB_K,
                        f->node->is_dir); /* M48 */
        return 0;
    }
    return -YOUSJ_EINVAL; /* pipes, epoll, console */
}

long fs_chown_path(const char *path, bool follow)
{
    struct yousj_stat_info info;
    if (!fs_path_stat2(path, follow, &info))
        return -YOUSJ_ENOENT_L;
    return 0;
}

bool fs_path_stat(const char *path, bool *is_dir_out,
                  uint64_t *size_out)
{
    struct yousj_stat_info info;
    if (!fs_path_stat2(path, true, &info))
        return false;
    *is_dir_out = (info.mode & 0170000u) == S_IFDIR_K;
    *size_out = info.size;
    return true;
}

int fs_mount_data(void)
{
    int rc = yfs_mount();
    if (rc > 0)
        ramfs_mkdir_path("/data"); /* the mountpoint (may exist) */
    return rc;
}

/* M47: character devices. Ids: 1 null, 2 zero, 3 full,
 * 4 random (urandom shares it -- there is no entropy
 * model, just a xorshift PRNG seeded at init from the
 * wall clock and the tick count). */
#define DEV_NULL   1
#define DEV_ZERO   2
#define DEV_FULL   3
#define DEV_RANDOM 4

static uint64_t g_rng_state = 0x9E3779B97F4A7C15ull;

static uint8_t rng_byte(void)
{
    uint64_t x = g_rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    g_rng_state = x;
    return (uint8_t)((x * 0x2545F4914F6CDD1Dull) >> 56);
}

static long dev_read(struct yousj_file *f, uint8_t *dst,
                     uint64_t count)
{
    uint64_t i;
    switch (f->node->dev) {
    case DEV_NULL:
        return 0;
    case DEV_ZERO:
    case DEV_FULL:
        for (i = 0; i < count; i++)
            dst[i] = 0;
        return (long)count;
    case DEV_RANDOM:
        for (i = 0; i < count; i++)
            dst[i] = rng_byte();
        return (long)count;
    default:
        return -YOUSJ_EBADF;
    }
}

static long dev_write(struct yousj_file *f, uint64_t count)
{
    if (f->node->dev == DEV_FULL)
        return -YOUSJ_ENOSPC;
    return (long)count; /* null, zero, random: sink */
}

long file_read(struct yousj_file *f, uint8_t *dst, uint64_t count)
{
    long n;
    if (f != NULL && f->backend == 2) {
        uint64_t avail;
        if (proc_is_dir_id(f->proc_id))
            return -YOUSJ_EISDIR;
        if ((f->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_WRONLY)
            return -YOUSJ_EBADF;
        if (f->offset >= f->proc_len)
            return 0;
        avail = f->proc_len - f->offset;
        n = (long)(count < avail ? count : avail);
        mem_copy(dst, (const uint8_t *)f->proc_buf + f->offset,
                 (uint64_t)n);
        f->offset += (uint64_t)n;
        return n;
    }
    if (f != NULL && f->backend == 1) {
        if (yfs_is_dir(f->yfs_ino))
            return -YOUSJ_EISDIR;
        if ((f->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_WRONLY)
            return -YOUSJ_EBADF;
        n = yfs_pread(f->yfs_ino, (uint32_t)f->offset, dst,
                      (uint32_t)count);
        if (n > 0)
            f->offset += (uint64_t)n;
        return n;
    }
    if (f == NULL || f->node == NULL || f->pipe != NULL)
        return -YOUSJ_EBADF;
    if (f->node->is_dir)
        return -YOUSJ_EISDIR;
    if ((f->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_WRONLY)
        return -YOUSJ_EBADF;
    if (f->node->dev != 0) /* M47 */
        return dev_read(f, dst, count);
    n = node_pread(f->node, f->offset, dst, count);
    if (n > 0) {
        f->offset += (uint64_t)n;
        /* M26: strict atime -- a successful read stamps the
         * node. (yfs keeps no separate atime; see fs.h.) */
        f->node->atime = (uint32_t)yousj_now_unix();
    }
    return n;
}

long file_write(struct yousj_file *f, const uint8_t *src,
                uint64_t count)
{
    long n;
    if (f != NULL && f->backend == 2)
        return -YOUSJ_EBADF; /* read-only by construction */
    if (f != NULL && f->backend == 1) {
        if (yfs_is_dir(f->yfs_ino))
            return -YOUSJ_EISDIR;
        if ((f->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_RDONLY)
            return -YOUSJ_EBADF;
        if (f->flags & YOUSJ_O_APPEND)
            f->offset = yfs_size(f->yfs_ino);
        n = yfs_pwrite(f->yfs_ino, (uint32_t)f->offset, src,
                       (uint32_t)count);
        if (n > 0) {
            f->offset += (uint64_t)n;
            f->written = true; /* M48 */
            inotify_self_id(1, NULL, f->yfs_ino,
                            IN_MODIFY_K, false);
        }
        return n;
    }
    if (f == NULL || f->node == NULL || f->pipe != NULL)
        return -YOUSJ_EBADF;
    if (f->node->is_dir)
        return -YOUSJ_EISDIR;
    if ((f->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_RDONLY)
        return -YOUSJ_EBADF;
    if (f->node->dev != 0) /* M47 */
        return dev_write(f, count);
    if (f->flags & YOUSJ_O_APPEND)
        f->offset = f->node->size;
    n = node_pwrite(f->node, f->offset, src, count);
    if (n > 0) {
        f->offset += (uint64_t)n;
        f->written = true; /* M48 */
        inotify_self_id(0, f->node, 0, IN_MODIFY_K, false);
    }
    return n;
}

bool fs_file_is_dir(struct yousj_file *f)
{
    if (f == NULL)
        return false;
    if (f->backend == 1)
        return yfs_is_dir(f->yfs_ino);
    if (f->backend == 2)
        return proc_is_dir_id(f->proc_id);
    return f->node != NULL && f->node->is_dir;
}

long fs_pread_file(struct yousj_file *f, uint64_t off,
                   uint8_t *dst, uint64_t count)
{
    uint64_t done = 0;
    if (f == NULL || f->pipe != NULL)
        return -YOUSJ_EBADF;
    if (f->backend == 2) {
        uint64_t avail;
        if (proc_is_dir_id(f->proc_id) || off >= f->proc_len)
            return 0;
        avail = f->proc_len - off;
        if (count > avail)
            count = avail;
        mem_copy(dst, (const uint8_t *)f->proc_buf + off, count);
        return (long)count;
    }
    if (f->backend == 0 && f->node == NULL)
        return -YOUSJ_EBADF; /* the console is not mappable */
    if (f->backend == 0 && f->node != NULL &&
        f->node->dev != 0) /* M47: offsets mean nothing */
        return dev_read(f, dst, count);
    while (done < count) {
        uint64_t chunk = count - done;
        long n;
        if (chunk > 65536)
            chunk = 65536;
        if (f->backend == 1)
            n = yfs_pread(f->yfs_ino, (uint32_t)(off + done),
                          dst + done, (uint32_t)chunk);
        else
            n = node_pread(f->node, (uint32_t)(off + done),
                           dst + done, chunk);
        if (n < 0)
            return done > 0 ? (long)done : n;
        if (n == 0)
            break; /* EOF: the rest stays zero */
        done += (uint64_t)n;
    }
    return (long)done;
}

long fs_pwrite_file(struct yousj_file *f, uint64_t off,
                    const uint8_t *src, uint64_t count)
{
    uint64_t done = 0;
    if (f == NULL || f->pipe != NULL)
        return -YOUSJ_EBADF;
    if (f->backend == 0 && f->node == NULL)
        return -YOUSJ_EBADF;
    if (f->backend == 0 && f->node != NULL &&
        f->node->dev != 0) /* M47 */
        return dev_write(f, count);
    while (done < count) {
        uint64_t chunk = count - done;
        long n;
        if (chunk > 65536)
            chunk = 65536;
        if (f->backend == 1)
            n = yfs_pwrite(f->yfs_ino, (uint32_t)(off + done),
                           src + done, (uint32_t)chunk);
        else
            n = node_pwrite(f->node, (uint32_t)(off + done),
                            src + done, chunk);
        if (n < 0)
            return done > 0 ? (long)done : n;
        done += (uint64_t)n;
    }
    if (done > 0) { /* M48 */
        f->written = true;
        if (f->backend == 1)
            inotify_self_id(1, NULL, f->yfs_ino,
                            IN_MODIFY_K, false);
        else if (f->node != NULL)
            inotify_self_id(0, f->node, 0, IN_MODIFY_K,
                            false);
    }
    return (long)done;
}

long file_seek(struct yousj_file *f, long off, long whence)
{
    int64_t base, np;
    if (f != NULL && f->backend == 2) {
        if (whence == 2 && proc_is_dir_id(f->proc_id))
            return -YOUSJ_EINVAL; /* M38: no SEEK_END on dirs */
        switch (whence) {
        case 0: base = 0; break;
        case 1: base = (int64_t)f->offset; break;
        case 2: base = (int64_t)f->proc_len;
            break;
        default: return -YOUSJ_EINVAL;
        }
        np = base + off;
        if (np < 0)
            return -YOUSJ_EINVAL;
        f->offset = (uint64_t)np;
        return np;
    }
    if (f != NULL && f->backend == 1) {
        if (whence == 2 && yfs_is_dir(f->yfs_ino))
            return -YOUSJ_EINVAL; /* M38 */
        switch (whence) {
        case 0: base = 0; break;
        case 1: base = (int64_t)f->offset; break;
        case 2: base = (int64_t)yfs_size(f->yfs_ino); break;
        default: return -YOUSJ_EINVAL;
        }
        np = base + off;
        if (np < 0)
            return -YOUSJ_EINVAL;
        f->offset = (uint64_t)np;
        return np;
    }
    if (f == NULL || f->node == NULL)
        return -YOUSJ_EBADF;
    /* Directories may seek too: the offset is the getdents
     * entry index (== the d_off cookie space), and SEEK_SET 0
     * is the rewind. SEEK_END on a directory is EINVAL since
     * M38 -- it used to land on node->size, which is 0 for
     * directories and silently rewound. */
    if (f->node->is_dir && whence == 2)
        return -YOUSJ_EINVAL;
    switch (whence) {
    case 0: base = 0; break;                    /* SEEK_SET */
    case 1: base = (int64_t)f->offset; break;   /* SEEK_CUR */
    case 2: base = (int64_t)f->node->size; break; /* SEEK_END */
    default: return -YOUSJ_EINVAL;
    }
    np = base + off;
    if (np < 0)
        return -YOUSJ_EINVAL;
    f->offset = (uint64_t)np;
    return np;
}

/* linux_dirent64: ino(8) off(8) reclen(2) type(1) name[] NUL pad */
long file_getdents(struct yousj_file *f, uint8_t *buf, uint64_t count)
{
    struct yousj_ramfs_node *c;
    uint64_t used = 0, idx = 0;
    if (f != NULL && f->backend == 2) {
        /* The root lists the three flat files plus one
         * directory per live task (M28); a pid directory
         * lists status, stat and cmdline. The live set is re-read
         * on every call -- entries may shift under a
         * concurrent exit, the same race Linux has. */
        static const char *const root_names[5] = {
            ".", "..", "uptime", "meminfo", "version"
        };
        static const char *const pid_names[11] = {
            ".", "..", "status", "stat", "cmdline",
            "environ", "cwd", "exe", "maps", "fd",
            "fdinfo"
        };
        uint64_t tids[YOUSJ_SCHED_MAX_TASKS];
        int fds[YOUSJ_NR_OPEN]; /* M41: PFDDIR listing */
        uint32_t ntids = 0;
        uint64_t total, i;
        long blocked = 0; /* M38: an entry existed but
                             did not fit an empty buffer */
        if (!proc_is_dir_id(f->proc_id))
            return -YOUSJ_ENOTDIR;
        if (f->proc_id == 0) {
            ntids = sched_live_tids(tids,
                                    YOUSJ_SCHED_MAX_TASKS);
            total = 5 + ntids;
        } else if (f->proc_id == PROC_ID_PFDDIR ||
                   f->proc_id == PROC_ID_PFDINFODIR) {
            struct task *t = sched_find_task(
                (int)f->proc_pid);
            int k;
            total = 2;
            if (t != NULL) {
                for (k = 0; k < YOUSJ_NR_OPEN; k++)
                    if (t->fd_file[k] != NULL)
                        fds[total++ - 2] = k;
            }
        } else {
            total = 11;
        }
        for (i = f->offset; i < total; i++) {
            const char *nm;
            char numbuf[16];
            uint64_t ino;
            uint8_t dtype;
            uint64_t namelen, reclen;
            uint8_t *rec;
            if (f->proc_id == PROC_ID_PIDDIR) {
                nm = pid_names[i];
                dtype = i <= 1 ? YOUSJ_DT_DIR
                        : (i == 6 || i == 7)
                              ? 10 /* DT_LNK: cwd/exe */
                              : YOUSJ_DT_REG;
                if (i == 0)
                    ino = proc_ino_for(PROC_ID_PIDDIR,
                                       f->proc_pid);
                else if (i == 1)
                    ino = 9000;
                else if (i == 2)
                    ino = proc_ino_for(PROC_ID_PSTATUS,
                                       f->proc_pid);
                else if (i == 3)
                    ino = proc_ino_for(PROC_ID_PSTAT,
                                       f->proc_pid);
                else if (i == 4)
                    ino = proc_ino_for(PROC_ID_PCMDLINE,
                                       f->proc_pid);
                else if (i == 5)
                    ino = proc_ino_for(PROC_ID_PENVIRON,
                                       f->proc_pid);
                else if (i == 6)
                    ino = proc_ino_for(PROC_ID_PCWD,
                                       f->proc_pid);
                else if (i == 7)
                    ino = proc_ino_for(PROC_ID_PEXE,
                                       f->proc_pid);
                else if (i == 8)
                    ino = proc_ino_for(PROC_ID_PMAPS,
                                       f->proc_pid);
                else if (i == 9) {
                    ino = 30000 + f->proc_pid; /* PFDDIR */
                    dtype = YOUSJ_DT_DIR;
                } else {
                    ino = 40000 + f->proc_pid; /* PFDINFODIR */
                    dtype = YOUSJ_DT_DIR;
                }
            } else if (f->proc_id == PROC_ID_PFDDIR ||
                       f->proc_id == PROC_ID_PFDINFODIR) {
                if (i == 0) {
                    nm = ".";
                    ino = (f->proc_id == PROC_ID_PFDDIR
                               ? 30000
                               : 40000) +
                          f->proc_pid;
                    dtype = YOUSJ_DT_DIR;
                } else if (i == 1) {
                    nm = "..";
                    ino = proc_ino_for(PROC_ID_PIDDIR,
                                       f->proc_pid);
                    dtype = YOUSJ_DT_DIR;
                } else {
                    int fdn = fds[i - 2];
                    u32_dec(numbuf, (uint32_t)fdn);
                    nm = numbuf;
                    if (f->proc_id == PROC_ID_PFDDIR) {
                        ino = 20000 +
                              (uint64_t)f->proc_pid * 16 +
                              (uint64_t)fdn;
                        dtype = 10; /* DT_LNK */
                    } else {
                        ino = 50000 +
                              (uint64_t)f->proc_pid * 16 +
                              (uint64_t)fdn;
                        dtype = YOUSJ_DT_REG;
                    }
                }
            } else if (i < 5) {
                nm = root_names[i];
                ino = i <= 1 ? 9000 : 9000 + (i - 1);
                dtype = i <= 1 ? YOUSJ_DT_DIR : YOUSJ_DT_REG;
            } else {
                uint64_t tid = tids[i - 5];
                uint64_t v = tid;
                int n = 0, a, b;
                if (v == 0)
                    numbuf[n++] = '0';
                while (v > 0) {
                    numbuf[n++] = (char)('0' + (v % 10));
                    v /= 10;
                }
                numbuf[n] = '\0';
                for (a = 0, b = n - 1; a < b; a++, b--) {
                    char tc = numbuf[a];
                    numbuf[a] = numbuf[b];
                    numbuf[b] = tc;
                }
                nm = numbuf;
                ino = proc_ino_for(PROC_ID_PIDDIR,
                                   (uint32_t)tid);
                dtype = YOUSJ_DT_DIR;
            }
            namelen = str_len(nm);
            reclen = (19 + namelen + 1 + 7) & ~7ULL;
            if (used + reclen > count) {
                if (used == 0)
                    blocked = 1;
                break;
            }
            rec = buf + used;
            mem_zero(rec, reclen);
            *(uint64_t *)(rec + 0) = ino;
            *(uint64_t *)(rec + 8) = i + 1;
            *(uint16_t *)(rec + 16) = (uint16_t)reclen;
            rec[18] = dtype;
            mem_copy(rec + 19, (const uint8_t *)nm, namelen);
            used += reclen;
            f->offset = i + 1;
        }
        if (used == 0 && blocked)
            return -YOUSJ_EINVAL; /* M38: not EOF */
        return (long)used;
    }
    if (f != NULL && f->backend == 1) {
        uint32_t slot = (uint32_t)f->offset;
        long blocked = 0; /* M38 */
        if (!yfs_is_dir(f->yfs_ino))
            return -YOUSJ_ENOTDIR;
        for (;;) {
            char nm[YFS_MAX_NAME + 1];
            uint32_t cino;
            int isdir;
            uint64_t namelen, reclen;
            uint8_t *rec;
            if (yfs_readdir(f->yfs_ino, &slot, nm, &cino,
                            &isdir) != 1)
                break;
            namelen = str_len(nm);
            reclen = (19 + namelen + 1 + 7) & ~7ULL;
            if (used + reclen > count) {
                if (used == 0)
                    blocked = 1;
                break;
            }
            rec = buf + used;
            mem_zero(rec, reclen);
            *(uint64_t *)(rec + 0) = cino + 2;
            *(uint64_t *)(rec + 8) = slot;
            *(uint16_t *)(rec + 16) = (uint16_t)reclen;
            rec[18] = isdir ? YOUSJ_DT_DIR
                      : (cino >= 0 && yfs_type_of((uint32_t)cino) == 3)
                          ? 10 : YOUSJ_DT_REG;
            mem_copy(rec + 19, (const uint8_t *)nm, namelen);
            used += reclen;
            f->offset = slot;
        }
        if (used == 0 && blocked)
            return -YOUSJ_EINVAL; /* M38 */
        return (long)used;
    }
    if (f == NULL || f->node == NULL)
        return -YOUSJ_ENOTDIR; /* M38: valid fd, wrong kind
                                   (pipes, console, files) */
    if (!f->node->is_dir)
        return -YOUSJ_ENOTDIR;
    {
        long blocked = 0; /* M38 */
        struct yousj_dent *dwalk;
        for (dwalk = f->node->dents; dwalk != NULL;
             dwalk = dwalk->next, idx++) {
            struct yousj_ramfs_node *c = dwalk->node;
            uint64_t namelen, reclen;
            uint8_t *rec;
            if (idx < f->offset)
                continue;
            namelen = str_len(dwalk->name);
            reclen = (19 + namelen + 1 + 7) & ~7ULL;
            if (used + reclen > count) {
                if (used == 0)
                    blocked = 1;
                break;
            }
            rec = buf + used;
            mem_zero(rec, reclen);
            *(uint64_t *)(rec + 0) = 1; /* ino: no stable ids yet */
            *(uint64_t *)(rec + 8) = (uint64_t)(idx + 1);
            *(uint16_t *)(rec + 16) = (uint16_t)reclen;
            rec[18] = c->is_dir ? YOUSJ_DT_DIR : c->is_link ? 10
                          : c->dev != 0 ? 2 /* DT_CHR, M47 */
                          : YOUSJ_DT_REG;
            mem_copy(rec + 19, (const uint8_t *)dwalk->name,
                     namelen);
            used += reclen;
            f->offset = idx + 1;
        }
        if (used == 0 && blocked)
            return -YOUSJ_EINVAL; /* M38 */
    }
    if (used > 0)
        f->node->atime = (uint32_t)yousj_now_unix(); /* M26 */
    return (long)used;
}

/* --- boot ---------------------------------------------------------- */

void fs_init(void)
{
    mem_zero((uint8_t *)g_nodes, sizeof(g_nodes));
    mem_zero((uint8_t *)g_files, sizeof(g_files));
    mem_zero((uint8_t *)g_pipes, sizeof(g_pipes));
    mem_zero((uint8_t *)&g_console_file, sizeof(g_console_file));
    g_console_file.in_use = true;
    g_console_file.flags = YOUSJ_O_RDWR;
    g_root = node_alloc();
    g_root->is_dir = true;
    g_root->mode = 0755; /* M36 */
    g_root->nlink = 1;
    g_root->parent = g_root;
    /* M25: the /proc mountpoint shows in the root listing;
     * every operation on it is intercepted by proc_open /
     * the stat and getdents branches before the ramfs node
     * is ever consulted. */
    ramfs_mkdir_path("/proc");
    /* M47: /dev with the classic character devices. */
    ramfs_mkdir_path("/dev");
    {
        struct yousj_ramfs_node *devdir = fs_lookup("/dev");
        static const char *const names[5] = {
            "null", "zero", "full", "random", "urandom"
        };
        static const uint32_t ids[5] = { 1, 2, 3, 4, 4 };
        int i;
        if (devdir != NULL) {
            for (i = 0; i < 5; i++) {
                struct yousj_ramfs_node *n = node_create(
                    devdir, names[i], str_len(names[i]),
                    false);
                if (n != NULL) {
                    n->dev = ids[i];
                    n->mode = 0666;
                }
            }
        }
        g_rng_state ^= (uint64_t)yousj_now_unix() << 32 ^
                       sched_uptime_ticks() ^
                       0x2545F4914F6CDD1Dull;
        if (g_rng_state == 0)
            g_rng_state = 0x9E3779B97F4A7C15ull;
    }
}

static long install_file(const char *path, const uint8_t *data,
                         uint64_t len)
{
    char base[NAME_MAX_LEN + 1];
    struct yousj_ramfs_node *parent = walk(path, true, base);
    struct yousj_ramfs_node *n;
    if (parent == NULL)
        return -YOUSJ_ENOENT;
    n = node_create(parent, base, str_len(base), false);
    if (n == NULL)
        return -YOUSJ_ENOSPC;
    return node_pwrite(n, 0, data, len) == (long)len ? 0 : -YOUSJ_ENOSPC;
}

extern const uint8_t _binary_initramfs_cpio_start[];
extern const uint8_t _binary_initramfs_cpio_end[];

/* --- M11: newc cpio initramfs unpacking -------------------------
 * The root filesystem arrives as one archive (built by
 * tools/mkinitramfs.py, embedded in the kernel image). Entry
 * header: 6-byte magic "070701" then thirteen 8-digit hex
 * fields; the name follows (namesize includes its NUL), header
 * + name padded to 4 bytes, then the data, also padded to 4.
 * Directories are created mkdir -p style; regular files go
 * through install_file; anything else (symlinks, devices) is
 * skipped -- userland has no concept of them yet. */

static uint32_t cpio_hex8(const uint8_t *p)
{
    uint32_t v = 0;
    int i;
    for (i = 0; i < 8; i++) {
        uint8_t c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (uint32_t)(c - 'A' + 10);
    }
    return v;
}

static void mkdir_p(const char *path)
{
    char buf[160];
    uint64_t i, n = str_len(path);

    if (n >= sizeof(buf))
        return;
    for (i = 0; i <= n; i++)
        buf[i] = path[i];
    for (i = 1; i < n; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            fs_mkdir_path(buf, 0);
            buf[i] = '/';
        }
    }
    fs_mkdir_path(buf, 0);
}

static uint64_t align4(uint64_t v)
{
    return (v + 3) & ~(uint64_t)3;
}

long fs_load_initramfs(void)
{
    const uint8_t *base = _binary_initramfs_cpio_start;
    uint64_t len = (uint64_t)(_binary_initramfs_cpio_end -
                              _binary_initramfs_cpio_start);
    uint64_t off = 0, files = 0;

    while (off + 110 <= len) {
        uint32_t mode, fsize, namesize;
        char name[128], path[160];
        uint64_t i, dstart, nlen;

        if (base[off] != '0' || base[off + 1] != '7' ||
            base[off + 2] != '0' || base[off + 3] != '7' ||
            base[off + 4] != '0' || base[off + 5] != '1')
            return files > 0 ? (long)files : -1;
        mode = cpio_hex8(base + off + 14);
        fsize = cpio_hex8(base + off + 54);
        namesize = cpio_hex8(base + off + 94);
        if (namesize < 1 || namesize > sizeof(name))
            return -1;
        for (i = 0; i + 1 < namesize; i++)
            name[i] = (char)base[off + 110 + i];
        name[namesize - 1] = '\0';
        if (name[0] == 'T' && str_len(name) == 10 &&
            name[1] == 'R' && name[2] == 'A' && name[3] == 'I')
            break; /* TRAILER!!! */
        dstart = off + align4(110 + namesize);
        if (dstart + fsize > len)
            return -1;
        /* build the absolute path "/" + name */
        nlen = str_len(name);
        if (nlen + 2 > sizeof(path))
            return -1;
        path[0] = '/';
        for (i = 0; i <= nlen; i++)
            path[1 + i] = name[i];
        if ((mode & 0170000) == 0040000) {
            mkdir_p(path);
        } else if ((mode & 0170000) == 0100000) {
            /* ensure the parent chain exists */
            char parent[160];
            uint64_t j = nlen + 1;
            while (j > 0 && path[j] != '/')
                j--;
            if (j > 0) {
                for (i = 0; i < j; i++)
                    parent[i] = path[i];
                parent[j] = '\0';
                mkdir_p(parent);
            }
            if (install_file(path, base + dstart, fsize) == 0)
                files++;
        }
        off = dstart + align4(fsize);
    }
    return (long)files;
}
