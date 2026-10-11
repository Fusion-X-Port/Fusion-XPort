/*
 * Fusion X-Port — app.c
 * Application state machine and main loop, mirroring NuclearRT's
 * Application/GameState design (Restart/Running/NextFrame/End...).
 *
 * This file is PLATFORM-NEUTRAL: it never includes console headers and
 * only knows SDL as the portable window/audio/FS substrate.  All actual
 * device access (libctru on the 3DS, desktop gamepads, analog sticks)
 * goes through fxp/platform.h, implemented per platform in
 * source/platform/.
 *
 * v9 — layering + camera:
 *
 *  - Games are selected from a list at boot, scanned from the platform
 *    games folder (fxp_platform_games_dir: sdmc:/3ds/FusionXPort/games
 *    on the 3DS, ./3ds/FusionXPort/games on desktop).  A/Enter launches,
 *    START/Esc in the list quits, START/Esc in a game returns to the
 *    list; the "End application" action also returns to the list.
 *  - INPUT: one fxp_input_poll per frame returns the OR of every device
 *    in Windows-VK space (the platform layer owns the button-role map
 *    and the console specifics); this file derives the pressed edges
 *    and translates them into Fusion's per-player control bitmasks.
 *  - CAMERA: rendering subtracts the frame scroll through each layer's
 *    scroll coefficients (fxp_frame_layer_coeffs), so Fusion frames
 *    larger than the screen scroll and coefficient layers parallax.
 */
#include "fxp/app.h"
#include "fxp/frame.h"
#include "fxp/runtime.h"
#include "fxp/backend.h"
#include "fxp/platform.h"
#include "fxp/mfa_format.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef enum {
    FXP_STATE_RESTART,          /* load frame 0                        */
    FXP_STATE_RUNNING,
    FXP_STATE_NEXT_FRAME,
    FXP_STATE_PREV_FRAME,
    FXP_STATE_JUMP_FRAME,
    FXP_STATE_RESTART_FRAME,
    FXP_STATE_END,
} fxp_state_t;

typedef struct fxp_app {
    fxp_mfa_t   *mfa;
    fxp_render_t *render;
    fxp_audio_t *audio;
    fxp_frame_t *frame;
    int32_t      frame_index;
    fxp_state_t  state;
    bool         quit_game;           /* leave the current game            */
    bool         quit_all;            /* terminate the launcher            */
    uint32_t     ticks;               /* rendered frames (for snapshots)   */
} fxp_app_t;

static fxp_app_t g_app;
static char g_last_error[128];   /* shown on the launcher list       */
static bool g_prev_back;         /* back-button edge state (game)    */
static fxp_input_map_t g_map;    /* console button -> Fusion VK map  */

/* ------------------------------------------------------------- input ---- */

/* Fusion default bindings (Fusion's own control table defaults). */
static void input_map_defaults(fxp_input_map_t *m)
{
    m->up = 0x26;  m->down = 0x28;
    m->left = 0x25; m->right = 0x27;
    m->btn1 = 0x10; m->btn2 = 0x11;
    m->btn3 = 0x20; m->btn4 = 0x0D;
    m->btn_l = 'Z'; m->btn_r = 'X';
    m->btn_zl = 'C'; m->btn_zr = 'V';
    m->back = 0x1B;
}

/* Rebuild the console button-role map from the loaded game's control
 * table (Player 1), keeping Fusion defaults for empty entries.  The
 * keys[] layout is Up, Down, Left, Right, Btn1-4, Btn5-12. */
