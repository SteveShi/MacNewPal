#ifndef PAL_MCP_BRIDGE_H
#define PAL_MCP_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAL_MCP_DEFAULT_SOCKET "/tmp/newpal-mcp.sock"

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} PalJsonBuf;

void pal_jbuf_init(PalJsonBuf *jb);
void pal_jbuf_free(PalJsonBuf *jb);
void pal_jbuf_reset(PalJsonBuf *jb);
void pal_jbuf_append_raw(PalJsonBuf *jb, const char *s, size_t n);
void pal_jbuf_append_cstr(PalJsonBuf *jb, const char *s);
void pal_jbuf_append_fmt(PalJsonBuf *jb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void pal_jbuf_append_json_str(PalJsonBuf *jb, const char *utf8);

typedef enum {
    PAL_MCP_WAIT_NONE = 0,
    PAL_MCP_WAIT_MOVE = 1,
    PAL_MCP_WAIT_COMBAT = 2,
    PAL_MCP_WAIT_TALK = 3,
    PAL_MCP_WAIT_FRAMES = 4,
    PAL_MCP_WAIT_CAPTURE = 5
} PalMcpWaitKind;

typedef struct {
    int64_t id;
    char cmd[48];
    char action[48];
    char str_arg[256];
    char button[16];
    char dir[16];

    int32_t tx, ty, map_id, steps;
    int32_t npc_obj_id, trigger_id;
    int32_t slot, index, hero_idx;
    int32_t item_id, skill_id, target_idx, equip_slot, qty;
    int32_t scale, radius;
    int32_t flag_id, flag_end, flag_val;
    int32_t frames, battle_id, timeout_ms;
    int32_t x, y;

    int32_t wait;
    int32_t god_mode, no_encounter, auto_kill, speed_mult, noclip, skip_wait, heal_all, level_up;
    int32_t level_up_count, target_level;
    int32_t add_money;

    int32_t has_tx, has_ty, has_map_id, has_dir, has_steps;
    int32_t has_npc_obj_id, has_trigger_id, has_slot, has_index, has_hero_idx;
    int32_t has_item_id, has_skill_id, has_target_idx, has_equip_slot, has_qty;
    int32_t has_scale, has_radius, has_flag_id, has_flag_end, has_flag_val;
    int32_t has_frames, has_battle_id, has_wait, has_timeout_ms;
    int32_t has_god_mode, has_no_encounter, has_auto_kill, has_speed_mult, has_noclip, has_skip_wait;
    int32_t has_heal_all, has_level_up, has_level_up_count, has_target_level, has_add_money, has_x, has_y;
} PalMcpReq;

int pal_mcp_parse_req_json(const char *json_line, PalMcpReq *out_req);

typedef struct {
    void *ctx;
    void (*build_state_json)(void *ctx, PalJsonBuf *out);
    void (*build_map_entities_json)(void *ctx, int radius, PalJsonBuf *out);
    void (*build_inventory_saves_json)(void *ctx, PalJsonBuf *out);
    void (*build_flags_json)(void *ctx, int flag_start, int flag_end, const char *list_csv, int include_vars, PalJsonBuf *out);
    int (*exec_command)(
        void *ctx, const PalMcpReq *req, int allow_qa,
        PalMcpWaitKind *out_wait, int *out_wait_param,
        PalJsonBuf *out_extra, char *err_buf, size_t err_cap
    );
    int (*check_wait)(void *ctx, PalMcpWaitKind kind, int param, char *reason_buf, size_t reason_cap);
    void (*request_quit)(void *ctx);
} PalMcpHostOps;

typedef struct {
    int enabled;
    int allow_qa_tools;
    int listen_fd;
    int client_fd;
    char socket_path[256];

    char rx_buf[16384];
    size_t rx_len;

    int has_pending;
    int64_t pending_id;
    char pending_cmd[48];
    PalMcpWaitKind wait_kind;
    int wait_param;
    uint64_t wait_deadline_ms;
    int capture_pending;
    int capture_scale;
    PalJsonBuf pending_extra;

    int video_skip_requested;
} PalMcpBridge;

int  pal_mcp_init(PalMcpBridge *b, const char *socket_path, int allow_qa);
void pal_mcp_shutdown(PalMcpBridge *b);
void pal_mcp_bind_active(PalMcpBridge *b);
void pal_mcp_unbind_active(PalMcpBridge *b);
void pal_mcp_tick(PalMcpBridge *b, const PalMcpHostOps *ops, uint64_t now_ms);
int  pal_mcp_capture_requested(const PalMcpBridge *b, int *out_scale);
void pal_mcp_submit_capture_rgba(PalMcpBridge *b, const uint8_t *rgba, int w, int h, const PalMcpHostOps *ops);
int  pal_mcp_poll_video_skip(void);

#ifdef __cplusplus
}
#endif

#endif /* PAL_MCP_BRIDGE_H */
