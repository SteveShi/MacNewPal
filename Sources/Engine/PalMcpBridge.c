#include "PalMcpBridge.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static PalMcpBridge *g_active_mcp_bridge = NULL;

/* =========================================================================
 * Dynamic JSON String Buffer
 * ========================================================================= */

void pal_jbuf_init(PalJsonBuf *jb) {
    if (!jb) return;
    jb->cap = 1024;
    jb->len = 0;
    jb->data = (char *)malloc(jb->cap);
    if (jb->data) jb->data[0] = '\0';
}

void pal_jbuf_free(PalJsonBuf *jb) {
    if (!jb) return;
    free(jb->data);
    jb->data = NULL;
    jb->len = 0;
    jb->cap = 0;
}

void pal_jbuf_reset(PalJsonBuf *jb) {
    if (!jb) return;
    jb->len = 0;
    if (jb->data) jb->data[0] = '\0';
}

static void pal_jbuf_ensure(PalJsonBuf *jb, size_t extra) {
    if (!jb) return;
    if (jb->len + extra + 1 <= jb->cap) return;
    size_t ncap = jb->cap ? jb->cap : 1024;
    while (ncap < jb->len + extra + 1) {
        ncap *= 2;
    }
    char *nd = (char *)realloc(jb->data, ncap);
    if (!nd) return;
    jb->data = nd;
    jb->cap = ncap;
}

void pal_jbuf_append_raw(PalJsonBuf *jb, const char *s, size_t n) {
    if (!jb || !s || n == 0) return;
    pal_jbuf_ensure(jb, n);
    if (!jb->data || jb->len + n + 1 > jb->cap) return;
    memcpy(jb->data + jb->len, s, n);
    jb->len += n;
    jb->data[jb->len] = '\0';
}

void pal_jbuf_append_cstr(PalJsonBuf *jb, const char *s) {
    if (!s) return;
    pal_jbuf_append_raw(jb, s, strlen(s));
}

void pal_jbuf_append_fmt(PalJsonBuf *jb, const char *fmt, ...) {
    if (!jb || !fmt) return;
    char stack_buf[512];
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
    va_end(ap);
    if (needed >= 0 && (size_t)needed < sizeof(stack_buf)) {
        va_end(ap2);
        pal_jbuf_append_raw(jb, stack_buf, (size_t)needed);
        return;
    }
    if (needed > 0) {
        size_t sz = (size_t)needed + 1;
        char *heap_buf = (char *)malloc(sz);
        if (heap_buf) {
            vsnprintf(heap_buf, sz, fmt, ap2);
            pal_jbuf_append_raw(jb, heap_buf, (size_t)needed);
            free(heap_buf);
        }
    }
    va_end(ap2);
}

void pal_jbuf_append_json_str(PalJsonBuf *jb, const char *utf8) {
    pal_jbuf_append_raw(jb, "\"", 1);
    if (utf8) {
        const unsigned char *p = (const unsigned char *)utf8;
        while (*p) {
            unsigned char c = *p++;
            switch (c) {
                case '\"': pal_jbuf_append_raw(jb, "\\\"", 2); break;
                case '\\': pal_jbuf_append_raw(jb, "\\\\", 2); break;
                case '\b': pal_jbuf_append_raw(jb, "\\b", 2); break;
                case '\f': pal_jbuf_append_raw(jb, "\\f", 2); break;
                case '\n': pal_jbuf_append_raw(jb, "\\n", 2); break;
                case '\r': pal_jbuf_append_raw(jb, "\\r", 2); break;
                case '\t': pal_jbuf_append_raw(jb, "\\t", 2); break;
                default:
                    if (c < 0x20) {
                        pal_jbuf_append_fmt(jb, "\\u%04x", (unsigned)c);
                    } else {
                        pal_jbuf_append_raw(jb, (const char *)&c, 1);
                    }
                    break;
            }
        }
    }
    pal_jbuf_append_raw(jb, "\"", 1);
}