static void build_input_map(const fxp_mfa_t *mfa)
{
    input_map_defaults(&g_map);
    if (!mfa || mfa->control_count <= 0)
        return;
    const int32_t *k = mfa->controls[0].keys;
    if (k[0]  > 0 && k[0]  < 256) g_map.up    = (uint8_t)k[0];
    if (k[1]  > 0 && k[1]  < 256) g_map.down  = (uint8_t)k[1];
    if (k[2]  > 0 && k[2]  < 256) g_map.left  = (uint8_t)k[2];
    if (k[3]  > 0 && k[3]  < 256) g_map.right = (uint8_t)k[3];
    if (k[4]  > 0 && k[4]  < 256) g_map.btn1  = (uint8_t)k[4];
    if (k[5]  > 0 && k[5]  < 256) g_map.btn2  = (uint8_t)k[5];
    if (k[6]  > 0 && k[6]  < 256) g_map.btn3  = (uint8_t)k[6];
    if (k[7]  > 0 && k[7]  < 256) g_map.btn4  = (uint8_t)k[7];
    if (k[8]  > 0 && k[8]  < 256) g_map.btn_l  = (uint8_t)k[8];
    if (k[9]  > 0 && k[9]  < 256) g_map.btn_r  = (uint8_t)k[9];
    if (k[10] > 0 && k[10] < 256) g_map.btn_zl = (uint8_t)k[10];
    if (k[11] > 0 && k[11] < 256) g_map.btn_zr = (uint8_t)k[11];
}

/* Fold one platform poll into a runtime input view: copy the held
 * bitmap and derive the "went down this tick" edges from the previous
 * view (covers buttons pressed while a frame transition ran, because
 * the view state survives across ticks while the runtime does not).
 * Any VK edge also raises "upon pressing any key" — on consoles every
 * button IS a mapped key, which is exactly Fusion's portable intent. */
static void input_apply(fxp_input_t *in, const fxp_platform_poll_t *poll)
{
    for (int vk = 0; vk < 256; vk++) {
        uint8_t down = poll->keys[vk];
        if (down && !in->keys_down[vk]) {
            in->keys_pressed[vk] = 1;
            in->any_key_pressed = true;
        }
        in->keys_down[vk] = down;
    }
    if (poll->mouse_down)
        in->mouse_down = true;
    if (poll->mouse_clicked)
        in->mouse_clicked = true;
    in->mouse_x = poll->mouse_x;
    in->mouse_y = poll->mouse_y;
}

/* Mirror the keyboard onto Fusion's player controls (MFA control table). */
static void update_controls(fxp_frame_t *frame)
{
    const fxp_mfa_t *mfa = frame->mfa;
    if (!mfa || mfa->control_count <= 0)
        return;
    for (int32_t p = 0; p < mfa->control_count && p < 4; p++) {
        uint16_t mask = 0;
        for (int32_t c = 0; c < 8 && c < 16; c++) {
            int32_t vk = mfa->controls[p].keys[c];
            if (vk > 0 && vk < 256 && frame->rt->input.keys_down[vk])
                mask |= (uint16_t)(1 << c);
        }
        fxp_input_t *in = &frame->rt->input;
        uint16_t pressed = (uint16_t)(mask & ~in->controls_down[p]);
        in->controls_pressed[p] |= pressed;
        in->controls_down[p] = mask;
    }
}

/* FXP_HOLD_KEY="VK:from[:to]" — deterministic input for headless tests. */
static struct { int vk, from, to; bool active; } g_hold_key;
/* Same, but applied inside the launcher list (separate tick counter).
 * One-shot: only the FIRST list phase runs the script, so returning from
 * a game does not relaunch it forever. */
static struct { int vk, from, to; bool active; } g_list_hold_key;
static bool g_list_hold_done;

static bool parse_hold(const char *s, int *vk, int *from, int *to)
{
    int n = 0;
    if (sscanf(s, "%d:%d:%d%n", vk, from, to, &n) >= 3)
        return true;
    if (sscanf(s, "%d:%d%n", vk, from, &n) >= 2) {
        *to = 0x7FFFFFFF;
        return true;
    }
    return false;
}

