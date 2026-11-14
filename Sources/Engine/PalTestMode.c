#define PAL_TEST_MODE_RUNTIME 1
#include "PalTestMode.h"

#if PAL_ENABLE_TEST_MODE

#include <stdio.h>
#include <string.h>

static PalTestState *g_pal_test_runtime = NULL;

static void pal_test_pill_rect(int scene_is_combat, float *ox, float *oy, float *ow, float *oh) {
    *ox = 486.0f;
    *oy = scene_is_combat ? 46.0f : 8.0f;
    *ow = 146.0f;
    *oh = 20.0f;
}

static void pal_test_panel_rect(float *ox, float *oy, float *ow, float *oh) {
    *ox = 110.0f;
    *oy = 52.0f;
    *ow = 420.0f;
    *oh = 374.0f;
}

static void pal_test_close_rect(float *ox, float *oy, float *ow, float *oh) {
    *ox = 500.0f;
    *oy = 57.0f;
    *ow = 22.0f;
    *oh = 20.0f;
}

static void pal_test_row_rect(int row, float *ox, float *oy, float *ow, float *oh) {
    *ox = 122.0f;
    *oy = 88.0f + (float)row * 22.0f;
    *ow = 396.0f;
    *oh = 20.0f;
}

static void pal_test_set_feedback(PalTestState *ts, const char *msg) {
    if (!ts || !msg) return;
    snprintf(ts->feedback, sizeof(ts->feedback), "%s", msg);
    ts->feedback_timer = 2.6f;
    ts->tex_dirty = 1;
}

void pal_test_init(PalTestState *ts, const PalTestConfig *init_cfg) {
    if (!ts) return;
    memset(ts, 0, sizeof(*ts));
    ts->hover_row = -1;
    ts->tex_dirty = 1;
    if (init_cfg) {
        ts->cfg = *init_cfg;
    }
    ts->cfg.enabled = ts->cfg.enabled ? 1 : 0;
    ts->cfg.god_mode = ts->cfg.god_mode ? 1 : 0;
    ts->cfg.no_encounter = ts->cfg.no_encounter ? 1 : 0;
    ts->cfg.auto_instant_kill = ts->cfg.auto_instant_kill ? 1 : 0;
    if (ts->cfg.speed_mult < 1 || ts->cfg.speed_mult > 3) ts->cfg.speed_mult = 1;
}

void pal_test_free(PalTestState *ts) {
    if (!ts) return;
    if (g_pal_test_runtime == ts) g_pal_test_runtime = NULL;
    if (ts->pill_tex) { SDL_DestroyTexture(ts->pill_tex); ts->pill_tex = NULL; }
    if (ts->toast_tex) { SDL_DestroyTexture(ts->toast_tex); ts->toast_tex = NULL; }
    if (ts->header_tex) { SDL_DestroyTexture(ts->header_tex); ts->header_tex = NULL; }
    if (ts->footer_tex) { SDL_DestroyTexture(ts->footer_tex); ts->footer_tex = NULL; }
    for (int i = 0; i < PAL_TEST_MENU_ROWS; i++) {
        if (ts->row_tex[i]) { SDL_DestroyTexture(ts->row_tex[i]); ts->row_tex[i] = NULL; }
    }
}

void pal_test_bind_runtime(PalTestState *ts) {
    g_pal_test_runtime = ts;
}

void pal_test_unbind_runtime(PalTestState *ts) {
    if (!ts || g_pal_test_runtime == ts) g_pal_test_runtime = NULL;
}

int pal_test_is_enabled(const PalTestState *ts) {
    return (ts && ts->cfg.enabled) ? 1 : 0;
}

int pal_test_is_god_mode(const PalTestState *ts) {
    return (ts && ts->cfg.enabled && ts->cfg.god_mode) ? 1 : 0;
}

int pal_test_runtime_god_mode(void) {
    return pal_test_is_god_mode(g_pal_test_runtime);
}

