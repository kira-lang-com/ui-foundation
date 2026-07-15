/* Immediate-mode icon draw for the legacy (non-Metal-batch) render path.
 *
 * The batched compositor (UiBatch, Metal context present) rasterizes icons into
 * its glyph atlas and draws tinted mode-1 quads. On every other backend — the
 * Sokol path, including WebGPU on wasm32-emscripten — UI primitives go through
 * kira-graphics' immediate-2D helpers instead: text uses kira_text_draw_run ->
 * kg_ui_blit_coverage. This file gives icons the exact same fallback: rasterize
 * the SVG once per (icon, pixel size) into a cached coverage bitmap and blit it
 * tinted through kg_ui_blit_coverage.
 *
 * Kept in its own translation unit (not kira_icon.c) so standalone users of the
 * rasterizer can link libkiraicon.a without pulling in the kg_ui_blit_coverage
 * dependency — the archive member is only selected when kira_icon_draw_run is
 * referenced, and that only happens in programs that also link kira-graphics.
 */

#include "kira_icon.h"

#include <stdlib.h>
#include <string.h>

/* Forward declaration of the kira-graphics GPU primitive. Both libraries are
 * static and linked into the same executable, so this resolves at final link
 * without ui-foundation taking a build-time include dependency on kira-graphics
 * (the same arrangement kira_text.c uses for text). */
extern void kg_ui_blit_coverage(double x, double y, int width, int rows,
                                int pitch, const unsigned char* coverage,
                                double r, double g, double b, double a);

/* Atlas-backed coverage draw (see kira-graphics sokol_impl.c): pack the icon's
 * coverage into the shared glyph atlas once, keyed by (icon hash, px), then draw
 * one tinted textured quad per frame instead of per-pixel coverage quads. Weakly
 * linked so non-sokol unit builds fall back to kg_ui_blit_coverage. */
extern void kg_ui_draw_glyph_coverage(int64_t key,
                                       double x, double y, int width, int rows,
                                       int pitch, const unsigned char* coverage,
                                       double r, double g, double b, double a)
    __attribute__((weak));

/* Logical-point <-> physical-pixel backing scale for the current UI pass (see
 * kira-graphics sokol_impl.c). On a Retina/high_dpi framebuffer this is > 1.0.
 * kg_ui_blit_coverage / kg_ui_draw_glyph_coverage treat the coverage bitmap's
 * width/rows as PHYSICAL pixels and map them back into point space by dividing
 * by this scale, so — exactly like text glyphs in kira_text.c — icons must
 * rasterize at side*scale physical pixels and pass those physical dimensions,
 * or they render at side/scale points (half size on 2x) anchored to the box
 * corner. Weakly linked so non-sokol unit builds fall back to 1.0. */
extern double kg_ui_dpi_scale(void) __attribute__((weak));

static double kira_icon_backing_scale(void) {
    if (kg_ui_dpi_scale) {
        double s = kg_ui_dpi_scale();
        return s >= 1.0 ? s : 1.0;
    }
    return 1.0;
}

/* Stable, non-zero atlas key for a rasterized icon. The SVG hash already
 * distinguishes icon shape; fold in the pixel size so different sizes get
 * distinct atlas entries. Kept disjoint from text keys (which fold a face
 * pointer + glyph id) by construction — collisions only cost a mis-draw, and the
 * hash+px space makes them vanishingly unlikely. */
static int64_t kira_icon_glyph_key(int64_t hash, int32_t px) {
    uint64_t k = 1469598103934665603ull; /* FNV offset basis */
    k = (k ^ (uint64_t)hash) * 1099511628211ull;
    k = (k ^ (uint64_t)(uint32_t)px) * 1099511628211ull;
    k = (k ^ 0x1C02Bull) * 1099511628211ull; /* domain separator vs text keys */
    return (int64_t)(k | 1ull);
}

