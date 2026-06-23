/*
 * Kira text engine — FreeType-backed implementation (Stage 1).
 *
 * Real glyph rasterization and metrics. No placeholder bitmaps, no hardcoded
 * advances: every measurement and coverage bitmap comes from FreeType loading
 * the actual font outlines. HarfBuzz shaping is added in a later stage on top
 * of this rasterization core.
 */
#include "kira_text.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MODULE_H

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct kira_text_engine {
    FT_Library library;
};

struct kira_text_face {
    FT_Face        face;
    unsigned char* owned_blob; /* non-NULL when loaded from memory */
    float          pixel_size;
    int            has_kerning;
};

/* 26.6 fixed-point -> float pixels. */
static float kira_text_f26dot6(FT_Pos value) {
    return (float)value / 64.0f;
}

kira_text_engine* kira_text_engine_create(void) {
    kira_text_engine* engine = (kira_text_engine*)calloc(1, sizeof(*engine));
    if (engine == NULL) {
        return NULL;
    }
    FT_Error err = FT_Init_FreeType(&engine->library);
    if (err != 0) {
        free(engine);
        return NULL;
    }
    return engine;
}

void kira_text_engine_destroy(kira_text_engine* engine) {
    if (engine == NULL) {
        return;
    }
    if (engine->library != NULL) {
        FT_Done_FreeType(engine->library);
    }
    free(engine);
}

const char* kira_text_backend_version(void) {
    /* Static buffer is fine: version is constant for the process lifetime. */
    static char buffer[32];
    if (buffer[0] == '\0') {
        snprintf(buffer, sizeof(buffer), "FreeType %d.%d.%d",
                 FREETYPE_MAJOR, FREETYPE_MINOR, FREETYPE_PATCH);
    }
    return buffer;
}

static kira_text_face* kira_text_face_wrap(FT_Face face, unsigned char* owned_blob) {
    kira_text_face* wrapper = (kira_text_face*)calloc(1, sizeof(*wrapper));
    if (wrapper == NULL) {
        FT_Done_Face(face);
        free(owned_blob);
        return NULL;
    }
    wrapper->face = face;
    wrapper->owned_blob = owned_blob;
    wrapper->pixel_size = 0.0f;
    wrapper->has_kerning = FT_HAS_KERNING(face) ? 1 : 0;
    return wrapper;
}

kira_text_face* kira_text_face_load(kira_text_engine* engine,
                                    const char* path,
                                    int face_index) {
    if (engine == NULL || engine->library == NULL || path == NULL) {
        return NULL;
    }
    FT_Face face = NULL;
    if (FT_New_Face(engine->library, path, (FT_Long)face_index, &face) != 0) {
        return NULL;
    }
    return kira_text_face_wrap(face, NULL);
}

kira_text_face* kira_text_face_load_memory(kira_text_engine* engine,
                                           const uint8_t* data,
                                           long length,
                                           int face_index) {
    if (engine == NULL || engine->library == NULL || data == NULL || length <= 0) {
        return NULL;
    }
    /* FreeType keeps a pointer to the memory for the face lifetime, so we own a
     * private copy and free it when the face is destroyed. */
    unsigned char* blob = (unsigned char*)malloc((size_t)length);
    if (blob == NULL) {
        return NULL;
    }
    memcpy(blob, data, (size_t)length);

    FT_Face face = NULL;
    if (FT_New_Memory_Face(engine->library, blob, (FT_Long)length,
                           (FT_Long)face_index, &face) != 0) {
        free(blob);
        return NULL;
    }
    return kira_text_face_wrap(face, blob);
}

void kira_text_face_destroy(kira_text_face* face) {
    if (face == NULL) {
        return;
    }
    if (face->face != NULL) {
        FT_Done_Face(face->face);
    }
    free(face->owned_blob);
    free(face);
}

int kira_text_face_set_pixel_size(kira_text_face* face, float pixel_size) {
    if (face == NULL || face->face == NULL || pixel_size <= 0.0f) {
        return 0;
    }
    /* Use the fractional-aware setter so sub-pixel UI sizes round consistently
     * with how the platform requests them. */
    FT_F26Dot6 size_26dot6 = (FT_F26Dot6)(pixel_size * 64.0f + 0.5f);
    if (FT_Set_Char_Size(face->face, 0, size_26dot6, 0, 0) != 0) {
        return 0;
    }
    face->pixel_size = pixel_size;
    return 1;
}

