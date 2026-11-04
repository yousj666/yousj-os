/* Yousj OS - window layer (M85; desktop M86; user windows
 * M88; real desktop M91).
 *
 * Written from scratch. One demonstration window, drawn
 * with fb primitives, proves the mechanics; M86 adds the
 * taskbar/start menu furniture; M88 generalises the layer
 * into a small service: user tasks open windows that are
 * character-cell text surfaces, stream bytes into them,
 * and receive the keyboard while focused.
 *
 * M91 turns the screen into a real desktop: the wallpaper
 * is the base surface (the M82 screen console no longer
 * paints over it), icons live in a column at the top left,
 * a double-click launches, and erasing a window repaints
 * wallpaper, then icons, then the windows still standing
 * over the area. The old demonstration window becomes the
 * About box.
 */
#include <yousj/types.h>
#include <yousj/wm.h>
#include <yousj/fb.h>
#include <yousj/ps2kbd.h> /* M89: the menu's Terminal entry */
#include <yousj/sched.h>  /* M91: uptime ticks time the
                             double-click */

#define WIN_W 560u
#define WIN_H 340u
#define WIN_TITLE 28u
#define WIN_MIN_Y 8u /* M91: no banner anymore; the whole
                        desktop is usable window space */

#define COL_BORDER 0x9FB8D8u
#define COL_TITLE  0x2E5E8Cu
#define COL_BODY   0x24466Bu
#define COL_CLOSE  0xC0392Bu
#define COL_TEXT   0xFFFFFFu
#define COL_BTEXT  0xD7E3F4u

/* Desktop furniture (M86): a 40px taskbar along the bottom,
 * a start button, a start menu with one entry, and a taskbar
 * chip naming the open window. All of it lives below the
 * console region, so it never fights the console's repaint. */
#define COL_BAR    0x1B3A5Cu
#define COL_FIELD  0x10243Eu
#define BAR_H      40u
#define BTN_X      8u
#define BTN_W      96u
#define CHIP_X     116u
#define CHIP_W     170u
#define MENU_W     220u
#define MENU_H     84u /* M89: two entries since the menu
                          grew a Terminal launcher */

/* --- desktop icons (M91) --------------------------------- */
/* A left column of launchers. Each cell is 96x116: a 72x72
 * glyph tile with a label under it. One click selects (the
 * label highlights), a second click inside 45 ticks
 * (about half a second) launches. */
#define ICON_X    24u
#define ICON_Y    24u
#define ICON_STEP 124u
#define ICON_CELL_W 96u
#define ICON_CELL_H 116u
#define ICON_TILE   72u
#define ICON_DBL_TICKS 60u

#define ICON_TERMINAL 0
#define ICON_ABOUT    1
#define ICON_COUNT    2

static const char *const g_icon_labels[ICON_COUNT] = {
    "Terminal",
    "About",
};

static int g_icon_sel = -1;
static uint64_t g_icon_last_tick;

static uint32_t icon_x(int i)
{
    (void)i;
    return ICON_X;
}

static uint32_t icon_y(int i)
{
    return ICON_Y + (uint32_t)i * ICON_STEP;
}

static void icon_draw(int i)
{
    uint32_t x = icon_x(i), y = icon_y(i);
    uint32_t tx = x + (ICON_CELL_W - ICON_TILE) / 2;
    const char *label = g_icon_labels[i];
    uint32_t len = 0, lx;
    while (label[len] != '\0')
        len++;
    lx = x + (ICON_CELL_W - len * 8) / 2;

    /* repaint the cell's wallpaper first, so deselecting
     * leaves no trace */
    fb_desktop_paint_rect(x, y, ICON_CELL_W, ICON_CELL_H);
    if (i == ICON_TERMINAL) {
        fb_fill_rect(tx, y, ICON_TILE, ICON_TILE, 0x0A141F);
        fb_draw_text(tx + 13, y + 24, ">_", 0x4ADE80,
                     0x0A141F, 3);
    } else {
        fb_fill_rect(tx, y, ICON_TILE, ICON_TILE, COL_TITLE);
        fb_draw_text(tx + 22, y + 20, "Y", COL_TEXT,
                     COL_TITLE, 4);
    }
    fb_fill_rect(tx, y, ICON_TILE, 1, COL_BORDER);
    fb_fill_rect(tx, y + ICON_TILE - 1, ICON_TILE, 1,
                 COL_BORDER);
    fb_fill_rect(tx, y, 1, ICON_TILE, COL_BORDER);
    fb_fill_rect(tx + ICON_TILE - 1, y, 1, ICON_TILE,
                 COL_BORDER);
    if (g_icon_sel == i)
        fb_draw_text(lx, y + 84, label, COL_TEXT, COL_TITLE,
                     1);
    else
        fb_draw_text(lx, y + 84, label, COL_TEXT,
                     FB_TEXT_TRANSPARENT, 1);
}

