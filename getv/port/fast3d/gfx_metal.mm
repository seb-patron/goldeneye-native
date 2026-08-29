// Native Metal backend for the Fast3D GfxRenderingAPI -- the tvOS/iOS unlock. GL ES is
// deprecated on Apple platforms and our fast3d wants desktop GL 2.1 (#version 120), which
// does not exist on tvOS at all; Metal is the only forward path there. See
// docs/ROADMAP.md "Phase 3: presentation and platforms" and docs/REUSE_AUDIT.md.
//
// PORTED FROM, NOT COPIED: kenix3/libultraship, branch port-maintenance,
// src/fast/backends/gfx_metal.{h,cpp} + gfx_metal_shader.cpp + shaders/metal/default.shader.metal
// (vendored locally at vendor/soh/libultraship -- MIT, Copyright (c) 2022 kenix3). That
// backend targets a C++ virtual-class GfxRenderingAPI with full multi-framebuffer,
// MSAA-variant and CPU-readback support; this one targets our much smaller C
// struct-of-function-pointers (gfx_rendering_api.h, 22 entries, no framebuffer concept at
// all) with a single render target -- the screen -- so the framebuffer/MSAA/readback
// machinery is deliberately not carried over. What IS taken, independently reimplemented
// against plain Objective-C Metal (not metal-cpp, so nothing extra needs vendoring): the
// device/layer/queue setup, per-shader-variant MTLRenderPipelineState caching, and the
// technique of building one MSL source string per N64 colour-combiner and compiling it at
// runtime. The combiner formula itself is transliterated from THIS project's own
// gfx_opengl.c (gfx_opengl_create_and_load_new_shader), not from LUS's shader.metal
// template, because it must match our exact SHADER_*/CC_C2_* bit layout (gfx_cc.h) --
// see the note below about why gfx_cc_get_features() is NOT used here.
//
// KNOWN v1 GAPS, deliberate, staged: no ImGui/launcher rendering on this backend yet (the
// ge_imgui.h hooks are safe no-ops when never initialised -- see gfx_sdl2.c), no
// supersample/CRT/FXAA post-processing (the GE_POSTFX/TVOS_SUPERSAMPLE machinery in
// gfx_opengl.c is GL-specific and has no Metal equivalent yet), no 3-point texture
// filtering (configFiltering==2). None of these block a first real frame; all are fast
// follows once one renders. GETV_SHOTFRAME capture (ge_shot_maybe_metal(), below) is done
// -- same env vars and BMP layout as gfx_opengl.c's ge_shot_maybe(), for headless
// verification without a screenshot-capable window (this port's whole reason to exist on
// tvOS/iOS, where nothing else can photograph the screen).
#ifdef RAPI_METAL

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <SDL2/SDL.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifndef _LANGUAGE_C
# define _LANGUAGE_C
#endif
#include <PR/gbi.h>

#include "../platform.h"
#include "../configfile.h"
#include "gfx_cc.h"
#include "gfx_rendering_api.h"
#include "gfx_pc.h"
#include "gfx_metal.h"

#ifdef GE_WITH_IMGUI
#include "imgui.h"
#include "imgui_impl_metal.h"
#include "../src/ge_imgui.h"
#endif

void *gePortMetalLayer = NULL;
void *gePortMetalWindow = NULL;

#define SHADER_PROGRAM_POOL_SIZE 128
#define TEX_CACHE_STEP 512
#define VBO_POOL_COUNT 3
#define VBO_POOL_BYTES (4 * 1024 * 1024)

struct ShaderProgram {
    uint64_t shader_id;
    id<MTLRenderPipelineState> pipeline;
    uint8_t num_inputs;
    bool used_textures[2];
    uint8_t num_floats;
    bool opt_alpha;
    bool used_noise;
};

struct MetalTexture {
    id<MTLTexture> tex;
    id<MTLSamplerState> sampler;
    float size[2];
    bool linear_filter;
    /* Parallax height companion, one per diffuse texture slot -- same reasoning as
     * gfx_opengl.c's identical has_height/height_gltex fields on struct GLTexture: this is
     * per-slot rather than a single shared "current" texture because the uniform refresh
     * (gfx_metal_draw_triangles, every draw) and the override upload
     * (ge_texpack_try_override, gfx_pc.c, only on a texture-cache miss) run at different
     * rates, so a shared flag would go stale the moment a different, already-cached texture
     * got rebound without re-importing. */
    bool has_height;
    id<MTLTexture> height_tex;
};

struct FrameUniforms {
    int32_t frame_count;
};

/* Mirrors gfx_opengl.c's uTex0Size/uTex1Size/uTex0Filter/uTex1Filter uniforms -- needed
 * for correctness whenever both textures are sampled (the TEXEL1 rescale, see the
 * fragment shader body below), not just for the 3-point filter this port does not yet
 * implement on Metal. has_height mirrors GL's uHasHeight: whether THIS draw's tile-0
 * texture has a real height companion bound (gfx_metal_draw_triangles decides, per draw,
 * from metal_tex[0]->has_height) or is reading the neutral placeholder. int32_t, not bool,
 * to match this struct's layout byte-for-byte against the MSL `struct DrawUniforms` it is
 * read as through a raw buffer binding -- MSL bool is not guaranteed the same size as C++
 * bool across compilers, int32_t is unambiguous on both sides. */
struct DrawUniforms {
    float tex0_size[2];
    float tex1_size[2];
    int32_t has_height;
};

static struct ShaderProgram shader_program_pool[SHADER_PROGRAM_POOL_SIZE];
static uint16_t shader_program_pool_size;
static struct ShaderProgram *cur_prg = NULL;

static int tex_cache_size = 0;
static int num_textures = 0;
static struct MetalTexture *tex_cache = NULL;
static struct MetalTexture *metal_tex[2];
static int metal_curtex = 0;

static uint32_t frame_count;

/* Parallax height channel's neutral placeholder -- see gfx_opengl.c's identical
 * ge_height_tex for the full reasoning. Bound whenever the currently selected tile-0
 * texture has no real height data of its own (MetalTexture::has_height false), so
 * uTexHeight is always legal to sample even on the frame before any override could
 * possibly have loaded one. 1x1, mid-grey (128,128,128,255): the value the fragment
 * shader's `- 0.5` reads as "no displacement". */
static id<MTLTexture> mtl_height_placeholder;
static id<MTLSamplerState> mtl_height_sampler;

static id<MTLDevice> mtl_device;
static CAMetalLayer *mtl_layer;
static id<MTLCommandQueue> mtl_queue;
static id<MTLCommandBuffer> mtl_cmdbuf;
static id<MTLRenderCommandEncoder> mtl_encoder;
#ifdef GE_WITH_IMGUI
/* The overlay's OWN encoder, separate from the game's mtl_encoder above -- a second render
 * pass on the same drawable texture, loadAction=Load so the game's pixels survive. See the
 * frame-lifecycle note on gfx_metal_end_frame()/gePortMetalFinishFrame(). */
static id<MTLRenderCommandEncoder> mtl_overlay_encoder;
#endif
static id<CAMetalDrawable> mtl_drawable;
static id<MTLTexture> mtl_depth_tex;
static uint32_t mtl_depth_w, mtl_depth_h;

/* What gfx_metal_set_viewport/set_scissor should clamp against for THIS frame's game
 * encoder -- native drawable size on the fast path, the inflated supersampled size on the
 * postfx path (gfx_metal_start_frame sets this each frame; see GETV-SUPERSAMPLE below for
 * why the game's own draws can land in a target larger than the drawable). Reading
 * mtl_drawable.texture directly here (the previous behaviour) is only correct on the fast
 * path -- on the postfx path mtl_drawable is not even acquired yet at this point (see
 * gfx_metal_end_frame's deferred nextDrawable), and even once it is, its size is the wrong
 * (native, not inflated) reference for scissoring the game's own draws. */
static uint32_t mtl_render_target_w, mtl_render_target_h;

/* GETV-SUPERSAMPLE / GETV_MSAA / GETV_FXAA offscreen path. One shared intermediate colour
 * target serves MSAA-resolve, supersample-downsample and (a fast follow) FXAA input alike --
 * mirroring how gfx_opengl.c's GE_POSTFX path unifies the same three into one pass, not
 * three separate mechanisms. mtl_pp_depth backs the game's own depth test/write during that
 * pass; nothing ever reads it back afterward, so MTLStorageModeMemoryless is valid for the
 * whole thing on this TBDR GPU, not just for MSAA specifically.
 *
 * mtl_pp_color/mtl_pp_depth are always single-sample: the composite pass (gfx_metal_end_frame)
 * only ever samples mtl_pp_color, and CAMetalLayer's drawable can never itself be multisampled,
 * so something single-sample has to exist as the hand-off point regardless of whether MSAA is
 * active. mtl_pp_color_ms/mtl_pp_depth_ms are the actual multisample render targets the game
 * draws into when GETV_MSAA is active -- both MTLStorageModeMemoryless, since Metal resolves
 * mtl_pp_color_ms into mtl_pp_color automatically via MTLStoreActionMultisampleResolve at
 * endEncoding, and depth is never resolved (nothing reads it back either way). nil when MSAA
 * is off, in which case the game draws straight into mtl_pp_color/mtl_pp_depth exactly as
 * increment 3 already did. */
static id<MTLTexture> mtl_pp_color;
static id<MTLTexture> mtl_pp_depth;
static id<MTLTexture> mtl_pp_color_ms;
static id<MTLTexture> mtl_pp_depth_ms;
static uint32_t mtl_pp_w, mtl_pp_h;
static uint32_t mtl_pp_built_samples;
static id<MTLRenderPipelineState> mtl_pp_pipeline;
static id<MTLSamplerState> mtl_pp_sampler;

