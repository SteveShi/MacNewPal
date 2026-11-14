#ifndef PAL_ENGINE_CORE_H
#define PAL_ENGINE_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAL_MAX_CHECKS 24
#define PAL_MAX_HEROES 12
#define PAL_ACT_MAX_INSNS 512

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

typedef struct {
    uint16_t op;
    int16_t args[7];
    int32_t nargs;
} PalActInsn;

typedef struct {
    uint32_t act_slot_id;
    int32_t sub_action_idx;
    int32_t total_sub_actions;
    int32_t nt_value;
    int32_t fr_tsw_id;       /* FR operand, e.g. 9054 -> all_sys.tsw slot_id 54 */
    int32_t duration_ticks;  /* DS base duration in 35ms ticks */
    int32_t delay_rem;       /* Countdown ticks before advancing */
    int32_t visible;         /* ON=1, NO=0 */
    int32_t blend_mode;      /* 0=normal, 4=additive (MA), 8=half (AH) */
    int32_t frame_index;     /* AP frame index */
    int32_t offset_x;        /* XY / XO offsets */
    int32_t offset_y;
    int32_t hitbox_w;        /* HW */
    int32_t hitbox_h;
    int32_t orientation;     /* OR */
    int32_t loop_remaining;  /* LP count (-1 = infinite) */
    int32_t loop_start_frame;
    int32_t loop_end_frame;
    int32_t last_sfx_id;     /* CS sound effect ID */
    int32_t pc;              /* Instruction pointer */
    int32_t finished;        /* VO/2O/OV reached without active loop */
    int32_t insn_count;
    PalActInsn insns[PAL_ACT_MAX_INSNS];
} PalActVM;

typedef struct {
    int32_t width;
    int32_t height;
    int32_t frame_count;
    double fps;
    int32_t has_audio;
    int32_t first_frame_rgba_bytes;
    char codec_name[32];
} PalBinkProbeInfo;

typedef struct {
    int32_t sample_rate;
    int32_t channels;
    int32_t pcm_bytes;
    double duration_sec;
    char codec_name[32];
} PalBgmProbeInfo;

typedef struct {
    int32_t width;
    int32_t height;
    int32_t line_count;
    int32_t non_transparent_pixels;
} PalRichTextMetrics;

/** Query available SDL3 render drivers on macOS and check Metal availability (returns 1 if Metal is present). */
int32_t pal_sdl3_query_metal_backend(char *out_drivers, int32_t out_cap);

/** Read-only validation of all required PalGame archives and databases in `game_dir`. */
int32_t pal_vfs_validate_directory(const char *game_dir, PalValidationReport *report);

/** Initialize a 35-opcode .act animation bytecode VM for `act_slot_id` and `sub_action_idx`. */
int32_t pal_act_vm_init(
    const char *game_dir, const char *act_file,
    uint32_t act_slot_id, int32_t sub_action_idx,
    PalActVM *out_vm
);

/** Advance `vm` by one engine tick (35 ms); returns 1 if stepped or finished cleanly, -1 on invalid VM. */
int32_t pal_act_vm_step_tick(PalActVM *vm);

/** Probe a Bink (.bik) video and decode its first video frame to RGBA in memory. */
int32_t pal_bink_probe(const char *game_dir, const char *rel_path, PalBinkProbeInfo *out_info);

/** Decode an MP3/WAV BGM file via FFmpeg into 44.1 kHz stereo S16 PCM and report metrics. */
int32_t pal_bgm_probe(const char *game_dir, const char *rel_path, PalBgmProbeInfo *out_info);

/** Measure and rasterize SWD2k Chinese rich-text (%N, %C0..9, %S, %K) via CoreText. */
int32_t pal_font_probe_rich_text(
    const char *utf8_rich_text, float font_size, int32_t max_width,
    PalRichTextMetrics *out_metrics
);

/** Launch interactive SDL3 + Metal runtime session reading directly from `cfg->game_dir` via VFS. */
int32_t pal_sdl3_run_session(const PalLaunchConfig *cfg, char *out_err, int32_t err_cap);

/** Non-interactive headless/hidden-window verification of Metal renderer and live VFS + M1 decoding. */
int32_t pal_sdl3_self_test(const char *game_dir);

#ifdef __cplusplus
}
#endif

#endif /* PAL_ENGINE_CORE_H */
