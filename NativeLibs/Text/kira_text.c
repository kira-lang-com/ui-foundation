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

#include <hb.h>
#include <hb-ft.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Bundled Figtree variable font data. */
#include "kira_figtree_font.h"

/* Forward declaration: embedded Figtree face loaded from the byte array above.
 * Defined further below alongside the draw-cache infrastructure. */
static kira_text_face* kira_text_embedded_face(float pixel_size);

struct kira_text_engine {
    FT_Library library;
};

struct kira_text_face {
    FT_Face        face;
    unsigned char* owned_blob; /* non-NULL when loaded from memory */
    float          pixel_size;
    int            has_kerning;
    hb_font_t*     hb_font;    /* lazily created HarfBuzz font over `face` */
};

/* Lazily create (and cache) a HarfBuzz font wrapping the FreeType face. The
 * face's pixel size must already be set; HarfBuzz reads its scale from FreeType,
 * so advances come back in 26.6 pixels matching the rasterized glyphs. */
static hb_font_t* kira_text_hb_font(kira_text_face* face) {
    if (face == NULL || face->face == NULL) {
        return NULL;
    }
    if (face->hb_font == NULL) {
        face->hb_font = hb_ft_font_create_referenced(face->face);
    } else {
        /* Pick up any size change since the font was created. */
        hb_ft_font_changed(face->hb_font);
    }
    return face->hb_font;
}

/* 26.6 fixed-point -> float pixels. */
static float kira_text_f26dot6(FT_Pos value) {
    return (float)value / 64.0f;
}

/* ------------------------------------------------------------------------
 * Process-lifetime registries for engines and faces.
 *
 * The Kira side (app/Backend/UiBatch.kira: uiBatchStateLoadFont) stores the
 * engine and face handles inside a UiBatchState that lives behind an opaque
 * nativeState slot for the whole program and is never explicitly destroyed
 * (state slots leak by design — kira_state_slot_reset). With no owner calling
 * kira_text_{face,engine}_destroy, the FT_Library/FT_Face allocations become
 * unreachable at process exit and `leaks --atExit` reports the entire FreeType
 * tree loaded from the system font as leaked.
 *
 * Mirror the native runtime's approach (kira-zig
 * packages/kira_native_bridge/src/runtime_helpers.c —
 * kira_native_state_registry / kira_task_registry): record every engine and
 * face at creation, unlink on explicit destroy, and free the survivors from a
 * single atexit handler registered on the first create. The public ABI is
 * unchanged, and this is double-destroy safe — an explicit destroy unlinks
 * before disposing, so the atexit sweep never touches an already-freed handle.
 *
 * Ordering is load-bearing: a FreeType face belongs to its library
 * (FT_Done_Face must precede FT_Done_FreeType), so the teardown disposes every
 * face before any engine. Faces from distinct engines are independent, so a
 * single faces-then-engines pass keeps every library alive while its faces are
 * torn down. The draw-cache/embedded-face statics (g_draw_engine,
 * g_embedded_engine, and their faces) route through these same create/destroy
 * paths, so they are tracked and reclaimed too; mid-run cache eviction and
 * embedded-face reload call kira_text_face_destroy, which unlinks first, so the
 * atexit sweep never double-frees an evicted face.
 */
typedef struct KiraTextEngineNode {
    kira_text_engine* engine;
    struct KiraTextEngineNode* next;
} KiraTextEngineNode;

typedef struct KiraTextFaceNode {
    kira_text_face* face;
    struct KiraTextFaceNode* next;
} KiraTextFaceNode;

static KiraTextEngineNode* g_engine_registry = NULL;
static KiraTextFaceNode*   g_face_registry   = NULL;

#if defined(_WIN32)
#include <windows.h>
static SRWLOCK g_text_registry_lock = SRWLOCK_INIT;
static void kira_text_registry_acquire(void) { AcquireSRWLockExclusive(&g_text_registry_lock); }
static void kira_text_registry_release(void) { ReleaseSRWLockExclusive(&g_text_registry_lock); }
#else
#include <pthread.h>
static pthread_mutex_t g_text_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static void kira_text_registry_acquire(void) { pthread_mutex_lock(&g_text_registry_lock); }
static void kira_text_registry_release(void) { pthread_mutex_unlock(&g_text_registry_lock); }
#endif

/* Raw teardown of a face's FreeType/HarfBuzz resources (no registry unlink).
 * hb_font holds a reference on the FT_Face, so it must go first. */
