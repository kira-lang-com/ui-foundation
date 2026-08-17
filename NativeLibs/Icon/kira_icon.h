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

/* The same, with the coverage grown outward by `dilate_px` pixels: the WEIGHT of
 * a symbol. The library is drawn at ONE weight, so a glyph standing beside
 * heavier type has to be thickened to match its stems — a hairline outline next
 * to semibold words reads as a different, lighter object sitting near them.
 * Zero is the artwork as drawn, which is what `kira_icon_rasterize` asks for. */
int32_t kira_icon_rasterize_weighted(const char *svg_utf8, int32_t px, double dilate_px, uint8_t *out_buffer);

#ifdef __cplusplus
}
#endif
