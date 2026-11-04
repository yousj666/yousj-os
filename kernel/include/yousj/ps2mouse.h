/* Yousj OS - PS/2 mouse (M84, GUI step 4).
 *
 * Written from scratch. The mouse shares the i8042 with the
 * keyboard: the keyboard driver's drain routes AUX-port bytes to
 * ps2mouse_feed(), which assembles the standard 3-byte packets.
 * Movement is applied once per timer tick by input_poll() (called
 * from irq0_handler next to net_poll), which also moves the
 * on-screen arrow cursor in the framebuffer driver. A left-button
 * press drops a small marker square at the pointer, the first
 * visible effect of a click; real click routing belongs to the
 * window layer.
 */
#ifndef YOUSJ_PS2MOUSE_H
#define YOUSJ_PS2MOUSE_H

#include <yousj/types.h>

/* Enable the AUX port and tell the mouse to start reporting.
 * Bounded waits throughout: a missing mouse must never hang
 * boot. Safe to call when no framebuffer exists (the pointer
 * then just has nowhere to show). */
void ps2mouse_init(void);

/* One AUX byte from the 8042 drain (ps2kbd.c). */
void ps2mouse_feed(uint8_t byte);

/* Per-tick service: drain the 8042, apply queued movement and
 * buttons, redraw the cursor. Called from irq0_handler. */
void input_poll(void);

int ps2mouse_x(void);
int ps2mouse_y(void);
int ps2mouse_buttons(void);

#endif /* YOUSJ_PS2MOUSE_H */
