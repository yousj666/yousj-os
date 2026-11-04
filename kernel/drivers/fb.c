/* Yousj OS - linear framebuffer driver (M81, GUI step 1).
 *
 * Written from scratch. Takes the GOP framebuffer the UEFI
 * bootloader set up (boot_info.fb), maps it into the master
 * address space on top of the HHDM offset -- the HHDM built by
 * vmm_hw_init() only spans RAM, and the framebuffer is a PCI BAR
 * above it, so the driver extends the direct map over exactly
 * the fb range -- and paints the first Yousj screen: a banner,
 * the mode line, and four color swatches that make a wrong
 * channel order visible at a glance.
 *
 * Everything is synchronous stores to the BAR; no double buffer
 * yet (QEMU and typical GOP firmware display stores directly).
 * A multiboot boot has fb all zero and this driver stays dark.
 */
#include <yousj/types.h>
#include <yousj/boot.h>
#include <yousj/vmm.h>
#include <yousj/fb.h>
#include <drivers/serial.h>

#include "font8x16.h"

static struct {
    int      present;
    uint32_t *px;      /* HHDM pointer to the first pixel */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;    /* bytes per scanline */
    int      bgr;      /* 1: lowest byte is blue (UEFI format 1) */
} g_fb;

/* 0xRRGGBB -> the 32-bit store value for this panel's order. */
static uint32_t pack(uint32_t rgb)
{
    if (g_fb.bgr)
        return rgb; /* memory bytes: B, G, R, X */
    /* memory bytes: R, G, B, X */
    return ((rgb & 0xFFu) << 16) | (rgb & 0xFF00u) |
           ((rgb >> 16) & 0xFFu);
}

int fb_present(void)
{
    return g_fb.present;
}

uint32_t fb_width(void)
{
    return g_fb.width;
}

uint32_t fb_height(void)
{
    return g_fb.height;
}

void fb_clear(uint32_t rgb)
{
    uint32_t v = pack(rgb);
    uint32_t y, x;
    if (!g_fb.present)
        return;
    for (y = 0; y < g_fb.height; y++) {
        uint32_t *row = (uint32_t *)((uint8_t *)g_fb.px +
                                     (uint64_t)y * g_fb.pitch);
        for (x = 0; x < g_fb.width; x++)
            row[x] = v;
    }
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t rgb)
{
    uint32_t v = pack(rgb);
    uint32_t xx, yy;
    if (!g_fb.present)
        return;
    if (x >= g_fb.width || y >= g_fb.height)
        return;
    if (x + w > g_fb.width)
        w = g_fb.width - x;
    if (y + h > g_fb.height)
        h = g_fb.height - y;
    for (yy = y; yy < y + h; yy++) {
        uint32_t *row = (uint32_t *)((uint8_t *)g_fb.px +
                                     (uint64_t)yy * g_fb.pitch);
        for (xx = x; xx < x + w; xx++)
            row[xx] = v;
    }
}

void fb_draw_text(uint32_t x, uint32_t y, const char *text,
                  uint32_t fg, uint32_t bg, uint32_t scale)
{
    uint32_t fg_v = pack(fg), bg_v = pack(bg);
    uint32_t cx = x;
    if (!g_fb.present || scale == 0)
        return;
    for (; *text != '\0'; text++) {
        uint8_t c = (uint8_t)*text;
        const uint8_t *glyph;
        uint32_t row, col, sx, sy;
        if (c < 32 || c > 126)
            c = '?';
        glyph = g_font8x16[c - 32];
        for (row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (col = 0; col < 8; col++) {
                uint32_t v = (bits & (0x80u >> col)) ? fg_v : bg_v;
                for (sy = 0; sy < scale; sy++) {
                    uint32_t py = y + row * scale + sy;
                    uint32_t *prow;
                    if (py >= g_fb.height)
                        break;
                    prow = (uint32_t *)((uint8_t *)g_fb.px +
                                        (uint64_t)py * g_fb.pitch);
                    for (sx = 0; sx < scale; sx++) {
                        uint32_t px = cx + col * scale + sx;
                        if (px < g_fb.width)
                            prow[px] = v;
                    }
                }
            }
        }
        cx += 8 * scale;
        if (cx + 8 * scale > g_fb.width)
            break; /* no wrapping in the raw layer */
    }
}

