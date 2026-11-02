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

struct yousj_ramfs_node {
    bool in_use;
    bool is_dir;
    bool is_link;                       /* M16: content = target */
    char name[NAME_MAX_LEN + 1];
    struct yousj_ramfs_node *parent;
    struct yousj_ramfs_node *child_head;
    struct yousj_ramfs_node *sibling;
    uint32_t size;                      /* regular files */
    uint32_t mtime;                     /* M23: epoch seconds */
    uint32_t atime;                     /* M26: independent */
    uint32_t ctime;                     /* M26: independent */
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

static struct yousj_ramfs_node *node_find_child(
    struct yousj_ramfs_node *dir, const char *name, uint64_t len)
{
    struct yousj_ramfs_node *c;
    for (c = dir->child_head; c != NULL; c = c->sibling) {
        if (str_len(c->name) == len &&
            mem_cmp_bytes(c->name, name, len))
            return c;
    }
    return NULL;
}

static struct yousj_ramfs_node *node_create(
    struct yousj_ramfs_node *parent, const char *name, uint64_t len,
    bool is_dir)
{
    struct yousj_ramfs_node *n = node_alloc();
    if (n == NULL)
        return NULL;
    if (len > NAME_MAX_LEN)
        len = NAME_MAX_LEN;
    mem_copy((uint8_t *)n->name, (const uint8_t *)name, len);
    n->name[len] = '\0';
    n->is_dir = is_dir;
    n->mtime = (uint32_t)yousj_now_unix();
    n->atime = n->mtime;
    n->ctime = n->mtime;
    n->parent = parent;
    n->sibling = parent->child_head;
    parent->child_head = n;
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

static void node_detach(struct yousj_ramfs_node *n)
{
    struct yousj_ramfs_node *p = n->parent;
    struct yousj_ramfs_node **link;

    if (p == NULL)
        return;
    link = &p->child_head;
    while (*link != NULL && *link != n)
        link = &(*link)->sibling;
    if (*link == n) {
        *link = n->sibling;
        n->sibling = NULL;
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
    return id == 0 || id == PROC_ID_PIDDIR;
}

/* Inode numbers: the flat files keep 9000+id; a pid entry
 * gets 10000 + pid*4 + leaf (0 dir, 1 status, 2 stat). */
static uint64_t proc_ino_for(int id, uint32_t pid)
{
    if (id == PROC_ID_PIDDIR)
        return 10000 + (uint64_t)pid * 4;
    if (id == PROC_ID_PSTATUS)
        return 10000 + (uint64_t)pid * 4 + 1;
    if (id == PROC_ID_PSTAT)
        return 10000 + (uint64_t)pid * 4 + 2;
    if (id == PROC_ID_PCMDLINE)
        return 10000 + (uint64_t)pid * 4 + 3;
    return 9000 + (uint64_t)id;
}

/* "" -> 0 (the directory), a known name -> its id,
 * "<pid>" / "<pid>/status" / "<pid>/stat" -> the M28 ids
 * (with *pid_out set), else -1. A numeric head that names
 * no live task is -1: from the outside it does not exist. */
static int proc_find(const char *rel, uint32_t *pid_out)
{
    int i;
    *pid_out = 0;
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
        }
        return -1;
    }
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

static uint32_t proc_generate(int id, uint32_t pid, char *buf,
                              uint32_t cap)
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
        pb_puts(&b, "Yousj OS version 0.9 (M31) x86_64 #1\n");
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
    uint32_t pid = 0;
    if (rel[0] == '/')
        rel++;
    id = proc_find(rel, &pid);
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
    f->flags = flags;
    f->offset = 0;
    if (!proc_is_dir_id(id))
        f->proc_len = proc_generate(id, pid, f->proc_buf,
                                    sizeof(f->proc_buf));
    *out = f;
    return 0;
}

long fs_open_path(const char *path, long flags,
                  struct yousj_file **out)
{
    char resolved[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(path, resolved, true);
    if (rc != 0)
        return rc;
    path = resolved;
    if (is_data_path(path))
        return yfs_open_path_fs(data_rel(path), flags, out);
    if (is_proc_path(path))
        return proc_open(path, flags, out);
    return ramfs_open_path(path, flags, out);
}

long fs_mkdir_path(const char *path)
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
        return ino >= 0 ? 0 : ino;
    }
    return ramfs_mkdir_path(path);
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
        split_parent_name(data_rel(path), parent, name);
        return yfs_unlink(parent, name, false);
    }
    {
        char base[NAME_MAX_LEN + 1];
        struct yousj_ramfs_node *parent = walk(path, true, base);
        struct yousj_ramfs_node *child;
        if (parent == NULL)
            return -YOUSJ_ENOENT;
        child = node_find_child(parent, base, str_len(base));
        if (child == NULL)
            return -YOUSJ_ENOENT;
        if (child->is_dir)
            return -YOUSJ_EISDIR;
        node_detach(child);
        child->unlinked = true;
        node_maybe_free(child);
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
        split_parent_name(data_rel(path), parent, name);
        return yfs_unlink(parent, name, true);
    }
    {
        char base[NAME_MAX_LEN + 1];
        struct yousj_ramfs_node *parent = walk(path, true, base);
        struct yousj_ramfs_node *child;
        if (parent == NULL)
            return -YOUSJ_ENOENT;
        child = node_find_child(parent, base, str_len(base));
        if (child == NULL)
            return -YOUSJ_ENOENT;
        if (!child->is_dir)
            return -YOUSJ_ENOTDIR;
        if (child == g_root)
            return -YOUSJ_EINVAL;
        if (child->child_head != NULL)
            return -YOUSJ_ENOTEMPTY_L;
        node_detach(child);
        child->unlinked = true;
        node_maybe_free(child);
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
    if (od)
        return yfs_rename(data_rel(old_path), data_rel(new_path));
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
    dst = node_find_child(nparent, nbase, str_len(nbase));
    if (dst == src)
        return 0;
    if (dst != NULL) {
        if (dst->is_dir) {
            if (!src->is_dir)
                return -YOUSJ_EISDIR;
            if (dst->child_head != NULL)
                return -YOUSJ_ENOTEMPTY_L;
        } else if (src->is_dir) {
            return -YOUSJ_ENOTDIR;
        }
        node_detach(dst);
        dst->unlinked = true;
        node_maybe_free(dst);
    }
    node_detach(src);
    mem_copy((uint8_t *)src->name, (const uint8_t *)nbase,
             str_len(nbase) + 1);
    src->parent = nparent;
    src->sibling = nparent->child_head;
    nparent->child_head = src;
    src->ctime = (uint32_t)yousj_now_unix(); /* M26 */
    return 0;
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
        if (node_pwrite(n, 0, (const uint8_t *)target,
                        str_len(target)) < 0)
            return -YOUSJ_ENOSPC;
        return 0;
    }
}

