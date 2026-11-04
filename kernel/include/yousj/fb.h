/* Yousj OS - linear framebuffer (M81, GUI step 1).
 *
 * Written from scratch. The UEFI bootloader (M81) sets a GOP
 * direct-color mode and hands the kernel the framebuffer through
 * boot_info.fb; fb_init() maps it into the kernel master space and
 * the drawing calls below paint into it. There is no window system
 * yet -- that is a later step of the GUI line; this header is the
 * raw-pixel layer everything else will stand on.
 *
 * Colors are 0xRRGGBB; the driver translates to the firmware's
 * channel order (boot_info.fb.reserved[0]).
 */
#ifndef YOUSJ_FB_H
#define YOUSJ_FB_H

#include <yousj/types.h>

struct yousj_framebuffer;

/* Claim and map the framebuffer described by a boot_info.fb
 * SNAPSHOT and paint the M81 banner. The caller must copy fb out
 * of boot_info early: boot_info is reached through the hand-off
 * identity map, which dies when the master space loads, long
 * before subsystems start. An all-zero fb (multiboot chain,
 * headless firmware) is a quiet no-op: the serial console
 * remains the only console. */
void fb_init(const struct yousj_framebuffer *fb_info);

/* 1 when a screen is live. */
int fb_present(void);

/* Fill the whole screen with one color (0xRRGGBB). */
void fb_clear(uint32_t rgb);

/* Filled rectangle in pixel coordinates (clipped). */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t rgb);

/* Draw text with the built-in 8x16 font at pixel position (x, y),
 * scaled by an integer factor (>= 1). bg is painted behind glyphs
 * so lines overwrite cleanly. */
void fb_draw_text(uint32_t x, uint32_t y, const char *text,
                  uint32_t fg, uint32_t bg, uint32_t scale);

uint32_t fb_width(void);
uint32_t fb_height(void);

#endif /* YOUSJ_FB_H */