static id<MTLBuffer> mtl_vbo_pool[VBO_POOL_COUNT];
static int mtl_vbo_index;
static size_t mtl_vbo_offset;

/* [depth_test][depth_mask][zmode_decal]. Precomputed once: MTLDepthStencilState is
 * immutable after creation, and there are only 8 combinations. */
static id<MTLDepthStencilState> mtl_depth_states[2][2][2];

static bool cur_depth_test = false, cur_depth_mask = true, cur_zmode_decal = false;

static bool gfx_metal_z_is_from_0_to_1(void) {
    /* Metal's NDC z range is [0,1], unlike GL's [-1,1] -- this is the one place that
     * difference surfaces in the abstract API; gfx_pc.c builds the projection matrix
     * accordingly. Getting this wrong clips/z-fights everything, silently. */
    return true;
}

// ------------------------------------------------------------------ shader source build
//
// Transliterated from gfx_opengl_create_and_load_new_shader() in gfx_opengl.c, MSL instead
// of GLSL. Deliberately NOT using gfx_cc_get_features() (gfx_cc.c): that helper's
// do_single/do_multiply/do_mix/color_alpha_same are computed only from cycle 1's c[0]/c[1]
// arrays and never from cycle 2's c2[0]/c2[1] -- silently wrong for every two-cycle
// combiner, and two-cycle combiners are nearly everywhere in this game. Decoding inline
// here, matching gfx_opengl.c's own (correct, two-cycle-aware) logic byte for byte, avoids
// depending on that latent bug. Flagged separately; not this file's job to fix gfx_cc.c.

static void m_append_str(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
}
static void m_append_line(char *buf, size_t *len, const char *str) {
    while (*str != '\0') buf[(*len)++] = *str++;
    buf[(*len)++] = '\n';
}

static const char *m_shader_item_to_str(uint32_t item, bool with_alpha, bool only_alpha, bool inputs_have_alpha, bool hint_single_element) {
    if (!only_alpha) {
        switch (item) {
            case SHADER_0:
                return with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : "float3(0.0, 0.0, 0.0)";
            case SHADER_INPUT_1:
                return with_alpha || !inputs_have_alpha ? "in.input1" : "in.input1.xyz";
            case SHADER_INPUT_2:
                return with_alpha || !inputs_have_alpha ? "in.input2" : "in.input2.xyz";
            case SHADER_INPUT_3:
                return with_alpha || !inputs_have_alpha ? "in.input3" : "in.input3.xyz";
            case SHADER_INPUT_4:
                return with_alpha || !inputs_have_alpha ? "in.input4" : "in.input4.xyz";
            case SHADER_TEXEL0:
                return with_alpha ? "texVal0" : "texVal0.xyz";
            case SHADER_TEXEL0A:
                return hint_single_element ? "texVal0.w" :
                    (with_alpha ? "float4(texVal0.w, texVal0.w, texVal0.w, texVal0.w)" : "float3(texVal0.w, texVal0.w, texVal0.w)");
            case SHADER_TEXEL1:
                return with_alpha ? "texVal1" : "texVal1.xyz";
            case SHADER_COMBINED:
                return with_alpha ? "texel" : (inputs_have_alpha ? "texel.xyz" : "texel");
            case SHADER_COMBINEDA:
                if (!inputs_have_alpha)
                    return with_alpha ? "float4(1.0, 1.0, 1.0, 1.0)" : "float3(1.0, 1.0, 1.0)";
                return hint_single_element ? "texel.w" :
                    (with_alpha ? "float4(texel.w, texel.w, texel.w, texel.w)" : "float3(texel.w, texel.w, texel.w)");
        }
    } else {
        switch (item) {
            case SHADER_0: return "0.0";
            case SHADER_INPUT_1: return "in.input1.w";
            case SHADER_INPUT_2: return "in.input2.w";
            case SHADER_INPUT_3: return "in.input3.w";
            case SHADER_INPUT_4: return "in.input4.w";
            case SHADER_TEXEL0: return "texVal0.w";
            case SHADER_TEXEL0A: return "texVal0.w";
            case SHADER_TEXEL1: return "texVal1.w";
            case SHADER_COMBINED:
            case SHADER_COMBINEDA:
                return "texel.w";
        }
    }
    return only_alpha ? "0.0" : (with_alpha ? "float4(0.0, 0.0, 0.0, 0.0)" : "float3(0.0, 0.0, 0.0)");
}

static void m_append_formula_row(char *buf, size_t *len, const uint8_t row[4], bool with_alpha, bool only_alpha, bool opt_alpha) {
    const bool do_single   = row[2] == 0;
    const bool do_multiply = row[1] == 0 && row[3] == 0;
    const bool do_mix      = row[1] == row[3];
    if (do_single) {
        m_append_str(buf, len, m_shader_item_to_str(row[3], with_alpha, only_alpha, opt_alpha, false));
    } else if (do_multiply) {
        m_append_str(buf, len, m_shader_item_to_str(row[0], with_alpha, only_alpha, opt_alpha, false));
        m_append_str(buf, len, " * ");
        m_append_str(buf, len, m_shader_item_to_str(row[2], with_alpha, only_alpha, opt_alpha, true));
    } else if (do_mix) {
        m_append_str(buf, len, "mix(");
        m_append_str(buf, len, m_shader_item_to_str(row[1], with_alpha, only_alpha, opt_alpha, false));
        m_append_str(buf, len, ", ");
        m_append_str(buf, len, m_shader_item_to_str(row[0], with_alpha, only_alpha, opt_alpha, false));
        m_append_str(buf, len, ", ");
        m_append_str(buf, len, m_shader_item_to_str(row[2], with_alpha, only_alpha, opt_alpha, true));
        m_append_str(buf, len, ")");
    } else {
        m_append_str(buf, len, "(");
        m_append_str(buf, len, m_shader_item_to_str(row[0], with_alpha, only_alpha, opt_alpha, false));
        m_append_str(buf, len, " - ");
        m_append_str(buf, len, m_shader_item_to_str(row[1], with_alpha, only_alpha, opt_alpha, false));
        m_append_str(buf, len, ") * ");
        m_append_str(buf, len, m_shader_item_to_str(row[2], with_alpha, only_alpha, opt_alpha, true));
        m_append_str(buf, len, " + ");
        m_append_str(buf, len, m_shader_item_to_str(row[3], with_alpha, only_alpha, opt_alpha, false));
    }
}

static void m_append_cycle(char *buf, size_t *len, uint8_t c[2][4], bool opt_alpha, bool halves_same) {
    if (!halves_same && opt_alpha) {
        m_append_str(buf, len, "float4(");
        m_append_formula_row(buf, len, c[0], false, false, true);
        m_append_str(buf, len, ", ");
        m_append_formula_row(buf, len, c[1], true, true, true);
        m_append_str(buf, len, ")");
    } else {
        m_append_formula_row(buf, len, c[0], opt_alpha, false, opt_alpha);
    }
}

/* Defined below (needs mtl_device, only valid after gfx_metal_init()); forward-declared here
 * because gfx_metal_build_pipeline needs it for rasterSampleCount and shader/pipeline building
 * lives ahead of the mipmap/aniso/MSAA sampler-state helpers in this file's layout. */
static uint32_t ge_metal_msaa_samples(void);

