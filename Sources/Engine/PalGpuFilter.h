#ifndef PAL_GPU_FILTER_H
#define PAL_GPU_FILTER_H

#include <SDL3/SDL.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PAL_SHADER_NONE = 0,
    PAL_SHADER_XBRZ = 1,
    PAL_SHADER_CRT_ROYALE = 2
} PalShaderFilterMode;

typedef struct {
    SDL_Renderer *renderer;
    SDL_GPUDevice *gpu_dev;
    SDL_GPUShader *xbrz_shader;
    SDL_GPUShader *crt_shader;
    SDL_GPURenderState *xbrz_state;
    SDL_GPURenderState *crt_state;
    int ready;
    int active_mode;
} PalGpuFilterPipeline;

/**
 * Create the SDL3 renderer backed by Metal.
 * When `want_gpu_shaders != 0`, requests the SDL3 GPU renderer with Metal Shading Language (MSL)
 * shader support (`SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN`), falling back to the
 * built-in `"metal"` renderer if GPU shader creation is unavailable.
 */
SDL_Renderer *pal_create_metal_or_gpu_renderer(
    SDL_Window *window,
    int want_gpu_shaders,
    const char **out_driver_name
);

/** Compile embedded Metal (MSL) fragment shaders for xBRZ and CRT-Royale on a GPU renderer. */
int pal_gpu_filter_init(PalGpuFilterPipeline *pf, SDL_Renderer *renderer);

/** Apply or clear (`PAL_SHADER_NONE`) the GPU fragment post-processing shader for subsequent draws. */
void pal_gpu_filter_apply(
    PalGpuFilterPipeline *pf,
    int filter_mode,
    int tex_w,
    int tex_h,
    int out_w,
    int out_h,
    float time_sec
);

/** Release all GPU shader and render-state handles prior to destroying the renderer. */
void pal_gpu_filter_destroy(PalGpuFilterPipeline *pf);

/** Non-interactive self-test probe verifying MSL GPU shader compilation & state binding on Metal. */
int pal_gpu_filter_self_test_probe(
    int *out_has_msl,
    int *out_xbrz_ok,
    int *out_crt_ok,
    char *out_drv,
    size_t drv_cap
);

#ifdef __cplusplus
}
#endif

#endif /* PAL_GPU_FILTER_H */