/* =========================================================================
 * Flat JSON Request Parser (for trusted local Swift MCP server NDJSON)
 * ========================================================================= */

static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *parse_json_string(const char *p, char *out, size_t cap) {
    if (*p != '\"') return NULL;
    p++;
    size_t len = 0;
    while (*p && *p != '\"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\' && *p) {
            char esc = *p++;
            switch (esc) {
                case '\"': c = '\"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/';  break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    unsigned code = 0;
                    for (int i = 0; i < 4 && isxdigit((unsigned char)*p); i++) {
                        char hc = *p++;
                        unsigned v = (unsigned)((hc >= '0' && hc <= '9') ? (hc - '0') :
                                     (hc >= 'a' && hc <= 'f') ? (hc - 'a' + 10) : (hc - 'A' + 10));
                        code = (code << 4) | v;
                    }
                    c = (unsigned char)(code & 0x7Fu);
                    break;
                }
                default: c = (unsigned char)esc; break;
            }
        }
        if (out && len + 1 < cap) {
            out[len++] = (char)c;
        }
    }
    if (out && cap > 0) out[len] = '\0';
    if (*p != '\"') return NULL;
    return p + 1;
}

static const char *skip_json_value(const char *p) {
    p = skip_ws(p);
    if (*p == '\"') {
        return parse_json_string(p, NULL, 0);
    }
    if (*p == '{' || *p == '[') {
        char open_ch = *p;
        char close_ch = (open_ch == '{') ? '}' : ']';
        int depth = 1;
        p++;
        while (*p && depth > 0) {
            if (*p == '\"') {
                p = parse_json_string(p, NULL, 0);
                if (!p) return NULL;
                continue;
            }
            if (*p == open_ch) depth++;
            else if (*p == close_ch) depth--;
            p++;
        }
        return (depth == 0) ? p : NULL;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']' && !isspace((unsigned char)*p)) {
        p++;
    }
    return p;
}

static void assign_req_int(PalMcpReq *r, const char *key, int64_t val) {
    if (strcmp(key, "id") == 0) { r->id = val; }
    else if (strcmp(key, "tx") == 0) { r->tx = (int32_t)val; r->has_tx = 1; }
    else if (strcmp(key, "ty") == 0) { r->ty = (int32_t)val; r->has_ty = 1; }
    else if (strcmp(key, "map_id") == 0) { r->map_id = (int32_t)val; r->has_map_id = 1; }
    else if (strcmp(key, "steps") == 0) { r->steps = (int32_t)val; r->has_steps = 1; }
    else if (strcmp(key, "npc_obj_id") == 0) { r->npc_obj_id = (int32_t)val; r->has_npc_obj_id = 1; }
    else if (strcmp(key, "trigger_id") == 0) { r->trigger_id = (int32_t)val; r->has_trigger_id = 1; }
    else if (strcmp(key, "slot") == 0) { r->slot = (int32_t)val; r->has_slot = 1; }
    else if (strcmp(key, "index") == 0) { r->index = (int32_t)val; r->has_index = 1; }
    else if (strcmp(key, "hero_idx") == 0) { r->hero_idx = (int32_t)val; r->has_hero_idx = 1; }
    else if (strcmp(key, "item_id") == 0) { r->item_id = (int32_t)val; r->has_item_id = 1; }
    else if (strcmp(key, "skill_id") == 0) { r->skill_id = (int32_t)val; r->has_skill_id = 1; }
    else if (strcmp(key, "target_idx") == 0) { r->target_idx = (int32_t)val; r->has_target_idx = 1; }
    else if (strcmp(key, "equip_slot") == 0) { r->equip_slot = (int32_t)val; r->has_equip_slot = 1; }
    else if (strcmp(key, "qty") == 0) { r->qty = (int32_t)val; r->has_qty = 1; }
    else if (strcmp(key, "scale") == 0) { r->scale = (int32_t)val; r->has_scale = 1; }
    else if (strcmp(key, "radius") == 0) { r->radius = (int32_t)val; r->has_radius = 1; }
    else if (strcmp(key, "flag_id") == 0) { r->flag_id = (int32_t)val; r->has_flag_id = 1; }
    else if (strcmp(key, "flag_end") == 0) { r->flag_end = (int32_t)val; r->has_flag_end = 1; }
    else if (strcmp(key, "flag_val") == 0) { r->flag_val = (int32_t)val; r->has_flag_val = 1; }
    else if (strcmp(key, "frames") == 0) { r->frames = (int32_t)val; r->has_frames = 1; }
    else if (strcmp(key, "battle_id") == 0) { r->battle_id = (int32_t)val; r->has_battle_id = 1; }
    else if (strcmp(key, "timeout_ms") == 0) { r->timeout_ms = (int32_t)val; r->has_timeout_ms = 1; }
    else if (strcmp(key, "x") == 0) { r->x = (int32_t)val; r->has_x = 1; }
    else if (strcmp(key, "y") == 0) { r->y = (int32_t)val; r->has_y = 1; }
    else if (strcmp(key, "speed_mult") == 0) { r->speed_mult = (int32_t)val; r->has_speed_mult = 1; }
    else if (strcmp(key, "add_money") == 0) { r->add_money = (int32_t)val; r->has_add_money = 1; }
    else if (strcmp(key, "level_up") == 0) {
        if (val > 0) { r->level_up_count = (int32_t)val; r->has_level_up_count = 1; r->level_up = 1; r->has_level_up = 1; }
        else { r->level_up = 0; r->has_level_up = 1; }
    }
    else if (strcmp(key, "level_up_count") == 0) { r->level_up_count = (int32_t)val; r->has_level_up_count = 1; }
    else if (strcmp(key, "target_level") == 0) { r->target_level = (int32_t)val; r->has_target_level = 1; }
    else if (strcmp(key, "dir") == 0) { snprintf(r->dir, sizeof(r->dir), "%lld", (long long)val); r->has_dir = 1; }
}