int pal_test_is_no_encounter(const PalTestState *ts) {
    return (ts && ts->cfg.enabled && ts->cfg.no_encounter) ? 1 : 0;
}

int pal_test_runtime_no_encounter(void) {
    return pal_test_is_no_encounter(g_pal_test_runtime);
}

int pal_test_speed_mult(const PalTestState *ts) {
    if (!ts || !ts->cfg.enabled) return 1;
    if (ts->cfg.speed_mult < 1 || ts->cfg.speed_mult > 3) return 1;
    return ts->cfg.speed_mult;
}

void pal_test_filter_hero_hp_mp(const PalTestState *ts, int32_t *io_dhp, int32_t *io_dmp) {
    if (!pal_test_is_god_mode(ts)) return;
    if (io_dhp && *io_dhp < 0) *io_dhp = 0;
    if (io_dmp && *io_dmp < 0) *io_dmp = 0;
}

void pal_test_runtime_filter_hero_delta(int32_t *io_dhp, int32_t *io_dmp) {
    pal_test_filter_hero_hp_mp(g_pal_test_runtime, io_dhp, io_dmp);
}

typedef enum {
    TEST_ACT_GOD = 0,
    TEST_ACT_NO_ENCOUNTER = 1,
    TEST_ACT_INSTANT_KILL = 2,
    TEST_ACT_AUTO_KILL = 3,
    TEST_ACT_HEAL_PARTY = 4,
    TEST_ACT_ADD_MONEY = 5,
    TEST_ACT_LEVEL_UP = 6,
    TEST_ACT_SUPPLY_ITEMS = 7,
    TEST_ACT_SPEED_CYCLE = 8,
    TEST_ACT_NOCLIP = 9,
    TEST_ACT_SKIP_WAIT = 10,
    TEST_ACT_QUICK_SAVE = 11,
    TEST_ACT_QUICK_LOAD = 12
} PalTestActionId;