void kira_text_face_vmetrics(const kira_text_face* face, kira_text_vmetrics* out) {
    if (out == NULL) {
        return;
    }
    out->ascender = 0.0f;
    out->descender = 0.0f;
    out->line_height = 0.0f;
    if (face == NULL || face->face == NULL) {
        return;
    }
    FT_Size_Metrics metrics = face->face->size->metrics;
    out->ascender = kira_text_f26dot6(metrics.ascender);
    out->descender = -kira_text_f26dot6(metrics.descender); /* descender is negative in FT */
    out->line_height = kira_text_f26dot6(metrics.height);
}

uint32_t kira_text_face_glyph_index(const kira_text_face* face,
                                        uint32_t codepoint) {
    if (face == NULL || face->face == NULL) {
        return 0;
    }
    return (unsigned int)FT_Get_Char_Index(face->face, (FT_ULong)codepoint);
}

float kira_text_measure_utf8(kira_text_face* face, const char* utf8, int byte_len) {
    if (face == NULL || face->face == NULL || utf8 == NULL) {
        return 0.0f;
    }
    if (byte_len < 0) {
        byte_len = (int)strlen(utf8);
    }
    if (byte_len == 0) {
        return 0.0f;
    }

    FT_Face ft = face->face;
    float pen_x = 0.0f;
    int index = 0;
    unsigned int previous_glyph = 0;
    unsigned int codepoint = 0;

    while (kira_text_utf8_next(utf8, byte_len, &index, &codepoint)) {
        FT_UInt glyph = FT_Get_Char_Index(ft, (FT_ULong)codepoint);

        if (face->has_kerning && previous_glyph != 0 && glyph != 0) {
            FT_Vector kerning;
            if (FT_Get_Kerning(ft, previous_glyph, glyph,
                               FT_KERNING_DEFAULT, &kerning) == 0) {
                pen_x += kira_text_f26dot6(kerning.x);
            }
        }

        if (FT_Load_Glyph(ft, glyph, FT_LOAD_DEFAULT) == 0) {
            pen_x += kira_text_f26dot6(ft->glyph->advance.x);
        }
        previous_glyph = glyph;
    }
    return pen_x;
}

int kira_text_face_render_glyph(kira_text_face* face,
                                uint32_t glyph_index,
                                kira_text_glyph_bitmap* out) {
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if (face == NULL || face->face == NULL) {
        return 0;
    }
    FT_Face ft = face->face;
    if (FT_Load_Glyph(ft, (FT_UInt)glyph_index, FT_LOAD_DEFAULT) != 0) {
        return 0;
    }
    if (FT_Render_Glyph(ft->glyph, FT_RENDER_MODE_NORMAL) != 0) {
        return 0;
    }
    FT_GlyphSlot slot = ft->glyph;
    out->width = (int)slot->bitmap.width;
    out->rows = (int)slot->bitmap.rows;
    out->bearing_x = slot->bitmap_left;
    out->bearing_y = slot->bitmap_top;
    out->advance = kira_text_f26dot6(slot->advance.x);
    out->pitch = slot->bitmap.pitch;
    out->buffer = slot->bitmap.buffer;
    return 1;
}

static const char* kira_text_discover_font(void) {
    /* Common system fonts across the host platforms this engine targets. */
    static const char* const candidates[] = {
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i += 1) {
        FILE* probe = fopen(candidates[i], "rb");
        if (probe != NULL) {
            fclose(probe);
            return candidates[i];
        }
    }
    return NULL;
}

