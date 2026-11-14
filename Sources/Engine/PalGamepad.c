#include "PalGamepad.h"
#include <string.h>

#define PAL_GP_AXIS_DEADZONE 10500
#define PAL_GP_TRIGGER_THRESHOLD 12000

static void pal_gamepad_attach_id(PalGamepadState *gp, SDL_JoystickID jid) {
    if (!gp || jid == 0 || !SDL_IsGamepad(jid)) return;
    for (int i = 0; i < PAL_MAX_GAMEPADS; i++) {
        if (gp->ids[i] == jid && gp->pads[i] != NULL) return;
    }
    for (int i = 0; i < PAL_MAX_GAMEPADS; i++) {
        if (gp->pads[i] == NULL) {
            SDL_Gamepad *pad = SDL_OpenGamepad(jid);
            if (pad) {
                gp->pads[i] = pad;
                gp->ids[i] = jid;
                gp->count++;
            }
            return;
        }
    }
}

static void pal_gamepad_detach_id(PalGamepadState *gp, SDL_JoystickID jid) {
    if (!gp || jid == 0) return;
    for (int i = 0; i < PAL_MAX_GAMEPADS; i++) {
        if (gp->ids[i] == jid && gp->pads[i] != NULL) {
            SDL_CloseGamepad(gp->pads[i]);
            gp->pads[i] = NULL;
            gp->ids[i] = 0;
            if (gp->count > 0) gp->count--;
            return;
        }
    }
}

void pal_gamepad_init(PalGamepadState *gp) {
    if (!gp) return;
    memset(gp, 0, sizeof(*gp));
    int n = 0;
    SDL_JoystickID *list = SDL_GetGamepads(&n);
    if (list) {
        for (int i = 0; i < n; i++) {
            pal_gamepad_attach_id(gp, list[i]);
        }
        SDL_free(list);
    }
}

void pal_gamepad_destroy(PalGamepadState *gp) {
    if (!gp) return;
    for (int i = 0; i < PAL_MAX_GAMEPADS; i++) {
        if (gp->pads[i]) {
            SDL_CloseGamepad(gp->pads[i]);
            gp->pads[i] = NULL;
            gp->ids[i] = 0;
        }
    }
    gp->count = 0;
    gp->stick_dir_mask = 0;
    gp->stick_repeat_timer = 0.0f;
    gp->stick_repeat_initial_done = 0;
}

static SDL_Keycode pal_gamepad_map_button(SDL_GamepadButton btn) {
    switch (btn) {
        case SDL_GAMEPAD_BUTTON_SOUTH:          return SDLK_RETURN;  /* A / Cross: Confirm / Interact */
        case SDL_GAMEPAD_BUTTON_EAST:           return SDLK_ESCAPE;  /* B / Circle: Cancel / Camp Menu */
        case SDL_GAMEPAD_BUTTON_WEST:           return SDLK_LSHIFT;  /* X / Square: Run Toggle */
        case SDL_GAMEPAD_BUTTON_NORTH:          return SDLK_I;       /* Y / Triangle: Camp Menu */
        case SDL_GAMEPAD_BUTTON_START:          return SDLK_I;       /* Start: Camp Menu */
        case SDL_GAMEPAD_BUTTON_BACK:           return SDLK_TAB;     /* Select / Share: HUD Toggle */
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return SDLK_Q;       /* L1/LB: Prev Hero / Prev Page */
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return SDLK_R;       /* R1/RB: Next Hero / Next Page */
        case SDL_GAMEPAD_BUTTON_DPAD_UP:        return SDLK_UP;
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return SDLK_DOWN;
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return SDLK_LEFT;
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return SDLK_RIGHT;
        default:                                return SDLK_UNKNOWN;
    }
}

SDL_Keycode pal_gamepad_handle_event(PalGamepadState *gp, const SDL_Event *ev) {
    if (!gp || !ev) return SDLK_UNKNOWN;
    if (ev->type == SDL_EVENT_GAMEPAD_ADDED) {
        pal_gamepad_attach_id(gp, ev->gdevice.which);
        return SDLK_UNKNOWN;
    }
    if (ev->type == SDL_EVENT_GAMEPAD_REMOVED) {
        pal_gamepad_detach_id(gp, ev->gdevice.which);
        return SDLK_UNKNOWN;
    }
    if (ev->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        return pal_gamepad_map_button((SDL_GamepadButton)ev->gbutton.button);
    }
    return SDLK_UNKNOWN;
}