static void pal_test_exec_action(PalTestState *ts, const PalTestHostOps *ops, int action_idx) {
    if (!ts || !ops || !ts->cfg.enabled) return;
    uint32_t sfx = 2u;
    switch (action_idx) {
        case TEST_ACT_GOD:
            ts->cfg.god_mode = !ts->cfg.god_mode;
            if (ts->cfg.god_mode && ops->heal_party) ops->heal_party(ops->ctx);
            pal_test_set_feedback(ts, ts->cfg.god_mode
                ? "人物無敵：已開啟（鎖血／異常免疫／仙術免耗藍）"
                : "人物無敵：已關閉");
            break;
        case TEST_ACT_NO_ENCOUNTER:
            ts->cfg.no_encounter = !ts->cfg.no_encounter;
            pal_test_set_feedback(ts, ts->cfg.no_encounter
                ? "免遇敵模式：已開啟（屏蔽漫游怪追擊與觸碰遇敵）"
                : "免遇敵模式：已關閉（恢復正常漫游遇敵）");
            break;
        case TEST_ACT_INSTANT_KILL:
            if (ops->is_in_combat && ops->is_in_combat(ops->ctx)) {
                if (ops->instant_kill_combat) ops->instant_kill_combat(ops->ctx);
                ts->panel_open = 0;
                pal_test_set_feedback(ts, "戰鬥一鍵秒殺：已消滅全部敵方並正常結算獎勵！");
            } else {
                sfx = 1u;
                pal_test_set_feedback(ts, "一鍵秒殺僅在戰鬥畫面中可用（可按 F 觸發戰鬥）。");
            }
            break;
        case TEST_ACT_AUTO_KILL:
            ts->cfg.auto_instant_kill = !ts->cfg.auto_instant_kill;
            pal_test_set_feedback(ts, ts->cfg.auto_instant_kill
                ? "入戰自動秒殺：已開啟（進入戰鬥自動結算勝利）"
                : "入戰自動秒殺：已關閉");
            break;
        case TEST_ACT_HEAL_PARTY:
            if (ops->heal_party) ops->heal_party(ops->ctx);
            pal_test_set_feedback(ts, "已將全隊 HP/MP 回滿並清除全部異常狀態！");
            break;
        case TEST_ACT_ADD_MONEY:
            if (ops->add_money) ops->add_money(ops->ctx, 50000);
            pal_test_set_feedback(ts, "測試金錢已增加 +50,000 文！");
            break;
        case TEST_ACT_LEVEL_UP:
            if (ops->level_up_party) ops->level_up_party(ops->ctx);
            pal_test_set_feedback(ts, "在隊全員已提升 1 級並更新屬性與仙術！");
            break;
        case TEST_ACT_SUPPLY_ITEMS:
            if (ops->supply_test_items) ops->supply_test_items(ops->ctx);
            pal_test_set_feedback(ts, "已補給測試物資（止血草／行軍丹／鼠兒果／還魂香／投擲物）！");
            break;
        case TEST_ACT_SPEED_CYCLE:
            ts->cfg.speed_mult = (ts->cfg.speed_mult % 3) + 1;
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "大圖移動倍速切換為：%d×", ts->cfg.speed_mult);
                pal_test_set_feedback(ts, msg);
            }
            break;
        case TEST_ACT_NOCLIP:
            if (ops->toggle_noclip) ops->toggle_noclip(ops->ctx);
            {
                int nc = (ops->get_noclip) ? ops->get_noclip(ops->ctx) : 0;
                pal_test_set_feedback(ts, nc ? "穿牆模式（NoClip）：已開啟" : "穿牆模式（NoClip）：已關閉");
            }
            break;
        case TEST_ACT_SKIP_WAIT:
            if (ops->skip_talk_wait) ops->skip_talk_wait(ops->ctx);
            pal_test_set_feedback(ts, "已跳過當前劇情等待／淡入淡出延遲！");
            break;
        case TEST_ACT_QUICK_SAVE:
            if (ops->quick_save) ops->quick_save(ops->ctx);
            pal_test_set_feedback(ts, "已執行快速存檔至測試欄位（Slot 9 / Save/9.sav）！");
            break;
        case TEST_ACT_QUICK_LOAD:
            if (ops->quick_load) ops->quick_load(ops->ctx);
            ts->panel_open = 0;
            pal_test_set_feedback(ts, "正在從測試欄位（Slot 9 / Save/9.sav）快速讀檔…");
            break;
        default:
            sfx = 1u;
            break;
    }
    if (ops->play_sfx) ops->play_sfx(ops->ctx, sfx);
    ts->tex_dirty = 1;
}

