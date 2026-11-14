#include "PalGpuFilter.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    float tex_size[2];
    float out_size[2];
    float time_sec;
    float mode;
    float param0;
    float param1;
} PalShaderUniforms;

static const char kPalXbrzFragmentMSL[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct main0_in {\n"
"    float4 v_color [[user(locn0)]];\n"
"    float2 v_uv    [[user(locn1)]];\n"
"};\n"
"\n"
"struct PalShaderUniforms {\n"
"    float2 tex_size;\n"
"    float2 out_size;\n"
"    float  time_sec;\n"
"    float  mode;\n"
"    float  param0;\n"
"    float  param1;\n"
"};\n"
"\n"
"inline float3 pal_ycbcr_diff(float3 a, float3 b) {\n"
"    float3 d = a - b;\n"
"    float y  =  0.299 * d.r + 0.587 * d.g + 0.114 * d.b;\n"
"    float cb = -0.169 * d.r - 0.331 * d.g + 0.500 * d.b;\n"
"    float cr =  0.500 * d.r - 0.419 * d.g - 0.081 * d.b;\n"
"    return float3(y, cb, cr);\n"
"}\n"
"\n"
"inline float pal_dist(float3 a, float3 b) {\n"
"    float3 ycc = pal_ycbcr_diff(a, b);\n"
"    return sqrt(dot(ycc * float3(48.0, 7.0, 6.0), ycc * float3(48.0, 7.0, 6.0)));\n"
"}\n"
"\n"
"inline bool pal_eq(float3 a, float3 b) {\n"
"    return pal_dist(a, b) < 15.0;\n"
"}\n"
"\n"
"inline float3 pal_texel(texture2d<float> tex, sampler s, float2 p_floor, float2 offset, float2 inv_size) {\n"
"    float2 uv = clamp((p_floor + offset + 0.5) * inv_size, 0.5 * inv_size, 1.0 - 0.5 * inv_size);\n"
"    return tex.sample(s, uv, level(0.0)).rgb;\n"
"}\n"
"\n"
"inline float3 pal_blend_corner(float3 dst, float3 nb1, float3 nb2, float2 fp, float2 corner_dir, float slope_ratio, float weight) {\n"
"    if (weight <= 0.001) return dst;\n"
"    float3 src = (pal_dist(dst, nb1) <= pal_dist(dst, nb2)) ? nb1 : nb2;\n"
"    float proj = dot(fp, corner_dir);\n"
"    float cross_d = (fp.x * corner_dir.y - fp.y * corner_dir.x);\n"
"    float edge = 0.5 - (proj + abs(cross_d) * (slope_ratio - 1.0) * 0.35);\n"
"    float aa = smoothstep(0.18, -0.18, edge);\n"
"    return mix(dst, src, clamp(aa * weight, 0.0, 1.0));\n"
"}\n"
"\n"
"fragment float4 main0(\n"
"    main0_in in [[stage_in]],\n"
"    constant PalShaderUniforms& u [[buffer(0)]],\n"
"    texture2d<float> u_texture [[texture(0)]],\n"
"    sampler u_sampler [[sampler(0)]]\n"
") {\n"
"    float2 tsize = max(u.tex_size, float2(1.0));\n"
"    float2 inv = 1.0 / tsize;\n"
"    float2 p = in.v_uv * tsize;\n"
"    float2 pf = floor(p);\n"
"    float2 fp = fract(p) - 0.5;\n"
"\n"
"    float3 A = pal_texel(u_texture, u_sampler, pf, float2(-1.0, -1.0), inv);\n"
"    float3 B = pal_texel(u_texture, u_sampler, pf, float2( 0.0, -1.0), inv);\n"
"    float3 C = pal_texel(u_texture, u_sampler, pf, float2( 1.0, -1.0), inv);\n"
"    float3 D = pal_texel(u_texture, u_sampler, pf, float2(-1.0,  0.0), inv);\n"
"    float3 E = pal_texel(u_texture, u_sampler, pf, float2( 0.0,  0.0), inv);\n"
"    float3 F = pal_texel(u_texture, u_sampler, pf, float2( 1.0,  0.0), inv);\n"
"    float3 G = pal_texel(u_texture, u_sampler, pf, float2(-1.0,  1.0), inv);\n"
"    float3 H = pal_texel(u_texture, u_sampler, pf, float2( 0.0,  1.0), inv);\n"
"    float3 I = pal_texel(u_texture, u_sampler, pf, float2( 1.0,  1.0), inv);\n"
"\n"
"    float3 N2 = pal_texel(u_texture, u_sampler, pf, float2( 0.0, -2.0), inv);\n"
"    float3 S2 = pal_texel(u_texture, u_sampler, pf, float2( 0.0,  2.0), inv);\n"
"    float3 W2 = pal_texel(u_texture, u_sampler, pf, float2(-2.0,  0.0), inv);\n"
"    float3 E2 = pal_texel(u_texture, u_sampler, pf, float2( 2.0,  0.0), inv);\n"
"\n"
"    float d_FH = pal_dist(F, H);\n"
"    float d_EI = pal_dist(E, I);\n"
"    float d_DH = pal_dist(D, H);\n"
"    float d_EG = pal_dist(E, G);\n"
"    float d_BF = pal_dist(B, F);\n"
"    float d_EC = pal_dist(E, C);\n"
"    float d_BD = pal_dist(B, D);\n"
"    float d_EA = pal_dist(E, A);\n"
"\n"
"    float w_se = (d_FH < 18.0 && d_FH < d_EI * 0.65 && !pal_eq(E, F) && !pal_eq(E, H))\n"
"                 ? clamp(1.0 - d_FH / 18.0, 0.0, 1.0) : 0.0;\n"
"    float w_sw = (d_DH < 18.0 && d_DH < d_EG * 0.65 && !pal_eq(E, D) && !pal_eq(E, H))\n"
"                 ? clamp(1.0 - d_DH / 18.0, 0.0, 1.0) : 0.0;\n"
"    float w_ne = (d_BF < 18.0 && d_BF < d_EC * 0.65 && !pal_eq(E, B) && !pal_eq(E, F))\n"
"                 ? clamp(1.0 - d_BF / 18.0, 0.0, 1.0) : 0.0;\n"
"    float w_nw = (d_BD < 18.0 && d_BD < d_EA * 0.65 && !pal_eq(E, B) && !pal_eq(E, D))\n"
"                 ? clamp(1.0 - d_BD / 18.0, 0.0, 1.0) : 0.0;\n"
"\n"
"    float d_bg = pal_dist(E, A) + pal_dist(E, I) + pal_dist(B, D) + pal_dist(F, H);\n"
"    float d_fg = pal_dist(D, H) + pal_dist(B, F) + pal_dist(E, C) + pal_dist(E, G);\n"
"    if (d_bg < d_fg) { w_nw *= 0.25; w_se *= 0.25; }\n"
"    if (d_fg < d_bg) { w_ne *= 0.25; w_sw *= 0.25; }\n"
"\n"
"    float3 col = E;\n"
"    if (fp.x >= 0.0 && fp.y >= 0.0) {\n"
"        float slope = (pal_eq(F, S2) || pal_eq(H, E2)) ? 1.45 : 1.0;\n"
"        col = pal_blend_corner(E, F, H, fp, float2(1.0, 1.0), slope, w_se);\n"
"    } else if (fp.x < 0.0 && fp.y >= 0.0) {\n"
"        float slope = (pal_eq(D, S2) || pal_eq(H, W2)) ? 1.45 : 1.0;\n"
"        col = pal_blend_corner(E, D, H, fp, float2(-1.0, 1.0), slope, w_sw);\n"
"    } else if (fp.x >= 0.0 && fp.y < 0.0) {\n"
"        float slope = (pal_eq(F, N2) || pal_eq(B, E2)) ? 1.45 : 1.0;\n"
"        col = pal_blend_corner(E, F, B, fp, float2(1.0, -1.0), slope, w_ne);\n"
"    } else {\n"
"        float slope = (pal_eq(D, N2) || pal_eq(B, W2)) ? 1.45 : 1.0;\n"
"        col = pal_blend_corner(E, D, B, fp, float2(-1.0, -1.0), slope, w_nw);\n"
"    }\n"
"\n"
"    // Crisp sub-texel anti-aliasing within flat pixel interior\n"
"    float3 h_nb = (fp.x >= 0.0) ? F : D;\n"
"    float3 v_nb = (fp.y >= 0.0) ? H : B;\n"
"    float hx = smoothstep(0.32, 0.5, abs(fp.x));\n"
"    float hy = smoothstep(0.32, 0.5, abs(fp.y));\n"
"    if (pal_dist(E, h_nb) < 8.0) col = mix(col, h_nb, hx * 0.25);\n"
"    if (pal_dist(E, v_nb) < 8.0) col = mix(col, v_nb, hy * 0.25);\n"
"\n"
"    return float4(clamp(col, 0.0, 1.0), 1.0) * in.v_color;\n"
"}\n";

static const char kPalCrtRoyaleFragmentMSL[] =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"\n"
"struct main0_in {\n"
"    float4 v_color [[user(locn0)]];\n"
"    float2 v_uv    [[user(locn1)]];\n"
"};\n"
"\n"
"struct PalShaderUniforms {\n"
"    float2 tex_size;\n"
"    float2 out_size;\n"
"    float  time_sec;\n"
"    float  mode;\n"
"    float  param0;\n"
"    float  param1;\n"
"};\n"
"\n"
"inline float2 crt_curve(float2 uv) {\n"
"    float2 c = uv * 2.0 - 1.0;\n"
"    float2 off = c.yx / float2(6.8, 5.4);\n"
"    c = c + c * (off * off);\n"
"    return c * 0.5 + 0.5;\n"
"}\n"
"\n"
"inline float3 sample_sharp_lanczos3(texture2d<float> tex, sampler s, float2 uv, float2 tsize) {\n"
"    float2 inv = 1.0 / tsize;\n"
"    float2 p = uv * tsize;\n"
"    float2 pf = floor(p - 0.5) + 0.5;\n"
"    float2 f = p - pf;\n"
"    float2 f2 = f * f;\n"
"    float2 f3 = f2 * f;\n"
"    // Catmull-Rom cubic horizontal + exact nearest/gaussian vertical row lock\n"
"    float wx0 = -0.5 * f3.x + f2.x - 0.5 * f.x;\n"
"    float wx1 =  1.5 * f3.x - 2.5 * f2.x + 1.0;\n"
"    float wx2 = -1.5 * f3.x + 2.0 * f2.x + 0.5 * f.x;\n"
"    float wx3 =  0.5 * f3.x - 0.5 * f2.x;\n"
"\n"
"    float row_y = clamp((floor(p.y) + 0.5) * inv.y, 0.5 * inv.y, 1.0 - 0.5 * inv.y);\n"
"    float3 c0 = tex.sample(s, float2(clamp((pf.x - 1.0) * inv.x, 0.0, 1.0), row_y), level(0.0)).rgb;\n"
"    float3 c1 = tex.sample(s, float2(clamp((pf.x + 0.0) * inv.x, 0.0, 1.0), row_y), level(0.0)).rgb;\n"
"    float3 c2 = tex.sample(s, float2(clamp((pf.x + 1.0) * inv.x, 0.0, 1.0), row_y), level(0.0)).rgb;\n"
"    float3 c3 = tex.sample(s, float2(clamp((pf.x + 2.0) * inv.x, 0.0, 1.0), row_y), level(0.0)).rgb;\n"
"    return clamp(c0 * wx0 + c1 * wx1 + c2 * wx2 + c3 * wx3, 0.0, 1.0);\n"
"}\n"
"\n"
"fragment float4 main0(\n"
"    main0_in in [[stage_in]],\n"
"    constant PalShaderUniforms& u [[buffer(0)]],\n"
"    texture2d<float> u_texture [[texture(0)]],\n"
"    sampler u_sampler [[sampler(0)]]\n"
") {\n"
"    float2 tsize = max(u.tex_size, float2(1.0));\n"
"    float2 osize = max(u.out_size, float2(640.0, 480.0));\n"
"    float2 uv = crt_curve(in.v_uv);\n"
"    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {\n"
"        return float4(0.0, 0.0, 0.0, 1.0);\n"
"    }\n"
"\n"
"    float3 center = sample_sharp_lanczos3(u_texture, u_sampler, uv, tsize);\n"
"    float3 lin = pow(center, float3(2.2));\n"
"\n"
"    // Soft phosphor halation bloom\n"
"    float2 inv = 1.0 / tsize;\n"
"    float3 b_up = u_texture.sample(u_sampler, clamp(uv + float2(0.0, -inv.y), 0.0, 1.0), level(0.0)).rgb;\n"
"    float3 b_dn = u_texture.sample(u_sampler, clamp(uv + float2(0.0,  inv.y), 0.0, 1.0), level(0.0)).rgb;\n"
"    float3 b_lt = u_texture.sample(u_sampler, clamp(uv + float2(-inv.x, 0.0), 0.0, 1.0), level(0.0)).rgb;\n"
"    float3 b_rt = u_texture.sample(u_sampler, clamp(uv + float2( inv.x, 0.0), 0.0, 1.0), level(0.0)).rgb;\n"
"    float3 bloom = pow((b_up + b_dn + b_lt + b_rt) * 0.25, float3(2.2));\n"
"\n"
"    // Gaussian electron beam scanline centered on each 480p row\n"
"    float dy = fract(uv.y * tsize.y) - 0.5;\n"
"    float luma = clamp(dot(center, float3(0.299, 0.587, 0.114)), 0.0, 1.0);\n"
"    float beam_w = mix(0.27, 0.44, luma);\n"
"    float scan = exp(-(dy * dy) / (2.0 * beam_w * beam_w));\n"
"    float scan_floor = 0.62;\n"
"    float scan_weight = mix(scan_floor, 1.12, scan);\n"
"\n"
"    // Subtle Trinitron aperture grille RGB triad on output physical grid\n"
"    float gx = floor(in.v_uv.x * osize.x);\n"
"    int triad = int(fmod(gx, 3.0));\n"
"    float3 mask = float3(0.92);\n"
"    if (triad == 0)      mask = float3(1.06, 0.90, 0.90);\n"
"    else if (triad == 1) mask = float3(0.90, 1.06, 0.90);\n"
"    else                 mask = float3(0.90, 0.90, 1.06);\n"
"\n"
"    // Gentle corner vignette\n"
"    float2 vig_uv = uv * (1.0 - uv);\n"
"    float vig = clamp(pow(vig_uv.x * vig_uv.y * 18.0, 0.14), 0.72, 1.0);\n"
"\n"
"    float3 lit = (lin * scan_weight + bloom * 0.16) * mask * vig * 1.14;\n"
"    float3 out_srgb = pow(clamp(lit, 0.0, 1.0), float3(1.0 / 2.2));\n"
"    return float4(out_srgb, 1.0) * in.v_color;\n"
"}\n";

SDL_Renderer *pal_create_metal_or_gpu_renderer(
    SDL_Window *window,
    int want_gpu_shaders,
    const char **out_driver_name
) {
    if (out_driver_name) *out_driver_name = NULL;
    SDL_Renderer *r = NULL;
    if (want_gpu_shaders) {
        SDL_PropertiesID props = SDL_CreateProperties();
        if (props) {
            SDL_SetPointerProperty(props, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER, window);
            SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, "gpu");
            SDL_SetBooleanProperty(props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
            r = SDL_CreateRendererWithProperties(props);
            SDL_DestroyProperties(props);
        }
    }
    if (!r) {
        r = SDL_CreateRenderer(window, "metal");
    }
    if (r && out_driver_name) {
        *out_driver_name = SDL_GetRendererName(r);
    }
    return r;
}

static int compile_msl_fragment_state(
    SDL_Renderer *renderer,
    SDL_GPUDevice *dev,
    const char *msl_src,
    const char *debug_name,
    SDL_GPUShader **out_shader,
    SDL_GPURenderState **out_state
) {
    if (!renderer || !dev || !msl_src || !out_shader || !out_state) return -1;
    *out_shader = NULL;
    *out_state = NULL;

    SDL_GPUShaderCreateInfo sci;
    SDL_zero(sci);
    sci.code = (const Uint8 *)msl_src;
    sci.code_size = strlen(msl_src);
    sci.entrypoint = "main0";
    sci.format = SDL_GPU_SHADERFORMAT_MSL;
    sci.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    sci.num_samplers = 1;
    sci.num_uniform_buffers = 1;

    SDL_PropertiesID sprops = SDL_CreateProperties();
    if (sprops && debug_name) {
        SDL_SetStringProperty(sprops, SDL_PROP_GPU_SHADER_CREATE_NAME_STRING, debug_name);
        sci.props = sprops;
    }
    SDL_GPUShader *shader = SDL_CreateGPUShader(dev, &sci);
    if (sprops) SDL_DestroyProperties(sprops);
    if (!shader) return -1;

    SDL_GPURenderStateCreateInfo rsci;
    SDL_zero(rsci);
    rsci.fragment_shader = shader;
    SDL_GPURenderState *state = SDL_CreateGPURenderState(renderer, &rsci);
    if (!state) {
        SDL_ReleaseGPUShader(dev, shader);
        return -1;
    }

    *out_shader = shader;
    *out_state = state;
    return 0;
}

int pal_gpu_filter_init(PalGpuFilterPipeline *pf, SDL_Renderer *renderer) {
    if (!pf) return -1;
    memset(pf, 0, sizeof(*pf));
    pf->renderer = renderer;
    if (!renderer) return -1;

    SDL_GPUDevice *dev = SDL_GetGPURendererDevice(renderer);
    if (!dev) return -1;
    if ((SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_MSL) == 0) return -1;
    pf->gpu_dev = dev;

    if (compile_msl_fragment_state(renderer, dev, kPalXbrzFragmentMSL, "NewPal_xBRZ",
                                   &pf->xbrz_shader, &pf->xbrz_state) != 0) {
        pal_gpu_filter_destroy(pf);
        return -1;
    }
    if (compile_msl_fragment_state(renderer, dev, kPalCrtRoyaleFragmentMSL, "NewPal_CRT_Royale",
                                   &pf->crt_shader, &pf->crt_state) != 0) {
        pal_gpu_filter_destroy(pf);
        return -1;
    }

    pf->ready = 1;
    return 0;
}

void pal_gpu_filter_apply(
    PalGpuFilterPipeline *pf,
    int filter_mode,
    int tex_w,
    int tex_h,
    int out_w,
    int out_h,
    float time_sec
) {
    if (!pf || !pf->renderer) return;
    if (!pf->ready || filter_mode == PAL_SHADER_NONE) {
        if (pf->active_mode != PAL_SHADER_NONE) {
            SDL_SetGPURenderState(pf->renderer, NULL);
            pf->active_mode = PAL_SHADER_NONE;
        }
        return;
    }

    SDL_GPURenderState *target = NULL;
    if (filter_mode == PAL_SHADER_XBRZ) target = pf->xbrz_state;
    else if (filter_mode == PAL_SHADER_CRT_ROYALE) target = pf->crt_state;

    if (!target) {
        SDL_SetGPURenderState(pf->renderer, NULL);
        pf->active_mode = PAL_SHADER_NONE;
        return;
    }

    PalShaderUniforms u;
    memset(&u, 0, sizeof(u));
    u.tex_size[0] = (float)(tex_w > 0 ? tex_w : 640);
    u.tex_size[1] = (float)(tex_h > 0 ? tex_h : 480);
    u.out_size[0] = (float)(out_w > 0 ? out_w : 1280);
    u.out_size[1] = (float)(out_h > 0 ? out_h : 960);
    u.time_sec = time_sec;
    u.mode = (float)filter_mode;
    u.param0 = 1.0f;

    SDL_SetGPURenderStateFragmentUniforms(target, 0, &u, (Uint32)sizeof(u));
    SDL_SetGPURenderState(pf->renderer, target);
    pf->active_mode = filter_mode;
}

void pal_gpu_filter_destroy(PalGpuFilterPipeline *pf) {
    if (!pf) return;
    if (pf->renderer) {
        SDL_SetGPURenderState(pf->renderer, NULL);
    }
    if (pf->xbrz_state) {
        SDL_DestroyGPURenderState(pf->xbrz_state);
        pf->xbrz_state = NULL;
    }
    if (pf->crt_state) {
        SDL_DestroyGPURenderState(pf->crt_state);
        pf->crt_state = NULL;
    }
    if (pf->gpu_dev) {
        if (pf->xbrz_shader) {
            SDL_ReleaseGPUShader(pf->gpu_dev, pf->xbrz_shader);
            pf->xbrz_shader = NULL;
        }
        if (pf->crt_shader) {
            SDL_ReleaseGPUShader(pf->gpu_dev, pf->crt_shader);
            pf->crt_shader = NULL;
        }
    }
    pf->gpu_dev = NULL;
    pf->renderer = NULL;
    pf->ready = 0;
    pf->active_mode = PAL_SHADER_NONE;
}

int pal_gpu_filter_self_test_probe(
    int *out_has_msl,
    int *out_xbrz_ok,
    int *out_crt_ok,
    char *out_drv,
    size_t drv_cap
) {
    if (out_has_msl) *out_has_msl = 0;
    if (out_xbrz_ok) *out_xbrz_ok = 0;
    if (out_crt_ok) *out_crt_ok = 0;
    if (out_drv && drv_cap > 0) out_drv[0] = '\0';

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_Renderer *r = NULL;
    if (props) {
        SDL_SetStringProperty(props, SDL_PROP_RENDERER_CREATE_NAME_STRING, "gpu");
        SDL_SetBooleanProperty(props, SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
        r = SDL_CreateRendererWithProperties(props);
        SDL_DestroyProperties(props);
    }
    if (!r) return -1;

    const char *drv = SDL_GetRendererName(r);
    if (out_drv && drv_cap > 0) {
        snprintf(out_drv, drv_cap, "%s", drv ? drv : "unknown");
    }

    SDL_GPUDevice *dev = SDL_GetGPURendererDevice(r);
    int has_msl = (dev && (SDL_GetGPUShaderFormats(dev) & SDL_GPU_SHADERFORMAT_MSL)) ? 1 : 0;
    if (out_has_msl) *out_has_msl = has_msl;

    PalGpuFilterPipeline pf;
    int init_rc = pal_gpu_filter_init(&pf, r);
    int xbrz_ok = (init_rc == 0 && pf.xbrz_state != NULL) ? 1 : 0;
    int crt_ok = (init_rc == 0 && pf.crt_state != NULL) ? 1 : 0;

    if (init_rc == 0) {
        SDL_Texture *src = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 64, 64);
        SDL_Texture *dst = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, 256, 256);
        if (src && dst) {
            SDL_SetRenderTarget(r, src);
            SDL_SetRenderDrawColor(r, 120, 200, 140, 255);
            SDL_RenderClear(r);

            SDL_SetRenderTarget(r, dst);
            pal_gpu_filter_apply(&pf, PAL_SHADER_XBRZ, 64, 64, 256, 256, 0.1f);
            SDL_RenderTexture(r, src, NULL, NULL);
            pal_gpu_filter_apply(&pf, PAL_SHADER_CRT_ROYALE, 64, 64, 256, 256, 0.1f);
            SDL_RenderTexture(r, src, NULL, NULL);
            pal_gpu_filter_apply(&pf, PAL_SHADER_NONE, 64, 64, 256, 256, 0.1f);
            SDL_RenderTexture(r, src, NULL, NULL);
            SDL_SetRenderTarget(r, NULL);
        }
        if (src) SDL_DestroyTexture(src);
        if (dst) SDL_DestroyTexture(dst);
    }

    if (out_xbrz_ok) *out_xbrz_ok = xbrz_ok;
    if (out_crt_ok) *out_crt_ok = crt_ok;

    pal_gpu_filter_destroy(&pf);
    SDL_DestroyRenderer(r);
    return (has_msl && xbrz_ok && crt_ok) ? 0 : -1;
}
