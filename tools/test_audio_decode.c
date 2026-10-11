/*
 * Fusion X-Port — tools/test_audio_decode.c
 * Host harness for the pure-C audio decoders.
 *
 *  1. Synthesizes a 440 Hz sine as a 44100 Hz stereo s16 WAV in memory and
 *     decodes it back (always runs, no external files needed).
 *  2. If /tmp test files exist (see README: generate with ffmpeg), also
 *     decodes WAV(u8 mono) / MP3 / OGG payloads.
 */
#include "fxp/audio_decode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_failures = 0;

static void report(const char *what, bool ok, const char *detail)
{
    printf("  %s: %s (%s)\n", ok ? "ok" : "FAIL", what, detail);
    if (!ok)
        g_failures++;
}

static bool load(const char *path, uint8_t **data, size_t *size)
{
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return false;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    *data = malloc((size_t)n);
    if (fread(*data, 1, (size_t)n, fp) != (size_t)n) {
        fclose(fp);
        free(*data);
        return false;
    }
    fclose(fp);
    *size = (size_t)n;
    return true;
}

static int peak_of(const fxp_pcm_t *pcm)
{
    int peak = 0;
    for (int32_t i = 0; i < pcm->frames * pcm->channels; i++) {
        int v = pcm->samples[i] < 0 ? -pcm->samples[i] : pcm->samples[i];
        if (v > peak)
            peak = v;
    }
    return peak;
}

static void check_decode(const char *what, const uint8_t *data, size_t size,
                         int want_rate, int want_ch, int want_min_frames,
                         bool required)
{
    fxp_pcm_t pcm;
    char err[128];
    if (!fxp_audio_decode(data, size, &pcm, err, sizeof(err))) {
        if (required)
            report(what, false, err);
        else
            printf("  skip: %s (decode failed: %s)\n", what, err);
        return;
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "rate=%d ch=%d frames=%d peak=%d",
             pcm.rate, pcm.channels, pcm.frames, peak_of(&pcm));

    bool ok = true;
    if (want_rate && pcm.rate != want_rate)
        ok = false;
    if (want_ch && pcm.channels != want_ch)
        ok = false;
    if (pcm.frames < want_min_frames)
        ok = false;
    if (peak_of(&pcm) < 1000)             /* sine must have amplitude     */
        ok = false;
    report(what, ok, detail);
    fxp_pcm_free(&pcm);
}

/* Build a RIFF/WAVE s16 stereo 44100 sine in memory. */
static void synth_wav(uint8_t **out, size_t *out_size, int seconds)
{
    int rate = 44100, ch = 2;
    int32_t frames = rate * seconds;
    size_t data_len = (size_t)frames * ch * sizeof(int16_t);
    size_t total = 44 + data_len;
    uint8_t *buf = calloc(1, total);
    memcpy(buf, "RIFF", 4);
    uint32_t riff_len = (uint32_t)(total - 8);
    memcpy(buf + 4, &riff_len, 4);
    memcpy(buf + 8, "WAVE", 4);
    memcpy(buf + 12, "fmt ", 4);
    uint32_t fmt_len = 16;
    memcpy(buf + 16, &fmt_len, 4);
    uint16_t fmt_tag = 1, chans = (uint16_t)ch;
    uint32_t rate32 = (uint32_t)rate;
    uint16_t bits = 16;
    uint32_t byte_rate = (uint32_t)(rate * ch * 2);
    uint16_t align = (uint16_t)(ch * 2);
    memcpy(buf + 20, &fmt_tag, 2);
    memcpy(buf + 22, &chans, 2);
    memcpy(buf + 24, &rate32, 4);
    memcpy(buf + 28, &byte_rate, 4);
    memcpy(buf + 32, &align, 2);
    memcpy(buf + 34, &bits, 2);
    memcpy(buf + 36, "data", 4);
    uint32_t dlen = (uint32_t)data_len;
    memcpy(buf + 40, &dlen, 4);
    int16_t *pcm = (int16_t *)(buf + 44);
    for (int32_t f = 0; f < frames; f++) {
        int16_t v = (int16_t)(sin(f * 440.0 * 2.0 * 3.14159265 / rate) *
                              12000.0);
        pcm[f * 2] = v;
        pcm[f * 2 + 1] = v;
    }
    *out = buf;
    *out_size = total;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== Fusion X-Port: audio decode ==\n");

    /* 1. synthesized WAV (self-contained) */
    uint8_t *wav = NULL;
    size_t wav_size = 0;
    synth_wav(&wav, &wav_size, 1);
    check_decode("WAV s16 stereo 44100 (synth)", wav, wav_size,
                 44100, 2, 44000, true);
    free(wav);

    /* 2. garbage must fail cleanly */
    {
        fxp_pcm_t pcm;
        char err[128];
        const uint8_t junk[16] = { 1, 2, 3, 4, 5, 6, 7, 8,
                                   9, 10, 11, 12, 13, 14, 15, 16 };
        bool ok = !fxp_audio_decode(junk, sizeof(junk), &pcm, err, sizeof(err));
        report("unknown format rejected", ok, ok ? err : "decoded junk!");
    }

    /* 3. optional ffmpeg-generated files (see tools/test_audio_decode.c) */
    uint8_t *d = NULL;
    size_t s = 0;
    if (load("/tmp/test44k.wav", &d, &s)) {
        check_decode("WAV s16 stereo 44100 (ffmpeg)", d, s, 44100, 2, 44000,
                     true);
        free(d);
    }
    if (load("/tmp/testmono.wav", &d, &s)) {
        check_decode("WAV u8 mono 22050 (ffmpeg)", d, s, 22050, 1, 21000,
                     true);
        free(d);
    }
    if (load("/tmp/test.mp3", &d, &s)) {
        check_decode("MP3 (ffmpeg, dr_mp3)", d, s, 44100, 0, 44000, true);
        free(d);
    } else {
        printf("  skip: MP3 (create /tmp/test.mp3 with ffmpeg)\n");
    }
    if (load("/tmp/test.ogg", &d, &s)) {
        check_decode("OGG (ffmpeg, stb_vorbis)", d, s, 44100, 0, 44000, true);
        free(d);
    } else {
        printf("  skip: OGG (create /tmp/test.ogg with ffmpeg)\n");
    }

    if (g_failures == 0)
        printf("== AUDIO DECODE: ALL OK ==\n");
    else
        printf("== AUDIO DECODE: %d FAILURE(S) ==\n", g_failures);
    return g_failures ? 1 : 0;
}