int pal_test_handle_key(
    PalTestState *ts, const PalTestHostOps *ops,
    SDL_Keycode key, SDL_Keymod mod,
    int scene_is_combat, int block_digit_hotkeys
) {
    if (!ts || !ops) return 0;

    /* F12 toggles master Test Mode switch at any time while module is compiled in. */
    if (key == SDLK_F12) {
        ts->cfg.enabled = !ts->cfg.enabled;
        if (!ts->cfg.enabled) {
            ts->panel_open = 0;
        } else {
            pal_test_set_feedback(ts, "測試模式已開啟（按 ~ 鍵展開工具箱，K 秒殺）");
        }
        ts->tex_dirty = 1;
        if (ops->play_sfx) ops->play_sfx(ops->ctx, ts->cfg.enabled ? 2u : 1u);
        return 1;
    }

    /* Alt+~ (Option+Grave) can also wake Test Mode even if initially disabled. */
    if (!ts->cfg.enabled) {
        if (key == SDLK_GRAVE && (mod & SDL_KMOD_ALT) != 0) {
            ts->cfg.enabled = 1;
            ts->panel_open = 1;
            ts->tex_dirty = 1;
            pal_test_set_feedback(ts, "測試模式已喚醒");
            if (ops->play_sfx) ops->play_sfx(ops->ctx, 2u);
            return 1;
        }
        return 0;
    }

    if (key == SDLK_GRAVE || key == SDLK_F11) {
        ts->panel_open = !ts->panel_open;
        ts->tex_dirty = 1;
        if (ops->play_sfx) ops->play_sfx(ops->ctx, ts->panel_open ? 2u : 1u);
        return 1;
    }

    if (ts->panel_open && key == SDLK_ESCAPE) {
        ts->panel_open = 0;
        ts->tex_dirty = 1;
        if (ops->play_sfx) ops->play_sfx(ops->ctx, 1u);
        return 1;
    }

    /* Direct global QA hotkeys (work without opening panel) */
    if (key == SDLK_F2) { pal_test_exec_action(ts, ops, TEST_ACT_GOD); return 1; }
    if (key == SDLK_F3) { pal_test_exec_action(ts, ops, TEST_ACT_NO_ENCOUNTER); return 1; }
    if (key == SDLK_F4) { pal_test_exec_action(ts, ops, TEST_ACT_INSTANT_KILL); return 1; }
    if (key == SDLK_K && scene_is_combat) { pal_test_exec_action(ts, ops, TEST_ACT_INSTANT_KILL); return 1; }
    if (key == SDLK_F5) { pal_test_exec_action(ts, ops, TEST_ACT_HEAL_PARTY); return 1; }
    if (key == SDLK_F6) { pal_test_exec_action(ts, ops, TEST_ACT_ADD_MONEY); return 1; }
    if (key == SDLK_F7) { pal_test_exec_action(ts, ops, TEST_ACT_LEVEL_UP); return 1; }
    if (key == SDLK_F8) { pal_test_exec_action(ts, ops, TEST_ACT_SPEED_CYCLE); return 1; }
    if (key == SDLK_F9) { pal_test_exec_action(ts, ops, TEST_ACT_NOCLIP); return 1; }
    if (key == SDLK_F10) { pal_test_exec_action(ts, ops, TEST_ACT_SKIP_WAIT); return 1; }

    /* When toolbox panel is open, absorb navigation/shortcut keys cleanly. */
    if (ts->panel_open && !block_digit_hotkeys) {
        if (key >= SDLK_1 && key <= SDLK_9) {
            pal_test_exec_action(ts, ops, (int)(key - SDLK_1));
            return 1;
        }
        if (key == SDLK_0) { pal_test_exec_action(ts, ops, TEST_ACT_NOCLIP); return 1; }
        if (key == SDLK_MINUS) { pal_test_exec_action(ts, ops, TEST_ACT_SKIP_WAIT); return 1; }
        if (key == SDLK_S && !scene_is_combat) { pal_test_exec_action(ts, ops, TEST_ACT_QUICK_SAVE); return 1; }
        if (key == SDLK_L && !scene_is_combat) { pal_test_exec_action(ts, ops, TEST_ACT_QUICK_LOAD); return 1; }
        if (key == SDLK_K) { pal_test_exec_action(ts, ops, TEST_ACT_INSTANT_KILL); return 1; }
    }

    return 0;
}

static int pt_in_rect(float lx, float ly, float x, float y, float w, float h) {
    return (lx >= x && lx <= x + w && ly >= y && ly <= y + h);
}