static void assign_req_bool(PalMcpReq *r, const char *key, int32_t bval) {
    if (strcmp(key, "wait") == 0) { r->wait = bval; r->has_wait = 1; }
    else if (strcmp(key, "god_mode") == 0) { r->god_mode = bval; r->has_god_mode = 1; }
    else if (strcmp(key, "no_encounter") == 0) { r->no_encounter = bval; r->has_no_encounter = 1; }
    else if (strcmp(key, "auto_instant_kill") == 0 || strcmp(key, "auto_kill") == 0) {
        r->auto_kill = bval; r->has_auto_kill = 1;
    }
    else if (strcmp(key, "noclip") == 0) { r->noclip = bval; r->has_noclip = 1; }
    else if (strcmp(key, "skip_wait") == 0) { r->skip_wait = bval; r->has_skip_wait = 1; }
    else if (strcmp(key, "heal_all") == 0) { r->heal_all = bval; r->has_heal_all = 1; }
    else if (strcmp(key, "level_up") == 0) { r->level_up = bval; r->has_level_up = 1; }
}

static void assign_req_str(PalMcpReq *r, const char *key, const char *val) {
    if (strcmp(key, "cmd") == 0) snprintf(r->cmd, sizeof(r->cmd), "%s", val);
    else if (strcmp(key, "action") == 0) snprintf(r->action, sizeof(r->action), "%s", val);
    else if (strcmp(key, "str_arg") == 0 || strcmp(key, "key") == 0 || strcmp(key, "flags_csv") == 0) {
        snprintf(r->str_arg, sizeof(r->str_arg), "%s", val);
    }
    else if (strcmp(key, "button") == 0) snprintf(r->button, sizeof(r->button), "%s", val);
    else if (strcmp(key, "dir") == 0) { snprintf(r->dir, sizeof(r->dir), "%s", val); r->has_dir = 1; }
}