static void kira_text_face_dispose(kira_text_face* face) {
    if (face == NULL) {
        return;
    }
    if (face->hb_font != NULL) {
        hb_font_destroy(face->hb_font);
    }
    if (face->face != NULL) {
        FT_Done_Face(face->face);
    }
    free(face->owned_blob);
    free(face);
}

/* Raw teardown of an engine's FreeType library (no registry unlink). All faces
 * belonging to this library must already be disposed. */
static void kira_text_engine_dispose(kira_text_engine* engine) {
    if (engine == NULL) {
        return;
    }
    if (engine->library != NULL) {
        FT_Done_FreeType(engine->library);
    }
    free(engine);
}

static void kira_text_registry_teardown(void) {
    kira_text_registry_acquire();
    KiraTextFaceNode* face_node = g_face_registry;
    g_face_registry = NULL;
    KiraTextEngineNode* engine_node = g_engine_registry;
    g_engine_registry = NULL;
    kira_text_registry_release();

    /* Faces first: FT_Done_Face needs its still-live FT_Library. */
    while (face_node != NULL) {
        KiraTextFaceNode* next = face_node->next;
        kira_text_face_dispose(face_node->face);
        free(face_node);
        face_node = next;
    }
    while (engine_node != NULL) {
        KiraTextEngineNode* next = engine_node->next;
        kira_text_engine_dispose(engine_node->engine);
        free(engine_node);
        engine_node = next;
    }
}

/* Register the atexit sweep exactly once. Caller must hold the registry lock. */
static void kira_text_registry_register_atexit(void) {
    static int registered = 0;
    if (!registered) {
        registered = 1;
        atexit(kira_text_registry_teardown);
    }
}

static void kira_text_engine_registry_add(kira_text_engine* engine) {
    KiraTextEngineNode* node = (KiraTextEngineNode*)malloc(sizeof(*node));
    if (node == NULL) {
        return; /* untracked: survives to exit unreclaimed, never unsafe */
    }
    node->engine = engine;
    kira_text_registry_acquire();
    kira_text_registry_register_atexit();
    node->next = g_engine_registry;
    g_engine_registry = node;
    kira_text_registry_release();
}

static void kira_text_engine_registry_remove(kira_text_engine* engine) {
    kira_text_registry_acquire();
    KiraTextEngineNode** link = &g_engine_registry;
    while (*link != NULL) {
        if ((*link)->engine == engine) {
            KiraTextEngineNode* dead = *link;
            *link = dead->next;
            free(dead);
            break;
        }
        link = &(*link)->next;
    }
    kira_text_registry_release();
}

static void kira_text_face_registry_add(kira_text_face* face) {
    KiraTextFaceNode* node = (KiraTextFaceNode*)malloc(sizeof(*node));
    if (node == NULL) {
        return; /* untracked: survives to exit unreclaimed, never unsafe */
    }
    node->face = face;
    kira_text_registry_acquire();
    kira_text_registry_register_atexit();
    node->next = g_face_registry;
    g_face_registry = node;
    kira_text_registry_release();
}

static void kira_text_face_registry_remove(kira_text_face* face) {
    kira_text_registry_acquire();
    KiraTextFaceNode** link = &g_face_registry;
    while (*link != NULL) {
        if ((*link)->face == face) {
            KiraTextFaceNode* dead = *link;
            *link = dead->next;
            free(dead);
            break;
        }
        link = &(*link)->next;
    }
    kira_text_registry_release();
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
    kira_text_engine_registry_add(engine);
    return engine;
}

