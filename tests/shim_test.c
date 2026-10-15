/* Public FAAD2 API regression tests. Fixtures are deliberately kept verbatim. */
#include "neaacdec.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned failures;
static const char *context;
static size_t frame_index;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s frame %zu line %d: %s\n", context, frame_index, __LINE__, #c); \
    failures++; } } while (0)
#define EQ(actual, expected) do { unsigned long a_ = (actual), e_ = (expected); \
    if (a_ != e_) { fprintf(stderr, "%s frame %zu: %s expected %lu, observed %lu\n", \
    context, frame_index, #actual, e_, a_); failures++; } } while (0)

typedef struct { unsigned char *data; size_t size, header; } Frame;
typedef struct { unsigned char *data; size_t size, count; Frame *frames; } Input;

static void *allocate(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "allocation failed\n"); exit(1); }
    return p;
}

static Input load(const char *dir, const char *name)
{
    Input in = {0};
    size_t offset = 0;
    long length;
    char *path = allocate(strlen(dir) + strlen(name) + 2);
    FILE *f;
    sprintf(path, "%s/%s", dir, name);
    f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    free(path);
    if (fseek(f, 0, SEEK_END) || (length = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) {
        fprintf(stderr, "cannot size fixture %s\n", name); exit(1);
    }
    in.size = (size_t)length;
    in.data = allocate(in.size);
    if (fread(in.data, 1, in.size, f) != in.size) { fprintf(stderr, "short read\n"); exit(1); }
    fclose(f);
    /* A frame occupies at least seven bytes, so this bounds the frame array. */
    in.frames = allocate((in.size / 7 + 1) * sizeof(*in.frames));
    while (offset < in.size) {
        unsigned char *b = in.data + offset;
        size_t n, header;
        if (in.size - offset < 7 || b[0] != 255 || (b[1] & 0xf6) != 0xf0) goto invalid;
        header = (b[1] & 1) ? 7 : 9;
        n = ((size_t)(b[3] & 3) << 11) | ((size_t)b[4] << 3) | (b[5] >> 5);
        if (n < header || n > in.size - offset) goto invalid;
        in.frames[in.count++] = (Frame){ b, n, header };
        offset += n;
    }
    if (in.count < 8) goto invalid;
    return in;
invalid:
    fprintf(stderr, "invalid ADTS fixture %s at %zu\n", name, offset);
    exit(1);
}

static void asc_for(const Input *in, unsigned char asc[2])
{
    const unsigned char *b = in->data;
    unsigned sfi = (b[2] >> 2) & 15;
    unsigned channels = ((b[2] & 1) << 2) | (b[3] >> 6);
    asc[0] = (2 << 3) | (sfi >> 1);
    asc[1] = ((sfi & 1) << 7) | (channels << 3);
}

static NeAACDecHandle open_input(const Input *in, int raw, int fmt, int downmix,
                                unsigned long rate, unsigned channels)
{
    NeAACDecHandle h = NeAACDecOpen();
    NeAACDecConfiguration cfg;
    unsigned long sr = 0;
    unsigned char ch = 0, asc[2];
    CHECK(h != NULL);
    if (!h) exit(1);
    cfg = *NeAACDecGetCurrentConfiguration(h);
    cfg.outputFormat = (unsigned char)fmt;
    cfg.downMatrix = (unsigned char)downmix;
    EQ(NeAACDecSetConfiguration(h, &cfg), 1);
    asc_for(in, asc);
    if (raw) EQ(NeAACDecInit2(h, asc, 2, &sr, &ch), 0);
    else EQ(NeAACDecInit(h, in->data, (unsigned long)in->size, &sr, &ch), 0);
    EQ(sr, rate);
    EQ(ch, channels);
    return h;
}

static void *decode(NeAACDecHandle h, const Input *in, size_t i, int raw, NeAACDecFrameInfo *fi)
{
    Frame f = in->frames[i];
    frame_index = i;
    return NeAACDecDecode(h, fi, f.data + (raw ? f.header : 0),
                         (unsigned long)(f.size - (raw ? f.header : 0)));
}

static void positions(const NeAACDecFrameInfo *fi, int surround)
{
    static const unsigned char stereo[] = {2, 3}, six[] = {1, 2, 3, 6, 7, 9};
    CHECK(memcmp(fi->channel_position, surround ? six : stereo, surround ? 6 : 2) == 0);
    EQ(fi->num_front_channels, surround ? 3 : 2);
    EQ(fi->num_back_channels, surround ? 2 : 0);
    EQ(fi->num_lfe_channels, surround ? 1 : 0);
}

static void stream_test(const Input *in, unsigned long rate, unsigned channels, int he, int compare_raw, int downmix)
{
    NeAACDecHandle adts = open_input(in, 0, 1, downmix, rate, channels);
    NeAACDecHandle raw = compare_raw ? open_input(in, 1, 1, 0, he ? 16000 : rate, channels) : NULL;
    unsigned long consumed = 0;
    size_t i;
    for (i = 0; i < in->count; i++) {
        NeAACDecFrameInfo a, r;
        void *p = decode(adts, in, i, 0, &a);
        unsigned out_ch = downmix ? 2 : channels;
        CHECK(p != NULL);
        EQ(a.error, 0);
        EQ(a.samples, i ? (he ? 2048 : 1024) * out_ch : 0);
        EQ(a.channels, out_ch);
        EQ(a.samplerate, rate);
        EQ(a.header_type, ADTS);
        EQ(a.object_type, he ? HE_AAC : LC);
        EQ(a.sbr, he ? SBR_UPSAMPLED : NO_SBR);
        if (out_ch != 1) positions(&a, out_ch == 6);
        consumed += a.bytesconsumed;
        if (raw) {
            void *q = decode(raw, in, i, 1, &r);
            CHECK(q != NULL);
            EQ(r.error, 0);
            EQ(r.samples, i ? (he ? 4096 : 2048) : 0);
            EQ(r.channels, channels);
            EQ(r.samplerate, rate);
            EQ(r.header_type, RAW);
            EQ(r.object_type, he ? HE_AAC : LC);
            EQ(r.sbr, he ? SBR_UPSAMPLED : NO_SBR);
            positions(&r, 0);
            EQ(r.bytesconsumed, in->frames[i].size - in->frames[i].header);
            if (p && q && a.samples == r.samples) CHECK(memcmp(p, q, a.samples * 2) == 0);
        }
    }
    EQ(consumed, in->size);
    NeAACDecClose(adts);
    NeAACDecClose(raw);
}

static void formats_test(const Input *in)
{
    void *pcm[5] = {0};
    unsigned fmt;
    size_t i;
    for (fmt = 1; fmt <= 5; fmt++) {
        NeAACDecHandle h = open_input(in, 0, fmt, 0, 44100, 2);
        NeAACDecFrameInfo fi;
        void *p;
        decode(h, in, 0, 0, &fi);
        EQ(fi.error, 0);
        p = decode(h, in, 1, 0, &fi);
        CHECK(p != NULL);
        EQ(fi.error, 0);
        EQ(fi.samples, 2048);
        pcm[fmt-1] = allocate(2048 * 8);
        memset(pcm[fmt-1], 0, 2048 * 8);
        if (p && fi.samples == 2048) memcpy(pcm[fmt-1], p, 2048 * (fmt == 1 ? 2 : fmt == 5 ? 8 : 4));
        NeAACDecClose(h);
    }
    for (i = 0; i < 2048; i++) {
        double f = ((float *)pcm[3])[i], d = ((double *)pcm[4])[i];
        double a = ((int16_t *)pcm[0])[i], b = ((int32_t *)pcm[1])[i], c = ((int32_t *)pcm[2])[i];
        CHECK(isfinite(f) && fabs(f) <= 1.0);
        CHECK(isfinite(d) && fabs(d) <= 1.0);
        CHECK(a >= -32768 && a <= 32767);
        CHECK(b >= -8388608 && b <= 8388607);
        CHECK(c >= -2147483648.0 && c <= 2147483647.0);
        CHECK(fabs(a - round(f * 32768)) <= 1);
        CHECK(fabs(b - round(f * 8388608)) <= 2);
        CHECK(fabs(c - round(f * 2147483648.0)) <= 512);
        CHECK(fabs(d - f) <= 1e-6);
    }
    for (i = 0; i < 5; i++) free(pcm[i]);
}

static void seek_test(const Input *in)
{
    static const long resets[] = {0, 1, -1};
    size_t k, i;
    for (k = 0; k < 3; k++) {
        NeAACDecHandle h = open_input(in, 0, 1, 0, 44100, 2);
        NeAACDecFrameInfo fi;
        for (i = 0; i < 5; i++) { CHECK(decode(h, in, i, 0, &fi) != NULL); EQ(fi.error, 0); }
        NeAACDecPostSeekReset(h, resets[k]);
        CHECK(decode(h, in, 5, 0, &fi) != NULL);
        EQ(fi.error, 0);
        EQ(fi.samples, resets[k] == 0 ? 0 : 2048);
        CHECK(decode(h, in, 6, 0, &fi) != NULL);
        EQ(fi.error, 0);
        EQ(fi.samples, 2048);
        NeAACDecClose(h);
    }
}

static void decode2_test(const Input *in)
{
    int small;
    for (small = 0; small <= 1; small++) {
        NeAACDecHandle a = open_input(in, 0, 1, 0, 44100, 2);
        NeAACDecHandle b = open_input(in, 0, 1, 0, 44100, 2);
        NeAACDecFrameInfo fi, ref;
        int16_t caller[2048];
        void *dest = caller, *p, *q;
        Frame f = in->frames[1];
        decode(a, in, 0, 0, &ref);
        decode(b, in, 0, 0, &fi);
        q = decode(a, in, 1, 0, &ref);
        p = NeAACDecDecode2(b, &fi, f.data, (unsigned long)f.size, &dest, small ? sizeof(caller)-1 : sizeof(caller));
        if (small) { CHECK(p == NULL); EQ(fi.error, 27); }
        else {
            CHECK(p == caller);
            EQ(fi.error, 0);
            EQ(fi.samples, 2048);
            EQ(ref.samples, 2048);
            if (p && q) CHECK(memcmp(p, q, sizeof(caller)) == 0);
        }
        NeAACDecClose(a);
        NeAACDecClose(b);
    }
}

static void hygiene_test(const Input *in)
{
    static const struct { unsigned char code; const char *message; } errors[] = {
        {5, "Unable to find ADTS syncword"}, {12, "Invalid number of channels"},
        {14, "Input data buffer too small"}, {21, "Unexpected channel configuration change"},
        {27, "Output data buffer too small"}
    };
    NeAACDecHandle h = NeAACDecOpen(), ref;
    NeAACDecConfiguration cfg;
    NeAACDecFrameInfo fi, ri;
    mp4AudioSpecificConfig info;
    unsigned char asc[2], garbage[] = {255,255}, zeros[100] = {0}, ch;
    unsigned long sr;
    size_t i;
    void *p, *q;
    CHECK(h != NULL);
    if (!h) exit(1);
    cfg = *NeAACDecGetCurrentConfiguration(h);
    EQ(cfg.defObjectType, MAIN);
    EQ(cfg.outputFormat, FAAD_FMT_16BIT);
    EQ(NeAACDecSetConfiguration(h, &cfg), 1);
    cfg.outputFormat = 0; EQ(NeAACDecSetConfiguration(h, &cfg), 0);
    cfg.outputFormat = 6; EQ(NeAACDecSetConfiguration(h, &cfg), 0);
    cfg.outputFormat = 1; EQ(NeAACDecSetConfiguration(h, &cfg), 1);
    CHECK(decode(h, in, 0, 0, &fi) == NULL); CHECK(fi.error != 0);
    CHECK(NeAACDecInit(h, zeros, sizeof(zeros), &sr, &ch) < 0);
    CHECK((signed char)NeAACDecInit2(h, garbage, 2, &sr, &ch) < 0); /* plain char is unsigned on ARM Linux */
    asc_for(in, asc);
    EQ(NeAACDecInit2(h, asc, 2, &sr, &ch), 0);
    cfg = *NeAACDecGetCurrentConfiguration(h);
    cfg.outputFormat = FAAD_FMT_FLOAT;
    EQ(NeAACDecSetConfiguration(h, &cfg), 1);
    ref = open_input(in, 1, FAAD_FMT_FLOAT, 0, 44100, 2);
    for (i = 0; i < 2; i++) {
        p = decode(h, in, i, 1, &fi); q = decode(ref, in, i, 1, &ri);
        CHECK(p != NULL && q != NULL); EQ(fi.error, 0); EQ(fi.samples, i ? 2048 : 0);
        if (i && p && q && fi.samples == 2048 && ri.samples == 2048)
            CHECK(memcmp(p, q, 2048 * sizeof(float)) == 0);
    }
    NeAACDecClose(ref);
    NeAACDecClose(h);
    for (i = 0; i < sizeof(errors)/sizeof(errors[0]); i++) {
        const char *s = NeAACDecGetErrorMessage(errors[i].code);
        CHECK(s != NULL && strcmp(s, errors[i].message) == 0);
    }
    CHECK(NeAACDecGetCapabilities() & LC_DEC_CAP);
    CHECK(NeAACDecAudioSpecificConfig(asc, 2, &info) >= 0);
    EQ(info.samplingFrequency, 44100); EQ(info.channelsConfiguration, 2);
    CHECK((signed char)NeAACDecAudioSpecificConfig(garbage, 2, &info) < 0);
    NeAACDecClose(NULL);
}

static void corrupt_test(const Input *in)
{
    NeAACDecHandle h;
    NeAACDecFrameInfo fi;
    Frame f = in->frames[0];
    unsigned char *bad = allocate(f.size), zeros[100] = {0};
    void *p;
    memcpy(bad, f.data, f.header);
    memset(bad + f.header, 0, f.size - f.header);
    h = open_input(in, 0, 1, 0, 44100, 2);
    p = NeAACDecDecode(h, &fi, bad, (unsigned long)f.size);
    CHECK((fi.error != 0 && fi.bytesconsumed > 0) || (p != NULL && fi.error == 0));
    NeAACDecClose(h);
    free(bad);
    h = open_input(in, 0, 1, 0, 44100, 2);
    CHECK(NeAACDecDecode(h, &fi, f.data, 3) == NULL);
    EQ(fi.error, 14); EQ(fi.bytesconsumed, 0);
    NeAACDecClose(h);
    h = open_input(in, 0, 1, 0, 44100, 2);
    CHECK(NeAACDecDecode(h, &fi, zeros, sizeof(zeros)) == NULL);
    EQ(fi.error, 5);
    NeAACDecClose(h);
}

/* FAAD2 skips junk before an ADTS syncword inside a single call, and a
 * configuration change after the first frame must not corrupt later output. */
static void resync_and_config_test(const Input *in)
{
    unsigned char *spliced;
    size_t junk = 40, cut = 0, i, frames = 0;
    unsigned long rate;
    unsigned char ch;
    NeAACDecHandle h;
    NeAACDecFrameInfo fi;
    NeAACDecConfigurationPtr cfg;

    for (i = 0; i < 5 && i < in->count; i++) cut += in->frames[i].size;
    spliced = allocate(in->size + junk);
    memcpy(spliced, in->data, cut);
    memset(spliced + cut, 0x11, junk);
    memcpy(spliced + cut + junk, in->data + cut, in->size - cut);

    h = NeAACDecOpen();
    CHECK(NeAACDecInit(h, spliced, in->size + junk, &rate, &ch) == 0);
    for (i = 0; i < in->size + junk;) {
        void *o = NeAACDecDecode(h, &fi, spliced + i, in->size + junk - i);
        CHECK(fi.error == 0);
        if (fi.error) break;
        CHECK(o != NULL);
        i += fi.bytesconsumed;
        frames++;
    }
    CHECK(frames == in->count);
    NeAACDecClose(h);
    free(spliced);

    /* configuration changes after the first frame apply from the next Init */
    h = NeAACDecOpen();
    CHECK(NeAACDecInit(h, in->data, in->size, &rate, &ch) == 0);
    CHECK(NeAACDecDecode(h, &fi, in->data, in->size) != NULL);
    cfg = NeAACDecGetCurrentConfiguration(h);
    cfg->downMatrix = 1;
    cfg->outputFormat = FAAD_FMT_FLOAT;
    CHECK(NeAACDecSetConfiguration(h, cfg) == 1);
    for (i = in->frames[0].size, frames = 1; frames < 6; frames++) {
        void *o = NeAACDecDecode(h, &fi, in->data + i, in->size - i);
        CHECK(o != NULL && fi.error == 0 && fi.samples == 2048 && fi.channels == 2);
        if (o && fi.samples == 2048) {
            /* the format in force when the decoder opened stays in effect */
            const short *p = o;
            int peak = 0;
            size_t k;
            for (k = 0; k < 2048; k++) {
                int a = p[k] < 0 ? -p[k] : p[k];
                if (a > peak) peak = a;
            }
            CHECK(peak > 10);
        }
        i += fi.bytesconsumed;
    }
    NeAACDecClose(h);
}

/* Enabling the downmix mid-stream must not reinterpret the 16-bit scratch. */
static void downmix_midstream_test(const Input *in)
{
    unsigned long rate;
    unsigned char ch;
    NeAACDecFrameInfo fi;
    NeAACDecConfigurationPtr cfg;
    NeAACDecHandle h = NeAACDecOpen();
    size_t i = 0, k;

    CHECK(NeAACDecInit(h, in->data, in->size, &rate, &ch) == 0);
    for (k = 0; k < 4; k++) {
        CHECK(NeAACDecDecode(h, &fi, in->data + i, in->size - i) != NULL);
        i += fi.bytesconsumed;
        if (k == 1) {
            cfg = NeAACDecGetCurrentConfiguration(h);
            cfg->downMatrix = 1;
            CHECK(NeAACDecSetConfiguration(h, cfg) == 1);
        }
        if (k >= 2) CHECK(fi.channels == 6 && fi.samples == 6144);
    }
    /* after a seek the new setting applies */
    NeAACDecPostSeekReset(h, 0);
    CHECK(NeAACDecDecode(h, &fi, in->data + i, in->size - i) != NULL);
    i += fi.bytesconsumed;
    CHECK(NeAACDecDecode(h, &fi, in->data + i, in->size - i) != NULL);
    CHECK(fi.channels == 2 && fi.samples == 2048);
    NeAACDecClose(h);
}

/* DAB+ streams: 960-sample granules and the sync-extension HE signalling. */
static void dab_test(const char *dir)
{
    static const struct { const char *name; unsigned ch, out_ch; unsigned long rate, per_frame; int sbr, ps; } v[] = {
        {"lc_mono32", 1, 1, 32000, 960, 0, 0},
        {"lc_st48", 2, 2, 48000, 1920, 0, 0},
        {"he2_st48", 2, 2, 48000, 3840, 1, 1}
    };
    size_t k;
    for (k = 0; k < 3; k++) {
        char path[512];
        FILE *f;
        unsigned char *d;
        long n, pos;
        unsigned long rate;
        unsigned char ch;
        NeAACDecHandle h = NeAACDecOpen();
        NeAACDecFrameInfo fi;
        unsigned frames = 0;
        uint32_t asc_len;

        snprintf(path, sizeof path, "%s/dab/%s.dab", dir, v[k].name);
        f = fopen(path, "rb");
        CHECK(f != NULL);
        if (!f) { NeAACDecClose(h); continue; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        d = allocate((size_t)n);
        CHECK(fread(d, 1, (size_t)n, f) == (size_t)n);
        fclose(f);
        asc_len = d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24;
        CHECK((signed char)NeAACDecInit2(h, d + 4, asc_len, &rate, &ch) == 0);
        for (pos = 4 + asc_len; pos + 4 <= n; frames++) {
            uint32_t len = d[pos] | d[pos + 1] << 8 | d[pos + 2] << 16 | (uint32_t)d[pos + 3] << 24;
            void *o = NeAACDecDecode(h, &fi, d + pos + 4, len);
            CHECK(o != NULL && fi.error == 0);
            if (frames == 0) CHECK(fi.samples == 0);
            else CHECK(fi.samples == v[k].per_frame && fi.channels == v[k].out_ch &&
                       fi.samplerate == v[k].rate && fi.sbr == v[k].sbr && fi.ps == v[k].ps);
            pos += 4 + len;
        }
        CHECK(frames >= 8);
        free(d);
        NeAACDecClose(h);
    }
    {
        /* dablin-style ASC: AOT 2 at the 24 kHz core plus the SBR sync extension */
        unsigned char asc[] = {0x13, 0x08, 0x56, 0xe5, 0x98};
        unsigned long rate;
        unsigned char ch;
        NeAACDecHandle h = NeAACDecOpen();
        CHECK((signed char)NeAACDecInit2(h, asc, sizeof asc, &rate, &ch) == 0);
        CHECK(rate == 48000 && ch == 1);
        NeAACDecClose(h);
    }
}

/* Junk that contains false syncwords must not hide the frames behind it. */
static size_t decode_stepping(const unsigned char *d, size_t n)
{
    unsigned long rate;
    unsigned char ch;
    NeAACDecHandle h = NeAACDecOpen();
    NeAACDecFrameInfo fi;
    size_t pos, ok = 0;

    if (NeAACDecInit(h, (unsigned char *)d, n, &rate, &ch) != 0) { NeAACDecClose(h); return 0; }
    for (pos = 0; pos < n;) {
        void *o = NeAACDecDecode(h, &fi, (unsigned char *)d + pos, n - pos);
        pos += fi.bytesconsumed ? fi.bytesconsumed : 1;
        if (o && !fi.error) ok++;
    }
    NeAACDecClose(h);
    return ok;
}

static void junk_test(const Input *in)
{
    static const unsigned char bad_rate[] = {0xff, 0xf1, 0x3c, 0x80, 0x10, 0x20, 0xfc};
    static const unsigned char bad_len[] = {0xff, 0xf1, 0x50, 0x80, 0xff, 0xe0, 0xfc};
    size_t cut = 0, i, k;
    unsigned char *buf = allocate(in->size + 4096);
    const unsigned char *fake[] = {bad_rate, bad_len};

    for (i = 0; i < 5; i++) cut += in->frames[i].size;
    for (k = 0; k < 2; k++) {
        size_t n = 0;
        memcpy(buf, in->data, cut); n = cut;
        memcpy(buf + n, fake[k], 7); n += 7;
        memset(buf + n, 0x22, 30); n += 30;
        memcpy(buf + n, in->data + cut, in->size - cut); n += in->size - cut;
        CHECK(decode_stepping(buf, n) == in->count);
    }
    {   /* repeated syncword-like junk */
        size_t n = cut;
        memcpy(buf, in->data, cut);
        for (i = 0; i < 10; i++) { buf[n++] = 0xff; buf[n++] = 0xf1; }
        memset(buf + n, 0x33, 20); n += 20;
        memcpy(buf + n, in->data + cut, in->size - cut); n += in->size - cut;
        CHECK(decode_stepping(buf, n) == in->count);
    }
    {   /* a tag longer than the search window in front of the stream */
        size_t n = 1000;
        memset(buf, 0, n);
        memcpy(buf, "ID3", 3);
        memcpy(buf + n, in->data, in->size); n += in->size;
        CHECK(decode_stepping(buf, n) == in->count);
    }
    free(buf);
}

int main(int argc, char **argv)
{
    static const char *names[] = {"stereo44_lc.aac", "surround51_lc.aac", "stereo32_he.aac", "mono24_lc.aac"};
    Input inputs[4];
    size_t i;
    if (argc != 2) { fprintf(stderr, "usage: %s data-directory\n", argv[0]); return 1; }
    for (i = 0; i < 4; i++) inputs[i] = load(argv[1], names[i]);
    context = names[0]; stream_test(&inputs[0], 44100, 2, 0, 1, 0);
    context = names[1]; stream_test(&inputs[1], 44100, 6, 0, 0, 0);
    context = "surround downmix"; stream_test(&inputs[1], 44100, 6, 0, 0, 1);
    context = names[2]; stream_test(&inputs[2], 32000, 2, 1, 1, 0);
    context = names[3]; stream_test(&inputs[3], 24000, 1, 0, 0, 0);
    context = "formats"; formats_test(&inputs[0]);
    context = "seek"; seek_test(&inputs[0]);
    context = "Decode2"; decode2_test(&inputs[0]);
    context = "API hygiene"; hygiene_test(&inputs[0]);
    context = "corrupt input"; corrupt_test(&inputs[0]);
    context = "resync and config"; resync_and_config_test(&inputs[0]);
    context = "mid-stream downmix"; downmix_midstream_test(&inputs[1]);
    context = "DAB+"; dab_test(argv[1]);
    context = "junk and false syncs"; junk_test(&inputs[0]);
    for (i = 0; i < 4; i++) { free(inputs[i].frames); free(inputs[i].data); }
    if (failures) fprintf(stderr, "%u checks failed\n", failures);
    else puts("All shim checks passed");
    return failures ? 1 : 0;
}
