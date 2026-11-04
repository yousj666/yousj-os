/* Yousj OS - window layer, first cut (M85).
 *
 * Written from scratch. A single window, drawn with fb
 * primitives: border, title bar with a red close box, body with
 * two lines of text. Dragging is anchored by the grab offset so
 * the window follows the pointer without jumping; erasing asks
 * the console to repaint the vacated rectangle from its
 * character backing store, so the text that was underneath
 * comes back exactly as it was.
 */
#include <yousj/types.h>
#include <yousj/wm.h>
#include <yousj/fb.h>

#define WIN_W 560u
#define WIN_H 340u
#define WIN_TITLE 28u
#define WIN_MIN_Y 200u

#define COL_BORDER 0x9FB8D8u
#define COL_TITLE  0x2E5E8Cu
#define COL_BODY   0x24466Bu
#define COL_CLOSE  0xC0392Bu
#define COL_TEXT   0xFFFFFFu
#define COL_BTEXT  0xD7E3F4u

static struct {
    int open;
    int dragging;
    int prev_buttons;
    uint32_t x, y;
    uint32_t grab_dx, grab_dy;
} g_win;

static void win_draw(void)
{
    uint32_t x = g_win.x, y = g_win.y;
    /* body first, then title bar, border, close box, texts */
    fb_fill_rect(x, y + WIN_TITLE, WIN_W, WIN_H - WIN_TITLE,
                 COL_BODY);
    fb_fill_rect(x, y, WIN_W, WIN_TITLE, COL_TITLE);
    fb_fill_rect(x, y, WIN_W, 1, COL_BORDER);
    fb_fill_rect(x, y + WIN_H - 1, WIN_W, 1, COL_BORDER);
    fb_fill_rect(x, y, 1, WIN_H, COL_BORDER);
    fb_fill_rect(x + WIN_W - 1, y, 1, WIN_H, COL_BORDER);
    fb_draw_text(x + 10, y + 6, "YOUSJ WINDOW", COL_TEXT,
                 COL_TITLE, 1);
    fb_fill_rect(x + WIN_W - 24, y + 4, 20, 20, COL_CLOSE);
    fb_draw_text(x + WIN_W - 19, y + 6, "x", COL_TEXT, COL_CLOSE,
                 1);
    fb_draw_text(x + 16, y + WIN_TITLE + 18,
                 "Drag me by the title bar.", COL_BTEXT, COL_BODY,
                 1);
    fb_draw_text(x + 16, y + WIN_TITLE + 38,
                 "Close me with the red box.", COL_BTEXT,
                 COL_BODY, 1);
}

static void win_erase(uint32_t x, uint32_t y)
{
    fb_console_repaint_rect(x, y, WIN_W, WIN_H);
}

void wm_init(void)
{
    if (!fb_present())
        return;
    g_win.x = 1050;
    g_win.y = 1080;
    if (g_win.x + WIN_W > fb_width())
        g_win.x = fb_width() > WIN_W ? fb_width() - WIN_W : 0;
    if (g_win.y + WIN_H > fb_height())
        g_win.y = fb_height() > WIN_H ? fb_height() - WIN_H : 0;
    g_win.open = 1;
    win_draw();
}

int wm_window_rect(uint32_t *x, uint32_t *y, uint32_t *w,
                   uint32_t *h)
{
    if (!g_win.open)
        return 0;
    *x = g_win.x;
    *y = g_win.y;
    *w = WIN_W;
    *h = WIN_H;
    return 1;
}

void wm_repair(void)
{
    if (g_win.open)
        win_draw();
}

void wm_mouse(uint32_t mx, uint32_t my, int buttons)
{
    int pressed = (buttons & 1) != 0;
    int was = (g_win.prev_buttons & 1) != 0;

    /* was this press aimed at the window? (decided before any
     * state change, so a closing click cannot fall through to
     * the desktop marker below) */
    int hit_window = g_win.open && mx >= g_win.x &&
                     mx < g_win.x + WIN_W && my >= g_win.y &&
                     my < g_win.y + WIN_H;

    if (pressed && !was && hit_window) {
        uint32_t x = g_win.x, y = g_win.y;
        if (mx >= x + WIN_W - 24 && mx < x + WIN_W - 4 &&
            my >= y + 4 && my < y + 24) {
            /* the red box: close, and the console reappears */
            g_win.open = 0;
            g_win.dragging = 0;
            win_erase(x, y);
        } else if (mx >= x && mx < x + WIN_W && my >= y &&
                   my < y + WIN_TITLE) {
            g_win.dragging = 1;
            g_win.grab_dx = mx - x;
            g_win.grab_dy = my - y;
        }
        /* body clicks: focus only, nothing to show yet */
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
            win_erase(g_win.x, g_win.y);
            g_win.x = ux;
            g_win.y = uy;
            win_draw();
        }
    }
    if (!pressed)
        g_win.dragging = 0;
    if (pressed && !was && !hit_window)
        fb_click_mark(mx, my); /* desktop click: the M84 marker */
    g_win.prev_buttons = buttons;
}
