/*
 * Fusion X-Port — platform.c
 * Shared SDL3 platform implementation: the event pump that turns OS
 * input into the Fusion VK space every other module understands, plus
 * the lifecycle/reporting entry points of fxp/platform.h.
 *
 * Console-specific devices are added by exactly one per-platform device
 * layer (platform_3ds.c on the 3DS, platform_desktop.c everywhere else):
 * both are compiled here — their bodies are guarded by their platform
 * macro — so this translation unit carries the only live copy.
 *
 * Layering rules enforced by this directory:
 *   core runtime (event_runtime/movement/frame/...)  -> no SDL, no 3DS
 *   app.c / launcher                                 -> fxp/platform.h only
 *   source/platform/                               -> SDL + console SDKs
 */
#include "platform_internal.h"

#include <SDL3/SDL.h>

/* ------------------------------------------------------- SDL input pump -- */

/* SDL scancode -> Windows VK code (Fusion key codes), common subset. */
static int sdl_scancode_to_vk(int sc)
{
    switch (sc) {
    case SDL_SCANCODE_BACKSPACE:   return 0x08;
    case SDL_SCANCODE_TAB:         return 0x09;
    case SDL_SCANCODE_RETURN:      return 0x0D;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL:   return 0x11;
    case SDL_SCANCODE_ESCAPE:      return 0x1B;
    case SDL_SCANCODE_SPACE:       return 0x20;
    case SDL_SCANCODE_PRIOR:       return 0x21;
    case SDL_SCANCODE_PAGEDOWN:    return 0x22;
    case SDL_SCANCODE_END:         return 0x23;
    case SDL_SCANCODE_HOME:        return 0x24;
    case SDL_SCANCODE_LEFT:        return 0x25;
    case SDL_SCANCODE_UP:          return 0x26;
    case SDL_SCANCODE_RIGHT:       return 0x27;
    case SDL_SCANCODE_DOWN:        return 0x28;
    case SDL_SCANCODE_INSERT:      return 0x2D;
    case SDL_SCANCODE_DELETE:      return 0x2E;
    default: break;
    }
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return 0x41 + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)
        return 0x31 + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0)
        return 0x30;
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)
        return 0x61 + (sc - SDL_SCANCODE_KP_1);   /* numpad end */
    if (sc == SDL_SCANCODE_KP_0)
        return 0x60;
    if (sc >= SDL_SCANCODE_KP_MULTIPLY && sc <= SDL_SCANCODE_KP_DIVIDE)
        return 0x6A + (sc - SDL_SCANCODE_KP_MULTIPLY);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)
        return 0x70 + (sc - SDL_SCANCODE_F1);
    return 0;
}

static struct {
    uint8_t kb[256];                  /* keyboard (SDL events)            */
    uint8_t pad[256];                 /* gamepad buttons (SDL events)     */
    float   axis[8];                  /* raw stick/trigger state          */
    int32_t mouse_x, mouse_y;
    bool    mouse_down;
    bool    started;                  /* fxp_input_init has run           */
} g_sdl;

static fxp_input_map_t g_map;

const fxp_input_map_t *fxp_input_map_get(void)
{
    return &g_map;
}

float fxp_sdl_axis(int axis)
{
    if (axis < 0 || axis >= 8)
        return 0.0f;
    return g_sdl.axis[axis];
}

/* Console button role -> VK (desktop gamepads mirror the 3DS roles). */
static int gamepad_button_to_vk(int btn)
{
    const fxp_input_map_t *m = &g_map;
    switch (btn) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:        return m->up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return m->down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return m->left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return m->right;
    case SDL_GAMEPAD_BUTTON_SOUTH:          return m->btn1;  /* A */
    case SDL_GAMEPAD_BUTTON_EAST:           return m->btn2;  /* B */
    case SDL_GAMEPAD_BUTTON_NORTH:          return m->btn3;  /* X */
    case SDL_GAMEPAD_BUTTON_WEST:           return m->btn4;  /* Y */
    case SDL_GAMEPAD_BUTTON_BACK:           return m->back;  /* Select */
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return m->btn_l;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return m->btn_r;
    default: return 0;
    }
}

static void sdl_pump(fxp_platform_poll_t *out)
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_QUIT || ev.type == SDL_EVENT_TERMINATING) {
            out->quit = true;
        } else if (ev.type == SDL_EVENT_KEY_DOWN) {
            int vk = sdl_scancode_to_vk(ev.key.scancode);
            if (vk)
                g_sdl.kb[vk] = 1;     /* state-based: repeats are no-ops */
        } else if (ev.type == SDL_EVENT_KEY_UP) {
            int vk = sdl_scancode_to_vk(ev.key.scancode);
            if (vk)
                g_sdl.kb[vk] = 0;
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            g_sdl.mouse_down = true;
            out->mouse_clicked = true;
        } else if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP) {
            g_sdl.mouse_down = false;
        } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
            g_sdl.mouse_x = (int32_t)ev.motion.x;
            g_sdl.mouse_y = (int32_t)ev.motion.y;
        } else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
            if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
                out->start_edge = true;
            } else {
                int vk = gamepad_button_to_vk(ev.gbutton.button);
                if (vk)
                    g_sdl.pad[vk] = 1;
            }
        } else if (ev.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
            int vk = gamepad_button_to_vk(ev.gbutton.button);
            if (vk)
                g_sdl.pad[vk] = 0;
        } else if (ev.type == SDL_EVENT_GAMEPAD_AXIS_MOTION &&
                   ev.gaxis.axis < 8) {
            g_sdl.axis[ev.gaxis.axis] = (float)ev.gaxis.value / 32768.0f;
        }
    }
}

/* ------------------------------------------------------------ lifecycle -- */

const char *fxp_platform_name(void)
{
    return fxp_platform_device_name();
}

void fxp_platform_init(void)
{
    fxp_platform_device_init();
}

void fxp_platform_quit(void)
{
    fxp_platform_device_exit();
}

const char *fxp_platform_games_dir(void)
{
    return fxp_platform_device_games_dir();
}

void fxp_input_init(const fxp_input_map_t *map)
{
    if (map)
        g_map = *map;
    g_sdl.started = true;
    fxp_input_reset();
}

void fxp_input_reset(void)
{
    memset(g_sdl.kb, 0, sizeof(g_sdl.kb));
    memset(g_sdl.pad, 0, sizeof(g_sdl.pad));
    memset(g_sdl.axis, 0, sizeof(g_sdl.axis));
    g_sdl.mouse_down = false;
    fxp_platform_device_reset();
}

void fxp_input_exit(void)
{
    fxp_input_reset();
    g_sdl.started = false;
}

void fxp_input_poll(fxp_platform_poll_t *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->mouse_x = g_sdl.mouse_x;
    out->mouse_y = g_sdl.mouse_y;
    if (!g_sdl.started)
        return;

    sdl_pump(out);                    /* keyboard/gamepad/mouse (SDL)     */
    fxp_platform_device_poll(out);    /* console devices + analog sticks  */

    for (int vk = 0; vk < 256; vk++)
        out->keys[vk] = (uint8_t)(g_sdl.kb[vk] || g_sdl.pad[vk] ||
                                  out->keys[vk]);
    out->mouse_down = g_sdl.mouse_down;
}

/*
 * The per-platform device layers (platform_3ds.c, platform_desktop.c)
 * are ordinary translation units compiled alongside this file — their
 * whole bodies are guarded by their platform macro, so exactly one of
 * them contributes the fxp_platform_device_* symbols per build.
 */