void kira_text_engine_destroy(kira_text_engine* engine) {
    if (engine == NULL) {
        return;
    }
    /* Unlink before disposing so the atexit sweep never double-frees. */
    kira_text_engine_registry_remove(engine);
    kira_text_engine_dispose(engine);
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
    kira_text_face_registry_add(wrapper);
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
    /* Unlink before disposing so the atexit sweep never double-frees. */
    kira_text_face_registry_remove(face);
    kira_text_face_dispose(face);
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

    hb_font_t* hb = kira_text_hb_font(face);
    if (hb == NULL) {
        return 0.0f;
    }

    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, utf8, byte_len, 0, byte_len);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(hb, buffer, NULL, 0);

    unsigned int count = hb_buffer_get_length(buffer);
    hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, NULL);
    double width = 0.0;
    for (unsigned int i = 0; i < count; i += 1) {
        width += (double)positions[i].x_advance / 64.0;
    }
    hb_buffer_destroy(buffer);
    return (float)width;
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
    /* Prefer the bundled Figtree font file shipped alongside the library. */
    static const char* const bundled_candidates[] = {
        "fonts/Figtree-VariableFont_wght.ttf",
        "../fonts/Figtree-VariableFont_wght.ttf",
    };
    for (size_t i = 0; i < sizeof(bundled_candidates) / sizeof(bundled_candidates[0]); i += 1) {
        FILE* probe = fopen(bundled_candidates[i], "rb");
        if (probe != NULL) {
            fclose(probe);
            return bundled_candidates[i];
        }
    }

    /* Figtree may be installed as a system font. */
    static const char* const figtree_candidates[] = {
        "C:/Windows/Fonts/Figtree-VariableFont_wght.ttf",
        "/System/Library/Fonts/Figtree-VariableFont_wght.ttf",
        "/usr/share/fonts/truetype/figtree/Figtree-VariableFont_wght.ttf",
        "/usr/share/fonts/opentype/figtree/Figtree-VariableFont_wght.ttf",
        "~/.fonts/Figtree-VariableFont_wght.ttf",
    };
    for (size_t i = 0; i < sizeof(figtree_candidates) / sizeof(figtree_candidates[0]); i += 1) {
        FILE* probe = fopen(figtree_candidates[i], "rb");
        if (probe != NULL) {
            fclose(probe);
            return figtree_candidates[i];
        }
    }

    /* Common system fonts across the host platforms this engine targets. */
    static const char* const sys_candidates[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "/System/Library/Fonts/SFNS.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
    };
    for (size_t i = 0; i < sizeof(sys_candidates) / sizeof(sys_candidates[0]); i += 1) {
        FILE* probe = fopen(sys_candidates[i], "rb");
        if (probe != NULL) {
            fclose(probe);
            return sys_candidates[i];
        }
    }

    /* No system font is available — use the embedded Figtree variable font.
     * The sentinel value "<builtin>" is handled by kira_text_cached_face(). */
    return "<builtin>";
}

/* Locate a CJK-capable system font. The bundled Figtree face (and Segoe/Arial)
 * have no CJK coverage, so committed Hanzi would render as .notdef tofu. These
 * fonts also carry Latin glyphs, so a mixed Latin+Hanzi run can be shaped
 * entirely with the CJK face. Returns NULL when none is installed. */
