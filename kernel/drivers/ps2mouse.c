/* Yousj OS - PS/2 mouse driver (M84).
 *
 * Written from scratch. Init talks to the 8042 controller:
 * enable the auxiliary port (0xA8), read/modify/write the
 * command byte (AUX clock on, AUX interrupt flag set -- we poll,
 * but the flag costs nothing), then address the mouse itself
 * (0xD4 prefix) with 0xF4 "enable data reporting". Every wait is
 * bounded; absence of a mouse degrades to silence, not a hang.
 *
 * Packets are the standard 3 bytes: [buttons|signs|overflow,
 * dx, dy]. Byte 0 must carry bit 3 set -- that rule also drops
 * the 0xFA ACK from the enable command. Deltas accumulate in
 * feed() and are applied once per tick by input_poll(), which
 * clamps to the screen and asks fb.c to move the arrow.
 */
#include <yousj/types.h>
#include <yousj/ps2mouse.h>
#include <yousj/ps2kbd.h>
#include <yousj/fb.h>
#include <yousj/wm.h> /* M85: clicks and drags route to windows */

#define KBD_STATUS 0x64
#define KBD_DATA   0x60
#define ST_OBF     0x01
#define ST_IBF     0x02

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static void wait_ibf_clear(void)
{
    uint32_t i;
    for (i = 0; i < 100000; i++) {
        if ((inb(KBD_STATUS) & ST_IBF) == 0)
            return;
    }
}

static void ctl_write(uint8_t b)
{
    wait_ibf_clear();
    outb(KBD_STATUS, b);
}

static void data_write(uint8_t b)
{
    wait_ibf_clear();
    outb(KBD_DATA, b);
}

static struct {
    int ready;
    uint8_t pkt[3];
    int phase;
    int32_t pend_dx, pend_dy;
    int buttons;
    int prev_buttons;
    int32_t x, y;
    int placed;
} g_m;

void ps2mouse_init(void)
{
    uint32_t i;
    uint8_t cmd_byte = 0x47; /* kbd on, aux irq on, sys flag */

    ctl_write(0xA8); /* enable auxiliary port */

    /* read the command byte (0x20), adjust, write back (0x60) */
    ctl_write(0x20);
    for (i = 0; i < 100000; i++) {
        if (inb(KBD_STATUS) & ST_OBF) {
            cmd_byte = inb(KBD_DATA);
            break;
        }
    }
    cmd_byte |= 0x02;  /* AUX interrupt enable (polling anyway) */
    cmd_byte &= (uint8_t)~0x20; /* AUX clock enabled */
    ctl_write(0x60);
    data_write(cmd_byte);

    /* tell the mouse to start reporting: D4 routes to AUX */
    ctl_write(0xD4);
    data_write(0xF4);

    g_m.ready = 1;
}

void ps2mouse_feed(uint8_t b)
{
    if (!g_m.ready)
        return;
    if (g_m.phase == 0) {
        if ((b & 0x08) == 0)
            return; /* not a packet start (e.g. the 0xFA ACK) */
        g_m.pkt[0] = b;
        g_m.phase = 1;
        return;
    }
    g_m.pkt[g_m.phase] = b;
    g_m.phase++;
    if (g_m.phase < 3)
        return;
    g_m.phase = 0;
    {
        int32_t dx = g_m.pkt[1];
        int32_t dy = g_m.pkt[2];
        if (g_m.pkt[0] & 0x10)
            dx -= 256;
        if (g_m.pkt[0] & 0x20)
            dy -= 256;
        if ((g_m.pkt[0] & 0xC0) == 0) { /* overflow: drop deltas */
            g_m.pend_dx += dx;
            g_m.pend_dy += dy;
        }
        g_m.buttons = g_m.pkt[0] & 0x07;
    }
}

void input_poll(void)
{
    int32_t nx, ny;
    ps2_poll(); /* drain the 8042 (keys + mouse bytes) */
    if (!g_m.ready || !fb_present())
        return;
    if (!g_m.placed) {
        g_m.x = (int32_t)(fb_width() / 2);
        g_m.y = (int32_t)(fb_height() / 2);
        g_m.placed = 1;
        fb_cursor_move((uint32_t)g_m.x, (uint32_t)g_m.y);
    }
    if (g_m.pend_dx == 0 && g_m.pend_dy == 0 &&
        g_m.buttons == g_m.prev_buttons)
        return;
    nx = g_m.x + g_m.pend_dx;
    ny = g_m.y - g_m.pend_dy; /* mouse dy is up-positive */
    g_m.pend_dx = 0;
    g_m.pend_dy = 0;
    if (nx < 0)
        nx = 0;
    if (ny < 0)
        ny = 0;
    if (nx > (int32_t)fb_width() - 1)
        nx = (int32_t)fb_width() - 1;
    if (ny > (int32_t)fb_height() - 1)
        ny = (int32_t)fb_height() - 1;
    g_m.x = nx;
    g_m.y = ny;
    fb_cursor_move((uint32_t)nx, (uint32_t)ny);
    /* M85: the window layer sees every position/button change
     * (it owns hit testing, dragging, closing; desktop clicks
     * still land as markers from inside wm_mouse). */
    wm_mouse((uint32_t)nx, (uint32_t)ny, g_m.buttons);
    g_m.prev_buttons = g_m.buttons;
}

int ps2mouse_x(void)
{
    return g_m.x;
}

int ps2mouse_y(void)
{
    return g_m.y;
}

int ps2mouse_buttons(void)
{
    return g_m.buttons;
}
