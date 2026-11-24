#ifndef PAL_TEST_MODE_H
#define PAL_TEST_MODE_H

#include <stdint.h>

#ifndef PAL_ENABLE_TEST_MODE
#define PAL_ENABLE_TEST_MODE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if PAL_ENABLE_TEST_MODE

/** Launch/runtime configuration for the optional QA Test Mode module. */
typedef struct {
    int32_t enabled;           /* 1 = QA Test Mode active */
    int32_t god_mode;          /* 1 = Party HP/MP loss & harmful ailments blocked, zero defeat */
    int32_t no_encounter;      /* 1 = Roaming monsters do not chase or auto-trigger combat */
    int32_t auto_instant_kill; /* 1 = Automatically defeat enemies on battle start */
    int32_t speed_mult;        /* 1, 2, or 3 = Field movement speed multiplier */
} PalTestConfig;

#endif /* PAL_ENABLE_TEST_MODE */

#ifdef __cplusplus
}
#endif

#endif /* PAL_TEST_MODE_H */

#if defined(PAL_TEST_MODE_RUNTIME) && !defined(PAL_TEST_MODE_RUNTIME_DECLARED)
#define PAL_TEST_MODE_RUNTIME_DECLARED 1

#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#if PAL_ENABLE_TEST_MODE

#define PAL_TEST_MENU_ROWS 13

/** Narrow host callback table so PalTestMode stays decoupled from PalEngineCore internals. */
typedef struct {
    void *ctx;
    void (*heal_party)(void *ctx);
    void (*add_money)(void *ctx, int32_t delta);
    void (*level_up_party)(void *ctx);
    void (*level_up_party_count)(void *ctx, int count);
    void (*supply_test_items)(void *ctx);
    void (*instant_kill_combat)(void *ctx);
    int  (*is_in_combat)(void *ctx);
    int  (*get_noclip)(void *ctx);
    void (*toggle_noclip)(void *ctx);
    void (*skip_talk_wait)(void *ctx);
    void (*quick_save)(void *ctx);
    void (*quick_load)(void *ctx);
    void (*play_sfx)(void *ctx, uint32_t sfx_id);
    SDL_Texture *(*create_rich_text)(
        SDL_Renderer *renderer, const char *utf8_rich_text,
        float font_size, int max_width, int *out_w, int *out_h
    );
} PalTestHostOps;

typedef struct {
    PalTestConfig cfg;
    int32_t panel_open;
    int32_t hover_row;
    int32_t hover_pill;
    int32_t hover_close;
    float feedback_timer;
    char feedback[160];

    int32_t tex_dirty;
    uint32_t last_state_sig;
    char last_pill_str[96];
    char last_toast_str[160];
    SDL_Texture *pill_tex;
    int32_t pill_w, pill_h;
    SDL_Texture *toast_tex;
    int32_t toast_w, toast_h;
    SDL_Texture *header_tex;
    int32_t header_w, header_h;
    SDL_Texture *footer_tex;
    int32_t footer_w, footer_h;
    SDL_Texture *row_tex[PAL_TEST_MENU_ROWS];
    int32_t row_w[PAL_TEST_MENU_ROWS];
    int32_t row_h[PAL_TEST_MENU_ROWS];
} PalTestState;

void pal_test_init(PalTestState *ts, const PalTestConfig *init_cfg);
void pal_test_free(PalTestState *ts);
void pal_test_bind_runtime(PalTestState *ts);
void pal_test_unbind_runtime(PalTestState *ts);

int pal_test_is_enabled(const PalTestState *ts);
int pal_test_is_god_mode(const PalTestState *ts);
int pal_test_runtime_god_mode(void);
int pal_test_is_no_encounter(const PalTestState *ts);
int pal_test_runtime_no_encounter(void);
int pal_test_speed_mult(const PalTestState *ts);
void pal_test_filter_hero_hp_mp(const PalTestState *ts, int32_t *io_dhp, int32_t *io_dmp);
void pal_test_runtime_filter_hero_delta(int32_t *io_dhp, int32_t *io_dmp);