int pal_test_handle_mouse_down(
    PalTestState *ts, const PalTestHostOps *ops,
    float lx, float ly, int scene_is_combat
) {
    if (!ts || !ops || !ts->cfg.enabled) return 0;

    float px = 0.0f, py = 0.0f, pw = 0.0f, ph = 0.0f;
    pal_test_pill_rect(scene_is_combat, &px, &py, &pw, &ph);
    if (pt_in_rect(lx, ly, px, py, pw, ph)) {
        ts->panel_open = !ts->panel_open;
        ts->tex_dirty = 1;
        if (ops->play_sfx) ops->play_sfx(ops->ctx, ts->panel_open ? 2u : 1u);
        return 1;
    }

    if (!ts->panel_open) return 0;

    float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
    pal_test_close_rect(&cx, &cy, &cw, &ch);
    if (pt_in_rect(lx, ly, cx, cy, cw, ch)) {
        ts->panel_open = 0;
        ts->tex_dirty = 1;
        if (ops->play_sfx) ops->play_sfx(ops->ctx, 1u);
        return 1;
    }

    for (int r = 0; r < PAL_TEST_MENU_ROWS; r++) {
        float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
        pal_test_row_rect(r, &rx, &ry, &rw, &rh);
        if (pt_in_rect(lx, ly, rx, ry, rw, rh)) {
            pal_test_exec_action(ts, ops, r);
            return 1;
        }
    }

    float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
    pal_test_panel_rect(&bx, &by, &bw, &bh);
    if (!pt_in_rect(lx, ly, bx, by, bw, bh)) {
        ts->panel_open = 0;
        ts->tex_dirty = 1;
        if (ops->play_sfx) ops->play_sfx(ops->ctx, 1u);
        return 1;
    }
    return 1;
}

void pal_test_handle_mouse_move(PalTestState *ts, float lx, float ly, int scene_is_combat) {
    if (!ts || !ts->cfg.enabled) return;
    float px = 0.0f, py = 0.0f, pw = 0.0f, ph = 0.0f;
    pal_test_pill_rect(scene_is_combat, &px, &py, &pw, &ph);
    ts->hover_pill = pt_in_rect(lx, ly, px, py, pw, ph) ? 1 : 0;

    ts->hover_row = -1;
    ts->hover_close = 0;
    if (!ts->panel_open) return;

    float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
    pal_test_close_rect(&cx, &cy, &cw, &ch);
    if (pt_in_rect(lx, ly, cx, cy, cw, ch)) {
        ts->hover_close = 1;
        return;
    }
    for (int r = 0; r < PAL_TEST_MENU_ROWS; r++) {
        float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
        pal_test_row_rect(r, &rx, &ry, &rw, &rh);
        if (pt_in_rect(lx, ly, rx, ry, rw, rh)) {
            ts->hover_row = r;
            break;
        }
    }
}

void pal_test_tick(PalTestState *ts, const PalTestHostOps *ops, int scene_is_combat, float dt) {
    if (!ts) return;
    if (ts->feedback_timer > 0.0f) {
        ts->feedback_timer -= dt;
        if (ts->feedback_timer <= 0.0f) {
            ts->feedback_timer = 0.0f;
            ts->feedback[0] = '\0';
        }
    }
    if (ts->cfg.enabled && ts->cfg.auto_instant_kill && scene_is_combat && ops &&
        ops->is_in_combat && ops->is_in_combat(ops->ctx) && ops->instant_kill_combat) {
        ops->instant_kill_combat(ops->ctx);
        pal_test_set_feedback(ts, "入戰自動秒殺已觸發（測試模式）");
    }
}

static uint32_t pal_test_compute_sig(const PalTestState *ts, const PalTestHostOps *ops, int scene_is_combat) {
    int nc = (ops && ops->get_noclip) ? ops->get_noclip(ops->ctx) : 0;
    uint32_t sig = (uint32_t)(ts->cfg.enabled & 1);
    sig |= (uint32_t)((ts->cfg.god_mode & 1) << 1);
    sig |= (uint32_t)((ts->cfg.no_encounter & 1) << 2);
    sig |= (uint32_t)((ts->cfg.auto_instant_kill & 1) << 3);
    sig |= (uint32_t)((ts->cfg.speed_mult & 3) << 4);
    sig |= (uint32_t)((nc & 1) << 6);
    sig |= (uint32_t)((scene_is_combat & 1) << 7);
    sig |= (uint32_t)((ts->panel_open & 1) << 8);
    return sig;
}

