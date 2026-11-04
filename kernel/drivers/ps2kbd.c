/* Yousj OS - PS/2 keyboard driver (M83).
 *
 * Written from scratch. Polled i8042: status port 0x64 (bit 0 =
 * output full, bit 5 = AUX/mouse byte), data port 0x60. Firmware
 * (OVMF or SeaBIOS) leaves the controller enabled with set-1
 * translation on, and QEMU's emulation keeps that state, so the
 * driver never reprograms the controller -- it just drains and
 * decodes. Scancode set 1, US layout.
 *
 * Tracked state: shift (either key), caps lock, ctrl, and the
 * 0xE0 extended prefix. Break codes only matter for the modifier
 * keys. Decoded bytes land in a 128-byte ring; overflow drops the
 * newest byte (a typematic flood should not eat old input).
 */
#include <yousj/types.h>
#include <yousj/ps2kbd.h>
#include <yousj/ps2mouse.h> /* M84: AUX bytes go to the mouse */

#define KBD_STATUS 0x64
#define KBD_DATA   0x60
#define ST_OBF     0x01
#define ST_AUX     0x20

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* make code -> {normal, shifted} for the printable block */
static const char g_map[0x3A][2] = {
    [0x02] = {'1', '!'}, [0x03] = {'2', '@'}, [0x04] = {'3', '#'},
    [0x05] = {'4', '$'}, [0x06] = {'5', '%'}, [0x07] = {'6', '^'},
    [0x08] = {'7', '&'}, [0x09] = {'8', '*'}, [0x0A] = {'9', '('},
    [0x0B] = {'0', ')'}, [0x0C] = {'-', '_'}, [0x0D] = {'=', '+'},
    [0x10] = {'q', 'Q'}, [0x11] = {'w', 'W'}, [0x12] = {'e', 'E'},
    [0x13] = {'r', 'R'}, [0x14] = {'t', 'T'}, [0x15] = {'y', 'Y'},
    [0x16] = {'u', 'U'}, [0x17] = {'i', 'I'}, [0x18] = {'o', 'O'},
    [0x19] = {'p', 'P'}, [0x1A] = {'[', '{'}, [0x1B] = {']', '}'},
    [0x1E] = {'a', 'A'}, [0x1F] = {'s', 'S'}, [0x20] = {'d', 'D'},
    [0x21] = {'f', 'F'}, [0x22] = {'g', 'G'}, [0x23] = {'h', 'H'},
    [0x24] = {'j', 'J'}, [0x25] = {'k', 'K'}, [0x26] = {'l', 'L'},
    [0x27] = {';', ':'}, [0x28] = {'\'', '"'}, [0x29] = {'`', '~'},
    [0x2B] = {'\\', '|'},
    [0x2C] = {'z', 'Z'}, [0x2D] = {'x', 'X'}, [0x2E] = {'c', 'C'},
    [0x2F] = {'v', 'V'}, [0x30] = {'b', 'B'}, [0x31] = {'n', 'N'},
    [0x32] = {'m', 'M'}, [0x33] = {',', '<'}, [0x34] = {'.', '>'},
    [0x35] = {'/', '?'}, [0x37] = {'*', '*'}, [0x39] = {' ', ' '},
};

static uint8_t g_ring[128];
static uint32_t g_head, g_tail; /* head = write, tail = read */
static int g_inited;
static int g_shift, g_caps, g_ctrl, g_e0;

static void ring_push(uint8_t c)
{
    uint32_t next = (g_head + 1) & 127;
    if (next == g_tail)
        return; /* full: drop */
    g_ring[g_head] = c;
    g_head = next;
}

static void decode(uint8_t code)
{
    int is_break = (code & 0x80) != 0;
    uint8_t make = code & 0x7F;

    if (g_e0) {
        g_e0 = 0;
        return; /* extended keys: consumed, dropped (for now) */
    }
    if (code == 0xE0) {
        g_e0 = 1;
        return;
    }
    if (make == 0x2A || make == 0x36) { /* shifts */
        g_shift = !is_break;
        return;
    }
    if (make == 0x1D) { /* ctrl */
        g_ctrl = !is_break;
        return;
    }
    if (is_break)
        return;
    if (make == 0x3A) { /* caps lock */
        g_caps = !g_caps;
        return;
    }
    if (make == 0x01) { ring_push(0x1B); return; }  /* esc */
    if (make == 0x0E) { ring_push(0x7F); return; }  /* backspace */
    if (make == 0x0F) { ring_push('\t'); return; }  /* tab */
    if (make == 0x1C) { ring_push('\n'); return; }  /* enter */
    if (make < 0x3A && g_map[make][0] != 0) {
        char c = g_map[make][0];
        int upper = g_shift;
        if (c >= 'a' && c <= 'z')
            upper = g_shift ^ g_caps;
        c = g_map[make][upper ? 1 : 0];
        if (g_ctrl && c >= 'a' && c <= 'z')
            c = (char)(c - 'a' + 1);
        else if (g_ctrl && c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 1);
        ring_push((uint8_t)c);
    }
}

static void drain(void)
{
    if (!g_inited) {
        g_inited = 1;
        /* throw away whatever the firmware left buffered */
        while (inb(KBD_STATUS) & ST_OBF)
            (void)inb(KBD_DATA);
        return;
    }
    while (inb(KBD_STATUS) & ST_OBF) {
        uint8_t st = inb(KBD_STATUS);
        uint8_t b = inb(KBD_DATA);
        if (st & ST_AUX) {
            ps2mouse_feed(b); /* M84: the mouse shares this drain */
            continue;
        }
        decode(b);
    }
}

int ps2kbd_getc(void)
{
    drain();
    if (g_tail == g_head)
        return -1;
    {
        int c = g_ring[g_tail];
        g_tail = (g_tail + 1) & 127;
        return c;
    }
}

int ps2kbd_ready(void)
{
    drain();
    return g_tail != g_head;
}

/* M84: the timer tick drains through here too (input_poll), so
 * mouse packets flow even when nobody is reading the console. */
void ps2_poll(void)
{
    drain();
}