static void parse_hold_key(void)
{
    const char *s = SDL_GetEnvironmentVariable(SDL_GetEnvironment(),
                                               "FXP_HOLD_KEY");
    if (s && *s && parse_hold(s, &g_hold_key.vk, &g_hold_key.from,
                              &g_hold_key.to)) {
        g_hold_key.active = true;
        fxp_log_info("input: FXP_HOLD_KEY vk=%d frames [%d..%d)",
                     g_hold_key.vk, g_hold_key.from, g_hold_key.to);
    }
    s = SDL_GetEnvironmentVariable(SDL_GetEnvironment(),
                                   "FXP_LIST_HOLD_KEY");
    if (s && *s && parse_hold(s, &g_list_hold_key.vk, &g_list_hold_key.from,
                              &g_list_hold_key.to)) {
        g_list_hold_key.active = true;
        fxp_log_info("input: FXP_LIST_HOLD_KEY vk=%d list-frames [%d..%d)",
                     g_list_hold_key.vk, g_list_hold_key.from,
                     g_list_hold_key.to);
    }
}

/* Test-scripted key: overrides one entry of the poll for this frame. */
static void apply_hold_key(fxp_platform_poll_t *poll)
{
    if (!g_hold_key.active)
        return;
    poll->keys[g_hold_key.vk] =
        (g_app.ticks >= (uint32_t)g_hold_key.from &&
         g_app.ticks <  (uint32_t)g_hold_key.to) ? 1 : 0;
}

static void apply_list_hold_key(uint32_t list_ticks,
                                fxp_platform_poll_t *poll)
{
    if (!g_list_hold_key.active || g_list_hold_done)
        return;
    if (list_ticks >= (uint32_t)g_list_hold_key.from) {
        g_list_hold_done = true;      /* this list phase consumed it    */
        poll->keys[g_list_hold_key.vk] =
            (list_ticks < (uint32_t)g_list_hold_key.to) ? 1 : 0;
    }
}

/* ---------------------------------------------------------- snapshots --- */

static void dump_frame_ppm(const char *path)
{
    if (g_app.render && fxp_render_snapshot(g_app.render, path))
        fxp_log_info("app: snapshot written to %s", path);
}

/* ------------------------------------------------------- game list UI ---
 *
 * The launcher scans the platform games folder for .mfa files.  On the
 * 3DS it is the standard homebrew location sdmc:/3ds/FusionXPort/games
 * (the /3ds/<AppName>/ convention); desktop builds mirror it relative
 * to the working directory so the documentation is identical everywhere.
 */

#define FXP_MAX_GAMES 64

typedef struct game_entry {
    char path[1024];
    char name[256];
} game_entry_t;

static game_entry_t g_games[FXP_MAX_GAMES];
static int          g_game_count = 0;

static void make_dirs(const char *path)
{
    /* create every path prefix (SDL_CreateDirectory makes one level) */
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char saved = *p;
            *p = '\0';
            SDL_CreateDirectory(buf);
            *p = saved;
        }
    }
    SDL_CreateDirectory(buf);
}

static void basename_of(const char *p, char *out, size_t cap)
{
    const char *slash = strrchr(p, '/');
    const char *bslash = strrchr(p, '\\');
    if (bslash > slash) slash = bslash;
    snprintf(out, cap, "%.240s", slash ? slash + 1 : p);
}

static bool ends_with_mfa(const char *s)
{
    size_t n = strlen(s);
    if (n < 4 || s[n - 4] != '.')
        return false;
    return (s[n-3] | 32) == 'm' && (s[n-2] | 32) == 'f' &&
           (s[n-1] | 32) == 'a';
}

static int cmp_game(const void *a, const void *b)
{
    return strcmp(((const game_entry_t *)a)->name,
                  ((const game_entry_t *)b)->name);
}