static const char* kira_text_discover_cjk_font(void) {
    static const char* const cjk_candidates[] = {
        "C:/Windows/Fonts/msyh.ttc",   /* Microsoft YaHei */
        "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/simsun.ttc", /* SimSun */
        "C:/Windows/Fonts/msgothic.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    };
    for (size_t i = 0; i < sizeof(cjk_candidates) / sizeof(cjk_candidates[0]); i += 1) {
        FILE* probe = fopen(cjk_candidates[i], "rb");
        if (probe != NULL) {
            fclose(probe);
            return cjk_candidates[i];
        }
    }
    return NULL;
}

/* Decode UTF-8 and report whether any scalar lands in a CJK/Kana/Hangul block
 * (>= U+3000), i.e. text that the default Latin face cannot render. */
static int kira_text_utf8_has_cjk(const char* utf8) {
    const unsigned char* p = (const unsigned char*)utf8;
    while (*p != '\0') {
        uint32_t cp;
        int len;
        if (p[0] < 0x80) {
            cp = p[0];
            len = 1;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
            len = 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
            len = 3;
        } else if ((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
            len = 4;
        } else {
            /* Malformed byte — skip it. */
            p += 1;
            continue;
        }
        if (cp >= 0x3000) {
            return 1;
        }
        p += len;
    }
    return 0;
}

/* Choose the face path for a run: when the caller left the path unset, prefer a
 * CJK font for runs that contain CJK scalars, otherwise the default Latin face. */
static const char* kira_text_resolve_font(const char* font_path, const char* utf8) {
    if (font_path != NULL && font_path[0] != '\0') {
        return font_path;
    }
    if (utf8 != NULL && kira_text_utf8_has_cjk(utf8)) {
        const char* cjk = kira_text_discover_cjk_font();
        if (cjk != NULL) {
            return cjk;
        }
    }
    return kira_text_discover_font();
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

    const float px = 32.0f;

    kira_text_face* face = NULL;
    if (strcmp(font_path, "<builtin>") == 0) {
        face = kira_text_embedded_face(px);
        if (face == NULL) {
            snprintf(report, sizeof(report),
                     "kira-text: ERROR could not load embedded Figtree font");
            kira_text_engine_destroy(engine);
            return report;
        }
    } else {
        face = kira_text_face_load(engine, font_path, 0);
        if (face == NULL) {
            snprintf(report, sizeof(report),
                     "kira-text: ERROR could not load face '%s'", font_path);
            kira_text_engine_destroy(engine);
            return report;
        }
    }

    if (face->pixel_size != px && !kira_text_face_set_pixel_size(face, px)) {
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

    if (strcmp(font_path, "<builtin>") != 0) {
        kira_text_face_destroy(face);
    }
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

/* Atlas-backed coverage draw: packs each glyph into the kira-graphics glyph
 * atlas once (keyed by `key`) and thereafter draws a single textured quad
 * instead of decomposing the coverage bitmap into per-pixel quads every frame.
 * Drop-in for kg_ui_blit_coverage with a stable, non-zero key. Weakly linked so
 * unit builds without the sokol backend fall back to per-pixel blitting. */
extern void kg_ui_draw_glyph_coverage(int64_t key,
                                       double x, double y, int width, int rows,
                                       int pitch, const unsigned char* coverage,
                                       double r, double g, double b, double a)
    __attribute__((weak));

/* Stable, non-zero atlas key for a rasterized glyph. Folds the resolved font
 * PATH (not the cached face pointer — that pointer is unstable across the
 * 16-slot face cache's LRU eviction and can be reused by malloc for a different
 * font, which would collide keys and mis-render), the shaped glyph id, and the
 * quantized physical pixel size — the exact triple that determines the coverage
 * bitmap — into a 64-bit FNV-1a mix. */
static int64_t kira_text_glyph_key(const char* font_path,
                                    uint32_t glyph, double phys_size) {
    int px_q = (int)(phys_size * 2.0 + 0.5); /* 0.5px buckets, matches face cache */
    uint64_t k = 1469598103934665603ull; /* FNV offset basis */
    for (const char* p = font_path; p != NULL && *p != '\0'; p += 1) {
        k = (k ^ (uint64_t)(unsigned char)*p) * 1099511628211ull;
    }
    k = (k ^ (uint64_t)glyph) * 1099511628211ull;
    k = (k ^ (uint64_t)(uint32_t)px_q) * 1099511628211ull;
    return (int64_t)(k | 1ull); /* never 0 (the atlas empty-slot sentinel) */
}

/* Physical-pixels-per-point backing scale of the current UI pass (kira-graphics
 * sokol backend). On a Retina/high_dpi framebuffer this is > 1.0; glyphs must be
 * rasterized at pixel_size * scale to stay crisp. Weakly linked so unit builds
 * without the sokol backend fall back to 1.0. */
extern double kg_ui_dpi_scale(void) __attribute__((weak));

static double kira_text_backing_scale(void) {
    if (kg_ui_dpi_scale) {
        double s = kg_ui_dpi_scale();
        if (s >= 1.0) {
            return s;
        }
    }
    return 1.0;
}

/* Embedded Figtree font face — lazily loaded from the bundled byte array. */
static kira_text_engine* g_embedded_engine = NULL;
static kira_text_face*   g_embedded_face   = NULL;
static float             g_embedded_ps     = 0.0f;

static kira_text_face* kira_text_embedded_face(float pixel_size) {
    if (g_embedded_face != NULL && g_embedded_ps == pixel_size) {
        return g_embedded_face;
    }
    if (g_embedded_face != NULL) {
        kira_text_face_destroy(g_embedded_face);
        g_embedded_face = NULL;
        g_embedded_ps = 0.0f;
    }
    if (g_embedded_engine == NULL) {
        g_embedded_engine = kira_text_engine_create();
        if (g_embedded_engine == NULL) return NULL;
    }
    g_embedded_face = kira_text_face_load_memory(
        g_embedded_engine,
        kira_figtree_font_data,
        (long)KIRA_FIGTREE_FONT_SIZE,
        0
    );
    if (g_embedded_face == NULL) return NULL;
    if (!kira_text_face_set_pixel_size(g_embedded_face, pixel_size)) {
        kira_text_face_destroy(g_embedded_face);
        g_embedded_face = NULL;
        return NULL;
    }
    g_embedded_ps = pixel_size;
    return g_embedded_face;
}

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
    /* The embedded Figtree font is loaded from the bundled byte array. */
    if (strcmp(path, "<builtin>") == 0) {
        return kira_text_embedded_face(pixel_size);
    }

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
    font_path = kira_text_resolve_font(font_path, utf8);
    if (font_path == NULL) {
        return;
    }

    /* All incoming geometry (x/y/w/h, pixel_size) is in logical POINTS. Rasterize
     * glyphs at physical resolution (point-size * backing scale) so text is crisp
     * on Retina/high_dpi, then divide FreeType/HarfBuzz metrics (which are now in
     * physical pixels) back into point space for positioning. kg_ui_blit_coverage
     * receives the glyph origin in points and maps the physical coverage bitmap
     * back down internally. Off Retina scale == 1.0, so this is a no-op. */
    const double scale = kira_text_backing_scale();
    const double inv_scale = 1.0 / scale;
    const double phys_size = pixel_size * scale;

    kira_text_face* face = kira_text_cached_face(font_path, (float)phys_size);
    if (face == NULL) {
        return;
    }

    kira_text_vmetrics vmetrics;
    kira_text_face_vmetrics(face, &vmetrics);
    double text_height = ((double)vmetrics.ascender + (double)vmetrics.descender) * inv_scale;
    double baseline = y + (h - text_height) * 0.5 + (double)vmetrics.ascender * inv_scale;

    hb_font_t* hb = kira_text_hb_font(face);
    if (hb == NULL) {
        return;
    }

    int byte_len = (int)strlen(utf8);
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, utf8, byte_len, 0, byte_len);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(hb, buffer, NULL, 0);

    unsigned int count = hb_buffer_get_length(buffer);
    hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, NULL);
    hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, NULL);

    double pen_x = x;
    for (unsigned int i = 0; i < count; i += 1) {
        uint32_t glyph = infos[i].codepoint; /* glyph id after shaping */
        double x_offset = (double)positions[i].x_offset / 64.0 * inv_scale;
        double y_offset = (double)positions[i].y_offset / 64.0 * inv_scale;

        kira_text_glyph_bitmap bitmap;
        if (kira_text_face_render_glyph(face, glyph, &bitmap)) {
            if (bitmap.width > 0 && bitmap.rows > 0) {
                double gx = pen_x + x_offset + (double)bitmap.bearing_x * inv_scale;
                double gy = baseline - y_offset - (double)bitmap.bearing_y * inv_scale;
                if (kg_ui_draw_glyph_coverage) {
                    kg_ui_draw_glyph_coverage(
                        kira_text_glyph_key(font_path, glyph, phys_size),
                        gx, gy, bitmap.width, bitmap.rows, bitmap.pitch,
                        bitmap.buffer, r, g, b, a);
                } else {
                    kg_ui_blit_coverage(gx, gy, bitmap.width, bitmap.rows, bitmap.pitch,
                                        bitmap.buffer, r, g, b, a);
                }
            }
        }
        pen_x += (double)positions[i].x_advance / 64.0 * inv_scale;
    }
    hb_buffer_destroy(buffer);
}