static void icons_draw(void)
{
    int i;
    for (i = 0; i < ICON_COUNT; i++)
        icon_draw(i);
}

/* Redraw any icon the rectangle touches (window erases). */
static void icons_repair(uint32_t x, uint32_t y, uint32_t w,
                         uint32_t h)
{
    int i;
    for (i = 0; i < ICON_COUNT; i++) {
        uint32_t ix = icon_x(i), iy = icon_y(i);
        if (x < ix + ICON_CELL_W && x + w > ix &&
            y < iy + ICON_CELL_H && y + h > iy)
            icon_draw(i);
    }
}

static int icon_hit(uint32_t mx, uint32_t my)
{
    int i;
    for (i = 0; i < ICON_COUNT; i++) {
        uint32_t ix = icon_x(i), iy = icon_y(i);
        if (mx >= ix && mx < ix + ICON_CELL_W && my >= iy &&
            my < iy + ICON_CELL_H)
            return i;
    }
    return -1;
}

static void window_open(void); /* below */

static void icon_launch(int i)
{
    if (i == ICON_TERMINAL) {
        /* the M89 mechanism: the console shell runs the
         * terminal; the desktop just types the command */
        ps2kbd_inject("gterm\n");
    } else if (i == ICON_ABOUT) {
        window_open(); /* a no-op while it stands */
    }
}

/* One press on icon i: select it, or launch it when it is
 * the second press on the same icon inside the window. */
static void icon_press(int i)
{
    uint64_t now = sched_uptime_ticks();
    if (g_icon_sel == i && g_icon_last_tick != 0 &&
        now - g_icon_last_tick <= ICON_DBL_TICKS) {
        g_icon_last_tick = 0;
        icon_launch(i);
        return;
    }
    g_icon_sel = i;
    g_icon_last_tick = now;
    icons_draw();
}

/* --- the About window (M85's demonstration window) ------- */
static struct {
    int open;
    int dragging;
    int prev_buttons;
    uint32_t x, y;
    uint32_t grab_dx, grab_dy;
    int menu_open;
} g_win;

/* --- user windows (M88) ---------------------------------- */
#define MAXUWIN 3
#define UW_COLS_MAX 120u
#define UW_ROWS_MAX 40u

struct uwin {
    int in_use;
    int gone; /* closed with the mouse; the slot is freed
                 when the owner closes it or dies */
    int owner; /* tid */
    uint32_t x, y, w, h;
    char title[24];
    uint32_t cols, rows;
    uint8_t cells[UW_ROWS_MAX][UW_COLS_MAX];
    uint32_t cx, cy;
    uint8_t kq[64];
    uint32_t khead, ktail; /* head = write end */
};

static struct uwin g_uw[MAXUWIN];
static int g_focus = -1; /* user slot with the keyboard,
                            -1 = the console */
static int g_dragu = -1; /* user slot being dragged */
static uint32_t g_dragu_dx, g_dragu_dy;
static uint32_t g_cascade;

static uint32_t bar_y(void)
{
    return fb_height() - BAR_H;
}

/* The chip names the focused user window when one exists,
 * else the demonstration window (M86 behavior). */