int pal_mcp_parse_req_json(const char *json_line, PalMcpReq *out_req) {
    if (!json_line || !out_req) return -1;
    memset(out_req, 0, sizeof(*out_req));
    const char *p = skip_ws(json_line);
    if (*p != '{') return -1;
    p++;
    while (*p) {
        p = skip_ws(p);
        if (*p == '}') return (out_req->cmd[0] != '\0') ? 0 : -1;
        if (*p == ',') { p++; continue; }

        char key[64];
        p = parse_json_string(p, key, sizeof(key));
        if (!p) return -1;
        p = skip_ws(p);
        if (*p != ':') return -1;
        p = skip_ws(p + 1);

        if (*p == '\"') {
            char val[256];
            p = parse_json_string(p, val, sizeof(val));
            if (!p) return -1;
            assign_req_str(out_req, key, val);
        } else if (strncmp(p, "true", 4) == 0 && (p[4] == ',' || p[4] == '}' || isspace((unsigned char)p[4]))) {
            assign_req_bool(out_req, key, 1);
            p += 4;
        } else if (strncmp(p, "false", 5) == 0 && (p[5] == ',' || p[5] == '}' || isspace((unsigned char)p[5]))) {
            assign_req_bool(out_req, key, 0);
            p += 5;
        } else if (strncmp(p, "null", 4) == 0 && (p[4] == ',' || p[4] == '}' || isspace((unsigned char)p[4]))) {
            p += 4;
        } else if (*p == '-' || isdigit((unsigned char)*p)) {
            char *endp = NULL;
            long long num = strtoll(p, &endp, 10);
            if (*endp == '.' || *endp == 'e' || *endp == 'E') {
                double dval = strtod(p, &endp);
                num = (long long)dval;
            }
            if (endp == p) return -1;
            assign_req_int(out_req, key, (int64_t)num);
            p = endp;
        } else {
            p = skip_json_value(p);
            if (!p) return -1;
        }
    }
    return -1;
}

/* =========================================================================
 * Socket Write & Wire Framing
 * ========================================================================= */

static int write_all_fd(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    int stall_retries = 0;
    while (off < len) {
        size_t chunk = (len - off > 65536u) ? 65536u : (len - off);
        ssize_t n = write(fd, p + off, chunk);
        if (n > 0) {
            off += (size_t)n;
            stall_retries = 0;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (++stall_retries > 500) return -1;
            struct pollfd pfd = {fd, POLLOUT, 0};
            if (poll(&pfd, 1, 10) < 0) {
                if (errno == EINTR) continue;
                return -1;
            }
            if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                return -1;
            }
            continue;
        }
        return -1;
    }
    return 0;
}

static void mcp_send_error(PalMcpBridge *b, int64_t id, const char *err_msg) {
    if (!b || b->client_fd < 0) return;
    PalJsonBuf jb;
    pal_jbuf_init(&jb);
    pal_jbuf_append_fmt(&jb, "{\"id\":%lld,\"ok\":false,\"error\":", (long long)id);
    pal_jbuf_append_json_str(&jb, err_msg ? err_msg : "Unknown error");
    pal_jbuf_append_raw(&jb, "}\n", 2);
    if (write_all_fd(b->client_fd, jb.data, jb.len) != 0) {
        close(b->client_fd);
        b->client_fd = -1;
    }
    pal_jbuf_free(&jb);
}

static void mcp_send_ok_obj(PalMcpBridge *b, int64_t id, const char *data_json_obj) {
    if (!b || b->client_fd < 0) return;
    PalJsonBuf jb;
    pal_jbuf_init(&jb);
    pal_jbuf_append_fmt(&jb, "{\"id\":%lld,\"ok\":true,\"data\":%s}\n",
                        (long long)id, (data_json_obj && data_json_obj[0]) ? data_json_obj : "{}");
    if (write_all_fd(b->client_fd, jb.data, jb.len) != 0) {
        close(b->client_fd);
        b->client_fd = -1;
    }
    pal_jbuf_free(&jb);
}

static int set_nonblocking_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int fdfl = fcntl(fd, F_GETFD, 0);
    if (fdfl >= 0) fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    int buf_sz = 262144;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_sz, sizeof(buf_sz));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_sz, sizeof(buf_sz));
    return 0;
}

