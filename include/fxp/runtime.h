/*
 * Fusion X-Port — runtime.h
 * Event interpreter: NuclearRT's event semantics as a C interpreter.
 *
 * Pipeline per tick (Frame::Update / Frame.template.cpp):
 *   true events -> per-instance movement+animation update ->
 *   named timer events -> timer-condition pass -> normal event loop ->
 *   end-of-frame pass -> deferred deletes
 *
 * Event execution (EventProcessor.BuildEventFunctions):
 *   - inactive lines (flags & 16384), comments, group markers skipped
 *   - per condition: reset selectors of referenced object infos once,
 *     evaluate (negation via OtherFlags & 1), fail -> next OR branch
 *   - OR markers (-1,-25) split conditions into branches; the event
 *     passes if any branch fully passes; selections survive into actions
 *   - per action: reset unreferenced selectors, iterate selection,
 *     execute
 */
#ifndef FXP_RUNTIME_H
#define FXP_RUNTIME_H

#include "fxp.h"
#include "value.h"
#include "object.h"
#include "mfa.h"

/* fxp_frame_t (frame.h) — forward declared to break the include cycle */
typedef struct fxp_frame fxp_frame_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Fusion control indexes (Application -> player controls order) */
enum {
    FXP_CTRL_UP = 0, FXP_CTRL_DOWN, FXP_CTRL_LEFT, FXP_CTRL_RIGHT,
    FXP_CTRL_BUTTON1, FXP_CTRL_BUTTON2, FXP_CTRL_BUTTON3, FXP_CTRL_BUTTON4,
    FXP_CTRL_COUNT = 16,
};

/* Input state (Input.h).  Fusion control bitmasks, one per player. */
typedef struct fxp_input {
    uint16_t controls_down[4];        /* bit (1<<ctrl) held               */
    uint16_t controls_pressed[4];     /* bit (1<<ctrl) went down this tick */
    uint8_t  keys_down[256];          /* by Windows VK code                */
    uint8_t  keys_pressed[256];       /* VK went down this tick            */
    bool     any_key_pressed;         /* "upon pressing any key" edge      */
    bool     mouse_down;              /* any button held                   */
    bool     mouse_clicked;           /* any button pressed this tick      */
    int32_t  mouse_x, mouse_y;        /* frame coordinates                 */
    bool     quit_requested;          /* End application action            */
} fxp_input_t;

/* Per-event timer state for "every"/"compare to timer" conditions
 * (Timer.h TimerEvent / CheckEvent). */
typedef struct fxp_timer_event {
    int32_t  id;                      /* event (group) index               */
    int32_t  check_time;              /* interval / compare value (ms)     */
    int32_t  type;                    /* 0=every 1=equals 2=less 3=greater */
    double   time;
    bool     triggered;               /* Equals: fired once                */
    bool     already_checked;
} fxp_timer_event_t;

/* Scheduled named timer events (Timer.h ScheduledTimerEvent). */
typedef struct fxp_named_timer {
    char    *name;                    /* lower-case                        */
    int32_t  remaining;
    int32_t  interval;
    double   next_fire_time;
    int32_t  index;
    bool     track_event_index;
} fxp_named_timer_t;

/* Fast loop state (Frame.h LoopState). */
typedef struct fxp_loop_state {
    char   *name;                     /* lower-case                        */
    int32_t index;
    bool    running;
} fxp_loop_state_t;