static void pal_test_format_pill(const PalTestState *ts, int scene_is_combat, char *buf, size_t cap) {
    char tags[64] = "";
    if (ts->cfg.god_mode) snprintf(tags + strlen(tags), sizeof(tags) - strlen(tags), " 無敵");
    if (ts->cfg.no_encounter) snprintf(tags + strlen(tags), sizeof(tags) - strlen(tags), " 免遇");
    if (ts->cfg.auto_instant_kill) snprintf(tags + strlen(tags), sizeof(tags) - strlen(tags), " 自動秒");
    if (ts->cfg.speed_mult > 1) snprintf(tags + strlen(tags), sizeof(tags) - strlen(tags), " %d×", ts->cfg.speed_mult);
    if (tags[0] == '\0') {
        snprintf(buf, cap, scene_is_combat ? "%%C6[測試 ~] %%C3K秒殺" : "%%C6[測試工具箱 ~]");
    } else {
        snprintf(buf, cap, "%%C6[測試]%s", tags);
    }
}

static void pal_test_rebuild_textures(
    PalTestState *ts, const PalTestHostOps *ops, SDL_Renderer *renderer, int scene_is_combat
) {
    if (!ts || !ops || !ops->create_rich_text || !renderer) return;

    uint32_t sig = pal_test_compute_sig(ts, ops, scene_is_combat);
    char pill_str[96];
    pal_test_format_pill(ts, scene_is_combat, pill_str, sizeof(pill_str));
    if (ts->tex_dirty || sig != ts->last_state_sig || strcmp(pill_str, ts->last_pill_str) != 0) {
        if (ts->pill_tex) SDL_DestroyTexture(ts->pill_tex);
        ts->pill_tex = ops->create_rich_text(renderer, pill_str, 11.5f, 140, &ts->pill_w, &ts->pill_h);
        snprintf(ts->last_pill_str, sizeof(ts->last_pill_str), "%s", pill_str);
    }

    if (ts->feedback_timer > 0.0f && ts->feedback[0] != '\0') {
        if (!ts->toast_tex || strcmp(ts->feedback, ts->last_toast_str) != 0) {
            if (ts->toast_tex) SDL_DestroyTexture(ts->toast_tex);
            char tfmt[192];
            snprintf(tfmt, sizeof(tfmt), "%%C6%s", ts->feedback);
            ts->toast_tex = ops->create_rich_text(renderer, tfmt, 12.0f, 340, &ts->toast_w, &ts->toast_h);
            snprintf(ts->last_toast_str, sizeof(ts->last_toast_str), "%s", ts->feedback);
        }
    }

    if (!ts->panel_open || (!ts->tex_dirty && sig == ts->last_state_sig)) {
        ts->last_state_sig = sig;
        ts->tex_dirty = 0;
        return;
    }

    if (!ts->header_tex) {
        ts->header_tex = ops->create_rich_text(
            renderer, "%%C6測試模式工具箱  %%C5(QA 全流程輔助模組)", 14.0f, 360, &ts->header_w, &ts->header_h
        );
    }
    if (!ts->footer_tex) {
        ts->footer_tex = ops->create_rich_text(
            renderer, "%%C0[~ / F11] 面板  [F12] 總開關  [K / F4] 秒殺  [滑鼠 / 1-9] 操作", 11.0f, 396,
            &ts->footer_w, &ts->footer_h
        );
    }

    int nc = (ops->get_noclip) ? ops->get_noclip(ops->ctx) : 0;
    int in_cb = (ops->is_in_combat) ? ops->is_in_combat(ops->ctx) : 0;
    char rows[PAL_TEST_MENU_ROWS][128];
    snprintf(rows[0], sizeof(rows[0]), "%%C5[1 / F2]  人物無敵 (鎖血／免疫／免藍)    %s",
             ts->cfg.god_mode ? "%%C3[開啟]" : "%%C0[關閉]");
    snprintf(rows[1], sizeof(rows[1]), "%%C5[2 / F3]  禁止漫游遇敵 (免追擊觸發)    %s",
             ts->cfg.no_encounter ? "%%C3[開啟]" : "%%C0[關閉]");
    snprintf(rows[2], sizeof(rows[2]), "%%C5[3 / K]   戰鬥一鍵秒殺 (正常結算獎勵)  %s",
             in_cb ? "%%C3[可執行]" : "%%C0[需在戰中]");
    snprintf(rows[3], sizeof(rows[3]), "%%C5[4]      入戰自動秒殺 (極速通關劇情)  %s",
             ts->cfg.auto_instant_kill ? "%%C3[開啟]" : "%%C0[關閉]");
    snprintf(rows[4], sizeof(rows[4]), "%%C5[5 / F5]  全隊滿血滿藍 + 淨化異常狀態    %%C6[執行]");
    snprintf(rows[5], sizeof(rows[5]), "%%C5[6 / F6]  獲得測試金錢 (+50,000 文)      %%C6[執行]");
    snprintf(rows[6], sizeof(rows[6]), "%%C5[7 / F7]  在隊全員立即升級 (+1 級)       %%C6[執行]");
    snprintf(rows[7], sizeof(rows[7]), "%%C5[8]      補給測試物資 (藥／香／果／鏢)  %%C6[執行]");
    snprintf(rows[8], sizeof(rows[8]), "%%C5[9 / F8]  大圖移動倍速                  %%C3[%d×]",
             ts->cfg.speed_mult);
    snprintf(rows[9], sizeof(rows[9]), "%%C5[0 / F9]  穿牆模式 (無視地形碰撞)        %s",
             nc ? "%%C3[開啟]" : "%%C0[關閉]");
    snprintf(rows[10], sizeof(rows[10]), "%%C5[- / F10] 快進劇情等待 (跳過演出延遲)    %%C6[執行]");
    snprintf(rows[11], sizeof(rows[11]), "%%C5[S]      快速存檔至測試槽 (Slot 9)      %s",
             in_cb ? "%%C0[限地圖]" : "%%C6[存檔]");
    snprintf(rows[12], sizeof(rows[12]), "%%C5[L]      快速讀取測試槽 (Slot 9)        %%C6[讀檔]");

    for (int i = 0; i < PAL_TEST_MENU_ROWS; i++) {
        if (ts->row_tex[i]) SDL_DestroyTexture(ts->row_tex[i]);
        ts->row_tex[i] = ops->create_rich_text(renderer, rows[i], 12.0f, 384, &ts->row_w[i], &ts->row_h[i]);
    }
    ts->last_state_sig = sig;
    ts->tex_dirty = 0;
}

