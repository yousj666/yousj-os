/* Yousj OS - yfs implementation (M12). Layout in yfs.h.
 *
 * Concurrency stance matches fs.c: no locks. Two tasks
 * writing yfs concurrently can interleave sector updates;
 * the preemption window is documented, not solved, at this
 * milestone. All buffers are static for the same reason.
 */
#include <yousj/yfs.h>
#include <yousj/ata.h>

#define E_NOENT_L   2
#define E_IO_L      5
#define E_EXIST_L   17
#define E_NOTDIR_L  20
#define E_ISDIR_L   21
#define E_INVAL_L   22
#define E_NOSPC_L   28
#define E_NAMETOOLONG_L 36
#define E_NOSYS_L   38

#define SB_SECTOR       0
#define INODE_SECTOR0   1
#define INODE_COUNT     128
#define INODE_SIZE      64
#define BITMAP_SECTOR0  17
#define BITMAP_SECTORS  8
/* M19: the write-ahead journal lives between the bitmap
 * and the data area: one header sector plus TXN_MAX
 * payload sectors, each holding a full-sector redo image
 * of one metadata operation. */
#define JRNL_SECTOR0    25
#define TXN_MAX         48
#define DATA_SECTOR0    (JRNL_SECTOR0 + 1 + TXN_MAX)
#define BLOCK_SIZE      4096
#define BLOCK_SECTORS   8
#define DIRECT          11
#define INDIRECT_ENTRIES 1024
#define MAX_BLOCKS      (DIRECT + INDIRECT_ENTRIES)
#define DIRENT_SIZE     32
#define UNALLOC         0xFFFFFFFFu

struct yfs_inode {
    uint8_t type; /* 0 free, 1 file, 2 dir */
    uint8_t pad[3];
    uint32_t size;
    uint32_t blocks[12]; /* DIRECT direct + 1 indirect slot;
                            the on-disk struct is ALWAYS 64
                            bytes -- sizing this array from
                            DIRECT once shrank the struct to
                            60 and corrupted every caller */
    uint32_t rsvd[2];
};

_Static_assert(sizeof(struct yfs_inode) == 64,
               "on-disk inode must be exactly 64 bytes");

static struct ata_drive g_drv;
static bool g_mounted;
static uint32_t g_data_blocks;

static uint8_t g_sec[ATA_SECTOR_SIZE];
static uint8_t g_blk[BLOCK_SIZE];

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

static uint64_t str_len(const char *s)
{
    uint64_t n = 0;
    while (s[n])
        n++;
    return n;
}

static bool str_eq_n(const char *a, const char *b, uint64_t n)
{
    uint64_t i;
    for (i = 0; i < n; i++) {
        if (a[i] != b[i])
            return false;
        if (a[i] == '\0')
            return true;
    }
    return b[n] == '\0';
}

/* --- M19: write-ahead journal -----------------------------------
 * Metadata operations (mknod/unlink/rename/truncate and
 * the deferred free) run inside a transaction: while one
 * is active, EVERY sector write in this file is staged in
 * memory (reads see staged images -- read-your-writes) and
 * nothing reaches the disk. On commit the staged images
 * are written to the journal payload area, the journal
 * header is written (the commit point), the images are
 * written home, and the header is cleared. On failure the
 * stage is simply dropped: a failed operation changes
 * nothing on disk. At mount, a surviving committed header
 * is replayed (full-sector redo is idempotent) and then
 * cleared. File CONTENT writes through yfs_pwrite are not
 * journaled (ordered-mode semantics: structure is atomic,
 * content may reflect a partial write after a crash), and
 * the open-count bumps in rsvd[0] are deliberately not
 * journaled -- a journal cycle per open/close would cost
 * more than the counter is worth. Like the rest of yfs,
 * transactions are not locked against concurrent tasks. */

static bool g_txn;
static int g_txn_depth;
static int g_txn_n;
static bool g_txn_failed;
static uint32_t g_txn_lba[TXN_MAX];
static uint8_t g_txn_img[TXN_MAX][ATA_SECTOR_SIZE];
static uint32_t g_jseq;

static uint32_t fnv1a(const uint8_t *p, uint32_t n)
{
    uint32_t h = 2166136261u, i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int txn_find(uint32_t lba)
{
    int i;
    for (i = 0; i < g_txn_n; i++)
        if (g_txn_lba[i] == lba)
            return i;
    return -1;
}

static int yfs_rd(uint32_t lba, uint32_t count, void *buf)
{
    uint8_t *p = (uint8_t *)buf;
    uint32_t s;
    if (!g_txn)
        return ata_read(&g_drv, lba, count, buf);
    for (s = 0; s < count; s++) {
        int idx = txn_find(lba + s);
        if (idx >= 0)
            mem_copy(p + s * ATA_SECTOR_SIZE,
                     g_txn_img[idx], ATA_SECTOR_SIZE);
        else if (ata_read(&g_drv, lba + s, 1,
                          p + s * ATA_SECTOR_SIZE) != 0)
            return -1;
    }
    return 0;
}

static int yfs_wr(uint32_t lba, uint32_t count, const void *buf)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t s;
    if (!g_txn)
        return ata_write(&g_drv, lba, count, buf);
    for (s = 0; s < count; s++) {
        int idx = txn_find(lba + s);
        if (idx < 0) {
            if (g_txn_n >= TXN_MAX) {
                g_txn_failed = true;
                return -1;
            }
            idx = g_txn_n++;
            g_txn_lba[idx] = lba + s;
        }
        mem_copy(g_txn_img[idx], p + s * ATA_SECTOR_SIZE,
                 ATA_SECTOR_SIZE);
    }
    return 0;
}