/* Runtime state attached to a frame. */
typedef struct fxp_runtime {
    const fxp_mfa_t  *mfa;            /* for sound bank lazy-load          */
    void             *audio;          /* backend audio (optional)          */
    /* audio hooks — set by the SDL app; core/test builds run silent */
    int  (*audio_load)(void *audio, const uint8_t *data, size_t size);
    void (*audio_play)(void *audio, int channel, int loops);
    void (*audio_stop_all)(void *audio);
    int32_t           sample_channel[128]; /* sound handle -> loaded channel
                                            * [0..63] samples, [64..127] music */

    double time;                      /* ms since frame start (Timer)      */
    uint32_t update_count;

    fxp_input_t input;

    /* application data (AppData.h): globals + player score/lives */
    fxp_value_t *global_values;
    int32_t      global_value_count;
    fxp_value_t *global_strings;
    int32_t      global_string_count;
    int32_t      score[4];
    int32_t      lives[4];

    /* per-event "every/equals" timer states, grow on demand */
    fxp_timer_event_t *timer_events;
    int32_t            timer_event_count;
    int32_t            timer_event_cap;

    fxp_named_timer_t *named_timers;
    int32_t            named_timer_count;
    int32_t            named_timer_cap;

    char              *current_named_event;   /* dispatching this name      */

    fxp_loop_state_t  *loops;
    int32_t            loop_count;
    int32_t            loop_cap;
    const char        *current_loop;          /* loop name while OnLoop     */

    /* Fusion group machinery: id -> active */
    int32_t  *group_ids;
    bool     *group_active;
    int32_t   group_cap;              /* array size (id-indexed)           */

    /* per-group runtime flags (indexed by group index) */
    bool     *run_once;               /* RunOnceCondition                  */
    bool     *action_ran_last;        /* OneActionLoopCondition            */
    uint32_t  group_state_cap;

    /* per-group classification + Fusion group owner id (computed once) */
    uint8_t  *gclass;
    int32_t  *gowner;

    uint64_t  rng_state;

    /* generated object events queued during instance updates
     * (animation over, movement stopped — Animation -> GenerateEvent) */
    struct {
        int16_t       type, num;
        fxp_object_t *source;
    } pending[32];
    int32_t       pending_count;

    /* interpreter scratch */
    fxp_object_t *loop_instance;      /* instance being iterated           */
    uint32_t      loop_oi;            /* its object info                   */
    bool          have_loop_instance;
    int32_t       loop_index;         /* it.index()                        */
    int32_t       true_event_group;   /* -1 unless dispatching true event  */

    /* pending application state change (queued, applied at end of tick) */
    int32_t  pending_state;           /* 0 none, 1 next, 2 prev, 3 jump,
                                        4 restart, 5 end                  */
    int32_t  pending_frame;           /* jump target (handle or position) */
    bool     pending_frame_is_handle; /* plain param = frame HANDLE       */
} fxp_runtime_t;

/* Runtime lifecycle.  Copies global values/strings from the MFA. */
fxp_runtime_t *fxp_runtime_create(const fxp_mfa_t *mfa, const fxp_mfa_frame_t *def);
void           fxp_runtime_free(fxp_runtime_t *rt);

/* One full event pipeline tick (call after fxp_object_update on all
 * instances; the runtime does NOT touch movement/animation).  `dt` is
 * the frame delta in seconds (drives the Fusion timer). */
void fxp_runtime_tick(fxp_frame_t *frame, double dt);

/* Queue an object-generated event (animation over, ...) — dispatched
 * within the current tick before the normal event loop. */
void fxp_runtime_generate_event(fxp_frame_t *frame, int32_t type, int32_t num,
                                fxp_object_t *source);

/* Clear the per-tick "pressed" edges (call at end of frame by the app). */
void fxp_runtime_end_tick(fxp_runtime_t *rt);

/* Expression evaluation (infix token stream -> value).  Uses the frame's
 * current selections; `instance`/`oi` give the "iterated instance"
 * context. */
fxp_value_t fxp_expr_eval(fxp_frame_t *frame, const fxp_expression_t *expr,
                          fxp_object_t *instance, uint32_t oi);

/* Pixel-perfect instance collision (NuclearRT Frame::IsColliding rules:
 * AABB pre-test + alpha-mask per-pixel scan for fine-detection objects).
 * Public for tests and tools. */
bool fxp_collision_objects(fxp_frame_t *frame, fxp_object_t *a, fxp_object_t *b);

/* Current image size of an instance (event_runtime.c; public for the
 * movement engines and tests). */
int32_t fxp_object_image_width(const fxp_frame_t *frame, const fxp_object_t *obj);
int32_t fxp_object_image_height(const fxp_frame_t *frame, const fxp_object_t *obj);

/* Built-in movement engines (movement.c): Bouncing Ball + 8 Directions.
 * init selects the object's movement entry (index within
 * item->common.movements) and sets up speed/direction/moving-at-start;
 * update runs once per tick BEFORE the event pipeline. */
void fxp_object_init_movement(fxp_frame_t *frame, fxp_object_t *obj);
void fxp_object_movement_update(fxp_frame_t *frame, fxp_object_t *obj,
                                double dt);
void fxp_object_movement_stop(fxp_object_t *obj);
void fxp_object_movement_start(fxp_object_t *obj);
void fxp_object_movement_set_speed(fxp_object_t *obj, int32_t speed);
void fxp_object_movement_select(fxp_frame_t *frame, fxp_object_t *obj,
                                int32_t index);
void fxp_object_movement_bounce(fxp_frame_t *frame, fxp_object_t *obj);

/* Compare a Fusion comparison code against a three-way cmp result. */
bool fxp_compare(int32_t comparison, int cmp);

#ifdef __cplusplus
}
#endif
#endif /* FXP_RUNTIME_H */
