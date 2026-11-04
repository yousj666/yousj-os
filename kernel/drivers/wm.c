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
#define MENU_H     48u

static struct {
    int open;
    int dragging;
    int prev_buttons;
    uint32_t x, y;
    uint32_t grab_dx, grab_dy;
    int menu_open;
} g_win;

static uint32_t bar_y(void)
{
    return fb_height() - BAR_H;
}

static void taskbar_draw(void)
{
    uint32_t w = fb_width(), by = bar_y();
    fb_fill_rect(0, by, w, BAR_H, COL_BAR);
    fb_fill_rect(0, by, w, 1, COL_BORDER);
    /* start button */
    fb_fill_rect(BTN_X, by + 6, BTN_W, 28, COL_TITLE);
    fb_draw_text(BTN_X + 16, by + 12, "YOUSJ", COL_TEXT,
                 COL_TITLE, 1);
    /* the open window's chip, or plain bar when it is closed */
    if (g_win.open) {
        fb_fill_rect(CHIP_X, by + 6, CHIP_W, 28, COL_BODY);
        fb_draw_text(CHIP_X + 10, by + 12, "YOUSJ WINDOW",
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
    fb_draw_text(24, my + 16, "Open the window", COL_TEXT,
                 COL_BODY, 1);
}

static void menu_erase(void)
{
    fb_fill_rect(8, bar_y() - MENU_H, MENU_W, MENU_H, COL_FIELD);
}

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

static void window_open(void)
{
    g_win.x = 1050;
    g_win.y = 1080;
    if (g_win.x + WIN_W > fb_width())
        g_win.x = fb_width() > WIN_W ? fb_width() - WIN_W : 0;
    if (g_win.y + WIN_H > fb_height())
        g_win.y = fb_height() > WIN_H ? fb_height() - WIN_H : 0;
    g_win.open = 1;
    win_draw();
    taskbar_draw(); /* the chip appears */
}

void wm_init(void)
{
    if (!fb_present())
        return;
    taskbar_draw();
    window_open();
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

    /* Desktop furniture first (M86): the start menu and the
     * start button consume their clicks outright. A press
     * elsewhere closes an open menu and then falls through to
     * the window/desktop handling below. */
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
                if (!g_win.open)
                    window_open();
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
        /* hit_window may have changed if the menu just ran;
         * it cannot have (menu clicks are consumed), but the
         * fall-through close-menu path leaves it valid. */
    }

    if (pressed && !was && hit_window) {
        uint32_t x = g_win.x, y = g_win.y;
        if (mx >= x + WIN_W - 24 && mx < x + WIN_W - 4 &&
            my >= y + 4 && my < y + 24) {
            /* the red box: close, and the console reappears */
            g_win.open = 0;
            g_win.dragging = 0;
            win_erase(x, y);
            taskbar_draw(); /* the chip disappears */
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