static id<MTLRenderPipelineState> gfx_metal_build_pipeline(uint64_t shader_id, struct ShaderProgram *prg) {
    uint8_t c[2][4], c2[2][4];
    for (int i = 0; i < 4; i++) {
        c[0][i] = (shader_id >> (i * 3)) & 7;
        c[1][i] = (shader_id >> (12 + i * 3)) & 7;
        c2[0][i] = (shader_id >> (CC_C2_RGB_SHIFT + i * CC_C2_SLOT_BITS)) & CC_C2_SLOT_MASK;
        c2[1][i] = (shader_id >> (CC_C2_ALPHA_SHIFT + i * CC_C2_SLOT_BITS)) & CC_C2_SLOT_MASK;
    }
    bool two_cycle = (shader_id & SHADER_OPT_2CYC) != 0;
    bool opt_alpha = (shader_id & SHADER_OPT_ALPHA) != 0;
    bool opt_fog = (shader_id & SHADER_OPT_FOG) != 0;
    bool opt_texture_edge = (shader_id & SHADER_OPT_TEXTURE_EDGE) != 0;
    bool opt_noise = (shader_id & SHADER_OPT_NOISE) != 0;

    bool used_textures[2] = { false, false };
    int num_inputs = 0;
    for (int pass = 0; pass < (two_cycle ? 2 : 1); pass++) {
        uint8_t (*src)[4] = pass ? c2 : c;
        for (int i = 0; i < 2; i++) {
            for (int j = 0; j < 4; j++) {
                if (src[i][j] >= SHADER_INPUT_1 && src[i][j] <= SHADER_INPUT_4 && src[i][j] > num_inputs)
                    num_inputs = src[i][j];
                if (src[i][j] == SHADER_TEXEL0 || src[i][j] == SHADER_TEXEL0A) used_textures[0] = true;
                if (src[i][j] == SHADER_TEXEL1) used_textures[1] = true;
            }
        }
    }
    bool color_alpha_same = (shader_id & 0xfff) == ((shader_id >> 12) & 0xfff);
    bool color_alpha_same2 = ((shader_id >> CC_C2_RGB_SHIFT) & 0xffff) == ((shader_id >> CC_C2_ALPHA_SHIFT) & 0xffff);

    // ---- Vertex layout: position(4), [texCoord(2)], [fog(4)], input1..N(3 or 4) --
    // same order gfx_pc.c always produces (mirrors gfx_opengl.c's attrib push order).
    size_t num_floats = 4;
    int attr = 0;
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    vd.attributes[attr].format = MTLVertexFormatFloat4;
    vd.attributes[attr].bufferIndex = 0;
    vd.attributes[attr].offset = 0;
    attr++;
    size_t off_tex = 0, off_fog = 0, off_viewpos = 0;
    if (used_textures[0] || used_textures[1]) {
        off_tex = num_floats * sizeof(float);
        vd.attributes[attr].format = MTLVertexFormatFloat2;
        vd.attributes[attr].bufferIndex = 0;
        vd.attributes[attr].offset = off_tex;
        attr++;
        num_floats += 2;
    }
    /* Parallax view-space position -- see the field comment on LoadedVertex::vpx/vpy/vpz
     * in gfx_pc.c. Gated on tex0 specifically (not tex0||tex1), matching exactly how
     * gfx_pc.c packs it into the VBO: right after texcoord, before fog, only when tex0
     * is in use, since a height-map override always pairs with the diffuse (tile 0)
     * override. Ported from gfx_opengl.c's identical aViewPos/vViewPos addition. */
    if (used_textures[0]) {
        off_viewpos = num_floats * sizeof(float);
        vd.attributes[attr].format = MTLVertexFormatFloat3;
        vd.attributes[attr].bufferIndex = 0;
        vd.attributes[attr].offset = off_viewpos;
        attr++;
        num_floats += 3;
    }
    if (opt_fog) {
        off_fog = num_floats * sizeof(float);
        vd.attributes[attr].format = MTLVertexFormatFloat4;
        vd.attributes[attr].bufferIndex = 0;
        vd.attributes[attr].offset = off_fog;
        attr++;
        num_floats += 4;
    }
    size_t input_offsets[4] = {0, 0, 0, 0};
    for (int i = 0; i < num_inputs; i++) {
        input_offsets[i] = num_floats * sizeof(float);
        vd.attributes[attr].format = opt_alpha ? MTLVertexFormatFloat4 : MTLVertexFormatFloat3;
        vd.attributes[attr].bufferIndex = 0;
        vd.attributes[attr].offset = input_offsets[i];
        attr++;
        num_floats += opt_alpha ? 4 : 3;
    }
    vd.layouts[0].stride = num_floats * sizeof(float);
    vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;

    // ---- MSL source ----
    char vs[3072]; size_t vs_len = 0;
    char fs[10240]; size_t fs_len = 0;

    m_append_line(vs, &vs_len, "#include <metal_stdlib>");
    m_append_line(vs, &vs_len, "using namespace metal;");
    m_append_line(vs, &vs_len, "struct VtxIn {");
    { char l[128]; snprintf(l, sizeof l, "  float4 pos [[attribute(0)]];"); m_append_line(vs, &vs_len, l); }
    int a = 1;
    if (used_textures[0] || used_textures[1]) {
        char l[128]; snprintf(l, sizeof l, "  float2 texCoord [[attribute(%d)]];", a++); m_append_line(vs, &vs_len, l);
    }
    if (used_textures[0]) {
        char l[128]; snprintf(l, sizeof l, "  float3 viewPos [[attribute(%d)]];", a++); m_append_line(vs, &vs_len, l);
    }
    if (opt_fog) {
        char l[128]; snprintf(l, sizeof l, "  float4 fog [[attribute(%d)]];", a++); m_append_line(vs, &vs_len, l);
    }
    for (int i = 0; i < num_inputs; i++) {
        char l[128];
        snprintf(l, sizeof l, "  %s input%d [[attribute(%d)]];", opt_alpha ? "float4" : "float3", i + 1, a++);
        m_append_line(vs, &vs_len, l);
    }
    m_append_line(vs, &vs_len, "};");
    m_append_line(vs, &vs_len, "struct V2F {");
    m_append_line(vs, &vs_len, "  float4 pos [[position]];");
    if (used_textures[0] || used_textures[1]) m_append_line(vs, &vs_len, "  float2 texCoord;");
    if (used_textures[0]) m_append_line(vs, &vs_len, "  float3 viewPos;");
    if (opt_fog) m_append_line(vs, &vs_len, "  float4 fog;");
    for (int i = 0; i < num_inputs; i++) {
        char l[64]; snprintf(l, sizeof l, "  %s input%d;", opt_alpha ? "float4" : "float3", i + 1);
        m_append_line(vs, &vs_len, l);
    }
    m_append_line(vs, &vs_len, "};");
    m_append_line(vs, &vs_len, "vertex V2F vertexShader(VtxIn in [[stage_in]]) {");
    m_append_line(vs, &vs_len, "  V2F out;");
    m_append_line(vs, &vs_len, "  out.pos = in.pos;");
    if (used_textures[0] || used_textures[1]) m_append_line(vs, &vs_len, "  out.texCoord = in.texCoord;");
    if (used_textures[0]) m_append_line(vs, &vs_len, "  out.viewPos = in.viewPos;");
    if (opt_fog) m_append_line(vs, &vs_len, "  out.fog = in.fog;");
    for (int i = 0; i < num_inputs; i++) {
        char l[64]; snprintf(l, sizeof l, "  out.input%d = in.input%d;", i + 1, i + 1);
        m_append_line(vs, &vs_len, l);
    }
    m_append_line(vs, &vs_len, "  return out;");
    m_append_line(vs, &vs_len, "}");
    vs[vs_len] = '\0';

    m_append_line(fs, &fs_len, "struct FrameUniforms { int frameCount; };");
    m_append_line(fs, &fs_len, "struct DrawUniforms { float2 tex0Size; float2 tex1Size; int hasHeight; };");
    if (used_textures[0] && used_textures[1]) {
        m_append_line(fs, &fs_len,
            "float4 sampleTex(texture2d<float> t, sampler s, float2 uv) { return t.sample(s, uv); }");
    }
    m_append_line(fs, &fs_len, "fragment float4 fragmentShader(");
    m_append_line(fs, &fs_len, "  V2F in [[stage_in]],");
    m_append_line(fs, &fs_len, "  constant FrameUniforms &uFrame [[buffer(0)]],");
    m_append_str(fs, &fs_len, "  constant DrawUniforms &uDraw [[buffer(1)]]");
    if (used_textures[0]) m_append_str(fs, &fs_len, "\n  , texture2d<float> uTex0 [[texture(0)]], sampler uSamp0 [[sampler(0)]]");
    if (used_textures[1]) m_append_str(fs, &fs_len, "\n  , texture2d<float> uTex1 [[texture(1)]], sampler uSamp1 [[sampler(1)]]");
    /* Parallax height channel -- see the long comment on struct MetalTexture's has_height
     * field and gfx_pc.c's ge_texpack_try_override for where uDraw.hasHeight actually gets
     * set true. Present in every shader that uses tex0 so the two cannot fall out of sync;
     * inert (texCoord0 == in.texCoord) whenever hasHeight is 0, which today is always,
     * since no texture pack ships a height companion yet. Ported from gfx_opengl.c's
     * identical uTexHeight/uHasHeight addition -- see its comments for the full reasoning,
     * including why vViewPos.xy stands in for a true tangent-space view direction. */
    if (used_textures[0]) m_append_str(fs, &fs_len, "\n  , texture2d<float> uTexHeight [[texture(2)]], sampler uSampHeight [[sampler(2)]]");
    m_append_line(fs, &fs_len, ") {");
    {
        const char *e = getenv("GETV_DEBUGCOLOR");
        if (e && *e == '7') {
            /* Absolute earliest possible exit -- before any texture sample, combiner term, or
             * discard_fragment() runs. If this does not visibly cover a shader's draws, nothing
             * downstream in this function is responsible: the fragment either never gets
             * generated by the rasterizer for that shader (culling/scissor/viewport), or its
             * pipeline/encoder state is wrong before this code ever runs. */
            m_append_line(fs, &fs_len, "  return float4(1.0, 0.6, 0.0, 1.0);");
            m_append_line(fs, &fs_len, "}");
            fs[fs_len] = '\0';
            goto ge_metal_fs_early_exit;
        }
    }

    if (used_textures[0]) {
        /* heightScale is deliberately small and fixed, not a uniform -- see gfx_opengl.c's
         * identical constant for why (this is a seam, not a tuned effect, with no real
         * height content anywhere yet to tune it against). One height sample, not a ray
         * march -- tier 1 (parallax texture mapping), not tier 2 (occlusion mapping). The
         * 0.5 subtraction reads the height texture's mid-grey as "no displacement", matching
         * gfx_metal_ensure_height_placeholder()'s own fill below. */
        m_append_line(fs, &fs_len, "  float2 texCoord0 = in.texCoord;");
        m_append_line(fs, &fs_len, "  if (uDraw.hasHeight != 0) {");
        m_append_line(fs, &fs_len, "    float3 parallaxViewDir = normalize(-in.viewPos);");
        m_append_line(fs, &fs_len, "    float parallaxHeight = uTexHeight.sample(uSampHeight, in.texCoord).r - 0.5;");
        m_append_line(fs, &fs_len, "    texCoord0 = in.texCoord + parallaxViewDir.xy * (parallaxHeight * 0.04);");
        m_append_line(fs, &fs_len, "  }");
        m_append_line(fs, &fs_len, "  float4 texVal0 = uTex0.sample(uSamp0, texCoord0);");
    }
    if (used_textures[1]) {
        /* Same TEXEL1 rescale as gfx_opengl.c: one shared UV, normalised by TEXEL0's
         * dimensions; TEXEL1 (often a different LOD/size) rescales by the size ratio. */
        if (used_textures[0])
            m_append_line(fs, &fs_len, "  float4 texVal1 = uTex1.sample(uSamp1, in.texCoord * (uDraw.tex0Size / uDraw.tex1Size));");
        else
            m_append_line(fs, &fs_len, "  float4 texVal1 = uTex1.sample(uSamp1, in.texCoord);");
    }

    m_append_str(fs, &fs_len, opt_alpha ? "  float4 texel = " : "  float3 texel = ");
    m_append_cycle(fs, &fs_len, c, opt_alpha, color_alpha_same);
    m_append_line(fs, &fs_len, ";");
    if (two_cycle) {
        m_append_str(fs, &fs_len, "  texel = ");
        m_append_cycle(fs, &fs_len, c2, opt_alpha, color_alpha_same2);
        m_append_line(fs, &fs_len, ";");
    }
    if (opt_texture_edge && opt_alpha) {
        m_append_line(fs, &fs_len, "  if (texel.w > 0.3) texel.w = 1.0; else discard_fragment();");
    }
    if (opt_fog) {
        if (opt_alpha)
            m_append_line(fs, &fs_len, "  texel = float4(mix(texel.xyz, in.fog.xyz, in.fog.w), texel.w);");
        else
            m_append_line(fs, &fs_len, "  texel = mix(texel, in.fog.xyz, in.fog.w);");
    }
    if (opt_alpha && opt_noise) {
        m_append_line(fs, &fs_len, "  float3 __rv = float3(in.pos.xy, float(uFrame.frameCount));");
        m_append_line(fs, &fs_len, "  float __r = fract(sin(dot(sin(__rv), float3(12.9898, 78.233, 37.719))) * 143758.5453);");
        m_append_line(fs, &fs_len, "  texel.w *= floor(__r + 0.5);");
    }
    {
        const char *e = getenv("GETV_DEBUGCOLOR");
        if (e && *e == '2' && (used_textures[0] || used_textures[1])) {
            m_append_line(fs, &fs_len, "  return float4(in.texCoord.x, in.texCoord.y, 0.0, 1.0);");
        } else if (e && *e == '3' && used_textures[0]) {
            m_append_line(fs, &fs_len, "  return texVal0;");
        } else if (e && *e == '4' && num_inputs >= 1) {
            m_append_line(fs, &fs_len, opt_alpha ? "  return in.input1;" : "  return float4(in.input1, 1.0);");
        } else if (e && *e == '6') {
            char l[128];
            snprintf(l, sizeof l, "  return float4(%g, %g, %g, 1.0);",
                     (double) (((shader_id >> 0) & 0xff) / 255.0),
                     (double) (((shader_id >> 8) & 0xff) / 255.0),
                     (double) (((shader_id >> 16) & 0xff) / 255.0));
            m_append_line(fs, &fs_len, l);
        } else if (e && *e == '1') {
            m_append_line(fs, &fs_len, "  return float4(1.0, 0.0, 1.0, 1.0);");
        } else if (opt_alpha) {
            m_append_line(fs, &fs_len, "  return texel;");
        } else {
            m_append_line(fs, &fs_len, "  return float4(texel, 1.0);");
        }
    }
    m_append_line(fs, &fs_len, "}");
    fs[fs_len] = '\0';

ge_metal_fs_early_exit:
    NSMutableString *src = [NSMutableString stringWithUTF8String:vs];
    [src appendString:@"\n"];
    [src appendString:[NSString stringWithUTF8String:fs]];

    NSError *err = nil;
    id<MTLLibrary> lib = [mtl_device newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        fprintf(stderr, "[getv][metal] shader compile failed:\n%s\n", err ? err.localizedDescription.UTF8String : "(no error)");
        fprintf(stderr, "---- source ----\n%s\n-----------------\n", src.UTF8String);
        sys_fatal("metal shader compilation failed (see terminal)");
    }
    id<MTLFunction> vfn = [lib newFunctionWithName:@"vertexShader"];
    id<MTLFunction> ffn = [lib newFunctionWithName:@"fragmentShader"];

    MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
    pd.vertexFunction = vfn;
    pd.fragmentFunction = ffn;
    pd.vertexDescriptor = vd;
    pd.colorAttachments[0].pixelFormat = mtl_layer.pixelFormat;
    pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    if (opt_alpha) {
        pd.colorAttachments[0].blendingEnabled = YES;
        pd.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pd.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
        pd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
        pd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;
        pd.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
    } else {
        pd.colorAttachments[0].blendingEnabled = NO;
    }

    id<MTLRenderPipelineState> pipeline = [mtl_device newRenderPipelineStateWithDescriptor:pd error:&err];
    if (!pipeline) {
        sys_fatal("metal pipeline state creation failed: %s", err ? err.localizedDescription.UTF8String : "?");
    }

    prg->shader_id = shader_id;
    prg->pipeline = pipeline;
    prg->num_inputs = (uint8_t)num_inputs;
    prg->used_textures[0] = used_textures[0];
    prg->used_textures[1] = used_textures[1];
    prg->num_floats = (uint8_t)num_floats;
    prg->opt_alpha = opt_alpha;
    prg->used_noise = opt_alpha && opt_noise;
    return pipeline;
}