static int is_socket_alive(const char *path) {
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) return 0;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        close(probe);
        return 0;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    int rc = connect(probe, (struct sockaddr *)&addr, (socklen_t)sizeof(addr));
    int alive = (rc == 0 || errno == EINPROGRESS || errno == EAGAIN) ? 1 : 0;
    close(probe);
    return alive;
}

int pal_mcp_init(PalMcpBridge *b, const char *socket_path, int allow_qa) {
    if (!b) return -1;
    memset(b, 0, sizeof(*b));
    b->listen_fd = -1;
    b->client_fd = -1;
    b->allow_qa_tools = allow_qa ? 1 : 0;
    pal_jbuf_init(&b->pending_extra);

    const char *path = (socket_path && socket_path[0] != '\0') ? socket_path : PAL_MCP_DEFAULT_SOCKET;
    if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        return -1;
    }
    snprintf(b->socket_path, sizeof(b->socket_path), "%s", path);

    struct stat st;
    if (lstat(b->socket_path, &st) == 0) {
        if (is_socket_alive(b->socket_path)) {
            return -1;
        }
        unlink(b->socket_path);
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", b->socket_path);

    if (bind(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    chmod(b->socket_path, 0600);
    if (listen(fd, 4) != 0) {
        close(fd);
        unlink(b->socket_path);
        return -1;
    }
    set_nonblocking_cloexec(fd);
    b->listen_fd = fd;
    b->enabled = 1;
    return 0;
}

void pal_mcp_shutdown(PalMcpBridge *b) {
    if (!b) return;
    if (g_active_mcp_bridge == b) g_active_mcp_bridge = NULL;
    if (b->client_fd >= 0) {
        close(b->client_fd);
        b->client_fd = -1;
    }
    if (b->listen_fd >= 0) {
        close(b->listen_fd);
        b->listen_fd = -1;
    }
    if (b->enabled && b->socket_path[0] != '\0') {
        unlink(b->socket_path);
    }
    pal_jbuf_free(&b->pending_extra);
    b->enabled = 0;
    b->has_pending = 0;
    b->capture_pending = 0;
}

void pal_mcp_bind_active(PalMcpBridge *b) {
    g_active_mcp_bridge = b;
}

void pal_mcp_unbind_active(PalMcpBridge *b) {
    if (g_active_mcp_bridge == b) g_active_mcp_bridge = NULL;
}

static void mcp_accept_waiting_client(PalMcpBridge *b) {
    if (!b || !b->enabled || b->listen_fd < 0) return;
    for (;;) {
        int cfd = accept(b->listen_fd, NULL, NULL);
        if (cfd < 0) break;
        set_nonblocking_cloexec(cfd);
        if (b->client_fd >= 0) {
            close(b->client_fd);
        }
        b->client_fd = cfd;
        b->rx_len = 0;
        b->has_pending = 0;
        b->capture_pending = 0;
        pal_jbuf_reset(&b->pending_extra);
    }
}

static void mcp_finish_pending(PalMcpBridge *b, const PalMcpHostOps *ops, const char *wait_reason) {
    if (!b || !b->has_pending) return;
    PalJsonBuf jb;
    pal_jbuf_init(&jb);
    pal_jbuf_append_raw(&jb, "{", 1);
    if (wait_reason && wait_reason[0] != '\0') {
        pal_jbuf_append_cstr(&jb, "\"wait_result\":");
        pal_jbuf_append_json_str(&jb, wait_reason);
        pal_jbuf_append_raw(&jb, ",", 1);
    }
    if (b->pending_extra.len > 0 && b->pending_extra.data) {
        pal_jbuf_append_raw(&jb, b->pending_extra.data, b->pending_extra.len);
        pal_jbuf_append_raw(&jb, ",", 1);
    }
    pal_jbuf_append_cstr(&jb, "\"state\":");
    if (ops && ops->build_state_json) {
        PalJsonBuf st;
        pal_jbuf_init(&st);
        ops->build_state_json(ops->ctx, &st);
        pal_jbuf_append_cstr(&jb, (st.data && st.len > 0) ? st.data : "{}");
        pal_jbuf_free(&st);
    } else {
        pal_jbuf_append_cstr(&jb, "{}");
    }
    pal_jbuf_append_raw(&jb, "}", 1);

    mcp_send_ok_obj(b, b->pending_id, jb.data);
    pal_jbuf_free(&jb);

    b->has_pending = 0;
    b->wait_kind = PAL_MCP_WAIT_NONE;
    pal_jbuf_reset(&b->pending_extra);
}

static void mcp_handle_line(PalMcpBridge *b, const PalMcpHostOps *ops, const char *line, uint64_t now_ms) {
    PalMcpReq req;
    if (pal_mcp_parse_req_json(line, &req) != 0) {
        mcp_send_error(b, 0, "Invalid MCP bridge JSON request");
        return;
    }

    if (strcmp(req.cmd, "ping") == 0) {
        mcp_send_ok_obj(b, req.id, "{\"pong\":true}");
        return;
    }
    if (strcmp(req.cmd, "quit") == 0) {
        mcp_send_ok_obj(b, req.id, "{\"quitting\":true}");
        if (ops && ops->request_quit) ops->request_quit(ops->ctx);
        return;
    }
    if (!ops) {
        mcp_send_error(b, req.id, "Host operations unavailable");
        return;
    }

    if (strcmp(req.cmd, "get_state") == 0) {
        PalJsonBuf st;
        pal_jbuf_init(&st);
        if (ops->build_state_json) ops->build_state_json(ops->ctx, &st);
        mcp_send_ok_obj(b, req.id, st.data);
        pal_jbuf_free(&st);
        return;
    }
    if (strcmp(req.cmd, "get_map_entities") == 0) {
        PalJsonBuf out;
        pal_jbuf_init(&out);
        int rad = req.has_radius ? req.radius : 12;
        if (ops->build_map_entities_json) ops->build_map_entities_json(ops->ctx, rad, &out);
        mcp_send_ok_obj(b, req.id, out.data);
        pal_jbuf_free(&out);
        return;
    }
    if (strcmp(req.cmd, "get_inventory_and_saves") == 0) {
        PalJsonBuf out;
        pal_jbuf_init(&out);
        if (ops->build_inventory_saves_json) ops->build_inventory_saves_json(ops->ctx, &out);
        mcp_send_ok_obj(b, req.id, out.data);
        pal_jbuf_free(&out);
        return;
    }
    if (strcmp(req.cmd, "query_flags") == 0) {
        PalJsonBuf out;
        pal_jbuf_init(&out);
        int fs = req.has_flag_id ? req.flag_id : 0;
        int fe = req.has_flag_end ? req.flag_end : fs;
        if (ops->build_flags_json) ops->build_flags_json(ops->ctx, fs, fe, req.str_arg, 1, &out);
        mcp_send_ok_obj(b, req.id, out.data);
        pal_jbuf_free(&out);
        return;
    }
    if (strcmp(req.cmd, "capture_screen") == 0) {
        if (b->has_pending || b->capture_pending) {
            mcp_send_error(b, req.id, "Another asynchronous command is in progress");
            return;
        }
        b->capture_pending = 1;
        b->capture_scale = (req.has_scale && req.scale == 2) ? 2 : 1;
        b->pending_id = req.id;
        return;
    }

    if (b->has_pending || b->capture_pending) {
        mcp_send_error(b, req.id, "Another asynchronous command is in progress");
        return;
    }

    PalMcpWaitKind wait_kind = PAL_MCP_WAIT_NONE;
    int wait_param = 0;
    char err[256] = {0};
    pal_jbuf_reset(&b->pending_extra);

    if (!ops->exec_command) {
        mcp_send_error(b, req.id, "Command executor unavailable");
        return;
    }
    int rc = ops->exec_command(
        ops->ctx, &req, b->allow_qa_tools,
        &wait_kind, &wait_param, &b->pending_extra, err, sizeof(err)
    );
    if (rc != 0) {
        pal_jbuf_reset(&b->pending_extra);
        mcp_send_error(b, req.id, err[0] ? err : "Command execution failed");
        return;
    }

    if (wait_kind != PAL_MCP_WAIT_NONE) {
        int32_t timeout_ms = req.has_timeout_ms ? req.timeout_ms : 15000;
        if (timeout_ms < 200) timeout_ms = 200;
        if (timeout_ms > 60000) timeout_ms = 60000;
        b->has_pending = 1;
        b->pending_id = req.id;
        snprintf(b->pending_cmd, sizeof(b->pending_cmd), "%s", req.cmd);
        b->wait_kind = wait_kind;
        b->wait_param = wait_param;
        b->wait_deadline_ms = now_ms + (uint64_t)timeout_ms;
    } else {
        b->has_pending = 1;
        b->pending_id = req.id;
        mcp_finish_pending(b, ops, NULL);
    }
}

void pal_mcp_tick(PalMcpBridge *b, const PalMcpHostOps *ops, uint64_t now_ms) {
    if (!b || !b->enabled) return;

    mcp_accept_waiting_client(b);

    if (b->has_pending && ops && ops->check_wait) {
        char reason[64] = {0};
        if (ops->check_wait(ops->ctx, b->wait_kind, b->wait_param, reason, sizeof(reason))) {
            mcp_finish_pending(b, ops, reason[0] ? reason : "completed");
        } else if (now_ms >= b->wait_deadline_ms) {
            mcp_finish_pending(b, ops, "timeout");
        }
    }

    if (b->client_fd < 0) return;
    if (b->has_pending || b->capture_pending) return;

    /* Read non-blockingly until one newline-terminated NDJSON command is available */
    for (;;) {
        if (b->rx_len > 0) {
            char *nl = (char *)memchr(b->rx_buf, '\n', b->rx_len);
            if (nl) {
                size_t line_len = (size_t)(nl - b->rx_buf);
                char line_copy[16384];
                if (line_len >= sizeof(line_copy)) line_len = sizeof(line_copy) - 1;
                memcpy(line_copy, b->rx_buf, line_len);
                line_copy[line_len] = '\0';

                size_t rem = b->rx_len - (size_t)(nl - b->rx_buf) - 1;
                if (rem > 0) memmove(b->rx_buf, nl + 1, rem);
                b->rx_len = rem;

                if (line_len > 0) {
                    mcp_handle_line(b, ops, line_copy, now_ms);
                    return;
                }
                continue;
            }
        }

        if (b->rx_len + 1 >= sizeof(b->rx_buf)) {
            b->rx_len = 0;
            mcp_send_error(b, 0, "Request line exceeds 16KB buffer");
            return;
        }

        ssize_t n = read(b->client_fd, b->rx_buf + b->rx_len, sizeof(b->rx_buf) - 1 - b->rx_len);
        if (n > 0) {
            b->rx_len += (size_t)n;
            b->rx_buf[b->rx_len] = '\0';
            continue;
        }
        if (n == 0) {
            close(b->client_fd);
            b->client_fd = -1;
            b->rx_len = 0;
            return;
        }
        if (errno == EINTR) continue;
        /* EAGAIN / EWOULDBLOCK: nothing more buffered right now */
        return;
    }
}

int pal_mcp_capture_requested(const PalMcpBridge *b, int *out_scale) {
    if (!b || !b->enabled || !b->capture_pending || b->client_fd < 0) return 0;
    if (out_scale) *out_scale = b->capture_scale ? b->capture_scale : 1;
    return 1;
}

void pal_mcp_submit_capture_rgba(PalMcpBridge *b, const uint8_t *rgba, int w, int h, const PalMcpHostOps *ops) {
    if (!b || !b->capture_pending) return;
    int64_t req_id = b->pending_id;
    b->capture_pending = 0;

    if (b->client_fd < 0) return;
    if (!rgba || w <= 0 || h <= 0) {
        mcp_send_error(b, req_id, "Failed to read rendered pixels for screenshot");
        return;
    }

    size_t byte_len = (size_t)w * (size_t)h * 4u;
    PalJsonBuf hdr;
    pal_jbuf_init(&hdr);
    pal_jbuf_append_fmt(&hdr,
                        "{\"id\":%lld,\"ok\":true,\"binary_rgba\":true,\"width\":%d,\"height\":%d,\"bytes\":%zu,\"state\":",
                        (long long)req_id, w, h, byte_len);
    if (ops && ops->build_state_json) {
        PalJsonBuf st;
        pal_jbuf_init(&st);
        ops->build_state_json(ops->ctx, &st);
        pal_jbuf_append_cstr(&hdr, (st.data && st.len > 0) ? st.data : "{}");
        pal_jbuf_free(&st);
    } else {
        pal_jbuf_append_cstr(&hdr, "{}");
    }
    pal_jbuf_append_raw(&hdr, "}\n", 2);

    if (write_all_fd(b->client_fd, hdr.data, hdr.len) != 0 ||
        write_all_fd(b->client_fd, rgba, byte_len) != 0) {
        close(b->client_fd);
        b->client_fd = -1;
    }
    pal_jbuf_free(&hdr);
}

int pal_mcp_poll_video_skip(void) {
    PalMcpBridge *b = g_active_mcp_bridge;
    if (!b || !b->enabled) return 0;
    if (b->video_skip_requested) {
        b->video_skip_requested = 0;
        return 1;
    }
    mcp_accept_waiting_client(b);
    if (b->client_fd < 0) return 0;

    for (;;) {
        if (b->rx_len > 0) {
            char *nl = (char *)memchr(b->rx_buf, '\n', b->rx_len);
            if (nl) {
                size_t line_len = (size_t)(nl - b->rx_buf);
                char line_copy[2048];
                if (line_len >= sizeof(line_copy)) line_len = sizeof(line_copy) - 1;
                memcpy(line_copy, b->rx_buf, line_len);
                line_copy[line_len] = '\0';
                size_t rem = b->rx_len - (size_t)(nl - b->rx_buf) - 1;
                if (rem > 0) memmove(b->rx_buf, nl + 1, rem);
                b->rx_len = rem;

                PalMcpReq req;
                if (pal_mcp_parse_req_json(line_copy, &req) == 0) {
                    if (strcmp(req.cmd, "ping") == 0) {
                        mcp_send_ok_obj(b, req.id, "{\"pong\":true,\"playing_video\":true}");
                    } else if (strcmp(req.cmd, "quit") == 0) {
                        mcp_send_ok_obj(b, req.id, "{\"quitting\":true}");
                        return 2;
                    } else if (strcmp(req.cmd, "raw_input") == 0 &&
                               (strcmp(req.action, "skip_video") == 0 ||
                                strcmp(req.str_arg, "escape") == 0 ||
                                strcmp(req.str_arg, "space") == 0 ||
                                strcmp(req.str_arg, "return") == 0)) {
                        mcp_send_ok_obj(b, req.id, "{\"video_skipped\":true}");
                        return 1;
                    } else if (strcmp(req.cmd, "get_state") == 0) {
                        mcp_send_ok_obj(b, req.id, "{\"scene\":\"video\",\"sub_state\":\"playing_video\"}");
                    } else {
                        mcp_send_error(b, req.id, "Cutscene video playing; send raw_input skip_video/escape to skip");
                    }
                }
                continue;
            }
        }
        ssize_t n = read(b->client_fd, b->rx_buf + b->rx_len, sizeof(b->rx_buf) - 1 - b->rx_len);
        if (n > 0) {
            b->rx_len += (size_t)n;
            b->rx_buf[b->rx_len] = '\0';
            continue;
        }
        if (n == 0) {
            close(b->client_fd);
            b->client_fd = -1;
            b->rx_len = 0;
        }
        break;
    }
    return 0;
}