static void scan_games(const char *games_dir)
{
    g_game_count = 0;

    make_dirs(games_dir);
    int count = 0;
    char **entries = SDL_GlobDirectory(games_dir, "*.mfa",
                                       SDL_GLOB_CASEINSENSITIVE, &count);
    if (entries) {
        for (int i = 0; i < count && g_game_count < FXP_MAX_GAMES; i++) {
            const char *entry = entries[i];
            if (!ends_with_mfa(entry))
                continue;
            game_entry_t *ge = &g_games[g_game_count];
            snprintf(ge->path, sizeof(ge->path), "%s/%s", games_dir, entry);
            char base[256];
            basename_of(entry, base, sizeof(base));
            size_t bl = strlen(base);
            if (bl > 4)
                base[bl - 4] = '\0';    /* strip ".mfa"                  */
            snprintf(ge->name, sizeof(ge->name), "%s", base);
            g_game_count++;
        }
        SDL_free(entries);
    }
    SDL_qsort(g_games, (size_t)g_game_count, sizeof(game_entry_t), cmp_game);

    /* Out-of-the-box fallback: the ROMFS copy bundled with the build. */
    if (g_game_count == 0) {
        static char builtin[1024];
        const char *base = SDL_GetBasePath();
        snprintf(builtin, sizeof(builtin), "%sgame.mfa",
                 base ? base : "");
        SDL_PathInfo info;
        if (SDL_GetPathInfo(builtin, &info) && info.type == SDL_PATHTYPE_FILE) {
            snprintf(g_games[0].path, sizeof(g_games[0].path), "%s", builtin);
            snprintf(g_games[0].name, sizeof(g_games[0].name),
                     "game (built-in)");
            g_game_count = 1;
        }
    }
}

/* Launcher colours (neutral dark + emerald accent). */
#define COL_BG        16, 16, 20, 255
#define COL_ROW_SEL   24, 84, 56, 255
#define COL_TITLE     110, 220, 160, 255
#define COL_TEXT      205, 205, 205, 255
#define COL_DIM       100, 100, 100, 255
#define COL_WARN      232, 186, 70, 255

static void draw_text(SDL_Renderer *rr, float x, float y, const char *s)
{
    SDL_RenderDebugText(rr, x, y, s);
}

static void draw_list_screen(int selected, const char *notice)
{
    SDL_Renderer *rr = fxp_render_sdl(g_app.render);
    if (!rr)
        return;
    fxp_color_t bg = { 16, 16, 20, 255 };
    fxp_render_begin(g_app.render, bg);

    const char *dir = fxp_platform_games_dir();
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "FUSION X-PORT  —  %d game%s",
             g_game_count, g_game_count == 1 ? "" : "s");
    SDL_SetRenderDrawColor(rr, 110, 220, 160, 255);
    draw_text(rr, 8, 6, hdr);
    SDL_SetRenderDrawColor(rr, 100, 100, 100, 255);
    draw_text(rr, 8, 16, dir);

    if (g_game_count == 0) {
        SDL_SetRenderDrawColor(rr, 232, 186, 70, 255);
        draw_text(rr, 8, 90, "No games found.");
        draw_text(rr, 8, 104, "Copy your .mfa files into the folder above.");
        draw_text(rr, 8, 118, "Press START (or Esc) to quit.");
    } else {
        const int rows = 11, row_h = 17;
        int first = selected - rows / 2;
        if (first > g_game_count - rows) first = g_game_count - rows;
        if (first < 0) first = 0;
        int last = first + rows;
        if (last > g_game_count) last = g_game_count;

        for (int i = first; i < last; i++) {
            float y = 30 + (float)(i - first) * row_h;
            if (i == selected) {
                SDL_SetRenderDrawColor(rr, 24, 84, 56, 255);
                SDL_FRect rect = { 4, y - 2, 392, row_h };
                SDL_RenderFillRect(rr, &rect);
            }
            SDL_SetRenderDrawColor(rr, i == selected ? 255 : 205,
                                   i == selected ? 255 : 205,
                                   i == selected ? 255 : 205, 255);
            draw_text(rr, 10, y + 2, g_games[i].name);
        }
        if (g_game_count > rows) {
            char more[32];
            snprintf(more, sizeof(more), "%d / %d", selected + 1,
                     g_game_count);
            SDL_SetRenderDrawColor(rr, 100, 100, 100, 255);
            draw_text(rr, 348, 226, more);
        }
    }

    SDL_SetRenderDrawColor(rr, notice ? 232 : 100,
                           notice ? 186 : 100, notice ? 70 : 100, 255);
    draw_text(rr, 8, 228,
              notice ? notice
                     : "A/Enter: Play    START/Esc: Quit");
    fxp_render_end(g_app.render);
}

