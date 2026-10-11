/*
 * Fusion X-Port — audio_decode.c
 * WAV/MP3/OGG -> canonical 16-bit PCM ("convert to WAV" in memory).
 *
 * MFA sound/music payloads are container-agnostic: Fusion stores whatever
 * the author imported.  The old backend only understood RIFF/WAVE and
 * rejected everything else, so MP3/OGG games were silent.  This module
 * normalises every supported container to S16 PCM which the SDL mixer
 * then resamples per channel.
 */
#include "fxp/audio_decode.h"

#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------ WAV (RIFF) -- */

static uint16_t rd_u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Convert one sample of `bytes` width (format 1=PCM int, 3=IEEE float). */
static int32_t wav_sample_to_s16(const uint8_t *p, int bytes, int fmt)
{
    if (fmt == 3) {                               /* float32 */
        uint32_t bits = rd_u32(p);
        float f;
        memcpy(&f, &bits, 4);
        if (f > 1.0f) f = 1.0f;
        if (f < -1.0f) f = -1.0f;
        return (int32_t)(f * 32767.0f);
    }
    switch (bytes) {
    case 1:  return (int32_t)((int16_t)((p[0] - 128) << 8));   /* u8 */
    case 2:  return (int32_t)(int16_t)rd_u16(p);               /* s16 */
    case 3: {                                                  /* s24 */
        uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16);
        if (v & 0x800000u)
            v |= 0xFF000000u;
        return (int32_t)v >> 8;
    }
    case 4:  return ((int32_t)rd_u32(p)) >> 16;                /* s32 */
    default: return 0;
    }
}

static bool wav_decode(const uint8_t *data, size_t size, fxp_pcm_t *out,
                       char *err, size_t err_cap)
{
    if (size < 12 || memcmp(data, "RIFF", 4) != 0 ||
        memcmp(data + 8, "WAVE", 4) != 0) {
        snprintf(err, err_cap, "not a RIFF/WAVE blob");
        return false;
    }

    int fmt = -1, channels = 0, rate = 0, bits = 0;
    const uint8_t *payload = NULL;
    size_t payload_size = 0;

    size_t pos = 12;
    while (pos + 8 <= size) {
        const uint8_t *id = data + pos;
        uint32_t len = rd_u32(data + pos + 4);
        pos += 8;
        if (len > size - pos)
            len = (uint32_t)(size - pos);
        if (memcmp(id, "fmt ", 4) == 0 && len >= 16) {
            fmt      = (int16_t)rd_u16(data + pos);
            channels = (int16_t)rd_u16(data + pos + 2);
            rate     = (int32_t)rd_u32(data + pos + 4);
            bits     = (int16_t)rd_u16(data + pos + 14);
            /* WAVE_FORMAT_EXTENSIBLE: real format lives in the GUID tail */
            if (fmt == 0xFFFE && len >= 40) {
                int sub = (int16_t)rd_u16(data + pos + 24);
                if (sub == 1 || sub == 3)
                    fmt = sub;
            }
        } else if (memcmp(id, "data", 4) == 0) {
            payload = data + pos;
            payload_size = len;
        }
        pos += len;
        if (len & 1)
            pos++;                                /* chunks are word-aligned */
    }

    if (fmt != 1 && fmt != 3) {
        snprintf(err, err_cap, "unsupported WAV format tag %d", fmt);
        return false;
    }
    if (channels < 1 || channels > 2 || rate <= 0 || !payload) {
        snprintf(err, err_cap, "unsupported WAV layout (ch=%d rate=%d)",
                 channels, rate);
        return false;
    }
    int bps = bits / 8;
    if ((fmt == 1 && bps != 1 && bps != 2 && bps != 3 && bps != 4) ||
        (fmt == 3 && bps != 4)) {
        snprintf(err, err_cap, "unsupported WAV bit depth %d", bits);
        return false;
    }

    size_t frame_bytes = (size_t)(bps * channels);
    int32_t frames = (int32_t)(payload_size / frame_bytes);
    int16_t *samples = fxp_malloc((size_t)(frames ? frames : 1) *
                                  (size_t)channels * sizeof(int16_t));
    for (int32_t f = 0; f < frames; f++) {
        const uint8_t *fr = payload + (size_t)f * frame_bytes;
        for (int c = 0; c < channels; c++)
            samples[(size_t)f * channels + c] =
                (int16_t)wav_sample_to_s16(fr + c * bps, bps, fmt);
    }

    out->samples  = samples;
    out->frames   = frames;
    out->channels = channels;
    out->rate     = rate;
    return true;
}