// -------------------------------------------------------------------------- GfxRenderingAPI

static void gfx_metal_unload_shader(struct ShaderProgram *old_prg) {
    if (cur_prg == old_prg) cur_prg = NULL;
}

static void gfx_metal_load_shader(struct ShaderProgram *new_prg) {
    cur_prg = new_prg;
}

static struct ShaderProgram *gfx_metal_create_and_load_new_shader(uint64_t shader_id) {
    if (shader_program_pool_size >= SHADER_PROGRAM_POOL_SIZE)
        sys_fatal("metal shader program pool exhausted (%d)", SHADER_PROGRAM_POOL_SIZE);
    struct ShaderProgram *prg = &shader_program_pool[shader_program_pool_size++];
    @autoreleasepool {
        gfx_metal_build_pipeline(shader_id, prg);
    }
    gfx_metal_load_shader(prg);
    return prg;
}

static struct ShaderProgram *gfx_metal_lookup_shader(uint64_t shader_id) {
    for (size_t i = 0; i < shader_program_pool_size; i++)
        if (shader_program_pool[i].shader_id == shader_id) return &shader_program_pool[i];
    return NULL;
}

static void gfx_metal_shader_get_info(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]) {
    *num_inputs = prg->num_inputs;
    used_textures[0] = prg->used_textures[0];
    used_textures[1] = prg->used_textures[1];
}

static uint32_t gfx_metal_new_texture(void) {
    if (num_textures >= tex_cache_size) {
        tex_cache_size += TEX_CACHE_STEP;
        tex_cache = (struct MetalTexture *)realloc(tex_cache, sizeof(struct MetalTexture) * tex_cache_size);
        if (!tex_cache) sys_fatal("out of memory allocating metal texture cache");
        metal_tex[0] = NULL;
        metal_tex[1] = NULL;
    }
    memset(&tex_cache[num_textures], 0, sizeof(struct MetalTexture));
    return num_textures++;
}

static void gfx_metal_select_texture(int tile, uint32_t texture_id) {
    metal_tex[tile] = &tex_cache[texture_id];
    metal_curtex = tile;
}

/* GETV_MIPMAPS=1 -- trilinear filtering, off by default. Same gate, same scope, same
 * width>1&&height>1 guard as gfx_opengl.c's ge_mipmap_enabled()/gfx_opengl_upload_texture:
 * only the diffuse path mipmaps (gfx_metal_upload_height_texture/
 * gfx_metal_ensure_height_placeholder below are deliberately untouched, matching GL, which
 * never mipmaps its own height texture either). Resolved once; see that file's own comment
 * for the full reasoning (GETV_MIPMAPS was previously set by three layers and read by
 * none). */
static bool ge_metal_mipmap_enabled(void) {
    static int resolved = -1;
    if (resolved < 0) {
        const char *e = getenv("GETV_MIPMAPS");
        resolved = (e && *e == '1') ? 1 : 0;
    }
    return resolved != 0;
}

/* GETV_ANISO=<n> -- anisotropic filtering, off by default. Unlike gfx_opengl.c's
 * ge_aniso_max(), no driver capability query is needed: MTLSamplerDescriptor.maxAnisotropy
 * self-clamps to its documented valid range (1...16), so asking for more than the hardware
 * supports is not the GL-style error that made a query necessary there. */
static uint32_t ge_metal_aniso_max(void) {
    static long resolved = -1;
    if (resolved < 0) {
        const char *e = getenv("GETV_ANISO");
        long want = (e && *e) ? strtol(e, NULL, 10) : 0;
        resolved = (want > 1) ? want : 0;
    }
    return (uint32_t) resolved;
}

/* GETV_MSAA=<0-8> -- requested sample count, 1 (off) by default. Unlike gfx_opengl.c's SDL
 * attribute request (gfx_sdl2.c, #ifndef RAPI_METAL -- GETV_MSAA is a no-op there under this
 * renderer), Metal has no "ask the windowing system and see what you actually got back": the
 * device is queried directly, descending from the requested count, since not every GPU
 * supports every count a user might type in (some skip 8; none skip 1). Resolved once --
 * mtl_device's capabilities cannot change at runtime, and this is only ever called after
 * gfx_metal_init() has set mtl_device. The result also has to be threaded into every game
 * combiner pipeline's rasterSampleCount (gfx_metal_build_pipeline below): Metal requires a
 * pipeline's sample count to match whatever render pass it draws into exactly, and unlike
 * mipmaps/aniso this is not purely a sampler-state concern. */
static uint32_t ge_metal_msaa_samples(void) {
    static long resolved = -1;
    if (resolved < 0) {
        const char *e = getenv("GETV_MSAA");
        long want = (e && *e) ? strtol(e, NULL, 10) : 0;
        if (want < 0) want = 0;
        if (want > 8) want = 8;
        resolved = 1;
        for (long n = want; n > 1; n--) {
            if ([mtl_device supportsTextureSampleCount:(NSUInteger)n]) { resolved = n; break; }
        }
    }
    return (uint32_t) resolved;
}