typedef enum {
    FXP_LIST_QUIT = 0,
    FXP_LIST_LAUNCH,
} fxp_list_result_t;

/* Run the launcher loop.  Returns the chosen game path via `out`. */
static fxp_list_result_t run_list(char *out, size_t out_cap,
                                  const char *notice)
{
    scan_games(fxp_platform_games_dir());
    int selected = 0;
    char own_notice[128];
    own_notice[0] = '\0';
    if (notice && *notice)
        snprintf(own_notice, sizeof(own_notice), "%s", notice);

    uint32_t list_ticks = 0;
    uint64_t last = SDL_GetTicks();
    const double frame_time = 1.0 / 60.0;

    /* input view for the list (no runtime behind it) */
    fxp_input_t list_input;
    memset(&list_input, 0, sizeof(list_input));

    /* scroll repeat state */
    uint64_t move_held_since = 0, last_repeat = 0;

    for (;;) {
        /* one platform poll per list frame (all devices, VK space) */
        fxp_platform_poll_t poll;
        fxp_input_poll(&poll);
        if (poll.quit)
            g_app.quit_all = true;

        apply_list_hold_key(list_ticks, &poll);
        input_apply(&list_input, &poll);

        bool launched = false;
        bool want_quit = false;

        if (list_input.keys_pressed[0x0D] ||
            list_input.keys_pressed[g_map.btn1] ||
            list_input.keys_pressed[0x20]) {
            if (g_game_count > 0) {
                snprintf(out, out_cap, "%s", g_games[selected].path);
                launched = true;
            }
        }
        if (poll.start_edge || list_input.keys_pressed[g_map.back]) {
            want_quit = true;         /* START in the list truly quits  */
        }

        /* navigation with hold-repeat */
        bool nav = false;
        uint64_t now = SDL_GetTicks();
        bool up_held   = list_input.keys_down[g_map.up] != 0;
        bool down_held = list_input.keys_down[g_map.down] != 0;
        if (list_input.keys_pressed[g_map.up] ||
            list_input.keys_pressed[g_map.down]) {
            nav = true;
            move_held_since = now;
            last_repeat = now;
        } else if (up_held || down_held) {
            if (now - move_held_since > 400 &&
                now - last_repeat > 110) {
                nav = true;
                last_repeat = now;
            }
        }
        if (nav && g_game_count > 0) {
            if (up_held)
                selected = (selected + g_game_count - 1) % g_game_count;
            else if (down_held)
                selected = (selected + 1) % g_game_count;
            own_notice[0] = '\0';
        }

        bool snapshot_exit = false;
        list_ticks++;                 /* always advance the list frame  */
        {
            const char *snap = SDL_GetEnvironmentVariable(SDL_GetEnvironment(),
                                                          "FXP_SNAPSHOT");
            uint32_t at = snap ? (uint32_t)atoi(snap) : 0;
            if (at && list_ticks >= at) {
                draw_list_screen(selected, own_notice[0] ? own_notice : NULL);
                dump_frame_ppm("/tmp/fxp_list.ppm");
                snapshot_exit = true;
            }
        }

        if (snapshot_exit)
            return FXP_LIST_QUIT;
        if (launched)
            return FXP_LIST_LAUNCH;
        if (want_quit || g_app.quit_all)
            return FXP_LIST_QUIT;

        draw_list_screen(selected, own_notice[0] ? own_notice : NULL);

        /* clear pressed edges for the next list frame */
        memset(list_input.keys_pressed, 0, 256);
        list_input.any_key_pressed = false;

        uint64_t after = SDL_GetTicks();
        double elapsed = (double)(after - last) / 1000.0;
        if (elapsed < frame_time)
            SDL_Delay((uint32_t)((frame_time - elapsed) * 1000.0));
        last = SDL_GetTicks();
    }
}

