#ifndef PAL_ENGINE_CORE_H
#define PAL_ENGINE_CORE_H

#include <stdint.h>
#include "PalTestMode.h"

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
    int32_t hd_mode;         /* 0 = Original 640x480 Classic, 1 = HD Remastered (sharp text + filters) */
    int32_t shader_filter;   /* 0 = Classic/None, 1 = Metal xBRZ Edge-Preserving HD, 2 = Metal CRT-Royale */
    float master_volume;     /* 0.0f .. 1.0f */
    int32_t mute_audio;      /* 0 or 1 */
    int32_t text_language;   /* 0 = Traditional Chinese (Original), 1 = Simplified Chinese */
    int32_t mcp_enabled;     /* 0 = disabled, 1 = enable local Unix socket MCP bridge */
    int32_t mcp_allow_qa_tools; /* 1 = allow teleport / test cheats / flag writes over MCP */
    int32_t headless_mode;   /* 1 = hidden SDL window + fast frame stepping + skip .bik/fades */
    int32_t skip_videos;     /* 1 = auto-skip .bik playback */
    char mcp_socket_path[256]; /* Empty = default "/tmp/newpal-mcp.sock" */
#if PAL_ENABLE_TEST_MODE
    PalTestConfig test_cfg;
#endif
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
    int32_t visible;         /* Always 1 by default (Palgame.exe: 0x4E4F/0x5649 toggle H-flip, not visibility) */
    int32_t flip_h;          /* 1 = horizontal mirror flip (IV 0x5649 -> [act+0x18]|=1, NO 0x4E4F -> &=~1) */
    int32_t act_scale_fp;    /* CS (0x5343) sprite scale in 10.10 fixed-point (default 1024 = 1.0x) */
    int32_t blend_mode;      /* 0=normal, 4=additive (MA), 8=half (AH) */
    uint8_t alpha_scale;     /* SG 16-level per-frame alpha (255=opaque) */
    int32_t hit_flag;        /* AT hit trigger count */
    int32_t frame_index;     /* AP frame index */
    int32_t offset_x;        /* XY / XO offsets */
    int32_t offset_y;
    int32_t hitbox_w;        /* HW */
    int32_t hitbox_h;
    int32_t orientation;     /* OR */
    int32_t loop_remaining;  /* LP count (-1 = infinite) */
    int32_t loop_start_frame;
    int32_t loop_end_frame;
    int32_t ap_step;         /* 1-based AP counter updated by AP opcode */
    int32_t start_at_ap;     /* 1-based slice start AP (0 = from beginning) */
    int32_t stop_at_ap;      /* 1-based slice end AP (0 = no limit) */
    int32_t loop_active;     /* 1 = active LP block */
    int32_t last_sfx_id;     /* VW sound effect ID (cleared after dispatch) */
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

typedef struct {
    int32_t map_id;
    int32_t width_tiles;
    int32_t height_tiles;
    int32_t total_cells;
    int32_t walkable_cells;
    int32_t blocked_cells;
    int32_t trigger_count;
    int32_t fg_sprite_defs;
    int32_t fg_sprite_instances;
    int32_t npc_count;
    int32_t astar_path_steps;
    int32_t step_speed_px;
    char map_title_utf8[64];
    char bgm_track[64];
} PalMapM2ProbeInfo;

typedef struct {
    int32_t bank_count;
    int32_t total_scripts;
    int32_t clean_scripts;
    int32_t core_scripts_ok;
    int32_t tp_map_id;
    int32_t tp_tx;
    int32_t tp_ty;
    int32_t tp_dir;
    int32_t chest_hidden;
    int32_t chest_item_id;
    int32_t cutscene_flag1001;
    int32_t cutscene_portrait_count;
    int32_t cutscene_dialogue_count;
    int32_t cutscene_npc_updates;
    int32_t npc_update_obj7_talk;
    int32_t npc_update_obj201_talk;
    char chest_text_utf8[128];
    char cutscene_sample_utf8[128];
    int32_t ok;
} PalTalkM3ProbeInfo;

typedef struct {
    int32_t hero_count;
    int32_t item_count;
    int32_t magic_count;
    int32_t level_template_count;
    int32_t hero2000_lv1_hp;
    int32_t hero2000_equip_atk;
    int32_t hero2000_equip_def;
    int32_t item4401_healed_hp;
    int32_t magic6901_healed_hp;
    int32_t hero2000_lv2_hp;
    int32_t chest7012_granted_item_id;
    char chest7012_item_name_utf8[64];
    int32_t ok;
} PalDbM4ProbeInfo;

typedef struct {
    int32_t monster_count;
    int32_t zone_count;
    int32_t fight_script_count;
    int32_t battle2801_bg_id;
    char battle2801_bgm[32];
    int32_t battle2801_enemy_id;
    char battle2801_enemy_name_utf8[64];
    int32_t battle2801_enemy_hp;
    int32_t battle2801_damage_dealt;
    int32_t battle2801_exp_gained;
    int32_t hero2000_post_level;
    int32_t shop58001_item_count;
    int32_t shop58001_bought_item_id;
    int32_t shop58001_money_after;
    int32_t save0_money;
    int32_t save0_map_id;
    char save0_title_utf8[64];
    int32_t roundtrip_ok;
    int32_t npc_ai_moved;
    int32_t cooldown_blocked;
    int32_t b2001_enemy_fr;
    int32_t b2801_enemy_fr;
    int32_t hero2000_battle_fr;
    int32_t magic_act_fr;
    int32_t ok;
} PalCombatM5ProbeInfo;

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

/** Probe MSF3 walkability/triggers/occlusion, maps.dat metadata, and A* pathfinding for `map_id`. */
int32_t pal_map_probe_m2(const char *game_dir, int32_t map_id, PalMapM2ProbeInfo *out_info);

/** Probe all 8 talk1..8.dat banks and headlessly execute scripts 6001, 7012, 104, and 105 for M3 verification. */
int32_t pal_talk_vm_probe_m3(const char *game_dir, PalTalkM3ProbeInfo *out_info);

/** Probe ghero/glevel/gitem/gmagic databases, party equipment/stat calculation, item/magic usage, level-up, and 0x309 chest item resolution for M4 verification. */
int32_t pal_db_menu_probe_m4(const char *game_dir, PalDbM4ProbeInfo *out_info);

/** Probe M5 monster/map-zone/GFight1..4 DB loading, turn-based battle 2801 simulation, gmagic shop 58001 transaction, and Save/Load round-trip. */
int32_t pal_combat_shop_save_probe_m5(const char *game_dir, PalCombatM5ProbeInfo *out_info);

/** Non-interactive headless/hidden-window verification of Metal renderer and live VFS + M1/M2/M3 decoding. */
int32_t pal_sdl3_self_test(const char *game_dir);

#ifdef __cplusplus
}
#endif

#endif /* PAL_ENGINE_CORE_H */
