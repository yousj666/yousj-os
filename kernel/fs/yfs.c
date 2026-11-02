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
#define DATA_SECTOR0    25
#define BLOCK_SIZE      4096
#define BLOCK_SECTORS   8
#define DIRECT          12
#define DIRENT_SIZE     32
#define UNALLOC         0xFFFFFFFFu

struct yfs_inode {
    uint8_t type; /* 0 free, 1 file, 2 dir */
    uint8_t pad[3];
    uint32_t size;
    uint32_t blocks[DIRECT];
    uint32_t rsvd[2];
};

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

/* --- raw inode access ------------------------------------------- */

static int inode_read(uint32_t ino, struct yfs_inode *out)
{
    uint32_t sec = INODE_SECTOR0 + (ino * INODE_SIZE) / ATA_SECTOR_SIZE;
    uint32_t off = (ino * INODE_SIZE) % ATA_SECTOR_SIZE;
    if (ata_read(&g_drv, sec, 1, g_sec) != 0)
        return -E_IO_L;
    mem_copy((uint8_t *)out, g_sec + off, INODE_SIZE);
    return 0;
}

static int inode_write(uint32_t ino, const struct yfs_inode *in)
{
    uint32_t sec = INODE_SECTOR0 + (ino * INODE_SIZE) / ATA_SECTOR_SIZE;
    uint32_t off = (ino * INODE_SIZE) % ATA_SECTOR_SIZE;
    if (ata_read(&g_drv, sec, 1, g_sec) != 0)
        return -E_IO_L;
    mem_copy(g_sec + off, (const uint8_t *)in, INODE_SIZE);
    if (ata_write(&g_drv, sec, 1, g_sec) != 0)
        return -E_IO_L;
    return 0;
}

/* --- block allocation ------------------------------------------- */