/* ---------------------------------------------------------------- game --- */

static const fxp_image_t *fxp_mfa_image(const fxp_mfa_t *mfa, int32_t handle)
{
    for (int32_t i = 0; i < mfa->images.count; i++)
        if (mfa->images.items[i].handle == handle)
            return &mfa->images.items[i];
    return NULL;
}

static bool app_load_frame(int32_t index)
{
    if (index < 0 || index >= g_app.mfa->frame_count) {
        fxp_log_info("app: no frame %d — ending game", index);
        g_app.state = FXP_STATE_END;
        return false;
    }
    fxp_frame_destroy(g_app.frame);
    g_app.frame = fxp_frame_create(g_app.mfa, g_app.mfa->frames[index]);
    if (g_app.frame->rt) {
        g_app.frame->rt->audio = g_app.audio;
        g_app.frame->rt->audio_load =
            (int (*)(void *, const uint8_t *, size_t))fxp_audio_load;
        g_app.frame->rt->audio_play =
            (void (*)(void *, int, int))fxp_audio_play;
        g_app.frame->rt->audio_stop_all =
            (void (*)(void *))fxp_audio_stop_all;
    }
    fxp_frame_spawn_instances(g_app.frame);
    g_app.frame_index = index;
    g_app.state = FXP_STATE_RUNNING;
    fxp_log_info("app: frame %d loaded ('%s')", index, g_app.mfa->frames[index]->name);
    return true;
}

static void apply_pending_state(void)
{
    fxp_runtime_t *rt = g_app.frame->rt;
    if (!rt || !rt->pending_state)
        return;
    switch (rt->pending_state) {
    case 1: g_app.state = FXP_STATE_NEXT_FRAME; break;
    case 2: g_app.state = FXP_STATE_PREV_FRAME; break;
    case 3: {
        /* The runtime queues either a frame HANDLE (plain param — the
         * MFA frame list stores per-file handle conventions: eventest
         * uses [0,1], other files [1,2,...]) or a 0-based position
         * (expression variant).  Resolve handles to positions here. */
        int32_t target = rt->pending_frame;
        int32_t position = -1;
        if (rt->pending_frame_is_handle) {
            for (int32_t i = 0; i < g_app.mfa->frame_count; i++) {
                if (g_app.mfa->frames[i]->handle == target) {
                    position = i;
                    break;
                }
            }
            if (position < 0) {
                /* no frame carries that handle: fall back to treating
                 * the value as a plain position (older/odd files) */
                position = target;
            }
        } else {
            position = target;
        }
        if (position < 0)
            position = 0;
        if (position < g_app.mfa->frame_count) {
            g_app.frame_index = position;
            g_app.state = FXP_STATE_JUMP_FRAME;
        } else {
            fxp_log_info("app: jump target %d past last frame — ending game",
                         position);
            g_app.quit_game = true;
        }
        break;
    }
    case 4: g_app.state = FXP_STATE_RESTART_FRAME; break;
    case 5:
        fxp_log_info("app: end application action — back to the list");
        g_app.quit_game = true;           /* launcher, not the process  */
        break;
    default:
        break;
    }
    rt->pending_state = 0;
}

