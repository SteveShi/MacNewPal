#ifndef PAL_GAMEPAD_H
#define PAL_GAMEPAD_H

#include <SDL3/SDL.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PAL_MAX_GAMEPADS 4

typedef struct {
    SDL_Gamepad *pads[PAL_MAX_GAMEPADS];
    SDL_JoystickID ids[PAL_MAX_GAMEPADS];
    int count;
    int stick_dir_mask; /* bit 0:L, 1:R, 2:U, 3:D */
    float stick_repeat_timer;
    int stick_repeat_initial_done;
} PalGamepadState;

/** Open all currently attached SDL3 gamepads cleanly. */
void pal_gamepad_init(PalGamepadState *gp);

/** Close all open SDL3 gamepad handles. */
void pal_gamepad_destroy(PalGamepadState *gp);

/**
 * Process a single SDL3 event (hotplug add/remove or button press).
 * Returns a mapped `SDL_Keycode` if the event triggers a UI/menu/action binding, or `SDLK_UNKNOWN`.
 */
SDL_Keycode pal_gamepad_handle_event(PalGamepadState *gp, const SDL_Event *ev);

/**
 * Sample current continuous D-Pad + Left Stick directional vector (-1/0/+1)
 * and sprint/run trigger state across connected gamepads.
 */
void pal_gamepad_movement_axes(
    const PalGamepadState *gp,
    int *out_kx,
    int *out_ky,
    int *out_run_held
);

/**
 * Emit repeated synthetic directional `SDL_Keycode` when holding Left Stick inside menus/UI.
 */
SDL_Keycode pal_gamepad_step_repeat(PalGamepadState *gp, float dt);

/** Non-interactive self-test verifying axis deadzone math and button-to-action mapping. */
int pal_gamepad_self_test_probe(int *out_attached);

#ifdef __cplusplus
}
#endif

#endif /* PAL_GAMEPAD_H */
