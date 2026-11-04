/* Yousj OS - window layer (M85, desktop M86, user windows
 * M88).
 *
 * Written from scratch. The kernel demonstration window
 * proved the mechanics every later window reuses: hit
 * testing (title bar drags, the red box closes, body clicks
 * are focus), erase-by-repaint from the console's backing
 * store, and repair after the console scrolls under it.
 * M86 added the desktop furniture (taskbar, start button
 * and menu). M88 generalises: user tasks can open their own
 * windows through the native GUI calls (see syscall.h).
 * A user window is a character-cell text surface -- the
 * owner streams bytes in (newlines scroll, backspace
 * erases) and the layer renders them; keyboard input goes
 * to the focused window's queue instead of the console.
 *
 * Mouse events arrive from input_poll() as absolute
 * positions plus the raw button bitmask; the layer keeps
 * its own edge state. Clicks that miss every window fall
 * through to the desktop, which keeps the M84 click-marker
 * behavior.
 */
#ifndef YOUSJ_WM_H
#define YOUSJ_WM_H

#include <yousj/types.h>

/* Open the demonstration window and the taskbar. No-op
 * without a framebuffer. */
void wm_init(void);

/* Enumerate open windows for the console's occlusion test:
 * idx 0 is the demonstration window, 1.. are user windows.
 * Returns 0 when idx is past the last open window. */
int wm_window_rect(int idx, uint32_t *x, uint32_t *y,
                   uint32_t *w, uint32_t *h);

/* Redraw every open window (the console scrolled and
 * smeared them). */
void wm_repair(void);

/* One mouse state sample: absolute position + buttons. */
void wm_mouse(uint32_t x, uint32_t y, int buttons);

/* --- M88: user windows ------------------------------------
 * IDs are small positive integers. The owner is the
 * creator's tid; only the owner may write, read keys, or
 * close. wm_gui_getc returns a byte (0..255), -1 when the
 * queue is empty, or -2 when the window was closed with
 * the mouse (the owner should wind down and close). */
int wm_gui_open(int owner, const char *title, uint32_t w,
                uint32_t h);
long wm_gui_write(int id, int owner, const uint8_t *buf,
                  uint64_t len);
long wm_gui_getc(int id, int owner);
long wm_gui_close(int id, int owner);

/* A task died: release every window it owned. */
void wm_task_exit(int owner);

/* Keyboard routing (called from the PS/2 decoder): the
 * slot of the focused user window, or -1 for the console.
 * wm_key_push appends a decoded byte to that window. */
int wm_key_target(void);
void wm_key_push(int slot, uint8_t c);

#endif /* YOUSJ_WM_H */