/* Coverage cache: SVG parsing + scanline/stroke rasterization is far too slow
 * to repeat per frame per icon. Keyed by (svg hash, pixel size); LRU-evicted by
 * shifting, mirroring kira_text.c's face cache. */
#define KIRA_ICON_DRAW_CACHE_SLOTS 64

typedef struct {
    int64_t  hash; /* kira_icon_hash of the SVG text (never 0 for live slots) */
    int32_t  px;
    uint8_t* coverage; /* px * px bytes, row-major */
} kira_icon_draw_cache_slot;

static kira_icon_draw_cache_slot g_icon_draw_cache[KIRA_ICON_DRAW_CACHE_SLOTS];
static int g_icon_draw_cache_count = 0;

static const uint8_t* kira_icon_cached_coverage(const char* svg_utf8, int32_t px) {
    int64_t hash = kira_icon_hash(svg_utf8);
    for (int i = 0; i < g_icon_draw_cache_count; i += 1) {
        if (g_icon_draw_cache[i].hash == hash && g_icon_draw_cache[i].px == px) {
            return g_icon_draw_cache[i].coverage;
        }
    }

    uint8_t* coverage = (uint8_t*)malloc((size_t)px * (size_t)px);
    if (coverage == NULL) {
        return NULL;
    }
    /* A parse failure leaves the buffer cleared; cache it anyway so a bad SVG
     * costs one parse attempt, not one per frame. Blitting zeros draws nothing. */
    kira_icon_rasterize(svg_utf8, px, coverage);

    kira_icon_draw_cache_slot* slot;
    if (g_icon_draw_cache_count < KIRA_ICON_DRAW_CACHE_SLOTS) {
        slot = &g_icon_draw_cache[g_icon_draw_cache_count++];
    } else {
        /* Evict the oldest slot. */
        free(g_icon_draw_cache[0].coverage);
        for (int i = 1; i < KIRA_ICON_DRAW_CACHE_SLOTS; i += 1) {
            g_icon_draw_cache[i - 1] = g_icon_draw_cache[i];
        }
        slot = &g_icon_draw_cache[KIRA_ICON_DRAW_CACHE_SLOTS - 1];
    }
    slot->hash = hash;
    slot->px = px;
    slot->coverage = coverage;
    return coverage;
}

void kira_icon_draw_run(const char* svg_utf8,
                        double x, double y, double w, double h,
                        double r, double g, double b, double a) {
    if (svg_utf8 == NULL || svg_utf8[0] == '\0' || a <= 0.0 || w <= 0.0 || h <= 0.0) {
        return;
    }
    /* `w`/`h`/`x`/`y` are logical POINTS. Rasterize at physical resolution
     * (side * backing scale) so the icon is crisp on Retina and — crucially —
     * so the physical coverage dimensions handed to the draw primitive map back
     * to the intended point size (the primitive divides width/rows by scale).
     * This mirrors kira_text.c's glyph path exactly; off Retina scale == 1.0. */
    double side = w < h ? w : h;
    const double scale = kira_icon_backing_scale();
    int32_t px = (int32_t)(side * scale + 0.5);
    if (px <= 0 || px > 512) {
        return;
    }
    const uint8_t* coverage = kira_icon_cached_coverage(svg_utf8, px);
    if (coverage == NULL) {
        return;
    }
    /* Centre the square icon box inside the given bounds. The coverage occupies
     * px/scale POINTS once the primitive divides by scale, so centering is done
     * in point space against that rendered size (not the physical px count). */
    double render_pts = (double)px / scale;
    double gx = x + (w - render_pts) * 0.5;
    double gy = y + (h - render_pts) * 0.5;
    if (kg_ui_draw_glyph_coverage) {
        kg_ui_draw_glyph_coverage(kira_icon_glyph_key(kira_icon_hash(svg_utf8), px),
                                  gx, gy, px, px, px, coverage, r, g, b, a);
    } else {
        kg_ui_blit_coverage(gx, gy, px, px, px, coverage, r, g, b, a);
    }
}
