/* Yousj OS - window layer, first cut (M85).
 *
 * Written from scratch. One demonstration window to prove the
 * mechanics every later window will reuse: hit testing (title
 * bar drags, the red box closes, body clicks are focus),
 * erase-by-repaint from the console's backing store, and repair
 * after the console scrolls under it. The window lives inside
 * the framebuffer console's region on purpose -- overlapping a
 * live text surface is the hard case, and it works because the
 * console keeps its characters (M85) instead of only pixels.
 *
 * Mouse events arrive from input_poll() as absolute positions
 * plus the raw button bitmask; the layer keeps its own edge
 * state. Clicks that miss the window fall through to the
 * desktop, which keeps the M84 click-marker behavior.
 */
#ifndef YOUSJ_WM_H
#define YOUSJ_WM_H

#include <yousj/types.h>

/* Open the demonstration window. No-op without a framebuffer. */
void wm_init(void);

/* The open window's rectangle, for the console's occlusion
 * test. Returns 0 when no window is open. */
int wm_window_rect(uint32_t *x, uint32_t *y, uint32_t *w,
                   uint32_t *h);

/* Redraw the window (the console scrolled and smeared it). */
void wm_repair(void);

/* One mouse state sample: absolute position + buttons. */
void wm_mouse(uint32_t x, uint32_t y, int buttons);

#endif /* YOUSJ_WM_H */
