/*
 * Fusion X-Port — audio_decode.h
 * Pure-C audio decoders: any Fusion sound payload (WAV / MP3 / OGG) is
 * decoded to a single canonical 16-bit interleaved PCM buffer ("converted
 * to WAV" without the file).  No SDL dependency, so the core test harness
 * can exercise it on any platform.
 *
 * Decoders (vendored under source/thirdparty/):
 *   WAV  — built-in RIFF parser (PCM u8/s16/s24/s32 + IEEE float32)
 *   MP3  — dr_mp3 (public domain / MIT-0, author David Reid)
 *   OGG  — stb_vorbis (public domain, author Sean Barrett)
 */
#ifndef FXP_AUDIO_DECODE_H
#define FXP_AUDIO_DECODE_H

#include "fxp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Decoded sample payload ("WAV-equivalent"): signed 16-bit interleaved. */
typedef struct fxp_pcm {
    int16_t *samples;                 /* interleaved, owned               */
    int32_t  frames;                  /* per-channel frames               */
    int32_t  channels;                /* 1..2                             */
    int32_t  rate;                    /* sample rate in Hz                */
} fxp_pcm_t;

void fxp_pcm_free(fxp_pcm_t *pcm);

/* Decode a sound bank payload.  Returns false and fills `err` on failure.
 * Sniffs the container: "RIFF....WAVE" -> WAV, "OggS" -> Vorbis,
 * "ID3" or MPEG frame sync -> MP3. */
bool fxp_audio_decode(const uint8_t *data, size_t size, fxp_pcm_t *out,
                      char *err, size_t err_cap);

#ifdef __cplusplus
}
#endif
#endif /* FXP_AUDIO_DECODE_H */