int pal_test_handle_key(
    PalTestState *ts, const PalTestHostOps *ops,
    SDL_Keycode key, SDL_Keymod mod,
    int scene_is_combat, int block_digit_hotkeys
);
int pal_test_handle_mouse_down(
    PalTestState *ts, const PalTestHostOps *ops,
    float lx, float ly, int scene_is_combat
);
void pal_test_handle_mouse_move(PalTestState *ts, float lx, float ly, int scene_is_combat);
void pal_test_tick(PalTestState *ts, const PalTestHostOps *ops, int scene_is_combat, float dt);
void pal_test_render_overlay(
    PalTestState *ts, const PalTestHostOps *ops, SDL_Renderer *renderer, int scene_is_combat
);

#else /* !PAL_ENABLE_TEST_MODE */

typedef struct { int32_t unused; } PalTestHostOps;
typedef struct { int32_t unused; } PalTestState;

static inline void pal_test_init(PalTestState *ts, const void *init_cfg) { (void)ts; (void)init_cfg; }
static inline void pal_test_free(PalTestState *ts) { (void)ts; }
static inline void pal_test_bind_runtime(PalTestState *ts) { (void)ts; }
static inline void pal_test_unbind_runtime(PalTestState *ts) { (void)ts; }
static inline int pal_test_is_enabled(const PalTestState *ts) { (void)ts; return 0; }
static inline int pal_test_is_god_mode(const PalTestState *ts) { (void)ts; return 0; }
static inline int pal_test_runtime_god_mode(void) { return 0; }
static inline int pal_test_is_no_encounter(const PalTestState *ts) { (void)ts; return 0; }
static inline int pal_test_runtime_no_encounter(void) { return 0; }
static inline int pal_test_speed_mult(const PalTestState *ts) { (void)ts; return 1; }
static inline void pal_test_filter_hero_hp_mp(const PalTestState *ts, int32_t *io_dhp, int32_t *io_dmp) {
    (void)ts; (void)io_dhp; (void)io_dmp;
}
static inline void pal_test_runtime_filter_hero_delta(int32_t *io_dhp, int32_t *io_dmp) {
    (void)io_dhp; (void)io_dmp;
}
static inline int pal_test_handle_key(
    PalTestState *ts, const PalTestHostOps *ops,
    SDL_Keycode key, SDL_Keymod mod,
    int scene_is_combat, int block_digit_hotkeys
) {
    (void)ts; (void)ops; (void)key; (void)mod; (void)scene_is_combat; (void)block_digit_hotkeys;
    return 0;
}
static inline int pal_test_handle_mouse_down(
    PalTestState *ts, const PalTestHostOps *ops,
    float lx, float ly, int scene_is_combat
) {
    (void)ts; (void)ops; (void)lx; (void)ly; (void)scene_is_combat;
    return 0;
}
static inline void pal_test_handle_mouse_move(PalTestState *ts, float lx, float ly, int scene_is_combat) {
    (void)ts; (void)lx; (void)ly; (void)scene_is_combat;
}
static inline void pal_test_tick(PalTestState *ts, const PalTestHostOps *ops, int scene_is_combat, float dt) {
    (void)ts; (void)ops; (void)scene_is_combat; (void)dt;
}
static inline void pal_test_render_overlay(
    PalTestState *ts, const PalTestHostOps *ops, SDL_Renderer *renderer, int scene_is_combat
) {
    (void)ts; (void)ops; (void)renderer; (void)scene_is_combat;
}

#endif /* PAL_ENABLE_TEST_MODE */

#ifdef __cplusplus
}
#endif

#endif /* PAL_TEST_MODE_RUNTIME && !PAL_TEST_MODE_RUNTIME_DECLARED */