static void gfx_metal_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    struct MetalTexture *t = metal_tex[metal_curtex];
    bool want_mips = ge_metal_mipmap_enabled() && width > 1 && height > 1;
    @autoreleasepool {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:width height:height mipmapped:want_mips];
        desc.usage = MTLTextureUsageShaderRead;
        t->tex = [mtl_device newTextureWithDescriptor:desc];
        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [t->tex replaceRegion:region mipmapLevel:0 withBytes:rgba32_buf bytesPerRow:(NSUInteger)width * 4];

        if (want_mips) {
            /* A fresh command buffer from mtl_queue, not the current frame's mtl_cmdbuf --
             * this upload can happen before any frame has started (asset precache) or
             * mid-frame at a texture-cache miss inside draw-list traversal, so there is no
             * guarantee mtl_cmdbuf/mtl_encoder are in a usable state here. Committed
             * immediately with no wait: Metal hazard-tracks a resource across command
             * buffers submitted to the same queue in commit order, so any later frame's
             * draw (submitted after this commit) is guaranteed to see the finished mips --
             * no CPU stall needed to make that true. */
            id<MTLCommandBuffer> mipcmd = [mtl_queue commandBuffer];
            id<MTLBlitCommandEncoder> blit = [mipcmd blitCommandEncoder];
            [blit generateMipmapsForTexture:t->tex];
            [blit endEncoding];
            [mipcmd commit];
        }
    }
    t->size[0] = (float)width;
    t->size[1] = (float)height;
}

static MTLSamplerAddressMode gfx_cm_to_metal(uint32_t val) {
    if (val & G_TX_CLAMP) return MTLSamplerAddressModeClampToEdge;
    return (val & G_TX_MIRROR) ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeRepeat;
}

static void gfx_metal_set_sampler_parameters(int tile, bool linear_filter, uint32_t cms, uint32_t cmt) {
    struct MetalTexture *t = metal_tex[tile];
    t->linear_filter = linear_filter;
    @autoreleasepool {
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = linear_filter ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.magFilter = sd.minFilter;
        /* Mipmapping only ever applies to minification -- MTLSamplerMinMagFilter has no
         * mip-level concept at all, magFilter above always samples the base level. Linear
         * mip blend unconditionally, independent of linear_filter, matching GL's own
         * asymmetric choice (GL_NEAREST_MIPMAP_LINEAR vs GL_LINEAR_MIPMAP_LINEAR): GL always
         * blends BETWEEN mip levels linearly, only the WITHIN-level sample varies
         * nearest/linear. Getting this backwards would silently diverge from the reference
         * look this port is matched against. */
        if (ge_metal_mipmap_enabled()) {
            sd.mipFilter = MTLSamplerMipFilterLinear;
        }
        /* Deliberately excluded when the game asked for point sampling (matching GL's own
         * `aniso > 0.0f && linear_filter` gate) -- GoldenEye chooses nearest for the HUD,
         * watch faces and text, pixel art that anisotropy would only blur. */
        uint32_t aniso = ge_metal_aniso_max();
        if (aniso > 1 && linear_filter) {
            sd.maxAnisotropy = aniso;
        }
        sd.sAddressMode = gfx_cm_to_metal(cms);
        sd.tAddressMode = gfx_cm_to_metal(cmt);
        t->sampler = [mtl_device newSamplerStateWithDescriptor:sd];
    }
}

/* Called once, from gfx_metal_init(), so the placeholder is always bound and legal to
 * sample from the first frame on -- every shader with used_textures[0] compiles in the
 * height-sampling code (gated at runtime by uDraw.hasHeight, not by a shader variant), so
 * uTexHeight must have something valid in it even on the frame before any real override
 * could possibly have loaded one. Ported from gfx_opengl.c's gfx_opengl_ensure_height_tex. */
static void gfx_metal_ensure_height_placeholder(void) {
    if (mtl_height_placeholder) return;
    @autoreleasepool {
        static const uint8_t neutral[4] = { 128, 128, 128, 255 };
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:1 height:1 mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        mtl_height_placeholder = [mtl_device newTextureWithDescriptor:desc];
        MTLRegion region = MTLRegionMake2D(0, 0, 1, 1);
        [mtl_height_placeholder replaceRegion:region mipmapLevel:0 withBytes:neutral bytesPerRow:4];

        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        mtl_height_sampler = [mtl_device newSamplerStateWithDescriptor:sd];
    }
}

/* Stored on metal_tex[metal_curtex] -- the SAME slot gfx_metal_upload_texture (the diffuse
 * upload right before this call, in ge_texpack_try_override, gfx_pc.c) just wrote to --
 * rather than a single shared "current" texture. See MetalTexture::has_height's own comment
 * for why per-slot storage is required, not optional. Ported from gfx_opengl.c's
 * gfx_opengl_upload_height_texture. */
static void gfx_metal_upload_height_texture(const uint8_t *rgba32_buf, int width, int height) {
    struct MetalTexture *t = metal_tex[metal_curtex];
    @autoreleasepool {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:width height:height mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        t->height_tex = [mtl_device newTextureWithDescriptor:desc];
        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [t->height_tex replaceRegion:region mipmapLevel:0 withBytes:rgba32_buf bytesPerRow:(NSUInteger)width * 4];
    }
    t->has_height = true;
}

/* Marks this slot as having no height companion. Does not release height_tex -- see
 * gfx_opengl.c's identical gfx_opengl_clear_height_texture for why (packs are static files,
 * not something that changes mid-session, so there is no case where a slot legitimately
 * loses a height companion it once had). */
static void gfx_metal_clear_height_texture(void) {
    metal_tex[metal_curtex]->has_height = false;
}

static void gfx_metal_apply_depth_state(void) {
    if (!mtl_encoder) return;
    static int force_no_depth = -1;
    if (force_no_depth < 0) { const char *e = getenv("GETV_NODEPTH"); force_no_depth = (e && *e == '1'); }
    if (force_no_depth) { [mtl_encoder setDepthStencilState:mtl_depth_states[0][0][0]]; return; }
    [mtl_encoder setDepthStencilState:mtl_depth_states[cur_depth_test][cur_depth_mask][cur_zmode_decal]];
    if (cur_zmode_decal) {
        [mtl_encoder setDepthBias:-2.0f slopeScale:-2.0f clamp:0.0f];
    } else {
        [mtl_encoder setDepthBias:0.0f slopeScale:0.0f clamp:0.0f];
    }
}

static void gfx_metal_set_depth_test(bool depth_test) {
    cur_depth_test = depth_test;
    gfx_metal_apply_depth_state();
}
static void gfx_metal_set_depth_mask(bool z_upd) {
    cur_depth_mask = z_upd;
    gfx_metal_apply_depth_state();
}
static void gfx_metal_set_zmode_decal(bool zmode_decal) {
    cur_zmode_decal = zmode_decal;
    gfx_metal_apply_depth_state();
}

static void gfx_metal_set_viewport(int x, int y, int width, int height) {
    if (!mtl_encoder) return;
    MTLViewport vp = { (double)x, (double)y, (double)width, (double)height, 0.0, 1.0 };
    [mtl_encoder setViewport:vp];
}

static void gfx_metal_set_scissor(int x, int y, int width, int height) {
    if (!mtl_encoder) return;
    NSUInteger dw = (NSUInteger)mtl_render_target_w, dh = (NSUInteger)mtl_render_target_h;
    NSUInteger sx = (NSUInteger)MAX(0, MIN(x, (int)dw));
    NSUInteger sy = (NSUInteger)MAX(0, MIN(y, (int)dh));
    NSUInteger sw = (NSUInteger)MAX(0, MIN(width, (int)dw - (int)sx));
    NSUInteger sh = (NSUInteger)MAX(0, MIN(height, (int)dh - (int)sy));
    MTLScissorRect sr = { sx, sy, sw, sh };
    [mtl_encoder setScissorRect:sr];
}

static void gfx_metal_set_use_alpha(bool use_alpha) {
    // Baked into the pipeline state at shader-build time (blendingEnabled), same as LUS.
    (void)use_alpha;
}

static void gfx_metal_draw_triangles(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris) {
    if (!mtl_encoder || !cur_prg) return;
    size_t bytes = sizeof(float) * buf_vbo_len;
    if (mtl_vbo_offset + bytes > VBO_POOL_BYTES) {
        sys_fatal("metal vertex buffer pool exhausted this frame (%zu > %d)", mtl_vbo_offset + bytes, VBO_POOL_BYTES);
    }
    id<MTLBuffer> vbo = mtl_vbo_pool[mtl_vbo_index];
    memcpy((uint8_t *)vbo.contents + mtl_vbo_offset, buf_vbo, bytes);

    [mtl_encoder setRenderPipelineState:cur_prg->pipeline];
    [mtl_encoder setVertexBuffer:vbo offset:mtl_vbo_offset atIndex:0];

    struct FrameUniforms fu = { (int32_t)frame_count };
    [mtl_encoder setFragmentBytes:&fu length:sizeof fu atIndex:0];

    struct DrawUniforms du;
    du.tex0_size[0] = metal_tex[0] ? metal_tex[0]->size[0] : 1.0f;
    du.tex0_size[1] = metal_tex[0] ? metal_tex[0]->size[1] : 1.0f;
    du.tex1_size[0] = metal_tex[1] ? metal_tex[1]->size[0] : 1.0f;
    du.tex1_size[1] = metal_tex[1] ? metal_tex[1]->size[1] : 1.0f;
    /* Parallax: whether TILE 0's currently-bound texture has a real height companion --
     * see MetalTexture::has_height's own comment for why this is read per-slot, per draw,
     * rather than cached anywhere. */
    du.has_height = (metal_tex[0] && metal_tex[0]->has_height) ? 1 : 0;
    [mtl_encoder setFragmentBytes:&du length:sizeof du atIndex:1];

    if (cur_prg->used_textures[0] && metal_tex[0]) {
        [mtl_encoder setFragmentTexture:metal_tex[0]->tex atIndex:0];
        [mtl_encoder setFragmentSamplerState:metal_tex[0]->sampler atIndex:0];
        /* uTexHeight/uSampHeight -- always bound to SOMETHING legal to sample (the real
         * height texture when this slot has one, the neutral placeholder otherwise), same
         * reasoning as gfx_opengl.c's GL_TEXTURE2 binding in gfx_opengl_set_texture_uniforms. */
        bool has_height = metal_tex[0]->has_height;
        [mtl_encoder setFragmentTexture:(has_height ? metal_tex[0]->height_tex : mtl_height_placeholder) atIndex:2];
        [mtl_encoder setFragmentSamplerState:mtl_height_sampler atIndex:2];
    }
    if (cur_prg->used_textures[1] && metal_tex[1]) {
        [mtl_encoder setFragmentTexture:metal_tex[1]->tex atIndex:1];
        [mtl_encoder setFragmentSamplerState:metal_tex[1]->sampler atIndex:1];
    }

    [mtl_encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:buf_vbo_num_tris * 3];
    mtl_vbo_offset += bytes;
}