struct yfs_jhdr {
    char magic[8]; /* "YOUSJJR1" */
    uint32_t seq;
    uint32_t count;
    uint32_t targets[TXN_MAX];
    uint32_t sums[TXN_MAX];
    uint32_t hdr_sum;
};

static void journal_write_header(const struct yfs_jhdr *h)
{
    mem_zero(g_sec, ATA_SECTOR_SIZE);
    mem_copy(g_sec, (const uint8_t *)h,
             sizeof(struct yfs_jhdr));
    ata_write(&g_drv, JRNL_SECTOR0, 1, g_sec);
}

static void journal_clear(void)
{
    struct yfs_jhdr h;
    mem_zero((uint8_t *)&h, sizeof(h));
    mem_copy((uint8_t *)h.magic, (const uint8_t *)"YOUSJJR1",
             8);
    h.seq = g_jseq;
    h.count = 0;
    h.hdr_sum = fnv1a((const uint8_t *)&h,
                      sizeof(h) - sizeof(uint32_t));
    journal_write_header(&h);
}

static void txn_begin(void)
{
    if (g_txn_depth++ == 0) {
        g_txn = true;
        g_txn_n = 0;
        g_txn_failed = false;
    }
}

static void txn_finish(bool commit)
{
    struct yfs_jhdr h;
    int i;
    if (--g_txn_depth > 0)
        return;
    g_txn = false;
    if (!commit || g_txn_failed || g_txn_n == 0) {
        g_txn_n = 0;
        return;
    }
    /* Sort the stage by LBA (rows move with their keys):
     * the payload area then holds the images in row order
     * -- one contiguous write -- and the checkpoint can
     * merge runs of consecutive home sectors. Without
     * this, a commit costs one flushed write per sector;
     * with it, roughly one per run. */
    {
        int a, b;
        for (a = 1; a < g_txn_n; a++) {
            uint32_t lba = g_txn_lba[a];
            static uint8_t tmp[ATA_SECTOR_SIZE];
            mem_copy(tmp, g_txn_img[a], ATA_SECTOR_SIZE);
            for (b = a - 1; b >= 0 && g_txn_lba[b] > lba;
                 b--) {
                g_txn_lba[b + 1] = g_txn_lba[b];
                mem_copy(g_txn_img[b + 1], g_txn_img[b],
                         ATA_SECTOR_SIZE);
            }
            g_txn_lba[b + 1] = lba;
            mem_copy(g_txn_img[b + 1], tmp, ATA_SECTOR_SIZE);
        }
    }
    /* payload images first, then the header: the header
     * write is the commit point */
    ata_write(&g_drv, JRNL_SECTOR0 + 1, (uint32_t)g_txn_n,
              g_txn_img);
    mem_zero((uint8_t *)&h, sizeof(h));
    mem_copy((uint8_t *)h.magic, (const uint8_t *)"YOUSJJR1",
             8);
    h.seq = ++g_jseq;
    h.count = (uint32_t)g_txn_n;
    for (i = 0; i < g_txn_n; i++) {
        h.targets[i] = g_txn_lba[i];
        h.sums[i] = fnv1a(g_txn_img[i], ATA_SECTOR_SIZE);
    }
    h.hdr_sum = fnv1a((const uint8_t *)&h,
                      sizeof(h) - sizeof(uint32_t));
    journal_write_header(&h);
    /* checkpoint: write the images home in LBA runs,
     * then clear */
    {
        int a = 0;
        while (a < g_txn_n) {
            int b = a;
            while (b + 1 < g_txn_n &&
                   g_txn_lba[b + 1] == g_txn_lba[b] + 1)
                b++;
            ata_write(&g_drv, g_txn_lba[a],
                      (uint32_t)(b - a + 1), g_txn_img[a]);
            a = b + 1;
        }
    }
    journal_clear();
    g_txn_n = 0;
}

