/* Yousj OS - PS/2 keyboard input (M83, GUI step 3).
 *
 * Written from scratch. The i8042 is polled, not interrupt
 * driven: the console read paths already sleep-poll their input
 * source, so ps2kbd_getc() drains the controller's output buffer
 * on each call, decodes scancode set 1 (what firmware leaves
 * active, translated) into ASCII, and queues it in a small ring.
 * serial_getc_nonblock() consults this ring before the UART, so
 * a local keyboard and the serial line feed the same console --
 * the input mirror of M82's output hook.
 *
 * US layout; shift and caps-lock tracked; ctrl produces control
 * codes; extended (0xE0) keys are consumed and dropped until the
 * window layer wants them. AUX-port (mouse) bytes are discarded
 * here so they can never corrupt the key stream.
 */
#ifndef YOUSJ_PS2KBD_H
#define YOUSJ_PS2KBD_H

/* Next typed character (0..255), or -1 when the ring is empty.
 * Lazily initialises the driver on first call. */
int ps2kbd_getc(void);

/* 1 when a typed character is waiting. */
int ps2kbd_ready(void);

/* Drain the 8042 once (keys into the ring, AUX bytes to the
 * mouse driver). Called per timer tick by input_poll (M84). */
void ps2_poll(void);

#endif /* YOUSJ_PS2KBD_H */
