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
#include <yousj/pmm.h>
#include <yousj/vmm.h> /* errno magnitudes + phys_to_virt */

#define YOUSJ_EBADF   9
#define YOUSJ_EEXIST_L 17
#define YOUSJ_ENOTDIR 20
#define YOUSJ_EISDIR  21
#define YOUSJ_ENOSPC  28
#define YOUSJ_EMFILE_L 24
#define YOUSJ_ENOMEM_L 12
#define YOUSJ_ENFILE  23

#define MAX_NODES      128
#define NAME_MAX_LEN   31
#define MAX_FILE_PAGES 64 /* 256 KiB per ramfs file */
#define MAX_FILES      96
#define PATH_MAX_LEN   255

struct yousj_ramfs_node {
    bool in_use;
    bool is_dir;
    char name[NAME_MAX_LEN + 1];
    struct yousj_ramfs_node *parent;
    struct yousj_ramfs_node *child_head;
    struct yousj_ramfs_node *sibling;
    uint32_t size;                      /* regular files */
    uint32_t pages[MAX_FILE_PAGES];     /* phys frames, 0 = hole */
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

long fs_mkdir_path(const char *path)
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
    if (done > 0 && off + done > n->size)
        n->size = (uint32_t)(off + done);
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

long fs_open_path(const char *path, long flags,
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
    f->flags = flags;
    f->offset = 0;
    *out = f;
    return 0;
}

long file_read(struct yousj_file *f, uint8_t *dst, uint64_t count)
{
    long n;
    if (f == NULL || f->node == NULL || f->pipe != NULL)
        return -YOUSJ_EBADF;
    if (f->node->is_dir)
        return -YOUSJ_EISDIR;
    if ((f->flags & YOUSJ_O_ACCMODE) == YOUSJ_O_WRONLY)
        return -YOUSJ_EBADF;
    n = node_pread(f->node, f->offset, dst, count);
    if (n > 0)
        f->offset += (uint64_t)n;
    return n;
}

long file_write(struct yousj_file *f, const uint8_t *src,
                uint64_t count)
{
    long n;
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

long file_seek(struct yousj_file *f, long off, long whence)
{
    int64_t base, np;
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
        rec[18] = c->is_dir ? YOUSJ_DT_DIR : YOUSJ_DT_REG;
        mem_copy(rec + 19, (const uint8_t *)c->name, namelen);
        used += reclen;
        f->offset = idx + 1;
    }
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