static long block_alloc(void)
{
    uint32_t s, i;
    for (s = 0; s < BITMAP_SECTORS; s++) {
        if (ata_read(&g_drv, BITMAP_SECTOR0 + s, 1, g_sec) != 0)
            return -E_IO_L;
        for (i = 0; i < ATA_SECTOR_SIZE * 8; i++) {
            uint32_t blk = s * ATA_SECTOR_SIZE * 8 + i;
            if (blk >= g_data_blocks)
                return -E_NOSPC_L;
            if (!(g_sec[i / 8] & (uint8_t)(1u << (i % 8)))) {
                g_sec[i / 8] |= (uint8_t)(1u << (i % 8));
                if (ata_write(&g_drv, BITMAP_SECTOR0 + s, 1,
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
    if (ata_read(&g_drv, BITMAP_SECTOR0 + s, 1, g_sec) != 0)
        return;
    g_sec[i / 8] &= (uint8_t)~(1u << (i % 8));
    ata_write(&g_drv, BITMAP_SECTOR0 + s, 1, g_sec);
}

static int block_read(uint32_t blk, uint8_t *dst)
{
    return ata_read(&g_drv, DATA_SECTOR0 + blk * BLOCK_SECTORS,
                    BLOCK_SECTORS, dst) == 0 ? 0 : -E_IO_L;
}

static int block_write(uint32_t blk, const uint8_t *src)
{
    return ata_write(&g_drv, DATA_SECTOR0 + blk * BLOCK_SECTORS,
                     BLOCK_SECTORS, src) == 0 ? 0 : -E_IO_L;
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
        if (bi >= DIRECT || n.blocks[bi] == UNALLOC) {
            uint32_t i;
            for (i = 0; i < chunk; i++)
                d[done + i] = 0;
        } else {
            if (block_read(n.blocks[bi], g_blk) != 0)
                return -E_IO_L;
            mem_copy(d + done, g_blk + bo, chunk);
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
        if (bi >= DIRECT)
            return done > 0 ? (long)done : -E_NOSPC_L;
        if (n.blocks[bi] == UNALLOC) {
            nb = block_alloc();
            if (nb < 0)
                return done > 0 ? (long)done : nb;
            n.blocks[bi] = (uint32_t)nb;
        }
        if (chunk < BLOCK_SIZE) {
            if (block_read(n.blocks[bi], g_blk) != 0)
                return -E_IO_L;
        }
        mem_copy(g_blk + bo, s + done, chunk);
        if (block_write(n.blocks[bi], g_blk) != 0)
            return -E_IO_L;
        done += chunk;
        if (off + done > n.size)
            n.size = off + done;
    }
    if (inode_write(ino, &n) != 0)
        return -E_IO_L;
    return (long)done;
}

int yfs_truncate(uint32_t ino, uint32_t size)
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
    for (i = keep; i < DIRECT; i++) {
        if (n.blocks[i] != UNALLOC) {
            block_free(n.blocks[i]);
            n.blocks[i] = UNALLOC;
        }
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

long yfs_mknod(const char *parent_rel, const char *name, bool is_dir)
{
    long pino;
    struct yfs_inode parent, fresh;
    uint32_t i, free_ino = 0xFFFFFFFFu, slot;
    struct dirent_raw de;

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
    fresh.type = is_dir ? 2 : 1;
    fresh.size = 0;
    for (i = 0; i < DIRECT; i++)
        fresh.blocks[i] = UNALLOC;
    if (inode_write(free_ino, &fresh) != 0)
        return -E_IO_L;
    /* link it into the parent: first empty slot, else append */
    slot = parent.size / DIRENT_SIZE;
    for (i = 0; i < parent.size / DIRENT_SIZE; i++) {
        if (yfs_pread((uint32_t)pino, i * DIRENT_SIZE, &de,
                      DIRENT_SIZE) != DIRENT_SIZE)
            return -E_IO_L;
        if (de.name[0] == '\0') {
            slot = i;
            break;
        }
    }
    de.ino = free_ino;
    mem_zero((uint8_t *)de.name, sizeof(de.name));
    mem_copy((uint8_t *)de.name, (const uint8_t *)name,
             str_len(name));
    {
        /* directories write their own content: go through the
         * inode directly (yfs_pwrite refuses dirs by design). */
        struct yfs_inode pn;
        uint32_t pos = slot * DIRENT_SIZE;
        uint32_t bi = pos / BLOCK_SIZE, bo = pos % BLOCK_SIZE;
        if (inode_read((uint32_t)pino, &pn) != 0)
            return -E_IO_L;
        if (bi >= DIRECT)
            return -E_NOSPC_L;
        if (pn.blocks[bi] == UNALLOC) {
            long nb = block_alloc();
            if (nb < 0)
                return nb;
            pn.blocks[bi] = (uint32_t)nb;
        }
        if (block_read(pn.blocks[bi], g_blk) != 0)
            return -E_IO_L;
        mem_copy(g_blk + bo, (const uint8_t *)&de, DIRENT_SIZE);
        if (block_write(pn.blocks[bi], g_blk) != 0)
            return -E_IO_L;
        if (pos + DIRENT_SIZE > pn.size)
            pn.size = pos + DIRENT_SIZE;
        if (inode_write((uint32_t)pino, &pn) != 0)
            return -E_IO_L;
    }
    return (long)free_ino;
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
    *(uint32_t *)(g_sec + 8) = 1; /* version */
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
    /* root inode: a directory */
    mem_zero((uint8_t *)&root, sizeof(root));
    root.type = 2;
    root.size = 0;
    for (i = 0; i < DIRECT; i++)
        root.blocks[i] = UNALLOC;
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
        if (sector_has_magic(g_sec)) {
            g_drv = d;
            g_data_blocks = *(const uint32_t *)(g_sec + 16);
            if (g_data_blocks == 0)
                g_data_blocks =
                    (d.sectors - DATA_SECTOR0) / BLOCK_SECTORS;
            g_mounted = true;
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
