/* Kira UI icon engine: rasterizes an SVG icon (a practical subset — see
 * kira_icon.c) into an 8-bit coverage bitmap. Coverage-only by design: color is
 * applied at draw time as a tint (the compositor's mode-1 atlas quads), exactly
 * like text glyphs, so one rasterization serves every icon color. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable 63-bit FNV-1a hash of the SVG text — the compositor's atlas cache key. */
int64_t kira_icon_hash(const char *svg_utf8);

/* Rasterize `svg_utf8` into a px-by-px 8-bit coverage bitmap (row-major, pitch ==
 * px), scaled from the document's viewBox with aspect preserved and centered.
 * `out_buffer` is caller-allocated (px * px bytes) and fully overwritten.
 * Returns 1 on success, 0 on parse failure (buffer cleared to 0). */
int32_t kira_icon_rasterize(const char *svg_utf8, int32_t px, uint8_t *out_buffer);

/* Immediate-mode draw for the legacy (non-Metal-batch) render path: rasterize
 * (cached per icon+size) and blit the coverage tinted (r,g,b,a) through
 * kira-graphics' kg_ui_blit_coverage, centred in the (x,y,w,h) box — the icon
 * counterpart of kira_text_draw_run. Requires the final executable to also link
 * kira-graphics (kira_icon_draw.c is a separate archive member, so programs
 * that only rasterize never pull in that dependency). */
void kira_icon_draw_run(const char *svg_utf8,
                        double x, double y, double w, double h,
                        double r, double g, double b, double a);

#ifdef __cplusplus
}
#endif