/* ------------------------------------------------------------ MP3 (dr_mp3) -- */

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_STDIO
#include "thirdparty/dr_mp3.h"

static bool mp3_decode(const uint8_t *data, size_t size, fxp_pcm_t *out,
                       char *err, size_t err_cap)
{
    drmp3_config cfg;                 /* out: channels + sample rate      */
    memset(&cfg, 0, sizeof(cfg));
    drmp3_uint64 frames = 0;
    drmp3_int16 *pcm = drmp3_open_memory_and_read_pcm_frames_s16(
        data, size, &cfg, &frames, NULL);
    if (!pcm || frames == 0 || cfg.sampleRate == 0) {
        snprintf(err, err_cap, "MP3 decode failed (dr_mp3)");
        if (pcm)
            drmp3_free(pcm, NULL);
        return false;
    }
    out->samples  = pcm;              /* dr_mp3 alloc is malloc-compatible */
    out->frames   = (int32_t)frames;
    out->channels = cfg.channels ? (int32_t)cfg.channels : 2;
    out->rate     = (int32_t)cfg.sampleRate;
    return true;
}

/* ------------------------------------------------------------ OGG (vorbis) -- */

#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
/* vendor code triggers -Wmaybe-uninitialized in the seek paths */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "thirdparty/stb_vorbis.c"
#pragma GCC diagnostic pop

static bool ogg_decode(const uint8_t *data, size_t size, fxp_pcm_t *out,
                       char *err, size_t err_cap)
{
    int channels = 0, rate = 0;
    short *pcm = NULL;
    int frames = stb_vorbis_decode_memory(data, (int)size,
                                          &channels, &rate, &pcm);
    if (frames <= 0 || !pcm) {
        snprintf(err, err_cap, "OGG decode failed (stb_vorbis)");
        if (pcm)
            free(pcm);
        return false;
    }
    out->samples  = pcm;
    out->frames   = frames;
    out->channels = channels;
    out->rate     = rate;
    return true;
}

/* ------------------------------------------------------------------ entry -- */

void fxp_pcm_free(fxp_pcm_t *pcm)
{
    if (!pcm)
        return;
    free(pcm->samples);
    memset(pcm, 0, sizeof(*pcm));
}

bool fxp_audio_decode(const uint8_t *data, size_t size, fxp_pcm_t *out,
                      char *err, size_t err_cap)
{
    memset(out, 0, sizeof(*out));
    if (err_cap)
        err[0] = '\0';
    if (!data || size < 4) {
        snprintf(err, err_cap, "empty sound payload");
        return false;
    }

    if (memcmp(data, "RIFF", 4) == 0 && size >= 12 &&
        memcmp(data + 8, "WAVE", 4) == 0)
        return wav_decode(data, size, out, err, err_cap);

    if (memcmp(data, "OggS", 4) == 0)
        return ogg_decode(data, size, out, err, err_cap);

    if (memcmp(data, "ID3", 3) == 0 ||
        (data[0] == 0xFF && (data[1] & 0xE0) == 0xE0))
        return mp3_decode(data, size, out, err, err_cap);

    snprintf(err, err_cap, "unknown sound format (%02X %02X %02X %02X)",
             data[0], data[1], data[2], data[3]);
    return false;
}