/* Run one MFA to completion.  Returns false if it could not be loaded. */
static bool run_game(const char *mfa_path)
{
    memset(&g_app.ticks, 0, sizeof(g_app.ticks));
    g_app.quit_game = false;

    FILE *fp = fopen(mfa_path, "rb");
    if (!fp) {
        fxp_log_error("app: cannot open '%s'", mfa_path);
        return false;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fxp_log_error("app: '%s' is empty", mfa_path);
        fclose(fp);
        return false;
    }
    uint8_t *data = fxp_malloc((size_t)size);
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
        fxp_log_error("app: short read on '%s'", mfa_path);
        fclose(fp);
        free(data);
        return false;
    }
    fclose(fp);

    fxp_mfa_t *mfa = fxp_mfa_load(data, (size_t)size);
    free(data);
    if (!mfa)
        return false;

    if (g_app.mfa)
        fxp_mfa_free(g_app.mfa);
    g_app.mfa = mfa;
    build_input_map(mfa);             /* console buttons follow the game */
    fxp_input_init(&g_map);

    /* The previous game's textures must go: image handles are only
     * unique within one MFA's bank. */
    fxp_render_clear_images(g_app.render);

    /* Upload the whole image bank — textures must be resident before any
     * blit can happen (this was the "only background renders" bug). */
    int32_t uploaded = 0;
    for (int32_t i = 0; i < mfa->images.count; i++)
        if (fxp_render_upload_image(g_app.render, &mfa->images.items[i]))
            uploaded++;
    fxp_log_info("app: uploaded %d/%d image(s) to the GPU", uploaded,
                 mfa->images.count);

    fxp_log_info("app: running '%s'", mfa->name);
    g_prev_back = false;

    const char *snap = SDL_GetEnvironmentVariable(SDL_GetEnvironment(),
                                                  "FXP_SNAPSHOT");
    uint32_t snapshot_at = snap ? (uint32_t)atoi(snap) : 0;

    g_app.state = FXP_STATE_RESTART;
    double fps = mfa->frame_rate > 0 ? mfa->frame_rate : 60.0;
    double frame_time = 1.0 / fps;

    uint64_t last = SDL_GetTicks();
    while (!g_app.quit_game && !g_app.quit_all) {
        /* one platform poll per frame — all devices, VK space */
        fxp_platform_poll_t poll;
        fxp_input_poll(&poll);
        if (poll.quit)
            g_app.quit_all = true;

        /* START / SELECT(-Esc) inside a game -> the launcher list.
         * Edge detection happens here at the app layer so it works even
         * while the frame (and its runtime) is being swapped. */
        bool back_now = poll.keys[g_map.back] != 0;
        bool back_pressed = back_now && !g_prev_back;
        g_prev_back = back_now;

        bool back_to_list = poll.start_edge || back_pressed;

        switch (g_app.state) {
        case FXP_STATE_RESTART:
            app_load_frame(0);
            break;
        case FXP_STATE_NEXT_FRAME:
            app_load_frame(g_app.frame_index + 1);
            break;
        case FXP_STATE_PREV_FRAME:
            app_load_frame(g_app.frame_index - 1);
            break;
        case FXP_STATE_JUMP_FRAME:
            app_load_frame(g_app.frame_index);
            break;
        case FXP_STATE_RESTART_FRAME:
            app_load_frame(g_app.frame_index);
            break;
        default:
            break;
        }

        if (back_to_list)
            break;

        if (g_app.state == FXP_STATE_RUNNING) {
            apply_hold_key(&poll);
            if (g_app.frame && g_app.frame->rt)
                input_apply(&g_app.frame->rt->input, &poll);
            update_controls(g_app.frame);
            fxp_frame_update(g_app.frame, frame_time);
            apply_pending_state();

            /* draw — screen position = frame position minus the camera
             * scroll scaled by the object's layer scroll coefficients
             * (1.0 on plain layers, 0 for HUD layers, 2+ for parallax) */
            const fxp_mfa_frame_t *def = g_app.frame->def;
            fxp_color_t bg = g_app.frame->has_background_override
                                 ? g_app.frame->background_override
                                 : def->background;
            fxp_render_begin(g_app.render, bg);

            for (int32_t l = 0; l < def->layer_count; l++) {
                for (int32_t i = 0; i < g_app.frame->instance_count; i++) {
                    fxp_object_t *obj = g_app.frame->instances[i];
                    if (!obj->visible || (int32_t)obj->layer != l)
                        continue;
                    float lcx, lcy;
                    fxp_frame_layer_coeffs(g_app.frame, obj->layer,
                                           &lcx, &lcy);
                    int32_t sx = obj->x -
                        (int32_t)(g_app.frame->scroll_x * lcx);
                    int32_t sy = obj->y -
                        (int32_t)(g_app.frame->scroll_y * lcy);
                    int32_t img = fxp_object_current_image(obj);
                    if (img < 0) {
                        if (obj->type == FXP_OT_QUICK_BACKDROP) {
                            fxp_color_t c = { 128, 128, 128, 255 };
                            fxp_render_fill_rect(g_app.render, sx, sy,
                                                 obj->item->as.quick_backdrop.w,
                                                 obj->item->as.quick_backdrop.h,
                                                 c);
                        }
                        continue;
                    }
                    const fxp_image_t *image = fxp_mfa_image(g_app.mfa, img);
                    if (!image)
                        continue;
                    uint8_t blend = obj->alpha_coefficient;
                    if (obj->has_tint) {
                        fxp_render_blit_tinted(
                            g_app.render, img, sx, sy,
                            image->hotspot_x, image->hotspot_y,
                            obj->angle, blend,
                            obj->tint_r, obj->tint_g, obj->tint_b);
                    } else {
                        fxp_render_blit(
                            g_app.render, img, sx, sy,
                            image->hotspot_x, image->hotspot_y,
                            obj->angle, blend);
                    }
                }
            }
            fxp_render_end(g_app.render);
            g_app.ticks++;

            if (snapshot_at && g_app.ticks >= snapshot_at) {
                dump_frame_ppm("/tmp/fxp_snapshot.ppm");
                g_app.quit_game = true;
            }

            if (g_app.frame && g_app.frame->rt && !g_app.quit_game)
                fxp_runtime_end_tick(g_app.frame->rt);
        }

        /* fixed timestep pacing (Application::Run) */
        uint64_t now = SDL_GetTicks();
        double elapsed = (double)(now - last) / 1000.0;
        if (elapsed < frame_time)
            SDL_Delay((uint32_t)((frame_time - elapsed) * 1000.0));
        last = SDL_GetTicks();
    }

    fxp_frame_destroy(g_app.frame);
    g_app.frame = NULL;
    return true;
}