/* The M81 first screen. Deliberately simple shapes (fills and one
 * font) so a screendump check can verify them mechanically: the
 * background color, the title-bar color, and four swatches whose
 * positions and colors are fixed fractions of the mode. */
static void fb_banner(void)
{
    char mode_line[64];
    uint32_t i = 0;
    const char *pre = "framebuffer online - ";
    uint32_t sw_y, sw_w;

    fb_clear(0x10243E);                       /* deep blue field */
    fb_fill_rect(0, 0, g_fb.width, 64, 0x1B3A5C); /* title bar */
    fb_draw_text(24, 12, "YOUSJ OS", 0xFFFFFF, 0x1B3A5C, 3);
    fb_draw_text(24, 96, "M81: first screen (GOP framebuffer)",
                 0xD7E3F4, 0x10243E, 2);

    /* "<W>x<H>" built by hand: no printf in this driver. */
    {
        char digits[16];
        uint32_t n = 0, v;
        const char *s;
        for (s = pre; *s; s++)
            mode_line[i++] = *s;
        v = g_fb.width;
        do { digits[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
        while (n)
            mode_line[i++] = digits[--n];
        mode_line[i++] = 'x';
        n = 0;
        v = g_fb.height;
        do { digits[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
        while (n)
            mode_line[i++] = digits[--n];
        mode_line[i] = '\0';
    }
    fb_draw_text(24, 140, mode_line, 0x9FB8D8, 0x10243E, 2);

    /* Channel-order swatches: pure red, green, blue, white. */
    sw_y = g_fb.height - 120;
    sw_w = g_fb.width / 8;
    fb_fill_rect(sw_w * 1, sw_y, sw_w, 60, 0xFF0000);
    fb_fill_rect(sw_w * 3, sw_y, sw_w, 60, 0x00FF00);
    fb_fill_rect(sw_w * 5, sw_y, sw_w, 60, 0x0000FF);
    fb_fill_rect(sw_w * 7 - 24, sw_y, 24, 60, 0xFFFFFF);
}

/* ---------------------------------------------------------------- */
/* Text console (M82). A character grid living under the banner:    */
/* x margin 8, top at y=192, bottom keeps the swatch strip clear.   */
/* serial_putc() mirrors every byte here once fb_init() flips       */
/* g_con.active, so kernel log, shell output and echo all appear    */
/* on screen as they happen. Scrolling moves the region up 16       */
/* pixel rows at a time; the cursor is a 2-row underline in the     */
/* current cell (erased by repainting those rows with bg -- the     */
/* cursor only ever rests on a fresh cell in left-to-right flow).   */
/* ---------------------------------------------------------------- */

#define FB_CON_FG 0xD7E3F4u
#define FB_CON_BG 0x10243Eu
#define FB_CON_TOP 192u
#define FB_CON_BOTTOM_MARGIN 140u

static struct {
    int      active;
    uint32_t cx, cy;
    uint32_t cols, rows;
    uint32_t top;      /* pixel y of row 0 */
} g_con;

static uint32_t con_cell_x(uint32_t cx)
{
    return 8 + cx * 8;
}

static uint32_t con_cell_y(uint32_t cy)
{
    return g_con.top + cy * 16;
}

static void con_cursor(int on)
{
    fb_fill_rect(con_cell_x(g_con.cx), con_cell_y(g_con.cy) + 13,
                 8, 2, on ? FB_CON_FG : FB_CON_BG);
}

static void con_scroll(void)
{
    uint32_t bottom = g_con.top + g_con.rows * 16;
    uint32_t y;
    for (y = g_con.top; y < bottom - 16; y++) {
        uint8_t *dst = (uint8_t *)g_fb.px + (uint64_t)y * g_fb.pitch;
        const uint8_t *src = dst + (uint64_t)16 * g_fb.pitch;
        uint32_t i, n = g_fb.width * 4;
        for (i = 0; i < n; i++)
            dst[i] = src[i];
    }
    fb_fill_rect(0, bottom - 16, g_fb.width, 16, FB_CON_BG);
}

static void con_newline(void)
{
    g_con.cx = 0;
    if (g_con.cy + 1 >= g_con.rows)
        con_scroll();
    else
        g_con.cy++;
}

void fb_console_putc(char c)
{
    if (!g_con.active)
        return;
    con_cursor(0);
    if (c == '\n') {
        con_newline();
    } else if (c == '\r') {
        g_con.cx = 0;
    } else if (c == '\t') {
        g_con.cx = (g_con.cx + 8) & ~7u;
        if (g_con.cx >= g_con.cols)
            con_newline();
    } else if (c >= 32 && c < 127) {
        char buf[2];
        buf[0] = c;
        buf[1] = '\0';
        fb_draw_text(con_cell_x(g_con.cx), con_cell_y(g_con.cy),
                     buf, FB_CON_FG, FB_CON_BG, 1);
        g_con.cx++;
        if (g_con.cx >= g_con.cols)
            con_newline();
    }
    /* other control bytes: ignored on screen (serial keeps them) */
    con_cursor(1);
}

static void con_activate(void)
{
    uint32_t bottom;
    g_con.top = FB_CON_TOP;
    bottom = g_fb.height > FB_CON_BOTTOM_MARGIN
                 ? g_fb.height - FB_CON_BOTTOM_MARGIN
                 : g_fb.height;
    if (bottom <= g_con.top + 64) { /* tiny mode: shrink the banner */
        g_con.top = 80;
        bottom = g_fb.height;
    }
    g_con.cols = (g_fb.width - 16) / 8;
    g_con.rows = (bottom - g_con.top) / 16;
    if (g_con.cols == 0 || g_con.rows == 0)
        return;
    fb_fill_rect(0, g_con.top, g_fb.width, g_con.rows * 16, FB_CON_BG);
    g_con.cx = 0;
    g_con.cy = 0;
    g_con.active = 1;
    con_cursor(1);
}

void fb_init(const struct yousj_framebuffer *f)
{
    uint64_t base, end;
    int rc;

    if (f == NULL)
        return;
    if (f->addr == 0 || f->bpp != 32 || f->width == 0 ||
        f->height == 0 || f->pitch < f->width * 4u)
        return; /* headless: serial only, as before M81 */

    base = f->addr & ~(uint64_t)0xFFF;
    end = (f->addr + (uint64_t)f->pitch * f->height + 0xFFF) &
          ~(uint64_t)0xFFF;
    rc = vmm_map(vmm_kernel_master(), YOUSJ_HHDM_BASE + base, base,
                 end - base, YOUSJ_VMM_READ | YOUSJ_VMM_WRITE);
    if (rc != 0) {
        serial_puts("Yousj OS: fb map FAILED, staying headless\n");
        return;
    }

    g_fb.px = (uint32_t *)(uintptr_t)yousj_phys_to_virt(f->addr);
    g_fb.width = f->width;
    g_fb.height = f->height;
    g_fb.pitch = f->pitch;
    g_fb.bgr = (f->reserved[0] == 1);
    g_fb.present = 1;

    fb_banner();

    serial_puts("Yousj OS: fb ");
    serial_put_u64(g_fb.width);
    serial_puts("x");
    serial_put_u64(g_fb.height);
    serial_puts(g_fb.bgr ? " BGRx @ " : " RGBx @ ");
    serial_put_hex64(f->addr);
    serial_puts("\n");

    /* M82: from the next byte on, the serial console mirrors to
     * the screen (see fb_console_putc). Activated last so the
     * report line above stays serial-only history. */
    con_activate();
}