static const char *chip_title(void)
{
    int i;
    if (g_focus >= 0 && g_uw[g_focus].in_use &&
        !g_uw[g_focus].gone)
        return g_uw[g_focus].title;
    for (i = 0; i < MAXUWIN; i++)
        if (g_uw[i].in_use && !g_uw[i].gone)
            return g_uw[i].title;
    return NULL;
}

static void taskbar_draw(void)
{
    uint32_t w = fb_width(), by = bar_y();
    const char *chip = chip_title();
    fb_fill_rect(0, by, w, BAR_H, COL_BAR);
    fb_fill_rect(0, by, w, 1, COL_BORDER);
    /* start button */
    fb_fill_rect(BTN_X, by + 6, BTN_W, 28, COL_TITLE);
    fb_draw_text(BTN_X + 16, by + 12, "YOUSJ", COL_TEXT,
                 COL_TITLE, 1);
    /* an open window's chip, or plain bar when none is */
    if (chip != NULL || g_win.open) {
        fb_fill_rect(CHIP_X, by + 6, CHIP_W, 28, COL_BODY);
        fb_draw_text(CHIP_X + 10, by + 12,
                     chip != NULL ? chip : "ABOUT",
                     COL_TEXT, COL_BODY, 1);
    }
    /* right-hand label */
    fb_draw_text(w - 110, by + 12, "Yousj OS 0.1", COL_BTEXT,
                 COL_BAR, 1);
}

static void menu_draw(void)
{
    uint32_t my = bar_y() - MENU_H;
    fb_fill_rect(8, my, MENU_W, MENU_H, COL_BODY);
    fb_fill_rect(8, my, MENU_W, 1, COL_BORDER);
    fb_fill_rect(8, my + MENU_H - 1, MENU_W, 1, COL_BORDER);
    fb_fill_rect(8, my, 1, MENU_H, COL_BORDER);
    fb_fill_rect(8 + MENU_W - 1, my, 1, MENU_H, COL_BORDER);
    fb_draw_text(24, my + 14, "About Yousj OS", COL_TEXT,
                 COL_BODY, 1);
    fb_draw_text(24, my + 48, "Terminal", COL_TEXT,
                 COL_BODY, 1);
}

static void menu_erase(void)
{
    /* M91: what is under the menu is wallpaper, not the
     * old flat field color */
    if (fb_desktop())
        fb_desktop_paint_rect(8, bar_y() - MENU_H, MENU_W,
                              MENU_H);
    else
        fb_fill_rect(8, bar_y() - MENU_H, MENU_W, MENU_H,
                     COL_FIELD);
}