const char* kira_text_probe_report(const char* font_path) {
    static char report[512];

    if (font_path == NULL || font_path[0] == '\0') {
        font_path = kira_text_discover_font();
        if (font_path == NULL) {
            snprintf(report, sizeof(report),
                     "kira-text: ERROR no system font found to probe");
            return report;
        }
    }

    kira_text_engine* engine = kira_text_engine_create();
    if (engine == NULL) {
        snprintf(report, sizeof(report),
                 "kira-text: ERROR FreeType failed to initialize");
        return report;
    }

    kira_text_face* face = kira_text_face_load(engine, font_path, 0);
    if (face == NULL) {
        snprintf(report, sizeof(report),
                 "kira-text: ERROR could not load face '%s'", font_path);
        kira_text_engine_destroy(engine);
        return report;
    }

    const float px = 32.0f;
    if (!kira_text_face_set_pixel_size(face, px)) {
        snprintf(report, sizeof(report),
                 "kira-text: ERROR could not size face '%s'", font_path);
        kira_text_face_destroy(face);
        kira_text_engine_destroy(engine);
        return report;
    }

    uint32_t glyph_h = kira_text_face_glyph_index(face, (uint32_t)'H');
    kira_text_glyph_bitmap bitmap;
    int rendered = kira_text_face_render_glyph(face, glyph_h, &bitmap);
    float hello = kira_text_measure_utf8(face, "Hello", -1);
    kira_text_vmetrics vmetrics;
    kira_text_face_vmetrics(face, &vmetrics);

    if (glyph_h == 0 || !rendered) {
        snprintf(report, sizeof(report),
                 "kira-text: ERROR glyph 'H' missing/unrenderable in '%s'",
                 font_path);
    } else {
        snprintf(report, sizeof(report),
                 "%s | font=%s | px=%.0f | line=%.1f asc=%.1f desc=%.1f | "
                 "glyph(H)=%u dims=%dx%d adv=%.1f bearing=(%d,%d) | "
                 "measure(\"Hello\")=%.1fpx",
                 kira_text_backend_version(), font_path, px,
                 vmetrics.line_height, vmetrics.ascender, vmetrics.descender,
                 glyph_h, bitmap.width, bitmap.rows, bitmap.advance,
                 bitmap.bearing_x, bitmap.bearing_y, hello);
    }

    kira_text_face_destroy(face);
    kira_text_engine_destroy(engine);
    return report;
}

/* Forward declaration of the kira-graphics GPU primitive. Both libraries are
 * static and linked into the same executable, so this resolves at final link
 * without ui-foundation taking a build-time include dependency on kira-graphics.
 */
extern void kg_ui_blit_coverage(double x, double y, int width, int rows,
                                int pitch, const unsigned char* coverage,
                                double r, double g, double b, double a);

/* Face cache: FT_New_Face parses the whole font, far too slow to repeat per
 * draw call. Cache a handful of faces keyed by (path, quantized pixel size). */
#define KIRA_TEXT_DRAW_CACHE_SLOTS 16

typedef struct {
    char            path[260];
    int             pixel_size_q; /* pixel_size rounded to 0.5px units */
    kira_text_face* face;
} kira_text_draw_cache_slot;

static kira_text_engine* g_draw_engine = NULL;
static kira_text_draw_cache_slot g_draw_cache[KIRA_TEXT_DRAW_CACHE_SLOTS];
static int g_draw_cache_count = 0;

static kira_text_face* kira_text_cached_face(const char* path, float pixel_size) {
    if (g_draw_engine == NULL) {
        g_draw_engine = kira_text_engine_create();
        if (g_draw_engine == NULL) {
            return NULL;
        }
    }
    int quantized = (int)(pixel_size * 2.0f + 0.5f);

    for (int i = 0; i < g_draw_cache_count; i += 1) {
        if (g_draw_cache[i].pixel_size_q == quantized &&
            strcmp(g_draw_cache[i].path, path) == 0) {
            return g_draw_cache[i].face;
        }
    }

    kira_text_face* face = kira_text_face_load(g_draw_engine, path, 0);
    if (face == NULL) {
        return NULL;
    }
    kira_text_face_set_pixel_size(face, pixel_size);

    kira_text_draw_cache_slot* slot;
    if (g_draw_cache_count < KIRA_TEXT_DRAW_CACHE_SLOTS) {
        slot = &g_draw_cache[g_draw_cache_count++];
    } else {
        /* Evict the oldest slot. */
        kira_text_face_destroy(g_draw_cache[0].face);
        for (int i = 1; i < KIRA_TEXT_DRAW_CACHE_SLOTS; i += 1) {
            g_draw_cache[i - 1] = g_draw_cache[i];
        }
        slot = &g_draw_cache[KIRA_TEXT_DRAW_CACHE_SLOTS - 1];
    }
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->pixel_size_q = quantized;
    slot->face = face;
    return face;
}

