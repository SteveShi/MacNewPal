#ifndef PAL_ENGINE_CORE_H
#define PAL_ENGINE_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAL_MAX_CHECKS 24
#define PAL_MAX_HEROES 12

typedef struct {
    char filename[64];
    char category[16];
    int32_t exists;
    int32_t signature_ok;
    int32_t item_count;
    char detail[128];
} PalArchiveCheck;

typedef struct {
    int32_t id;
    char name[64];
    int32_t lv;
    int32_t max_hp;
    int32_t cur_hp;
    int32_t max_mp;
    int32_t cur_mp;
    int32_t atk;
    int32_t mag;
    int32_t def;
    int32_t spd;
    int32_t luck;
} PalHeroSummary;

typedef struct {
    int32_t is_valid;
    int32_t valid_archives_count;
    int32_t total_required_archives;
    int32_t check_count;
    int32_t char_sprite_slots;
    int32_t char_act_slots;
    int32_t snd_slots;
    int32_t huge_map_count;
    int32_t huge2_map_count;
    int32_t hero_count;
    int32_t monster_count;
    int32_t item_count;
    int32_t magic_count;
    int32_t map_title_count;
    int32_t namex_count;
    int32_t hero_summary_count;
    PalHeroSummary heroes[PAL_MAX_HEROES];
    PalArchiveCheck checks[PAL_MAX_CHECKS];
    char encoding_name[32];
    char sdl_render_driver[32];
    char sample_map_title[64];
    char sample_dialogue[256];
    char summary_message[512];
} PalValidationReport;

typedef struct {
    char game_dir[1024];
    int32_t initial_map_index;
    int32_t initial_char_slot;
    int32_t window_scale;    /* 1 = 640x480, 2 = 1280x960, 0 = fullscreen */
    int32_t vsync_enabled;   /* 0 or 1 */
    int32_t nearest_filter;  /* 1 = nearest pixel-art, 0 = linear */
    float master_volume;     /* 0.0f .. 1.0f */
    int32_t mute_audio;      /* 0 or 1 */
} PalLaunchConfig;

/** Query available SDL3 render drivers on macOS and check Metal availability (returns 1 if Metal is present). */
int32_t pal_sdl3_query_metal_backend(char *out_drivers, int32_t out_cap);

/** Read-only validation of all required PalGame archives and databases in `game_dir`. */
int32_t pal_vfs_validate_directory(const char *game_dir, PalValidationReport *report);

/** Launch interactive SDL3 + Metal runtime session reading directly from `cfg->game_dir` via VFS. */
int32_t pal_sdl3_run_session(const PalLaunchConfig *cfg, char *out_err, int32_t err_cap);

/** Non-interactive headless/hidden-window verification of Metal renderer and live VFS decoding. */
int32_t pal_sdl3_self_test(const char *game_dir);

#ifdef __cplusplus
}
#endif

#endif /* PAL_ENGINE_CORE_H */