static void gfx_metal_build_depth_states(void) {
    for (int test = 0; test < 2; test++) {
        for (int mask = 0; mask < 2; mask++) {
            for (int decal = 0; decal < 2; decal++) {
                MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new];
                dd.depthWriteEnabled = mask ? YES : NO;
                dd.depthCompareFunction = !test ? MTLCompareFunctionAlways
                                                 : (decal ? MTLCompareFunctionLessEqual : MTLCompareFunctionLess);
                mtl_depth_states[test][mask][decal] = [mtl_device newDepthStencilStateWithDescriptor:dd];
            }
        }
    }
}

/* Whether THIS frame's game draws should land in the offscreen mtl_pp_color/mtl_pp_depth
 * pair instead of the drawable directly. Supersample>1 only for now (increment 3) --
 * increments 4/5 (MSAA, FXAA) extend this same gate, since both also need the offscreen
 * path even when supersample itself is 1 (unlike gfx_opengl.c's desktop GE_POSTFX gate,
 * which deliberately excludes MSAA because GL can multisample the default framebuffer
 * directly; CAMetalLayer's drawable has no equivalent, so Metal MSAA has no path that
 * avoids this offscreen target -- see gfx_metal_ensure_offscreen_targets' own MSAA
 * extension when it lands). */
static bool ge_metal_postfx_active(void) {
    return gfx_supersample > 1;
}

/* One shared offscreen colour+depth pair, sized to the INFLATED (w,h) -- gfx_current_dimensions
 * once GETV_SUPERSAMPLE has activated gfx_pc.c's existing dimension inflation (gfx_pc.c's
 * gfx_start_frame, unconditional, backend-agnostic), not re-derived from drawableSize*factor.
 * mtl_pp_color: MTLStorageModePrivate + ShaderRead, since the composite pass samples it.
 * mtl_pp_depth: MTLStorageModeMemoryless -- nothing ever reads game depth back after this
 * pass ends, so it never needs to leave tile memory on this TBDR GPU. */
static void gfx_metal_ensure_offscreen_targets(uint32_t w, uint32_t h) {
    if (mtl_pp_color && mtl_pp_w == w && mtl_pp_h == h) return;
    @autoreleasepool {
        MTLTextureDescriptor *cd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:mtl_layer.pixelFormat
                                                                                        width:w height:h mipmapped:NO];
        cd.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        cd.storageMode = MTLStorageModePrivate;
        mtl_pp_color = [mtl_device newTextureWithDescriptor:cd];

        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                        width:w height:h mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget;
        dd.storageMode = MTLStorageModeMemoryless;
        mtl_pp_depth = [mtl_device newTextureWithDescriptor:dd];
    }
    mtl_pp_w = w; mtl_pp_h = h;
}

/* Full-screen-triangle composite pass: samples mtl_pp_color (the offscreen target the
 * game just drew into, post-MSAA-resolve if that's active) and writes the downsampled
 * result into the real drawable. Built once, lazily, the first time postfx activates --
 * unlike every per-combiner shader in gfx_metal_build_pipeline() above, which compiles a
 * new MTLLibrary per N64 shader_id, this is a SEPARATE, single pipeline with no vertex
 * buffer at all (vertex_id-driven), since none of the 128 possible combiner variants have
 * that shape -- every one of them assumes real per-vertex [[stage_in]] attributes from the
 * game's own VBO layout. FXAA (a fast follow) extends this same fragment shader rather than
 * adding a second pass, so it is written with that branch already in mind. */
static void gfx_metal_ensure_postfx_pipeline(void) {
    if (mtl_pp_pipeline) return;
    @autoreleasepool {
        NSString *src =
            @"#include <metal_stdlib>\n"
             "using namespace metal;\n"
             "struct PPVarying { float4 position [[position]]; float2 uv; };\n"
             "vertex PPVarying ppVertex(uint vid [[vertex_id]]) {\n"
             "    float2 uv = float2(float((vid << 1) & 2), float(vid & 2));\n"
             "    PPVarying out;\n"
             "    out.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
             "    out.uv = uv;\n"
             "    return out;\n"
             "}\n"
             "fragment float4 ppFragment(PPVarying in [[stage_in]],\n"
             "                           texture2d<float> tex [[texture(0)]],\n"
             "                           sampler samp [[sampler(0)]]) {\n"
             "    return tex.sample(samp, in.uv);\n"
             "}\n";
        NSError *err = nil;
        id<MTLLibrary> lib = [mtl_device newLibraryWithSource:src options:nil error:&err];
        if (!lib) {
            sys_fatal("gfx_metal: postfx shader compile failed: %s",
                      err.localizedDescription.UTF8String);
        }
        id<MTLFunction> vfn = [lib newFunctionWithName:@"ppVertex"];
        id<MTLFunction> ffn = [lib newFunctionWithName:@"ppFragment"];

        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vfn;
        pd.fragmentFunction = ffn;
        pd.colorAttachments[0].pixelFormat = mtl_layer.pixelFormat;

        NSError *perr = nil;
        mtl_pp_pipeline = [mtl_device newRenderPipelineStateWithDescriptor:pd error:&perr];
        if (!mtl_pp_pipeline) {
            sys_fatal("gfx_metal: postfx pipeline creation failed: %s",
                      perr.localizedDescription.UTF8String);
        }

        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = MTLSamplerMinMagFilterLinear;
        sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
        sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
        mtl_pp_sampler = [mtl_device newSamplerStateWithDescriptor:sd];
    }
}

static void gfx_metal_init(void) {
    if (!gePortMetalLayer) sys_fatal("gfx_metal_init: no CAMetalLayer -- gfx_sdl.init() must run first");
    mtl_layer = (__bridge CAMetalLayer *)gePortMetalLayer;
    mtl_device = mtl_layer.device ?: MTLCreateSystemDefaultDevice();
    mtl_layer.device = mtl_device;
    mtl_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    /* framebufferOnly=YES (the normal case) forbids getBytes/blit-copy from the drawable's
     * own texture -- Apple's documented restriction, not a bug to route around cleverly.
     * GETV_SHOTFRAME (see ge_shot_maybe_metal() below) needs to read it, so drop the
     * optimization only when a capture was actually asked for. */
    { const char *e = getenv("GETV_SHOTFRAME");
      mtl_layer.framebufferOnly = (e && *e) ? NO : YES; }

    /* GETV_SUPERSAMPLE=<1-4> -- off (1, gfx_pc.c's own compiled-in default) unless
     * explicitly requested, matching every other GETV_* enhancement gate in this file
     * (mipmaps/aniso/HD textures all opt-in, retail-safe by default). Deliberately NOT
     * gfx_opengl.c's TVOS_SUPERSAMPLE default-2 -- that is a tvOS-GL-specific workaround for
     * having only one fixed 1920x1080 mode with no other way to get a sharper image; RAPI_METAL
     * serves iOS too, which has no such constraint, so "97 Console" must mean supersample=1
     * here same as everywhere else. Setting gfx_supersample (gfx_pc.h) is the ONLY thing this
     * needs to do to activate gfx_pc.c's already-existing, backend-agnostic dimension inflation
     * (gfx_start_frame) -- see ge_metal_postfx_active()/gfx_metal_ensure_offscreen_targets
     * above for what Metal does with the result. */
    { const char *e = getenv("GETV_SUPERSAMPLE");
      if (e && *e) {
          int v = atoi(e);
          if (v >= 1 && v <= 4) gfx_supersample = (unsigned) v;
      } }

    mtl_queue = [mtl_device newCommandQueue];

    tex_cache_size = TEX_CACHE_STEP;
    tex_cache = (struct MetalTexture *)calloc(tex_cache_size, sizeof(struct MetalTexture));
    if (!tex_cache) sys_fatal("out of memory allocating metal texture cache");

    gfx_metal_ensure_height_placeholder();

    for (int i = 0; i < VBO_POOL_COUNT; i++)
        mtl_vbo_pool[i] = [mtl_device newBufferWithLength:VBO_POOL_BYTES options:MTLResourceStorageModeShared];

    gfx_metal_build_depth_states();

    printf("[getv][metal] device=%s\n", mtl_device.name.UTF8String);
    fflush(stdout);

#ifdef GE_WITH_IMGUI
    /* Not called from gfx_sdl2.c's gfx_sdl_init(), unlike the GL path -- gePortImguiInit()
     * needs mtl_device, which does not exist until this function has run this far, and
     * gfx_pc.c's gfx_init() always calls gfx_wapi->init() (where gfx_sdl_init() would have
     * called it) before gfx_rapi->init() (this function). Passing NULL for glctx: ImGui
     * doesn't use it here (see ge_imgui.cpp's RAPI_METAL branch, which calls
     * ImGui_ImplSDL2_InitForMetal(window) instead of ...InitForOpenGL(window, glctx)). */
    { extern void gePortImguiInit(void *window, void *glctx);
      gePortImguiInit(gePortMetalWindow, NULL); }
#endif
}

