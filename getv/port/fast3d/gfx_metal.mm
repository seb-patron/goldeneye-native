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
// gfx_opengl.c is GL-specific and has no Metal equivalent yet), no GETV_SHOTFRAME capture,
// no 3-point texture filtering (configFiltering==2). None of these block a first real
// frame; all are fast follows once one renders.
#ifdef RAPI_METAL

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <SDL2/SDL.h>

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
};

struct FrameUniforms {
    int32_t frame_count;
};

/* Mirrors gfx_opengl.c's uTex0Size/uTex1Size/uTex0Filter/uTex1Filter uniforms -- needed
 * for correctness whenever both textures are sampled (the TEXEL1 rescale, see the
 * fragment shader body below), not just for the 3-point filter this port does not yet
 * implement on Metal. */
struct DrawUniforms {
    float tex0_size[2];
    float tex1_size[2];
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
// combiner, which CLAUDE.md notes is "nearly everywhere" in this game. Decoding inline
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
    size_t off_tex = 0, off_fog = 0;
    if (used_textures[0] || used_textures[1]) {
        off_tex = num_floats * sizeof(float);
        vd.attributes[attr].format = MTLVertexFormatFloat2;
        vd.attributes[attr].bufferIndex = 0;
        vd.attributes[attr].offset = off_tex;
        attr++;
        num_floats += 2;
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
    if (opt_fog) m_append_line(vs, &vs_len, "  out.fog = in.fog;");
    for (int i = 0; i < num_inputs; i++) {
        char l[64]; snprintf(l, sizeof l, "  out.input%d = in.input%d;", i + 1, i + 1);
        m_append_line(vs, &vs_len, l);
    }
    m_append_line(vs, &vs_len, "  return out;");
    m_append_line(vs, &vs_len, "}");
    vs[vs_len] = '\0';

    m_append_line(fs, &fs_len, "struct FrameUniforms { int frameCount; };");
    m_append_line(fs, &fs_len, "struct DrawUniforms { float2 tex0Size; float2 tex1Size; };");
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
    m_append_line(fs, &fs_len, ") {");

    if (used_textures[0]) m_append_line(fs, &fs_len, "  float4 texVal0 = uTex0.sample(uSamp0, in.texCoord);");
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
    if (opt_alpha) {
        m_append_line(fs, &fs_len, "  return texel;");
    } else {
        m_append_line(fs, &fs_len, "  return float4(texel, 1.0);");
    }
    m_append_line(fs, &fs_len, "}");
    fs[fs_len] = '\0';

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

static void gfx_metal_upload_texture(const uint8_t *rgba32_buf, int width, int height) {
    struct MetalTexture *t = metal_tex[metal_curtex];
    @autoreleasepool {
        MTLTextureDescriptor *desc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:width height:height mipmapped:NO];
        desc.usage = MTLTextureUsageShaderRead;
        t->tex = [mtl_device newTextureWithDescriptor:desc];
        MTLRegion region = MTLRegionMake2D(0, 0, width, height);
        [t->tex replaceRegion:region mipmapLevel:0 withBytes:rgba32_buf bytesPerRow:(NSUInteger)width * 4];
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
        sd.sAddressMode = gfx_cm_to_metal(cms);
        sd.tAddressMode = gfx_cm_to_metal(cmt);
        t->sampler = [mtl_device newSamplerStateWithDescriptor:sd];
    }
}

static void gfx_metal_apply_depth_state(void) {
    if (!mtl_encoder) return;
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
    NSUInteger dw = (NSUInteger)mtl_drawable.texture.width, dh = (NSUInteger)mtl_drawable.texture.height;
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
    [mtl_encoder setFragmentBytes:&du length:sizeof du atIndex:1];

    if (cur_prg->used_textures[0] && metal_tex[0]) {
        [mtl_encoder setFragmentTexture:metal_tex[0]->tex atIndex:0];
        [mtl_encoder setFragmentSamplerState:metal_tex[0]->sampler atIndex:0];
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

static void gfx_metal_init(void) {
    if (!gePortMetalLayer) sys_fatal("gfx_metal_init: no CAMetalLayer -- gfx_sdl.init() must run first");
    mtl_layer = (__bridge CAMetalLayer *)gePortMetalLayer;
    mtl_device = mtl_layer.device ?: MTLCreateSystemDefaultDevice();
    mtl_layer.device = mtl_device;
    mtl_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    mtl_layer.framebufferOnly = YES;

    mtl_queue = [mtl_device newCommandQueue];

    tex_cache_size = TEX_CACHE_STEP;
    tex_cache = (struct MetalTexture *)calloc(tex_cache_size, sizeof(struct MetalTexture));
    if (!tex_cache) sys_fatal("out of memory allocating metal texture cache");

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

static void gfx_metal_start_frame(void) {
    frame_count++;
    @autoreleasepool {
        CGSize sz = mtl_layer.drawableSize;
        if (sz.width < 1 || sz.height < 1) return;
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
        mtl_encoder = [mtl_cmdbuf renderCommandEncoderWithDescriptor:pass];
        cur_depth_test = false; cur_depth_mask = true; cur_zmode_decal = false;
        gfx_metal_apply_depth_state();
        gfx_metal_set_viewport(0, 0, (int)sz.width, (int)sz.height);
        gfx_metal_set_scissor(0, 0, (int)sz.width, (int)sz.height);
    }
    mtl_vbo_offset = 0;
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
}

void gePortMetalFinishFrame(void) {
    if (!mtl_cmdbuf) return;
    if (mtl_drawable) [mtl_cmdbuf presentDrawable:mtl_drawable];
    [mtl_cmdbuf commit];
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