void pal_test_render_overlay(
    PalTestState *ts, const PalTestHostOps *ops, SDL_Renderer *renderer, int scene_is_combat
) {
    if (!ts || !ops || !renderer || !ts->cfg.enabled) return;
    pal_test_rebuild_textures(ts, ops, renderer, scene_is_combat);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* 1. Compact floating badge pill */
    float px = 0.0f, py = 0.0f, pw = 0.0f, ph = 0.0f;
    pal_test_pill_rect(scene_is_combat, &px, &py, &pw, &ph);
    SDL_FRect pr = {px, py, pw, ph};
    SDL_SetRenderDrawColor(renderer, ts->hover_pill ? 44 : 18, ts->hover_pill ? 62 : 28, ts->hover_pill ? 90 : 44, 210);
    SDL_RenderFillRect(renderer, &pr);
    SDL_SetRenderDrawColor(renderer, 224, 188, 108, 220);
    SDL_RenderRect(renderer, &pr);
    if (ts->pill_tex) {
        SDL_FRect pt = {px + (pw - (float)ts->pill_w) * 0.5f, py + (ph - (float)ts->pill_h) * 0.5f,
                        (float)ts->pill_w, (float)ts->pill_h};
        SDL_RenderTexture(renderer, ts->pill_tex, NULL, &pt);
    }

    /* 2. Transient action toast banner */
    if (ts->feedback_timer > 0.0f && ts->toast_tex) {
        float ty = scene_is_combat ? 46.0f : 8.0f;
        float tw = (float)(ts->toast_w + 22);
        if (tw > 350.0f) tw = 350.0f;
        float tx = 480.0f - tw;
        if (tx < 12.0f) tx = 12.0f;
        SDL_FRect tr = {tx, ty, tw, 22.0f};
        SDL_SetRenderDrawColor(renderer, 14, 22, 34, 220);
        SDL_RenderFillRect(renderer, &tr);
        SDL_SetRenderDrawColor(renderer, 228, 192, 114, 235);
        SDL_RenderRect(renderer, &tr);
        SDL_FRect tt = {tx + 11.0f, ty + (22.0f - (float)ts->toast_h) * 0.5f,
                        (float)ts->toast_w, (float)ts->toast_h};
        SDL_RenderTexture(renderer, ts->toast_tex, NULL, &tt);
    }

    if (!ts->panel_open) return;

    /* 3. Interactive Toolbox Overlay Modal */
    float bx = 0.0f, by = 0.0f, bw = 0.0f, bh = 0.0f;
    pal_test_panel_rect(&bx, &by, &bw, &bh);
    SDL_FRect bg = {bx, by, bw, bh};
    SDL_SetRenderDrawColor(renderer, 12, 18, 28, 238);
    SDL_RenderFillRect(renderer, &bg);
    SDL_SetRenderDrawColor(renderer, 224, 190, 112, 245);
    SDL_RenderRect(renderer, &bg);
    SDL_FRect inner = {bx + 3.0f, by + 3.0f, bw - 6.0f, bh - 6.0f};
    SDL_SetRenderDrawColor(renderer, 96, 82, 52, 160);
    SDL_RenderRect(renderer, &inner);

    if (ts->header_tex) {
        SDL_FRect hd = {bx + 14.0f, by + 9.0f, (float)ts->header_w, (float)ts->header_h};
        SDL_RenderTexture(renderer, ts->header_tex, NULL, &hd);
    }

    float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
    pal_test_close_rect(&cx, &cy, &cw, &ch);
    SDL_FRect cr = {cx, cy, cw, ch};
    SDL_SetRenderDrawColor(renderer, ts->hover_close ? 160 : 72, ts->hover_close ? 48 : 34, 38, 230);
    SDL_RenderFillRect(renderer, &cr);
    SDL_SetRenderDrawColor(renderer, 224, 188, 108, 220);
    SDL_RenderRect(renderer, &cr);
    SDL_SetRenderDrawColor(renderer, 255, 244, 214, 255);
    SDL_RenderDebugTextFormat(renderer, cx + 7.0f, cy + 6.0f, "X");

    for (int r = 0; r < PAL_TEST_MENU_ROWS; r++) {
        float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
        pal_test_row_rect(r, &rx, &ry, &rw, &rh);
        SDL_FRect rr = {rx, ry, rw, rh};
        int hov = (ts->hover_row == r);
        SDL_SetRenderDrawColor(renderer, hov ? 42 : 22, hov ? 64 : 32, hov ? 94 : 48, hov ? 230 : 170);
        SDL_RenderFillRect(renderer, &rr);
        if (hov) {
            SDL_SetRenderDrawColor(renderer, 232, 198, 118, 225);
            SDL_RenderRect(renderer, &rr);
        }
        if (ts->row_tex[r]) {
            SDL_FRect rd = {rx + 8.0f, ry + (rh - (float)ts->row_h[r]) * 0.5f,
                            (float)ts->row_w[r], (float)ts->row_h[r]};
            SDL_RenderTexture(renderer, ts->row_tex[r], NULL, &rd);
        }
    }

    if (ts->footer_tex) {
        SDL_FRect fd = {bx + 12.0f, by + bh - 26.0f, (float)ts->footer_w, (float)ts->footer_h};
        SDL_RenderTexture(renderer, ts->footer_tex, NULL, &fd);
    }
}

#else

typedef int pal_test_mode_disabled_unit_t;

#endif /* PAL_ENABLE_TEST_MODE */