static void journal_replay(void)
{
    struct yfs_jhdr h;
    uint32_t i;
    static uint8_t img[ATA_SECTOR_SIZE];
    if (ata_read(&g_drv, JRNL_SECTOR0, 1, g_sec) != 0)
        return;
    mem_copy((uint8_t *)&h, g_sec, sizeof(h));
    if (!str_eq_n(h.magic, "YOUSJJR1", 8) || h.count == 0 ||
        h.count > TXN_MAX)
        return;
    if (h.hdr_sum != fnv1a((const uint8_t *)&h,
                           sizeof(h) - sizeof(uint32_t))) {
        journal_clear(); /* never committed: drop it */
        return;
    }
    g_jseq = h.seq;
    for (i = 0; i < h.count; i++) {
        if (ata_read(&g_drv, JRNL_SECTOR0 + 1 + i, 1,
                     img) != 0 ||
            fnv1a(img, ATA_SECTOR_SIZE) != h.sums[i]) {
            journal_clear(); /* corrupt payload: drop */
            return;
        }
    }
    for (i = 0; i < h.count; i++) {
        ata_read(&g_drv, JRNL_SECTOR0 + 1 + i, 1, img);
        ata_write(&g_drv, h.targets[i], 1, img);
    }
    journal_clear();
}

/* --- raw inode access ------------------------------------------- */

static int inode_read(uint32_t ino, struct yfs_inode *out)
{
    uint32_t sec = INODE_SECTOR0 + (ino * INODE_SIZE) / ATA_SECTOR_SIZE;
    uint32_t off = (ino * INODE_SIZE) % ATA_SECTOR_SIZE;
    if (yfs_rd(sec, 1, g_sec) != 0)
        return -E_IO_L;
    mem_copy((uint8_t *)out, g_sec + off, INODE_SIZE);
    return 0;
}

static int inode_write(uint32_t ino, const struct yfs_inode *in)
{
    uint32_t sec = INODE_SECTOR0 + (ino * INODE_SIZE) / ATA_SECTOR_SIZE;
    uint32_t off = (ino * INODE_SIZE) % ATA_SECTOR_SIZE;
    if (yfs_rd(sec, 1, g_sec) != 0)
        return -E_IO_L;
    mem_copy(g_sec + off, (const uint8_t *)in, INODE_SIZE);
    if (yfs_wr(sec, 1, g_sec) != 0)
        return -E_IO_L;
    return 0;
}

/* --- block allocation ------------------------------------------- */

static long block_alloc(void)
{
    uint32_t s, i;
    for (s = 0; s < BITMAP_SECTORS; s++) {
        if (yfs_rd(BITMAP_SECTOR0 + s, 1, g_sec) != 0)
            return -E_IO_L;
        for (i = 0; i < ATA_SECTOR_SIZE * 8; i++) {
            uint32_t blk = s * ATA_SECTOR_SIZE * 8 + i;
            if (blk >= g_data_blocks)
                return -E_NOSPC_L;
            if (!(g_sec[i / 8] & (uint8_t)(1u << (i % 8)))) {
                g_sec[i / 8] |= (uint8_t)(1u << (i % 8));
                if (yfs_wr(BITMAP_SECTOR0 + s, 1,
                              g_sec) != 0)
                    return -E_IO_L;
                /* hand out a zeroed block */
                mem_zero(g_blk, BLOCK_SIZE);
                if (ata_write(&g_drv,
                              DATA_SECTOR0 + blk * BLOCK_SECTORS,
                              BLOCK_SECTORS, g_blk) != 0)
                    return -E_IO_L;
                return (long)blk;
            }
        }
    }
    return -E_NOSPC_L;
}

static void block_free(uint32_t blk)
{
    uint32_t s = blk / (ATA_SECTOR_SIZE * 8);
    uint32_t i = blk % (ATA_SECTOR_SIZE * 8);
    if (yfs_rd(BITMAP_SECTOR0 + s, 1, g_sec) != 0)
        return;
    g_sec[i / 8] &= (uint8_t)~(1u << (i % 8));
    yfs_wr(BITMAP_SECTOR0 + s, 1, g_sec);
}

static int block_read(uint32_t blk, uint8_t *dst)
{
    return yfs_rd(DATA_SECTOR0 + blk * BLOCK_SECTORS,
                    BLOCK_SECTORS, dst) == 0 ? 0 : -E_IO_L;
}

static int block_write(uint32_t blk, const uint8_t *src)
{
    return yfs_wr(DATA_SECTOR0 + blk * BLOCK_SECTORS,
                     BLOCK_SECTORS, src) == 0 ? 0 : -E_IO_L;
}

/* --- block index translation (M13: single indirect) -------------
 * blocks[0..10] are direct; blocks[11] names the indirect
 * block, whose 1024 entries are block numbers or UNALLOC.
 * A fresh indirect block is filled with UNALLOC markers --
 * NOT zeros, because block 0 is a real data block. */

static uint8_t g_iblk[BLOCK_SIZE];

