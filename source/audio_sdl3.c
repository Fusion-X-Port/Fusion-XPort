/*
 * Fusion X-Port — audio_sdl3.c
 * SDL3 audio backend.  Sound payloads (WAV/MP3/OGG) are normalised to
 * 16-bit PCM by audio_decode.c ("converted to WAV" in memory) and mixed
 * with per-channel linear-interpolated resampling into one output
 * stream, so any sample rate / channel layout plays at the correct pitch.
 *
 * Callback contract (SDL3): the on-demand stream callback receives
 * `additional_amount` = how many MORE input-format bytes the stream
 * needs right now.  Feeding `total_amount` instead (the old behaviour)
 * overfills the stream on every call — buffered latency grew without
 * bound and sounds drifted seconds behind the game.  Mix output is now
 * also saturated: S16 wraparound made overlapping samples crackle.
 */
#include "fxp/backend.h"
#include "fxp/audio_decode.h"

#include <SDL3/SDL.h>

#define FXP_MAX_CHANNELS 8
#define FXP_DEV_RATE     44100
#define FXP_DEV_CH       2

typedef struct snd_entry {
    bool     active;
    bool     loaded;
    int16_t *pcm;                     /* interleaved S16 (audio_decode)   */
    int32_t  frames;                  /* per-channel frames               */
    int32_t  channels;
    int32_t  rate;
    double   cursor;                  /* fractional playback position     */
    int      loops_left;              /* -1 = infinite                    */
} snd_entry_t;

struct fxp_audio {
    SDL_AudioStream *out;             /* device-facing stream             */
    snd_entry_t      chans[FXP_MAX_CHANNELS];
};

static int16_t clamp16(int32_t v)
{
    if (v > 32767)
        return 32767;
    if (v < -32768)
        return -32768;
    return (int16_t)v;
}

/* Sample `c` (0/1) of source frame `idx`, clamped; mono is centred. */
static int32_t entry_sample(const snd_entry_t *s, int32_t idx, int c)
{
    if (idx < 0)
        idx = 0;
    if (idx > s->frames - 1)
        idx = s->frames - 1;
    if (s->channels == 1)
        return s->pcm[idx];
    return s->pcm[(size_t)idx * 2 + (c & 1)];
}

/* Per-channel mix: linear-interpolated fetch with rate conversion,
 * saturating into the stereo S16 destination. */
static void mix_entry(snd_entry_t *s, int16_t *dst, int dst_frames)
{
    if (s->channels < 1 || s->rate <= 0 || s->frames <= 0)
        return;
    const double step = (double)s->rate / (double)FXP_DEV_RATE;
    for (int f = 0; f < dst_frames; f++) {
        if (s->cursor >= (double)s->frames) {
            if (s->loops_left == 0) {
                s->active = false;
                break;
            }
            if (s->loops_left > 0)
                s->loops_left--;
            s->cursor = 0.0;
        }
        int32_t i0 = (int32_t)s->cursor;
        if (i0 > s->frames - 1)
            i0 = s->frames - 1;
        int32_t i1 = (i0 + 1 < s->frames) ? i0 + 1 : i0;
        double t = s->cursor - (double)i0;
        for (int c = 0; c < 2; c++) {
            int32_t a = entry_sample(s, i0, c);
            int32_t b = entry_sample(s, i1, c);
            int32_t v = a + (int32_t)((double)(b - a) * t);
            size_t di = (size_t)f * 2 + (size_t)c;
            dst[di] = clamp16((int32_t)dst[di] + v);
        }
        s->cursor += step;
    }
}

/*
 * Runs on the SDL audio thread WITH the stream lock held (SDL3 calls it
 * from inside the locked SDL_GetAudioStreamData), so the main thread's
 * SDL_LockAudioStream in play/load/stop_all serialises against us and
 * we must NOT take the lock again here (non-recursive mutex).
 */
static void mix_cb(void *userdata, SDL_AudioStream *stream,
                   int additional_amount, int total_amount)
{
    (void)total_amount;
    fxp_audio_t *a = (fxp_audio_t *)userdata;

    const int frame_bytes = FXP_DEV_CH * (int)sizeof(int16_t);
    int want = additional_amount - additional_amount % frame_bytes;
    if (want <= 0)
        return;
    int frames = want / frame_bytes;

    int16_t *buf = (int16_t *)SDL_malloc((size_t)frames * frame_bytes);
    if (!buf)
        return;
    memset(buf, 0, (size_t)frames * frame_bytes);

    for (int i = 0; i < FXP_MAX_CHANNELS; i++) {
        snd_entry_t *s = &a->chans[i];
        if (s->active && s->loaded)
            mix_entry(s, buf, frames);
    }

    SDL_PutAudioStreamData(stream, buf, frames * frame_bytes);
    SDL_free(buf);
}

fxp_audio_t *fxp_audio_create(void)
{
    fxp_audio_t *a = fxp_calloc(1, sizeof(fxp_audio_t));

    const SDL_AudioSpec want = { SDL_AUDIO_S16, FXP_DEV_CH, FXP_DEV_RATE };
    a->out = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK,
                                       &want, mix_cb, a);
    if (!a->out) {
        fxp_log_warn("audio: no playback device (%s) — running silent",
                     SDL_GetError());
        return a;
    }
    SDL_ResumeAudioStreamDevice(a->out);
    return a;
}

void fxp_audio_destroy(fxp_audio_t *a)
{
    if (!a)
        return;
    for (int i = 0; i < FXP_MAX_CHANNELS; i++)
        free(a->chans[i].pcm);
    if (a->out)
        SDL_DestroyAudioStream(a->out);
    free(a);
}

int fxp_audio_load(fxp_audio_t *a, const uint8_t *data, size_t size)
{
    int slot = -1;
    for (int i = 0; i < FXP_MAX_CHANNELS; i++) {
        if (!a->chans[i].loaded) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        fxp_log_warn("audio: all %d channels used", FXP_MAX_CHANNELS);
        return -1;
    }

    fxp_pcm_t pcm;
    char err[128];
    if (!fxp_audio_decode(data, size, &pcm, err, sizeof(err))) {
        fxp_log_warn("audio: decode failed — %s", err);
        return -1;
    }
    fxp_log_info("audio: decoded %d Hz, %d ch, %d frame(s)",
                 pcm.rate, pcm.channels, pcm.frames);

    if (a->out)
        SDL_LockAudioStream(a->out);
    snd_entry_t *s = &a->chans[slot];
    s->loaded   = true;
    s->pcm      = pcm.samples;
    s->frames   = pcm.frames;
    s->channels = pcm.channels;
    s->rate     = pcm.rate;
    s->cursor   = 0.0;
    s->active   = false;
    if (a->out)
        SDL_UnlockAudioStream(a->out);
    return slot;
}

void fxp_audio_play(fxp_audio_t *a, int channel, int loops)
{
    if (channel < 0 || channel >= FXP_MAX_CHANNELS)
        return;
    snd_entry_t *s = &a->chans[channel];
    if (!s->loaded)
        return;
    if (a->out)
        SDL_LockAudioStream(a->out);
    s->cursor = 0;
    s->loops_left = loops;
    s->active = true;
    if (a->out)
        SDL_UnlockAudioStream(a->out);
}

void fxp_audio_stop_all(fxp_audio_t *a)
{
    if (a->out)
        SDL_LockAudioStream(a->out);
    for (int i = 0; i < FXP_MAX_CHANNELS; i++)
        a->chans[i].active = false;
    if (a->out)
        SDL_UnlockAudioStream(a->out);
}