static void gfx_metal_on_resize(void) {
}

static void gfx_metal_ensure_depth_target(uint32_t w, uint32_t h) {
    if (mtl_depth_tex && mtl_depth_w == w && mtl_depth_h == h) return;
    @autoreleasepool {
        MTLTextureDescriptor *dd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                        width:w height:h mipmapped:NO];
        dd.usage = MTLTextureUsageRenderTarget;
        dd.storageMode = MTLStorageModePrivate;
        mtl_depth_tex = [mtl_device newTextureWithDescriptor:dd];
    }
    mtl_depth_w = w; mtl_depth_h = h;
}

/* Shared by both branches below -- the parts of starting the game's own render pass that
 * don't depend on which target it's aimed at. Kept as a macro-like inline block rather than
 * its own function so the depth-clip-mode comment (below) stays exactly once, not duplicated
 * per branch. */
static void gfx_metal_begin_game_pass(MTLRenderPassDescriptor *pass, uint32_t w, uint32_t h) {
    mtl_encoder = [mtl_cmdbuf renderCommandEncoderWithDescriptor:pass];
    /* Default MTLDepthClipMode is .clip: a triangle with ANY vertex outside the valid
     * [0,1] NDC z range (after the perspective divide) is discarded WHOLESALE by the
     * GPU, not clamped and kept -- unlike GL, which this renderer's CPU-side
     * GETV_NEARCLAMP (gfx_pc.c) was written against, and unlike the near-plane clamp
     * that N64 hardware's own RDP performs (see that code's own comment: "the RDP's
     * depth clamp produces [minimum depth]... The exact fix is GL_DEPTH_CLAMP /
     * ARB_depth_clamp / EXT_depth_clamp... That belongs in the rendering backend").
     * .clamp is that fix, at the encoder level, for every draw through this encoder at
     * once -- ported from kenix3/libultraship's gfx_metal.cpp (port-maintenance
     * branch), which sets this on every encoder it creates, for exactly this reason.
     * Matters most for large, camera-adjacent geometry whose vertices are likeliest to
     * straddle the near plane -- room walls and animated characters, not the small,
     * stable gun/HUD geometry that kept rendering without it. */
    [mtl_encoder setDepthClipMode:MTLDepthClipModeClamp];
    cur_depth_test = false; cur_depth_mask = true; cur_zmode_decal = false;
    gfx_metal_apply_depth_state();
    mtl_render_target_w = w; mtl_render_target_h = h;
    gfx_metal_set_viewport(0, 0, (int)w, (int)h);
    gfx_metal_set_scissor(0, 0, (int)w, (int)h);
}

static void gfx_metal_start_frame(void) {
    frame_count++;
    @autoreleasepool {
        CGSize sz = mtl_layer.drawableSize;
        if (sz.width < 1 || sz.height < 1) return;

        if (ge_metal_postfx_active()) {
            /* Inflated internal size -- gfx_pc.c's gfx_start_frame already computed this
             * before gfx_rapi->start_frame() runs (backend-agnostic, unconditional; see
             * GETV_SUPERSAMPLE's own comment in gfx_metal_init above). Falls back to native
             * size only if that inflation somehow produced nothing usable, so this can never
             * hand a zero-sized texture descriptor to Metal. */
            uint32_t iw = gfx_current_dimensions.width;
            uint32_t ih = gfx_current_dimensions.height;
            if (iw < 1 || ih < 1) { iw = (uint32_t)sz.width; ih = (uint32_t)sz.height; }
            gfx_metal_ensure_offscreen_targets(iw, ih);

            mtl_cmdbuf = [mtl_queue commandBuffer];

            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = mtl_pp_color;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.depthAttachment.texture = mtl_pp_depth;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.clearDepth = 1.0;
            pass.depthAttachment.storeAction = MTLStoreActionDontCare;

            gfx_metal_begin_game_pass(pass, iw, ih);
            /* mtl_drawable deliberately NOT acquired here -- deferred to
             * gfx_metal_end_frame()'s composite pass, the first point this frame that
             * actually needs it. Safe now that gfx_metal_set_scissor above reads
             * mtl_render_target_w/h instead of mtl_drawable.texture directly: nothing
             * mid-frame touches mtl_drawable on this path any more. A real frame-pacing
             * win (Apple's own guidance: acquire as late as possible so the drawable
             * spends less time held out of the display's presentation queue), free once
             * that read was removed rather than requiring its own justification. */
        } else {
            /* Fast path, byte-for-byte the pre-supersampling behaviour: draws straight into
             * the drawable, depth target sized to it directly. This is what makes "the
             * 97-Console default must not change at all" trivially true -- when
             * ge_metal_postfx_active() is false, every line below is identical to what ran
             * before increment 3 existed. */
            gfx_metal_ensure_depth_target((uint32_t)sz.width, (uint32_t)sz.height);

            mtl_drawable = [mtl_layer nextDrawable];
            if (!mtl_drawable) return;

            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = mtl_drawable.texture;
            pass.colorAttachments[0].loadAction = MTLLoadActionClear;
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            pass.depthAttachment.texture = mtl_depth_tex;
            pass.depthAttachment.loadAction = MTLLoadActionClear;
            pass.depthAttachment.clearDepth = 1.0;
            pass.depthAttachment.storeAction = MTLStoreActionDontCare;

            mtl_cmdbuf = [mtl_queue commandBuffer];
            gfx_metal_begin_game_pass(pass, (uint32_t)sz.width, (uint32_t)sz.height);
        }
    }
    mtl_vbo_offset = 0;
}

/* GETV_SHOTFRAME: deterministic frame capture, Metal side. Same env vars, same 24-bit BMP
 * layout, same log line as gfx_opengl.c's ge_shot_maybe() -- see that function's own
 * comment for why a wall-clock screenshot cannot substitute for this in an A/B. Kept as a
 * literal byte-for-byte port of the file format so nothing downstream (a diff tool, a
 * script) needs to know which renderer produced a given .bmp.
 *
 * Must run after [mtl_cmdbuf waitUntilCompleted] -- the GPU has to have actually finished
 * writing the drawable's texture before getBytes reads it back; unlike glReadPixels, which
 * blocks on the GL command stream itself, Metal's command buffer is asynchronous until
 * explicitly waited on. Reading AFTER presentDrawable:/commit is safe: presenting hands the
 * texture to the compositor for display, it does not invalidate CPU access to it, and
 * nothing else in this file mutates mtl_drawable.texture's contents between commit and the
 * mtl_drawable = nil a few lines below in gePortMetalFinishFrame(). */
static void ge_shot_maybe_metal(id<MTLCommandBuffer> cmdbuf, id<MTLTexture> tex) {
    static int shot_frame = -2;
    static unsigned long fno;
    static const char *shot_path;
    static char shot_path_buf[1024];
    if (shot_frame == -2) {
        const char *e = getenv("GETV_SHOTFRAME");
        shot_frame = (e && *e) ? atoi(e) : -1;
        shot_path = getenv("GETV_SHOTPATH");
        if (!shot_path || !*shot_path) {
            /* A bare relative name only works where the process's CWD is writable -- true
             * on desktop, false on tvOS/iOS, where the app bundle itself is read-only and
             * fopen() fails with EPERM (found the hard way: this failed completely
             * silently before the diagnostic a few lines below existed). TMPDIR is a
             * standard POSIX env var every process on the platform already has set, to
             * that app's own sandboxed container -- no per-install UUID to discover or
             * guess at from outside the process. */
            const char *tmp = getenv("TMPDIR");
            if (tmp && *tmp) {
                snprintf(shot_path_buf, sizeof shot_path_buf, "%s/getv_shot.bmp", tmp);
                shot_path = shot_path_buf;
            } else {
                shot_path = "getv_shot.bmp";
            }
        }
    }
    fno++;
    /* Cheap on every other frame: the counter above has to run unconditionally to know
     * which frame this is, but the GPU stall below is the one thing GETV_SHOTFRAME is
     * supposed to cost only on the single frame actually being captured. */
    if (shot_frame <= 0 || (long)fno != (long)shot_frame) return;
    if (!tex) return;

    /* mtl_cmdbuf was already committed by the caller (presentDrawable: schedules
     * presentation for when the GPU finishes, it does not itself block) -- wait for that
     * GPU work to land before getBytes, or this reads whatever was in the texture before
     * this frame's draws, not this frame. */
    [cmdbuf waitUntilCompleted];

    const int w = (int)tex.width;
    const int h = (int)tex.height;
    if (w <= 0 || h <= 0) return;

    /* BGRA8Unorm (mtl_layer.pixelFormat above), 4 bytes/px, tight rows -- getBytes wants
     * the real per-row stride, not a padded one, unlike the BMP output rows below. */
    unsigned char *px = (unsigned char *)malloc((size_t)w * h * 4);
    if (!px) return;
    [tex getBytes:px
      bytesPerRow:(NSUInteger)(w * 4)
       fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
      mipmapLevel:0];

    /* BMP rows are bottom-up and 4-byte aligned; getBytes hands back top-down BGRA, so both
     * the row order and the dropped alpha byte are handled in the write loop below (mirrors
     * gfx_opengl.c's ge_shot_maybe() exactly, source channel order and origin aside). */
    const int pad = (4 - (w * 3) % 4) % 4;
    const unsigned long imgsz = (unsigned long)(w * 3 + pad) * h;
    FILE *f = fopen(shot_path, "wb");
    if (!f) {
        /* Silent otherwise: a bad GETV_SHOTPATH (a sandboxed tvOS/iOS container's real
         * writable path is a per-install UUID, not something to hand-guess) previously
         * failed with no output at all, which reads identically to the capture never
         * having run in the first place. */
        fprintf(stderr, "[getv][shot] fopen failed for '%s': %s\n", shot_path, strerror(errno));
        fflush(stderr);
        free(px);
        return;
    }
    unsigned char hdr[54] = {0};
    unsigned long fsz = 54 + imgsz;
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (unsigned char)(fsz); hdr[3] = (unsigned char)(fsz >> 8);
    hdr[4] = (unsigned char)(fsz >> 16); hdr[5] = (unsigned char)(fsz >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (unsigned char)(w); hdr[19] = (unsigned char)(w >> 8);
    hdr[20] = (unsigned char)(w >> 16); hdr[21] = (unsigned char)(w >> 24);
    hdr[22] = (unsigned char)(h); hdr[23] = (unsigned char)(h >> 8);
    hdr[24] = (unsigned char)(h >> 16); hdr[25] = (unsigned char)(h >> 24);
    hdr[26] = 1; hdr[28] = 24;
    hdr[34] = (unsigned char)(imgsz); hdr[35] = (unsigned char)(imgsz >> 8);
    hdr[36] = (unsigned char)(imgsz >> 16); hdr[37] = (unsigned char)(imgsz >> 24);
    fwrite(hdr, 1, 54, f);
    static const unsigned char zero[3] = {0, 0, 0};
    /* getBytes is top-down; BMP wants bottom-up, so walk source rows in reverse. */
    for (int y = h - 1; y >= 0; y--) {
        const unsigned char *row = px + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            /* BGRA8Unorm in memory is already B,G,R,A -- the BMP's own pixel order is
             * B,G,R, so this is a straight copy of the first three bytes, alpha dropped. */
            fwrite(row + x * 4, 1, 3, f);
        }
        if (pad) fwrite(zero, 1, (size_t)pad, f);
    }
    fclose(f);
    free(px);
    fprintf(stderr, "[getv][shot] frame %lu -> %s (%dx%d)\n", fno, shot_path, w, h);
    fflush(stderr);
}