double kira_text_measure_run(const char* font_path, const char* utf8, double pixel_size) {
    if (utf8 == NULL || utf8[0] == '\0' || pixel_size <= 0.0) {
        return 0.0;
    }
    font_path = kira_text_resolve_font(font_path, utf8);
    if (font_path == NULL) {
        return 0.0;
    }
    kira_text_face* face = kira_text_cached_face(font_path, (float)pixel_size);
    if (face == NULL) {
        return 0.0;
    }
    return (double)kira_text_measure_utf8(face, utf8, -1);
}

double kira_text_line_height(const char* font_path, double pixel_size) {
    if (pixel_size <= 0.0) {
        return 0.0;
    }
    if (font_path == NULL || font_path[0] == '\0') {
        font_path = kira_text_discover_font();
        if (font_path == NULL) {
            return 0.0;
        }
    }
    kira_text_face* face = kira_text_cached_face(font_path, (float)pixel_size);
    if (face == NULL) {
        return 0.0;
    }
    kira_text_vmetrics vmetrics;
    kira_text_face_vmetrics(face, &vmetrics);
    return (double)vmetrics.line_height;
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

/* Decode a whole NUL-terminated UTF-8 string into codepoints in one call, so the
 * Kira side gets an editable buffer from an opaque String without per-codepoint
 * FFI round-trips. Writes up to `max` int32 codepoints into `out`, returns the
 * count. */
int kira_text_decode_codepoints(const char* s, int32_t* out, int max) {
    if (s == NULL || out == NULL || max <= 0) {
        return 0;
    }
    int len = (int)strlen(s);
    int32_t index = 0;
    int n = 0;
    uint32_t cp;
    while (n < max) {
        if (!kira_text_utf8_next(s, len, &index, &cp)) {
            break;
        }
        out[n] = (int32_t)cp;
        n += 1;
    }
    return n;
}