/* Append v's decimal digits to buf at *n (no libc here). */
static void fmt_u32(char *buf, uint32_t *n, uint32_t v)
{
    char digits[10];
    uint32_t d = 0;
    do {
        digits[d++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v);
    while (d)
        buf[(*n)++] = digits[--d];
    buf[*n] = '\0';
}

static void win_draw(void)
{
    uint32_t x = g_win.x, y = g_win.y;
    char disp[40];
    uint32_t n = 0;
    const char *s;
    /* body first, then title bar, border, close box, texts */
    fb_fill_rect(x, y + WIN_TITLE, WIN_W, WIN_H - WIN_TITLE,
                 COL_BODY);
    fb_fill_rect(x, y, WIN_W, WIN_TITLE, COL_TITLE);
    fb_fill_rect(x, y, WIN_W, 1, COL_BORDER);
    fb_fill_rect(x, y + WIN_H - 1, WIN_W, 1, COL_BORDER);
    fb_fill_rect(x, y, 1, WIN_H, COL_BORDER);
    fb_fill_rect(x + WIN_W - 1, y, 1, WIN_H, COL_BORDER);
    fb_draw_text(x + 10, y + 6, "ABOUT YOUSJ OS", COL_TEXT,
                 COL_TITLE, 1);
    fb_fill_rect(x + WIN_W - 24, y + 4, 20, 20, COL_CLOSE);
    fb_draw_text(x + WIN_W - 19, y + 6, "x", COL_TEXT, COL_CLOSE,
                 1);
    fb_draw_text(x + 16, y + WIN_TITLE + 18,
                 "Yousj OS 0.1, written from scratch:",
                 COL_BTEXT, COL_BODY, 1);
    fb_draw_text(x + 16, y + WIN_TITLE + 40,
                 "no Linux kernel code, a Linux-compatible",
                 COL_BTEXT, COL_BODY, 1);
    fb_draw_text(x + 16, y + WIN_TITLE + 62,
                 "syscall interface, a UEFI bootloader,",
                 COL_BTEXT, COL_BODY, 1);
    fb_draw_text(x + 16, y + WIN_TITLE + 84,
                 "windows and a graphical terminal.",
                 COL_BTEXT, COL_BODY, 1);
    for (s = "Display: "; *s; s++)
        disp[n++] = *s;
    fmt_u32(disp, &n, fb_width());
    disp[n++] = 'x';
    disp[n] = '\0';
    fmt_u32(disp, &n, fb_height());
    fb_draw_text(x + 16, y + WIN_TITLE + 126, disp, COL_BTEXT,
                 COL_BODY, 1);
    fb_draw_text(x + 16, y + WIN_TITLE + 168,
                 "Double-click the Terminal icon on the",
                 COL_BTEXT, COL_BODY, 1);
    fb_draw_text(x + 16, y + WIN_TITLE + 190,
                 "desktop for a shell.", COL_BTEXT, COL_BODY,
                 1);
}

/* --- user window rendering -------------------------------- */

static uint32_t uw_cell_x(const struct uwin *u, uint32_t c)
{
    return u->x + 8 + c * 8;
}

static uint32_t uw_cell_y(const struct uwin *u, uint32_t r)
{
    return u->y + WIN_TITLE + 8 + r * 16;
}

static void uw_draw_cursor(struct uwin *u, int on)
{
    fb_fill_rect(uw_cell_x(u, u->cx), uw_cell_y(u, u->cy) + 13,
                 8, 2, on ? COL_TEXT : COL_BODY);
}

static void uw_draw_row(struct uwin *u, uint32_t r)
{
    char line[UW_COLS_MAX + 1];
    uint32_t c;
    for (c = 0; c < u->cols; c++)
        line[c] = u->cells[r][c] ? (char)u->cells[r][c] : ' ';
    line[u->cols] = '\0';
    fb_draw_text(uw_cell_x(u, 0), uw_cell_y(u, r), line,
                 COL_TEXT, COL_BODY, 1);
}

static void uwin_draw(struct uwin *u)
{
    uint32_t r;
    fb_fill_rect(u->x, u->y + WIN_TITLE, u->w,
                 u->h - WIN_TITLE, COL_BODY);
    fb_fill_rect(u->x, u->y, u->w, WIN_TITLE, COL_TITLE);
    fb_fill_rect(u->x, u->y, u->w, 1, COL_BORDER);
    fb_fill_rect(u->x, u->y + u->h - 1, u->w, 1, COL_BORDER);
    fb_fill_rect(u->x, u->y, 1, u->h, COL_BORDER);
    fb_fill_rect(u->x + u->w - 1, u->y, 1, u->h, COL_BORDER);
    fb_draw_text(u->x + 10, u->y + 6, u->title, COL_TEXT,
                 COL_TITLE, 1);
    fb_fill_rect(u->x + u->w - 24, u->y + 4, 20, 20, COL_CLOSE);
    fb_draw_text(u->x + u->w - 19, u->y + 6, "x", COL_TEXT,
                 COL_CLOSE, 1);
    for (r = 0; r < u->rows; r++)
        uw_draw_row(u, r);
    uw_draw_cursor(u, 1);
}

/* Restore what a rectangle of screen used to show: the
 * wallpaper first (M91; the console's backing store before
 * it), then the icons, then every open window that
 * overlaps it. */
static void rect_repair(uint32_t x, uint32_t y, uint32_t w,
                        uint32_t h)
{
    int i;
    if (fb_desktop())
        fb_desktop_paint_rect(x, y, w, h);
    else
        fb_console_repaint_rect(x, y, w, h);
    icons_repair(x, y, w, h);
    if (g_win.open && x < g_win.x + WIN_W && x + w > g_win.x &&
        y < g_win.y + WIN_H && y + h > g_win.y)
        win_draw();
    for (i = 0; i < MAXUWIN; i++) {
        struct uwin *u = &g_uw[i];
        if (u->in_use && !u->gone && x < u->x + u->w &&
            x + w > u->x && y < u->y + u->h && y + h > u->y)
            uwin_draw(u);
    }
}

static void window_open(void)
{
    if (g_win.open)
        return; /* the About box is a singleton */
    /* M91: centered on the desktop */
    g_win.x = fb_width() > WIN_W ? (fb_width() - WIN_W) / 2 : 0;
    g_win.y = fb_height() > WIN_H ? (fb_height() - WIN_H) / 2
                                  : 0;
    g_win.open = 1;
    win_draw();
    taskbar_draw(); /* the chip appears */
}

void wm_init(void)
{
    if (!fb_present())
        return;
    /* M91: a clean desktop -- icons and the taskbar, no
     * window standing open. */
    taskbar_draw();
    icons_draw();
}

int wm_window_rect(int idx, uint32_t *x, uint32_t *y,
                   uint32_t *w, uint32_t *h)
{
    int i, seen = 0;
    if (idx == 0) {
        if (!g_win.open)
            return 0;
        *x = g_win.x;
        *y = g_win.y;
        *w = WIN_W;
        *h = WIN_H;
        return 1;
    }
    for (i = 0; i < MAXUWIN; i++) {
        if (!g_uw[i].in_use || g_uw[i].gone)
            continue;
        seen++;
        if (seen == idx) {
            *x = g_uw[i].x;
            *y = g_uw[i].y;
            *w = g_uw[i].w;
            *h = g_uw[i].h;
            return 1;
        }
    }
    return 0;
}

void wm_repair(void)
{
    int i;
    if (g_win.open)
        win_draw();
    for (i = 0; i < MAXUWIN; i++)
        if (g_uw[i].in_use && !g_uw[i].gone)
            uwin_draw(&g_uw[i]);
}

/* --- user window lifecycle -------------------------------- */

int wm_gui_open(int owner, const char *title, uint32_t w,
                uint32_t h)
{
    int i;
    struct uwin *u = NULL;

    if (!fb_present())
        return -19; /* ENODEV */
    for (i = 0; i < MAXUWIN; i++) {
        if (!g_uw[i].in_use) {
            u = &g_uw[i];
            break;
        }
    }
    if (u == NULL)
        return -28; /* ENOSPC */
    {
        /* With no user window standing, the cascade starts
         * over -- a fresh desktop corner for a fresh open. */
        int i2, any = 0;
        for (i2 = 0; i2 < MAXUWIN; i2++)
            if (g_uw[i2].in_use)
                any = 1;
        if (!any)
            g_cascade = 0;
    }
    if (w < 240)
        w = 240;
    if (w > fb_width())
        w = fb_width();
    if (h < 140)
        h = 140;
    if (h > fb_height() - BAR_H)
        h = fb_height() - BAR_H;
    u->in_use = 1;
    u->gone = 0;
    u->owner = owner;
    u->w = w;
    u->h = h;
    u->x = 300 + 40 * g_cascade;
    u->y = 300 + 40 * g_cascade;
    g_cascade = (g_cascade + 1) & 3;
    if (u->x + w > fb_width())
        u->x = fb_width() - w;
    if (u->y + h > fb_height() - BAR_H)
        u->y = fb_height() - BAR_H - h;
    for (i = 0; i < 23 && title[i] != '\0'; i++)
        u->title[i] = title[i];
    u->title[i] = '\0';
    u->cols = (w - 16) / 8;
    if (u->cols > UW_COLS_MAX)
        u->cols = UW_COLS_MAX;
    if (u->cols < 10)
        u->cols = 10;
    u->rows = (h - WIN_TITLE - 16) / 16;
    if (u->rows > UW_ROWS_MAX)
        u->rows = UW_ROWS_MAX;
    if (u->rows < 4)
        u->rows = 4;
    {
        uint32_t r, c;
        for (r = 0; r < UW_ROWS_MAX; r++)
            for (c = 0; c < UW_COLS_MAX; c++)
                u->cells[r][c] = ' ';
    }
    u->cx = 0;
    u->cy = 0;
    u->khead = 0;
    u->ktail = 0;
    g_focus = (int)(u - g_uw);
    uwin_draw(u);
    taskbar_draw();
    return (int)(u - g_uw) + 1;
}

static struct uwin *uw_of(int id, int owner)
{
    struct uwin *u;
    if (id < 1 || id > MAXUWIN)
        return NULL;
    u = &g_uw[id - 1];
    if (!u->in_use || u->owner != owner)
        return NULL;
    return u;
}

static void uw_scroll(struct uwin *u)
{
    uint32_t r, c;
    for (r = 0; r + 1 < u->rows; r++)
        for (c = 0; c < u->cols; c++)
            u->cells[r][c] = u->cells[r + 1][c];
    for (c = 0; c < u->cols; c++)
        u->cells[u->rows - 1][c] = ' ';
    fb_fill_rect(u->x, u->y + WIN_TITLE, u->w,
                 u->h - WIN_TITLE, COL_BODY);
    for (r = 0; r < u->rows; r++)
        uw_draw_row(u, r);
}

static void uw_newline(struct uwin *u)
{
    u->cx = 0;
    u->cy++;
    if (u->cy >= u->rows) {
        uw_scroll(u);
        u->cy = u->rows - 1;
    }
}

static void uw_putc(struct uwin *u, uint8_t ch)
{
    uw_draw_cursor(u, 0);
    if (ch == '\n') {
        uw_newline(u);
    } else if (ch == '\r') {
        u->cx = 0;
    } else if (ch == 0x08 || ch == 0x7F) {
        if (u->cx > 0) {
            char cell[2];
            u->cx--;
            u->cells[u->cy][u->cx] = ' ';
            cell[0] = ' ';
            cell[1] = '\0';
            fb_draw_text(uw_cell_x(u, u->cx),
                         uw_cell_y(u, u->cy), cell, COL_TEXT,
                         COL_BODY, 1);
        }
    } else if (ch == '\t') {
        uint32_t nx = (u->cx + 8) & ~7u;
        if (nx >= u->cols)
            uw_newline(u);
        else
            u->cx = nx;
    } else if (ch >= 0x20 && ch < 0x7F) {
        char cell[2];
        u->cells[u->cy][u->cx] = ch;
        cell[0] = (char)ch;
        cell[1] = '\0';
        fb_draw_text(uw_cell_x(u, u->cx), uw_cell_y(u, u->cy),
                     cell, COL_TEXT, COL_BODY, 1);
        u->cx++;
        if (u->cx >= u->cols)
            uw_newline(u);
    }
    /* other control bytes are ignored */
    uw_draw_cursor(u, 1);
}

long wm_gui_write(int id, int owner, const uint8_t *buf,
                  uint64_t len)
{
    struct uwin *u = uw_of(id, owner);
    uint64_t i;
    if (u == NULL)
        return -9; /* EBADF */
    if (u->gone)
        return -5; /* EIO: the window is gone */
    for (i = 0; i < len; i++)
        uw_putc(u, buf[i]);
    return (long)len;
}

long wm_gui_getc(int id, int owner)
{
    struct uwin *u = uw_of(id, owner);
    uint8_t c;
    if (u == NULL)
        return -9; /* EBADF */
    if (u->gone)
        return -2; /* closed with the mouse */
    if (u->ktail == u->khead)
        return -1; /* empty */
    c = u->kq[u->ktail];
    u->ktail = (u->ktail + 1) & 63;
    return (long)c;
}

static void uw_free(struct uwin *u)
{
    int slot = (int)(u - g_uw);
    if (!u->gone) {
        u->gone = 1; /* so rect_repair does not redraw it */
        rect_repair(u->x, u->y, u->w, u->h);
    }
    u->in_use = 0;
    u->gone = 0;
    if (g_focus == slot)
        g_focus = -1;
    if (g_dragu == slot)
        g_dragu = -1;
    taskbar_draw();
}

long wm_gui_close(int id, int owner)
{
    struct uwin *u = uw_of(id, owner);
    if (u == NULL)
        return -9; /* EBADF */
    uw_free(u);
    return 0;
}

void wm_task_exit(int owner)
{
    int i;
    for (i = 0; i < MAXUWIN; i++)
        if (g_uw[i].in_use && g_uw[i].owner == owner)
            uw_free(&g_uw[i]);
}

int wm_key_target(void)
{
    if (g_focus >= 0 && g_uw[g_focus].in_use &&
        !g_uw[g_focus].gone)
        return g_focus;
    return -1;
}

void wm_key_push(int slot, uint8_t c)
{
    struct uwin *u;
    uint32_t next;
    if (slot < 0 || slot >= MAXUWIN)
        return;
    u = &g_uw[slot];
    if (!u->in_use || u->gone)
        return;
    next = (u->khead + 1) & 63;
    if (next == u->ktail)
        return; /* full: drop */
    u->kq[u->khead] = c;
    u->khead = next;
}

/* --- mouse -------------------------------------------------- */

void wm_mouse(uint32_t mx, uint32_t my, int buttons)
{
    int pressed = (buttons & 1) != 0;
    int was = (g_win.prev_buttons & 1) != 0;

    /* was this press aimed at the demo window? (decided
     * before any state change, so a closing click cannot
     * fall through to the desktop marker below) */
    int hit_window = g_win.open && mx >= g_win.x &&
                     mx < g_win.x + WIN_W && my >= g_win.y &&
                     my < g_win.y + WIN_H;

    /* Desktop furniture first (M86): the start menu and the
     * start button consume their clicks outright. A press
     * elsewhere closes an open menu and then falls through
     * to the window/desktop handling below. */
    if (pressed && !was) {
        uint32_t by = bar_y();
        int on_button = mx >= BTN_X && mx < BTN_X + BTN_W &&
                        my >= by + 6 && my < by + 34;
        int consumed = 0;
        if (g_win.menu_open) {
            uint32_t myy = by - MENU_H;
            if (mx >= 8 && mx < 8 + MENU_W && my >= myy &&
                my < myy + MENU_H) {
                g_win.menu_open = 0;
                menu_erase();
                if (my < myy + 42) {
                    /* entry 0: reopen the demo window */
                    if (!g_win.open)
                        window_open();
                } else {
                    /* entry 1 (M89): launch a terminal --
                     * type the command into the console
                     * ring; the shell does the rest. */
                    ps2kbd_inject("gterm\n");
                }
                consumed = 1;
            } else if (!on_button) {
                g_win.menu_open = 0;
                menu_erase();
            }
        }
        if (!consumed && on_button) {
            g_win.menu_open = !g_win.menu_open;
            if (g_win.menu_open)
                menu_draw();
            else
                menu_erase();
            consumed = 1;
        }
        if (consumed) {
            g_win.prev_buttons = buttons;
            return;
        }
    }

    /* User windows sit above the demonstration window
     * (M88): a press inside one focuses it; its title bar
     * drags; its red box closes it (the owner learns via
     * wm_gui_getc). */
    if (pressed && !was) {
        int i;
        for (i = MAXUWIN - 1; i >= 0; i--) {
            struct uwin *u = &g_uw[i];
            if (!u->in_use || u->gone)
                continue;
            if (mx < u->x || mx >= u->x + u->w || my < u->y ||
                my >= u->y + u->h)
                continue;
            if (mx >= u->x + u->w - 24 &&
                mx < u->x + u->w - 4 && my >= u->y + 4 &&
                my < u->y + 24) {
                u->gone = 1;
                if (g_focus == i)
                    g_focus = -1;
                rect_repair(u->x, u->y, u->w, u->h);
                taskbar_draw();
            } else {
                g_focus = i;
                if (my < u->y + WIN_TITLE) {
                    g_dragu = i;
                    g_dragu_dx = mx - u->x;
                    g_dragu_dy = my - u->y;
                }
                taskbar_draw(); /* the chip follows focus */
            }
            g_win.prev_buttons = buttons;
            return;
        }
    }

    /* user window drag motion */
    if (pressed && g_dragu >= 0) {
        struct uwin *u = &g_uw[g_dragu];
        if (u->in_use && !u->gone) {
            int32_t nx = (int32_t)mx - (int32_t)g_dragu_dx;
            int32_t ny = (int32_t)my - (int32_t)g_dragu_dy;
            uint32_t ux, uy;
            if (nx < 0)
                nx = 0;
            if (ny < (int32_t)WIN_MIN_Y)
                ny = (int32_t)WIN_MIN_Y;
            if (nx + (int32_t)u->w > (int32_t)fb_width())
                nx = (int32_t)fb_width() - (int32_t)u->w;
            if (ny + (int32_t)u->h > (int32_t)fb_height())
                ny = (int32_t)fb_height() - (int32_t)u->h;
            ux = (uint32_t)nx;
            uy = (uint32_t)ny;
            if (ux != u->x || uy != u->y) {
                uint32_t ox = u->x, oy = u->y;
                u->x = ux;
                u->y = uy;
                rect_repair(ox, oy, u->w, u->h);
                uwin_draw(u);
            }
        }
    }
    if (!pressed)
        g_dragu = -1;

    if (pressed && !was && hit_window) {
        uint32_t x = g_win.x, y = g_win.y;
        g_focus = -1; /* the console takes the keyboard back */
        if (mx >= x + WIN_W - 24 && mx < x + WIN_W - 4 &&
            my >= y + 4 && my < y + 24) {
            /* the red box: close, and the console reappears */
            g_win.open = 0;
            g_win.dragging = 0;
            rect_repair(x, y, WIN_W, WIN_H);
            taskbar_draw(); /* the chip disappears */
        } else if (mx >= x && mx < x + WIN_W && my >= y &&
                   my < y + WIN_TITLE) {
            g_win.dragging = 1;
            g_win.grab_dx = mx - x;
            g_win.grab_dy = my - y;
        }
        /* body clicks: focus only, nothing to show yet */
    }
    if (pressed && !was && !hit_window) {
        /* M91: a press on a desktop icon selects it, or
         * launches it on a double-click. Icons sit under
         * windows: reaching here means no window took the
         * press (the user-window block above returns on a
         * hit, and the demo window vetoes via hit_window). */
        int ic = icon_hit(mx, my);
        if (ic >= 0) {
            icon_press(ic);
            g_win.prev_buttons = buttons;
            return;
        }
    }
    if (pressed && g_win.dragging && g_win.open) {
        int32_t nx = (int32_t)mx - (int32_t)g_win.grab_dx;
        int32_t ny = (int32_t)my - (int32_t)g_win.grab_dy;
        uint32_t ux, uy;
        if (nx < 0)
            nx = 0;
        if (ny < (int32_t)WIN_MIN_Y)
            ny = (int32_t)WIN_MIN_Y;
        if (nx + (int32_t)WIN_W > (int32_t)fb_width())
            nx = (int32_t)fb_width() - (int32_t)WIN_W;
        if (ny + (int32_t)WIN_H > (int32_t)fb_height())
            ny = (int32_t)fb_height() - (int32_t)WIN_H;
        ux = (uint32_t)nx;
        uy = (uint32_t)ny;
        if (ux != g_win.x || uy != g_win.y) {
            uint32_t ox = g_win.x, oy = g_win.y;
            g_win.x = ux;
            g_win.y = uy;
            rect_repair(ox, oy, WIN_W, WIN_H);
            win_draw();
        }
    }
    if (!pressed)
        g_win.dragging = 0;
    if (pressed && !was && !hit_window) {
        /* bare desktop: keyboard back to the console, and
         * any icon selection clears (M91; the M84 click
         * mark is gone -- the desktop stays clean) */
        g_focus = -1;
        if (g_icon_sel >= 0) {
            g_icon_sel = -1;
            g_icon_last_tick = 0;
            icons_draw();
        }
    }
    g_win.prev_buttons = buttons;
}