static int inode_get_block(const struct yfs_inode *n, uint32_t bi,
                           uint32_t *out)
{
    if (bi < DIRECT) {
        *out = n->blocks[bi];
        return 0;
    }
    if (bi >= MAX_BLOCKS || n->blocks[DIRECT] == UNALLOC) {
        *out = UNALLOC;
        return 0;
    }
    if (block_read(n->blocks[DIRECT], g_iblk) != 0)
        return -E_IO_L;
    *out = ((const uint32_t *)g_iblk)[bi - DIRECT];
    return 0;
}

/* Point index bi at blk, allocating the indirect block on
 * first use. The caller writes the inode back. */
static int inode_set_block(struct yfs_inode *n, uint32_t bi,
                           uint32_t blk)
{
    uint32_t i;
    if (bi < DIRECT) {
        n->blocks[bi] = blk;
        return 0;
    }
    if (bi >= MAX_BLOCKS)
        return -E_NOSPC_L;
    if (n->blocks[DIRECT] == UNALLOC) {
        long nb = block_alloc();
        if (nb < 0)
            return (int)nb;
        for (i = 0; i < INDIRECT_ENTRIES; i++)
            ((uint32_t *)g_iblk)[i] = UNALLOC;
        if (block_write((uint32_t)nb, g_iblk) != 0)
            return -E_IO_L;
        n->blocks[DIRECT] = (uint32_t)nb;
    } else {
        if (block_read(n->blocks[DIRECT], g_iblk) != 0)
            return -E_IO_L;
    }
    ((uint32_t *)g_iblk)[bi - DIRECT] = blk;
    return block_write(n->blocks[DIRECT], g_iblk);
}

/* --- content I/O ------------------------------------------------- */

long yfs_pread(uint32_t ino, uint32_t off, void *dst, uint32_t count)
{
    struct yfs_inode n;
    uint8_t *d = (uint8_t *)dst;
    uint32_t done = 0;

    if (inode_read(ino, &n) != 0)
        return -E_IO_L;
    if (off >= n.size)
        return 0;
    if (off + count > n.size)
        count = n.size - off;
    while (done < count) {
        uint32_t pos = off + done;
        uint32_t bi = pos / BLOCK_SIZE;
        uint32_t bo = pos % BLOCK_SIZE;
        uint32_t chunk = BLOCK_SIZE - bo;
        if (chunk > count - done)
            chunk = count - done;
        {
            uint32_t phys;
            if (inode_get_block(&n, bi, &phys) != 0)
                return -E_IO_L;
            if (phys == UNALLOC) {
                uint32_t i;
                for (i = 0; i < chunk; i++)
                    d[done + i] = 0;
            } else {
                if (block_read(phys, g_blk) != 0)
                    return -E_IO_L;
                mem_copy(d + done, g_blk + bo, chunk);
            }
        }
        done += chunk;
    }
    return (long)done;
}

long yfs_pwrite(uint32_t ino, uint32_t off, const void *src,
                uint32_t count)
{
    struct yfs_inode n;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t done = 0;

    if (inode_read(ino, &n) != 0)
        return -E_IO_L;
    if (n.type == 2)
        return -E_ISDIR_L;
    while (done < count) {
        uint32_t pos = off + done;
        uint32_t bi = pos / BLOCK_SIZE;
        uint32_t bo = pos % BLOCK_SIZE;
        uint32_t chunk = BLOCK_SIZE - bo;
        long nb;
        if (chunk > count - done)
            chunk = count - done;
        {
            uint32_t phys;
            if (bi >= MAX_BLOCKS)
                return done > 0 ? (long)done : -E_NOSPC_L;
            if (inode_get_block(&n, bi, &phys) != 0)
                return -E_IO_L;
            if (phys == UNALLOC) {
                nb = block_alloc();
                if (nb < 0)
                    return done > 0 ? (long)done : nb;
                phys = (uint32_t)nb;
                if (inode_set_block(&n, bi, phys) != 0)
                    return -E_IO_L;
            }
            if (chunk < BLOCK_SIZE) {
                if (block_read(phys, g_blk) != 0)
                    return -E_IO_L;
            }
            mem_copy(g_blk + bo, s + done, chunk);
            if (block_write(phys, g_blk) != 0)
                return -E_IO_L;
        }
        done += chunk;
        if (off + done > n.size)
            n.size = off + done;
    }
    if (inode_write(ino, &n) != 0)
        return -E_IO_L;
    return (long)done;
}

long yfs_mknod(const char *parent_rel, const char *name,
               bool is_dir)
{
    return yfs_mknod_t(parent_rel, name, is_dir ? 2 : 1);
}

int yfs_type_of(uint32_t ino)
{
    struct yfs_inode n;
    if (inode_read(ino, &n) != 0)
        return -1;
    return n.type;
}

/* M16: a symlink inode (type 3) whose content is the target
 * text, written through the regular content path. */