long fs_readlink_path(const char *path, char *buf, uint64_t cap)
{
    char resolved[PATH_MAX_LEN + 1];
    long rc = fs_resolve_links(path, resolved, false);
    if (rc != 0)
        return rc;
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
    if (n->is_link) {
        info->mode = S_IFLNK_K | 0777u;
        info->size = n->size;
    } else if (n->is_dir) {
        info->mode = S_IFDIR_K | 0755u;
        info->size = 0;
    } else {
        info->mode = S_IFREG_K | 0644u;
        info->size = n->size;
    }
    info->ino = (uint64_t)(n - g_nodes) + 2;
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
        info->mode = S_IFLNK_K | 0777u;
        info->size = yfs_size(ino);
    } else if (t == 2) {
        info->mode = S_IFDIR_K | 0755u;
        info->size = 0;
    } else if (t == 1) {
        info->mode = S_IFREG_K | 0644u;
        info->size = yfs_size(ino);
    } else {
        return false;
    }
    info->ino = (uint64_t)ino + 1000000u;
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
        uint32_t pid = 0;
        if (rel[0] == '/')
            rel++;
        id = proc_find(rel, &pid);
        if (id < 0)
            return false;
        info->mtime = (uint32_t)yousj_now_unix();
        info->atime = info->mtime;
        info->ctime = info->mtime;
        info->ino = proc_ino_for(id, pid);
        if (proc_is_dir_id(id)) {
            info->mode = S_IFDIR_K | 0555u;
            info->size = 0;
        } else {
            char scratch[288];
            info->mode = S_IFREG_K | 0444u;
            info->size = proc_generate(id, pid, scratch,
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
        if (n > 0)
            f->offset += (uint64_t)n;
        return n;
    }
    if (f == NULL || f->node == NULL || f->pipe != NULL)
        return -YOUSJ_EBADF;
    if (f->node->is_dir)
        return -YOUSJ_EISDIR;
    if ((f->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_RDONLY)
        return -YOUSJ_EBADF;
    if (f->flags & YOUSJ_O_APPEND)
        f->offset = f->node->size;
    n = node_pwrite(f->node, f->offset, src, count);
    if (n > 0)
        f->offset += (uint64_t)n;
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
    return (long)done;
}

long file_seek(struct yousj_file *f, long off, long whence)
{
    int64_t base, np;
    if (f != NULL && f->backend == 2) {
        switch (whence) {
        case 0: base = 0; break;
        case 1: base = (int64_t)f->offset; break;
        case 2: base = proc_is_dir_id(f->proc_id) ? 0 : (int64_t)f->proc_len;
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
     * entry index, and SEEK_SET 0 is the rewind. (SEEK_END on a
     * directory uses a zero base -- nothing saner exists without
     * a child count, and nothing uses it.) */
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
        static const char *const pid_names[5] = {
            ".", "..", "status", "stat", "cmdline"
        };
        uint64_t tids[YOUSJ_SCHED_MAX_TASKS];
        uint32_t ntids = 0;
        uint64_t total, i;
        if (!proc_is_dir_id(f->proc_id))
            return -YOUSJ_ENOTDIR;
        if (f->proc_id == 0) {
            ntids = sched_live_tids(tids,
                                    YOUSJ_SCHED_MAX_TASKS);
            total = 5 + ntids;
        } else {
            total = 5;
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
                dtype = i <= 1 ? YOUSJ_DT_DIR : YOUSJ_DT_REG;
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
                else
                    ino = proc_ino_for(PROC_ID_PCMDLINE,
                                       f->proc_pid);
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
            if (used + reclen > count)
                break;
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
        return (long)used;
    }
    if (f != NULL && f->backend == 1) {
        uint32_t slot = (uint32_t)f->offset;
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
            if (used + reclen > count)
                break;
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
        return (long)used;
    }
    if (f == NULL || f->node == NULL)
        return -YOUSJ_EBADF;
    if (!f->node->is_dir)
        return -YOUSJ_ENOTDIR;
    for (c = f->node->child_head; c != NULL; c = c->sibling, idx++) {
        uint64_t namelen, reclen;
        uint8_t *rec;
        if (idx < f->offset)
            continue;
        namelen = str_len(c->name);
        reclen = (19 + namelen + 1 + 7) & ~7ULL;
        if (used + reclen > count)
            break;
        rec = buf + used;
        mem_zero(rec, reclen);
        *(uint64_t *)(rec + 0) = 1; /* ino: no stable ids yet */
        *(uint64_t *)(rec + 8) = (uint64_t)(idx + 1);
        *(uint16_t *)(rec + 16) = (uint16_t)reclen;
        rec[18] = c->is_dir ? YOUSJ_DT_DIR : c->is_link ? 10 : YOUSJ_DT_REG;
        mem_copy(rec + 19, (const uint8_t *)c->name, namelen);
        used += reclen;
        f->offset = idx + 1;
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
    g_root->name[0] = '/';
    g_root->name[1] = '\0';
    g_root->parent = g_root;
    /* M25: the /proc mountpoint shows in the root listing;
     * every operation on it is intercepted by proc_open /
     * the stat and getdents branches before the ramfs node
     * is ever consulted. */
    ramfs_mkdir_path("/proc");
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
            fs_mkdir_path(buf);
            buf[i] = '/';
        }
    }
    fs_mkdir_path(buf);
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