/* Ends the GAME's render encoder only -- does NOT present or commit. That split (and this
 * whole file being reachable from outside gfx_pc.c's GfxRenderingAPI table at all) exists
 * for one reason: ImGui. gfx_pc.c calls gfx_rapi->end_frame() and then
 * gfx_wapi->swap_buffers_begin() (gfx_sdl2.c), which is where the launcher/dev-overlay
 * draws and where GL's SDL_GL_SwapWindow() lives -- i.e. the overlay draws INTO THE SAME
 * FRAME, after the game but before it reaches the screen. Metal has no equivalent of "draw
 * more into an already-presented drawable": presentDrawable+commit is terminal. So end_frame
 * here only closes out the game's own encoder, the command buffer and drawable stay alive,
 * and gePortMetalFinishFrame() below -- called from gfx_sdl2.c in GL's SDL_GL_SwapWindow
 * slot, i.e. AFTER the overlay's own encoder (gePortMetalImguiBeginPass/EndPass) has run --
 * is what actually presents and commits. Without ImGui built in this still runs exactly the
 * same way; gePortMetalFinishFrame() is unconditional, not GE_WITH_IMGUI-gated, because
 * something has to present the frame either way. */
static void gfx_metal_end_frame(void) {
    if (!mtl_encoder) return;
    [mtl_encoder endEncoding];
    mtl_encoder = nil;

    if (ge_metal_postfx_active()) {
        /* The composite pass: acquire the drawable now (deferred from start_frame, see its
         * own comment), sample mtl_pp_color -- the game's just-finished offscreen frame --
         * and downsample it into the real output. mtl_drawable has to be set by the time
         * this function returns: gePortMetalImguiBeginPass() (gfx_sdl2.c, runs between here
         * and gePortMetalFinishFrame()) guards on `if (!mtl_cmdbuf || !mtl_drawable) return;`
         * and its own loadAction=Load depends on this pass having already written real
         * pixels for it to preserve. */
        @autoreleasepool {
            mtl_drawable = [mtl_layer nextDrawable];
            if (mtl_drawable) {
                gfx_metal_ensure_postfx_pipeline();

                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = mtl_drawable.texture;
                /* Fully overwritten by the full-screen triangle below -- unlike the ImGui
                 * overlay pass, which deliberately uses Load to preserve THIS pass's
                 * output, this pass has nothing to preserve from before it. */
                pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;

                id<MTLRenderCommandEncoder> ppenc = [mtl_cmdbuf renderCommandEncoderWithDescriptor:pass];
                [ppenc setRenderPipelineState:mtl_pp_pipeline];
                [ppenc setFragmentTexture:mtl_pp_color atIndex:0];
                [ppenc setFragmentSamplerState:mtl_pp_sampler atIndex:0];
                [ppenc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [ppenc endEncoding];
            }
        }
        mtl_render_target_w = (uint32_t)mtl_layer.drawableSize.width;
        mtl_render_target_h = (uint32_t)mtl_layer.drawableSize.height;
    }
}

void gePortMetalFinishFrame(void) {
    if (!mtl_cmdbuf) return;
    if (mtl_drawable) [mtl_cmdbuf presentDrawable:mtl_drawable];
    [mtl_cmdbuf commit];
    ge_shot_maybe_metal(mtl_cmdbuf, mtl_drawable.texture);
    mtl_cmdbuf = nil;
    mtl_drawable = nil;
    mtl_vbo_index = (mtl_vbo_index + 1) % VBO_POOL_COUNT;
}

#ifdef GE_WITH_IMGUI
int gePortMetalImguiInit(void) {
    if (!ImGui_ImplMetal_Init(mtl_device)) return 0;
    /* Build EVERY device object -- depth-stencil state and the font atlas texture -- NOW, via
     * CreateDeviceObjects(), not lazily inside the first ImGui_ImplMetal_NewFrame() call. Two
     * bugs, found on this exact code path (ge_launcher_metal.mm hit both first; see its much
     * longer comment for the full story):
     *   1. ImGui::NewFrame() asserts g.IO.Fonts->IsBuilt() before any renderer backend call
     *      runs at all, so the atlas must exist before the loop's first NewFrame() -- not
     *      merely before the first draw, which is where gePortMetalImguiBeginPass() runs.
     *   2. ImGui_ImplMetal_NewFrame() lazily calls CreateDeviceObjects() itself the first time
     *      (`if (depthStencilState == nil)`), and CreateDeviceObjects() unconditionally
     *      REBUILDS the font texture even if one already exists. Calling just
     *      CreateFontsTexture() here (the first, incomplete fix) left depthStencilState nil,
     *      so that lazy rebuild still fired on the first BeginPass() -- after ImGui::Render()
     *      had already recorded draw commands referencing the FIRST texture, which ARC then
     *      deallocated out from under them the moment the second one replaced it in the
     *      strong property. CreateDeviceObjects() sets depthStencilState too, so the lazy
     *      branch never fires. */
    return ImGui_ImplMetal_CreateDeviceObjects(mtl_device) ? 1 : 0;
}

/* A second render pass on the SAME drawable texture the game just drew into, loadAction=
 * Load so those pixels are kept rather than cleared. ImGui_ImplMetal_NewFrame only stashes
 * this pass's pixel format/sample count for pipeline-state matching and lazily creates
 * device objects (font texture) on first call -- it does not touch ImGuiIO/layout state, so
 * calling it here rather than at the conventional start-of-frame point (where we have no
 * pass yet -- gfx_wapi->start_frame() runs before gfx_rapi->start_frame(), see gfx_pc.c) is
 * safe. No depth attachment: ImGui doesn't test or write depth. */
void gePortMetalImguiBeginPass(void) {
    if (!mtl_cmdbuf || !mtl_drawable) return;
    @autoreleasepool {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = mtl_drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        ImGui_ImplMetal_NewFrame(pass);
        mtl_overlay_encoder = [mtl_cmdbuf renderCommandEncoderWithDescriptor:pass];
        /* Same reasoning as the game encoder's identical call above -- matching upstream
         * libultraship, which sets this on every encoder it creates, not just one. */
        [mtl_overlay_encoder setDepthClipMode:MTLDepthClipModeClamp];
    }
}

void gePortMetalImguiRenderDrawData(void *draw_data) {
    if (!mtl_overlay_encoder) return;
    ImGui_ImplMetal_RenderDrawData((ImDrawData *)draw_data, mtl_cmdbuf, mtl_overlay_encoder);
}

void gePortMetalImguiEndPass(void) {
    if (!mtl_overlay_encoder) return;
    [mtl_overlay_encoder endEncoding];
    mtl_overlay_encoder = nil;
}

void gePortMetalImguiShutdown(void) {
    ImGui_ImplMetal_Shutdown();
}
#endif /* GE_WITH_IMGUI */

static void gfx_metal_finish_render(void) {
}

static void gfx_metal_shutdown(void) {
}

struct GfxRenderingAPI gfx_metal_api = {
    gfx_metal_z_is_from_0_to_1,
    gfx_metal_unload_shader,
    gfx_metal_load_shader,
    gfx_metal_create_and_load_new_shader,
    gfx_metal_lookup_shader,
    gfx_metal_shader_get_info,
    gfx_metal_new_texture,
    gfx_metal_select_texture,
    gfx_metal_upload_texture,
    gfx_metal_upload_height_texture,
    gfx_metal_clear_height_texture,
    gfx_metal_set_sampler_parameters,
    gfx_metal_set_depth_test,
    gfx_metal_set_depth_mask,
    gfx_metal_set_zmode_decal,
    gfx_metal_set_viewport,
    gfx_metal_set_scissor,
    gfx_metal_set_use_alpha,
    gfx_metal_draw_triangles,
    gfx_metal_init,
    gfx_metal_on_resize,
    gfx_metal_start_frame,
    gfx_metal_end_frame,
    gfx_metal_finish_render,
    gfx_metal_shutdown
};

#endif // RAPI_METAL