long yfs_symlink(const char *parent_rel, const char *name,
                 const char *target)
{
    long ino = yfs_mknod_t(parent_rel, name, 3);
    uint32_t len;
    if (ino < 0)
        return ino;
    len = (uint32_t)str_len(target);
    if (yfs_pwrite((uint32_t)ino, 0, (const uint8_t *)target,
                   len) != (long)len)
        return -E_IO_L;
    return ino;
}

static int yfs_truncate_impl(uint32_t ino, uint32_t size)
{
    struct yfs_inode n;
    uint32_t i, keep;

    if (inode_read(ino, &n) != 0)
        return -E_IO_L;
    if (size > n.size) { /* only shrinking is supported */
        n.size = size;
        return inode_write(ino, &n);
    }
    keep = (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (i = keep; i < MAX_BLOCKS; i++) {
        uint32_t phys;
        if (inode_get_block(&n, i, &phys) != 0)
            return -E_IO_L;
        if (phys != UNALLOC) {
            block_free(phys);
            if (inode_set_block(&n, i, UNALLOC) != 0)
                return -E_IO_L;
        }
    }
    if (keep <= DIRECT && n.blocks[DIRECT] != UNALLOC) {
        /* nothing indirect survives: free the table itself */
        block_free(n.blocks[DIRECT]);
        n.blocks[DIRECT] = UNALLOC;
    }
    n.size = size;
    return inode_write(ino, &n);
}

/* --- directories -------------------------------------------------- */

struct dirent_raw {
    uint32_t ino;
    char name[YFS_MAX_NAME + 1];
};

static long dir_find(uint32_t dino, const char *name,
                     uint32_t *slot_out)
{
    struct yfs_inode n;
    uint32_t slots, i;
    struct dirent_raw de;

    if (inode_read(dino, &n) != 0)
        return -E_IO_L;
    if (n.type != 2)
        return -E_NOTDIR_L;
    slots = n.size / DIRENT_SIZE;
    for (i = 0; i < slots; i++) {
        if (yfs_pread(dino, i * DIRENT_SIZE, &de, DIRENT_SIZE) !=
            DIRENT_SIZE)
            return -E_IO_L;
        if (de.name[0] != '\0' && str_eq_n(de.name, name, YFS_MAX_NAME + 1)) {
            if (slot_out != NULL)
                *slot_out = i;
            return (long)de.ino;
        }
    }
    return -E_NOENT_L;
}

/* Split "a/b/c" -- walk components from the root. */
long yfs_lookup(const char *rel)
{
    uint32_t cur = 0;
    const char *p = rel;
    char comp[YFS_MAX_NAME + 2];

    if (rel[0] == '\0' || (rel[0] == '/' && rel[1] == '\0'))
        return 0;
    while (*p != '\0') {
        uint64_t len = 0;
        long next;
        while (*p == '/')
            p++;
        if (*p == '\0')
            break;
        while (p[len] != '\0' && p[len] != '/') {
            if (len >= YFS_MAX_NAME + 1)
                return -E_NAMETOOLONG_L;
            comp[len] = p[len];
            len++;
        }
        comp[len] = '\0';
        next = dir_find(cur, comp, NULL);
        if (next < 0)
            return next;
        cur = (uint32_t)next;
        p += len;
    }
    return (long)cur;
}

bool yfs_is_dir(uint32_t ino)
{
    struct yfs_inode n;
    if (inode_read(ino, &n) != 0)
        return false;
    return n.type == 2;
}

uint32_t yfs_size(uint32_t ino)
{
    struct yfs_inode n;
    if (inode_read(ino, &n) != 0)
        return 0;
    return n.size;
}

/* Write bytes into a directory's own content (yfs_pwrite
 * refuses directories by design). Direct blocks only: a
 * directory tops out at DIRECT blocks of entries. */
static int dir_write_bytes(uint32_t dino, uint32_t off,
                           const void *src, uint32_t count)
{
    struct yfs_inode pn;
    const uint8_t *s8 = (const uint8_t *)src;
    uint32_t done = 0;

    if (inode_read(dino, &pn) != 0)
        return -E_IO_L;
    if (pn.type != 2)
        return -E_NOTDIR_L;
    while (done < count) {
        uint32_t pos = off + done;
        uint32_t bi = pos / BLOCK_SIZE, bo = pos % BLOCK_SIZE;
        uint32_t chunk = BLOCK_SIZE - bo;
        if (chunk > count - done)
            chunk = count - done;
        if (bi >= DIRECT)
            return -E_NOSPC_L;
        if (pn.blocks[bi] == UNALLOC) {
            long nb = block_alloc();
            if (nb < 0)
                return (int)nb;
            pn.blocks[bi] = (uint32_t)nb;
        }
        if (block_read(pn.blocks[bi], g_blk) != 0)
            return -E_IO_L;
        mem_copy(g_blk + bo, s8 + done, chunk);
        if (block_write(pn.blocks[bi], g_blk) != 0)
            return -E_IO_L;
        done += chunk;
        if (off + done > pn.size)
            pn.size = off + done;
    }
    return inode_write(dino, &pn);
}

static int dir_link(uint32_t pino, const char *name, uint32_t target)
{
    struct yfs_inode parent;
    struct dirent_raw de;
    uint32_t i, slot;

    if (inode_read(pino, &parent) != 0)
        return -E_IO_L;
    slot = parent.size / DIRENT_SIZE;
    for (i = 0; i < parent.size / DIRENT_SIZE; i++) {
        if (yfs_pread(pino, i * DIRENT_SIZE, &de, DIRENT_SIZE) !=
            DIRENT_SIZE)
            return -E_IO_L;
        if (de.name[0] == '\0') {
            slot = i;
            break;
        }
    }
    de.ino = target;
    mem_zero((uint8_t *)de.name, sizeof(de.name));
    mem_copy((uint8_t *)de.name, (const uint8_t *)name,
             str_len(name));
    return dir_write_bytes(pino, slot * DIRENT_SIZE, &de,
                           DIRENT_SIZE);
}

/* --- M13: removal, rename, open references -----------------------
 * rsvd[0] of an inode carries the open-file reference count
 * in its low 16 bits and the unlinked flag at 0x10000. An
 * unlinked inode's blocks are freed when the count reaches
 * zero (immediately when it is already zero). */

#define REFS_OF(n)   ((n)->rsvd[0] & 0xFFFFu)
#define UNLINKED_OF(n) (((n)->rsvd[0] & 0x10000u) != 0)

static int free_inode_impl(uint32_t ino)
{
    struct yfs_inode n;
    if (yfs_truncate(ino, 0) != 0)
        return -E_IO_L;
    if (inode_read(ino, &n) != 0)
        return -E_IO_L;
    n.type = 0;
    n.rsvd[0] = 0;
    return inode_write(ino, &n);
}

static int free_inode(uint32_t ino)
{
    int rc;
    txn_begin();
    rc = free_inode_impl(ino);
    txn_finish(rc == 0);
    return rc;
}

static int remove_dirent(uint32_t pino, uint32_t slot,
                         uint32_t target)
{
    struct dirent_raw zero;
    struct yfs_inode t;

    mem_zero((uint8_t *)&zero, sizeof(zero));
    if (dir_write_bytes(pino, slot * DIRENT_SIZE, &zero,
                        DIRENT_SIZE) != 0)
        return -E_IO_L;
    if (inode_read(target, &t) != 0)
        return -E_IO_L;
    t.rsvd[0] |= 0x10000u;
    if (inode_write(target, &t) != 0)
        return -E_IO_L;
    if (REFS_OF(&t) == 0)
        return free_inode(target);
    return 0;
}

static int dir_is_empty(uint32_t ino)
{
    struct yfs_inode n;
    struct dirent_raw de;
    uint32_t i, slots;
    if (inode_read(ino, &n) != 0)
        return -E_IO_L;
    slots = n.size / DIRENT_SIZE;
    for (i = 0; i < slots; i++) {
        if (yfs_pread(ino, i * DIRENT_SIZE, &de, DIRENT_SIZE) !=
            DIRENT_SIZE)
            return -E_IO_L;
        if (de.name[0] != '\0')
            return 0;
    }
    return 1;
}

static long yfs_unlink_impl(const char *parent_rel,
                            const char *name, bool want_dir)
{
    long pino = yfs_lookup(parent_rel);
    uint32_t slot;
    long target;
    struct yfs_inode t;

    if (pino < 0)
        return pino;
    target = dir_find((uint32_t)pino, name, &slot);
    if (target < 0)
        return target;
    if (inode_read((uint32_t)target, &t) != 0)
        return -E_IO_L;
    if (want_dir) {
        int empty;
        if (t.type != 2)
            return -E_NOTDIR_L;
        if (target == 0)
            return -E_INVAL_L;
        empty = dir_is_empty((uint32_t)target);
        if (empty < 0)
            return empty;
        if (!empty)
            return -39; /* ENOTEMPTY */
    } else {
        if (t.type == 2)
            return -E_ISDIR_L;
    }
    return remove_dirent((uint32_t)pino, slot, (uint32_t)target);
}

/* Split "a/b/c" into parent "a/b" + name "c" ("" parent = root). */
static void split_rel(const char *rel, char *parent, char *name)
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

static long yfs_rename_impl(const char *old_rel,
                            const char *new_rel)
{
    char oparent[128], oname[40], nparent[128], nname[40];
    long src, npino, existing;
    struct yfs_inode sn, tn;
    uint32_t oslot, tslot;

    src = yfs_lookup(old_rel);
    if (src < 0)
        return src;
    if (src == 0)
        return -E_INVAL_L;
    if (inode_read((uint32_t)src, &sn) != 0)
        return -E_IO_L;
    split_rel(old_rel, oparent, oname);
    split_rel(new_rel, nparent, nname);
    if (str_len(nname) > YFS_MAX_NAME)
        return -E_NAMETOOLONG_L;
    npino = yfs_lookup(nparent);
    if (npino < 0)
        return npino;
    if (inode_read((uint32_t)npino, &tn) != 0)
        return -E_IO_L;
    if (tn.type != 2)
        return -E_NOTDIR_L;
    existing = dir_find((uint32_t)npino, nname, &tslot);
    if (existing >= 0 && existing != src) {
        struct yfs_inode en;
        int rc;
        if (inode_read((uint32_t)existing, &en) != 0)
            return -E_IO_L;
        if (en.type == 2) {
            int empty;
            if (sn.type != 2)
                return -E_ISDIR_L;
            empty = dir_is_empty((uint32_t)existing);
            if (empty < 0)
                return empty;
            if (!empty)
                return -39;
        } else if (sn.type == 2) {
            return -E_NOTDIR_L;
        }
        rc = remove_dirent((uint32_t)npino, tslot,
                           (uint32_t)existing);
        if (rc != 0)
            return rc;
    }
    if (existing == src)
        return 0; /* rename onto itself */
    if (dir_link((uint32_t)npino, nname, (uint32_t)src) != 0)
        return -E_IO_L;
    {
        long opino = yfs_lookup(oparent);
        if (opino < 0)
            return opino;
        if (dir_find((uint32_t)opino, oname, &oslot) != src)
            return -E_IO_L;
        {
            struct dirent_raw zero;
            mem_zero((uint8_t *)&zero, sizeof(zero));
            if (dir_write_bytes((uint32_t)opino,
                                oslot * DIRENT_SIZE, &zero,
                                DIRENT_SIZE) != 0)
                return -E_IO_L;
        }
    }
    return 0;
}

void yfs_note_open(uint32_t ino)
{
    struct yfs_inode n;
    if (inode_read(ino, &n) != 0)
        return;
    n.rsvd[0] = (n.rsvd[0] & 0xFFFF0000u) | ((REFS_OF(&n) + 1) & 0xFFFFu);
    inode_write(ino, &n);
}

void yfs_note_close(uint32_t ino)
{
    struct yfs_inode n;
    if (inode_read(ino, &n) != 0)
        return;
    if (REFS_OF(&n) > 0)
        n.rsvd[0] = (n.rsvd[0] & 0xFFFF0000u) | (REFS_OF(&n) - 1);
    else
        return;
    inode_write(ino, &n);
    if (UNLINKED_OF(&n) && REFS_OF(&n) == 0)
        free_inode(ino);
}

static long yfs_mknod_t_impl(const char *parent_rel,
                             const char *name, uint8_t type)
{
    long pino;
    struct yfs_inode parent, fresh;
    uint32_t i, free_ino = 0xFFFFFFFFu;

    if (str_len(name) > YFS_MAX_NAME)
        return -E_NAMETOOLONG_L;
    pino = yfs_lookup(parent_rel);
    if (pino < 0)
        return pino;
    if (inode_read((uint32_t)pino, &parent) != 0)
        return -E_IO_L;
    if (parent.type != 2)
        return -E_NOTDIR_L;
    if (dir_find((uint32_t)pino, name, NULL) >= 0)
        return -E_EXIST_L;
    for (i = 1; i < INODE_COUNT; i++) {
        struct yfs_inode t;
        if (inode_read(i, &t) != 0)
            return -E_IO_L;
        if (t.type == 0) {
            free_ino = i;
            break;
        }
    }
    if (free_ino == 0xFFFFFFFFu)
        return -E_NOSPC_L;
    mem_zero((uint8_t *)&fresh, sizeof(fresh));
    fresh.type = type;
    fresh.size = 0;
    for (i = 0; i < 12; i++)
        fresh.blocks[i] = UNALLOC; /* all slots incl. indirect */
    if (inode_write(free_ino, &fresh) != 0)
        return -E_IO_L;
    if (dir_link((uint32_t)pino, name, free_ino) != 0)
        return -E_IO_L;
    return (long)free_ino;
}

/* --- M19: transactional wrappers. The public metadata
 * operations stage every sector write and commit through
 * the journal; a negative result discards the stage, so a
 * failed operation leaves the disk untouched. */

int yfs_truncate(uint32_t ino, uint32_t size)
{
    int rc;
    txn_begin();
    rc = yfs_truncate_impl(ino, size);
    txn_finish(rc == 0);
    return rc;
}

long yfs_unlink(const char *parent_rel, const char *name,
                bool want_dir)
{
    long rc;
    txn_begin();
    rc = yfs_unlink_impl(parent_rel, name, want_dir);
    txn_finish(rc >= 0);
    return rc;
}

long yfs_rename(const char *old_rel, const char *new_rel)
{
    long rc;
    txn_begin();
    rc = yfs_rename_impl(old_rel, new_rel);
    txn_finish(rc >= 0);
    return rc;
}

long yfs_mknod_t(const char *parent_rel, const char *name,
                 uint8_t type)
{
    long rc;
    txn_begin();
    rc = yfs_mknod_t_impl(parent_rel, name, type);
    txn_finish(rc >= 0);
    return rc;
}

int yfs_readdir(uint32_t ino, uint32_t *slot, char *name_out,
                uint32_t *ino_out, int *is_dir_out)
{
    struct yfs_inode n;
    struct dirent_raw de;
    uint32_t slots, i;

    if (inode_read(ino, &n) != 0 || n.type != 2)
        return 0;
    slots = n.size / DIRENT_SIZE;
    for (i = *slot; i < slots; i++) {
        struct yfs_inode cn;
        if (yfs_pread(ino, i * DIRENT_SIZE, &de, DIRENT_SIZE) !=
            DIRENT_SIZE)
            return 0;
        if (de.name[0] == '\0')
            continue;
        mem_copy((uint8_t *)name_out, (const uint8_t *)de.name,
                 YFS_MAX_NAME + 1);
        name_out[YFS_MAX_NAME] = '\0';
        *ino_out = de.ino;
        if (inode_read(de.ino, &cn) != 0)
            return 0;
        *is_dir_out = cn.type == 2 ? 1 : 0;
        *slot = i + 1;
        return 1;
    }
    *slot = slots;
    return 0;
}

/* --- format & mount ----------------------------------------------- */

static bool sector_is_zero(const uint8_t *sec)
{
    int i;
    for (i = 0; i < ATA_SECTOR_SIZE; i++)
        if (sec[i] != 0)
            return false;
    return true;
}

static bool sector_has_magic(const uint8_t *sec)
{
    const char *magic = "YOUSJFS1";
    int i;
    for (i = 0; i < 8; i++)
        if (sec[i] != (uint8_t)magic[i])
            return false;
    return true;
}

static int yfs_format(struct ata_drive *d)
{
    uint32_t s;
    struct yfs_inode root;
    uint32_t i;

    g_drv = *d;
    g_data_blocks = (d->sectors - DATA_SECTOR0) / BLOCK_SECTORS;
    mem_zero(g_sec, ATA_SECTOR_SIZE);
    mem_copy(g_sec, (const uint8_t *)"YOUSJFS1", 8);
    *(uint32_t *)(g_sec + 8) = 3; /* version: M19 journal */
    *(uint32_t *)(g_sec + 12) = d->sectors;
    *(uint32_t *)(g_sec + 16) = g_data_blocks;
    if (ata_write(d, SB_SECTOR, 1, g_sec) != 0)
        return -E_IO_L;
    mem_zero(g_sec, ATA_SECTOR_SIZE);
    for (s = INODE_SECTOR0; s < INODE_SECTOR0 + 16; s++)
        if (ata_write(d, s, 1, g_sec) != 0)
            return -E_IO_L;
    for (s = 0; s < BITMAP_SECTORS; s++)
        if (ata_write(d, BITMAP_SECTOR0 + s, 1, g_sec) != 0)
            return -E_IO_L;
    for (s = 0; s < 1 + TXN_MAX; s++)
        if (ata_write(d, JRNL_SECTOR0 + s, 1, g_sec) != 0)
            return -E_IO_L;
    /* root inode: a directory */
    mem_zero((uint8_t *)&root, sizeof(root));
    root.type = 2;
    root.size = 0;
    for (i = 0; i < 12; i++)
        root.blocks[i] = UNALLOC; /* all slots incl. indirect */
    if (inode_write(0, &root) != 0)
        return -E_IO_L;
    g_mounted = true;
    return 0;
}

int yfs_mount(void)
{
    int bus;
    struct ata_drive cand;
    bool have_cand = false;

    g_mounted = false;
    for (bus = 0; bus < 2; bus++) {
        struct ata_drive d;
        if (ata_probe(bus, &d) != 0)
            continue;
        if (ata_read(&d, SB_SECTOR, 1, g_sec) != 0)
            continue;
        if (sector_has_magic(g_sec) &&
            *(const uint32_t *)(g_sec + 8) == 3) {
            g_drv = d;
            g_data_blocks = *(const uint32_t *)(g_sec + 16);
            if (g_data_blocks == 0)
                g_data_blocks =
                    (d.sectors - DATA_SECTOR0) / BLOCK_SECTORS;
            g_mounted = true;
            journal_replay();
            return 1;
        }
        if (!have_cand && sector_is_zero(g_sec)) {
            cand = d;
            have_cand = true;
        }
    }
    if (have_cand) {
        if (yfs_format(&cand) != 0)
            return -E_IO_L;
        return 2;
    }
    return 0;
}

bool yfs_mounted(void)
{
    return g_mounted;
}

uint32_t yfs_disk_sectors(void)
{
    return g_mounted ? g_drv.sectors : 0;
}