/* ----------------------------------------------------------------- app --- */

bool fxp_app_run(const char *mfa_path)
{
    memset(&g_app, 0, sizeof(g_app));
    input_map_defaults(&g_map);
    fxp_input_init(&g_map);

    g_app.render = fxp_render_create();
    if (!g_app.render)
        return false;
    g_app.audio = fxp_audio_create();

    parse_hold_key();

    if (mfa_path) {
        run_game(mfa_path);           /* direct launch (tests/tools)    */
    } else {
        char selected[1024];
        for (;;) {
            fxp_list_result_t r = run_list(selected, sizeof(selected),
                                           g_last_error);
            g_last_error[0] = '\0';
            if (r == FXP_LIST_QUIT)
                break;
            if (!run_game(selected)) {
                char base[256];
                basename_of(selected, base, sizeof(base));
                snprintf(g_last_error, sizeof(g_last_error),
                         "Could not load '%.96s'", base);
            }
            /* never carry a held key/button from the game into the list */
            fxp_input_reset();
            g_prev_back = false;
            if (g_app.quit_all)
                break;
        }
    }

    fxp_log_info("app: shutting down");
    fxp_frame_destroy(g_app.frame);
    fxp_audio_destroy(g_app.audio);
    fxp_render_destroy(g_app.render);
    if (g_app.mfa)
        fxp_mfa_free(g_app.mfa);
    fxp_input_exit();
    return true;
}