static int pal_stick_mask_from_axes(Sint16 ax, Sint16 ay) {
    int mask = 0;
    if (ax <= -PAL_GP_AXIS_DEADZONE) mask |= 1;
    else if (ax >= PAL_GP_AXIS_DEADZONE) mask |= 2;
    if (ay <= -PAL_GP_AXIS_DEADZONE) mask |= 4;
    else if (ay >= PAL_GP_AXIS_DEADZONE) mask |= 8;
    return mask;
}

void pal_gamepad_movement_axes(
    const PalGamepadState *gp,
    int *out_kx,
    int *out_ky,
    int *out_run_held
) {
    int kx = 0, ky = 0, run_held = 0;
    if (gp) {
        for (int i = 0; i < PAL_MAX_GAMEPADS; i++) {
            SDL_Gamepad *pad = gp->pads[i];
            if (!pad) continue;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT))  kx -= 1;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) kx += 1;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP))    ky -= 1;
            if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN))  ky += 1;

            Sint16 ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
            Sint16 ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
            int smask = pal_stick_mask_from_axes(ax, ay);
            if (smask & 1) kx -= 1;
            if (smask & 2) kx += 1;
            if (smask & 4) ky -= 1;
            if (smask & 8) ky += 1;

            if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > PAL_GP_TRIGGER_THRESHOLD) {
                run_held = 1;
            }
        }
    }
    if (kx < -1) kx = -1;
    if (kx > 1)  kx = 1;
    if (ky < -1) ky = -1;
    if (ky > 1)  ky = 1;
    if (out_kx) *out_kx = kx;
    if (out_ky) *out_ky = ky;
    if (out_run_held) *out_run_held = run_held;
}

static SDL_Keycode pal_key_from_mask(int mask) {
    if (mask & 4) return SDLK_UP;
    if (mask & 8) return SDLK_DOWN;
    if (mask & 1) return SDLK_LEFT;
    if (mask & 2) return SDLK_RIGHT;
    return SDLK_UNKNOWN;
}

SDL_Keycode pal_gamepad_step_repeat(PalGamepadState *gp, float dt) {
    if (!gp || gp->count <= 0) return SDLK_UNKNOWN;
    int mask = 0;
    for (int i = 0; i < PAL_MAX_GAMEPADS; i++) {
        SDL_Gamepad *pad = gp->pads[i];
        if (!pad) continue;
        Sint16 ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        Sint16 ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        mask |= pal_stick_mask_from_axes(ax, ay);
    }
    if (mask == 0) {
        gp->stick_dir_mask = 0;
        gp->stick_repeat_timer = 0.0f;
        gp->stick_repeat_initial_done = 0;
        return SDLK_UNKNOWN;
    }
    if (mask != gp->stick_dir_mask) {
        gp->stick_dir_mask = mask;
        gp->stick_repeat_timer = 0.0f;
        gp->stick_repeat_initial_done = 0;
        return pal_key_from_mask(mask);
    }
    gp->stick_repeat_timer += dt;
    float threshold = gp->stick_repeat_initial_done ? 0.14f : 0.38f;
    if (gp->stick_repeat_timer >= threshold) {
        gp->stick_repeat_timer = 0.0f;
        gp->stick_repeat_initial_done = 1;
        return pal_key_from_mask(mask);
    }
    return SDLK_UNKNOWN;
}

int pal_gamepad_self_test_probe(int *out_attached) {
    int n = 0;
    SDL_JoystickID *list = SDL_GetGamepads(&n);
    if (list) SDL_free(list);
    if (out_attached) *out_attached = n;

    int dead_zero = (pal_stick_mask_from_axes(4000, -5000) == 0);
    int diag_ok = (pal_stick_mask_from_axes(20000, -18000) == (2 | 4));
    int btn_ok = (pal_gamepad_map_button(SDL_GAMEPAD_BUTTON_SOUTH) == SDLK_RETURN &&
                  pal_gamepad_map_button(SDL_GAMEPAD_BUTTON_EAST) == SDLK_ESCAPE &&
                  pal_gamepad_map_button(SDL_GAMEPAD_BUTTON_START) == SDLK_I);
    return (dead_zero && diag_ok && btn_ok) ? 0 : -1;
}