void kira_text_draw_run(const char* font_path,
                        const char* utf8,
                        double x, double y, double w, double h,
                        double r, double g, double b, double a,
                        double pixel_size) {
    if (utf8 == NULL || utf8[0] == '\0' || pixel_size <= 0.0 || a <= 0.0) {
        return;
    }
    if (font_path == NULL || font_path[0] == '\0') {
        font_path = kira_text_discover_font();
        if (font_path == NULL) {
            return;
        }
    }

    kira_text_face* face = kira_text_cached_face(font_path, (float)pixel_size);
    if (face == NULL) {
        return;
    }

    kira_text_vmetrics vmetrics;
    kira_text_face_vmetrics(face, &vmetrics);
    double text_height = (double)vmetrics.ascender + (double)vmetrics.descender;
    double baseline = y + (h - text_height) * 0.5 + (double)vmetrics.ascender;

    double pen_x = x;
    int byte_len = (int)strlen(utf8);
    int index = 0;
    uint32_t codepoint = 0;
    uint32_t previous_glyph = 0;

    while (kira_text_utf8_next(utf8, byte_len, &index, &codepoint)) {
        uint32_t glyph = (uint32_t)FT_Get_Char_Index(face->face, (FT_ULong)codepoint);

        if (face->has_kerning && previous_glyph != 0 && glyph != 0) {
            FT_Vector kerning;
            if (FT_Get_Kerning(face->face, previous_glyph, glyph,
                               FT_KERNING_DEFAULT, &kerning) == 0) {
                pen_x += kira_text_f26dot6(kerning.x);
            }
        }

        kira_text_glyph_bitmap bitmap;
        if (kira_text_face_render_glyph(face, glyph, &bitmap)) {
            if (bitmap.width > 0 && bitmap.rows > 0) {
                kg_ui_blit_coverage(pen_x + (double)bitmap.bearing_x,
                                    baseline - (double)bitmap.bearing_y,
                                    bitmap.width, bitmap.rows, bitmap.pitch,
                                    bitmap.buffer, r, g, b, a);
            }
            pen_x += (double)bitmap.advance;
        }
        previous_glyph = glyph;
    }
}

int kira_text_utf8_next(const char* s, int len, int32_t* index, uint32_t* codepoint) {
    if (s == NULL || index == NULL || codepoint == NULL) {
        return 0;
    }
    int i = *index;
    if (i < 0 || i >= len) {
        return 0;
    }

    unsigned char first = (unsigned char)s[i];
    unsigned int cp;
    int extra;

    if (first < 0x80u) {
        cp = first;
        extra = 0;
    } else if ((first & 0xE0u) == 0xC0u) {
        cp = first & 0x1Fu;
        extra = 1;
    } else if ((first & 0xF0u) == 0xE0u) {
        cp = first & 0x0Fu;
        extra = 2;
    } else if ((first & 0xF8u) == 0xF0u) {
        cp = first & 0x07u;
        extra = 3;
    } else {
        /* Invalid leading byte. */
        *index = i + 1;
        *codepoint = 0xFFFDu;
        return 1;
    }

    if (i + extra >= len) {
        /* Truncated sequence. */
        *index = len;
        *codepoint = 0xFFFDu;
        return 1;
    }

    for (int k = 1; k <= extra; k += 1) {
        unsigned char cont = (unsigned char)s[i + k];
        if ((cont & 0xC0u) != 0x80u) {
            /* Malformed continuation: emit replacement, resync at this byte. */
            *index = i + k;
            *codepoint = 0xFFFDu;
            return 1;
        }
        cp = (cp << 6) | (cont & 0x3Fu);
    }

    *index = i + extra + 1;
    *codepoint = cp;
    return 1;
}
