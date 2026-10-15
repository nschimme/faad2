/*
** FAAD2 compatibility layer over libfaad3.
**
** Exports the NeAACDec* API of FAAD2 on top of the faad3 decoder API.
** Everything legacy (frame counter, output format conversion, FAAD2 channel
** order and downmix, error numbers) lives here so libfaad3 stays clean.
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
*/

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <faad3.h>

#include "neaacdec.h"

#define SHIM_MAX_CHANNELS 64
#define SYNC_SEARCH 768
#define DRC_TARGET 80   /* quarter-dB below full scale, i.e. -20 dBFS */

/* FAAD2 error numbers; the strings are indexed by number. */
enum {
    ERR_NONE = 0,
    ERR_BITSTREAM = 10,
    ERR_NO_SYNC = 5,
    ERR_BAD_CHANNELS = 12,
    ERR_INPUT_SMALL = 14,
    ERR_OUTPUT_SMALL = 27,
    ERR_NOT_ALLOWED = 32
};

static char *const err_msg[] = {
    "No error",
    "Gain control not yet implemented",
    "Pulse coding not allowed in short blocks",
    "Invalid huffman codebook",
    "Scalefactor out of range",
    "Unable to find ADTS syncword",
    "Channel coupling not yet implemented",
    "Channel configuration not allowed in error resilient frame",
    "Bit error in error resilient scalefactor decoding",
    "Error decoding huffman scalefactor (bitstream error)",
    "Error decoding huffman codeword (bitstream error)",
    "Non existent huffman codebook number found",
    "Invalid number of channels",
    "Maximum number of bitstream elements exceeded",
    "Input data buffer too small",
    "Array index out of range",
    "Maximum number of scalefactor bands exceeded",
    "Quantised value out of range",
    "LTP lag out of range",
    "Invalid SBR parameter decoded",
    "SBR called without being initialised",
    "Unexpected channel configuration change",
    "Error in program_config_element",
    "First SBR frame is not the same as first AAC frame",
    "Unexpected fill element with SBR data",
    "Not all elements were provided with SBR data",
    "LTP decoding not available",
    "Output data buffer too small",
    "CRC error in DRM data",
    "PNS not allowed in DRM data stream",
    "No standard extension payload allowed in DRM",
    "PCE shall be the first element in a frame",
    "Bitstream value not allowed by specification",
    "MAIN prediction not initialised",
    "Long term prediction not initialised"
};

/* Sample type delivered by faad3 into the scratch buffer. */
typedef enum { T_I16, T_I32, T_F32 } pcm_type;

typedef struct {
    NeAACDecConfiguration cfg;
    faad_decoder *dec;
    enum faad_stream_format stream;
    uint8_t *asc;
    uint32_t asc_len;
    pcm_type type;
    unsigned fmt;   /* output format and downmix in force for the open decoder */
    int downmix_on;
    int dirty;      /* configuration changed since the decoder was opened */
    int decoded;    /* a frame has been decoded since the decoder was opened */
    long frame;     /* FAAD2 frame counter; drives the silent first frame */
    uint8_t *pcm;   /* scratch for faad3 output, max_output_bytes */
    uint32_t pcm_cap;
    uint8_t *out;   /* converted output returned by NeAACDecDecode */
    size_t out_cap;
} shim_t;

static const uint32_t sr_table[12] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000
};

char *NeAACDecGetErrorMessage(unsigned char errcode)
{
    if (errcode >= sizeof(err_msg) / sizeof(err_msg[0]))
        return NULL;
    return err_msg[errcode];
}

unsigned long NeAACDecGetCapabilities(void)
{
    return LC_DEC_CAP;
}

int NeAACDecGetVersion(char **faad_id_string, char **faad_copyright_string)
{
    static char id[] = FAAD_VERSION_STRING " (FAAD2 API shim)";
    static char copy[] = "Code from FAAD2 is copyright (c) Nero AG, www.nero.com";
    if (faad_id_string)
        *faad_id_string = id;
    if (faad_copyright_string)
        *faad_copyright_string = copy;
    return 0;
}

NeAACDecHandle NeAACDecOpen(void)
{
    shim_t *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->cfg.outputFormat = FAAD_FMT_16BIT;
    s->cfg.defObjectType = MAIN;
    s->cfg.defSampleRate = 44100;
    return s;
}

NeAACDecConfigurationPtr NeAACDecGetCurrentConfiguration(NeAACDecHandle h)
{
    shim_t *s = h;
    return s ? &s->cfg : NULL;
}

unsigned char NeAACDecSetConfiguration(NeAACDecHandle h, NeAACDecConfigurationPtr c)
{
    shim_t *s = h;
    if (!s || !c)
        return 0;
    /* defObjectType is not validated: callers read the defaults (MAIN), change
     * one field and write the structure back. */
    if (c->defSampleRate == 0)
        return 0;
    if (c->outputFormat < FAAD_FMT_16BIT || c->outputFormat > FAAD_FMT_DOUBLE)
        return 0;
    if (c->downMatrix > 1)
        return 0;
    /* Callers edit the structure returned by GetCurrentConfiguration in place,
     * so compare with the settings the open decoder uses, not with s->cfg. */
    s->dirty = c->outputFormat != s->fmt || c->downMatrix != s->downmix_on;
    s->cfg = *c;
    return 1;
}

static void shim_release(shim_t *s)
{
    if (s->dec)
        faad_decoder_close(&s->dec);
    free(s->pcm);
    s->pcm = NULL;
    s->pcm_cap = 0;
}

/* Output format requested from faad3 for the configured FAAD2 format. The
 * 32-bit and double formats and the FAAD2 downmix go through float. */
static pcm_type wanted_type(const NeAACDecConfiguration *c)
{
    if (c->downMatrix)
        return T_F32;
    switch (c->outputFormat) {
    case FAAD_FMT_16BIT: return T_I16;
    case FAAD_FMT_24BIT: return T_I32;
    default: return T_F32;
    }
}

static enum faad_output_format faad3_format(pcm_type t)
{
    return t == T_I16 ? FAAD_OUTPUT_16BIT : t == T_I32 ? FAAD_OUTPUT_32BIT : FAAD_OUTPUT_FLOAT;
}

static faad_status shim_open_dec(const shim_t *s, enum faad_stream_format stream, pcm_type type,
                                 const uint8_t *asc, uint32_t asc_len, faad_decoder **dec)
{
    faad_config cfg;
    faad_status st = faad_config_init(&cfg, sizeof(cfg));
    if (st != FAAD_OK)
        return st;
    (void)s;
    cfg.stream_format = stream;
    cfg.output_format = faad3_format(type);
    /* FAAD2 applies the full compression and normalises to -20 dBFS. */
    cfg.drc_cut = 127;
    cfg.drc_boost = 127;
    cfg.drc_target_level = DRC_TARGET;
    return faad_decoder_open(&cfg, asc, asc_len, dec);
}

/* (Re)open the faad3 decoder for the stored stream parameters. */
static int shim_reopen(shim_t *s)
{
    faad_stream_info info = { .struct_size = sizeof(info) };
    if (s->dec)
        faad_decoder_close(&s->dec);
    free(s->pcm);
    s->pcm = NULL;
    s->type = wanted_type(&s->cfg);
    s->fmt = s->cfg.outputFormat;
    s->downmix_on = s->cfg.downMatrix;
    if (shim_open_dec(s, s->stream, s->type, s->asc, s->asc_len, &s->dec) != FAAD_OK)
        return -1;
    if (faad_decoder_get_info(s->dec, &info) != FAAD_OK)
        return -1;
    s->pcm = malloc(info.max_output_bytes);
    if (!s->pcm)
        return -1;
    s->pcm_cap = info.max_output_bytes;
    s->dirty = 0;
    s->decoded = 0;
    return 0;
}

static int adts_sniff(const unsigned char *b, unsigned long n)
{
    return n >= 7 && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0;
}

long NeAACDecInit(NeAACDecHandle h, unsigned char *buffer, unsigned long buffer_size,
                  unsigned long *samplerate, unsigned char *channels)
{
    shim_t *s = h;
    faad_decoder *probe = NULL;
    faad_stream_info info = { .struct_size = sizeof(info) };
    uint32_t used = 0, wr = 0, fl = 0;
    uint8_t *pcm;

    if (!s || !samplerate || !channels || buffer_size == 0 || !buffer)
        return -1;
    {
        unsigned long k;
        for (k = 0; k < buffer_size && !adts_sniff(buffer + k, buffer_size - k); k++)
            ;
        if (k >= buffer_size)
            return -1;
        buffer += k;
        buffer_size -= k;
    }

    free(s->asc);
    s->asc = NULL;
    s->asc_len = 0;
    s->stream = FAAD_STREAM_ADTS;
    s->frame = 0;
    if (shim_reopen(s) != 0)
        return -1;

    /* Report the format the decoder will really emit: decode the first frame
     * on a scratch decoder. A buffer too short for a frame falls back to the
     * ADTS header. */
    {
        unsigned sfi = (buffer[2] >> 2) & 15, cc = ((buffer[2] & 1) << 2) | (buffer[3] >> 6);
        *samplerate = sr_table[sfi < 12 ? sfi : 0];
        *channels = (unsigned char)(cc == 7 ? 8 : cc);
    }
    if (shim_open_dec(s, FAAD_STREAM_ADTS, T_I16, NULL, 0, &probe) == FAAD_OK) {
        if (faad_decoder_get_info(probe, &info) == FAAD_OK &&
            (pcm = malloc(info.max_output_bytes)) != NULL) {
            if (faad_decode_frame(probe, buffer, (uint32_t)buffer_size, &used, pcm,
                                  info.max_output_bytes, &wr, &fl) == FAAD_OK &&
                faad_decoder_get_info(probe, &info) == FAAD_OK && info.format_known) {
                *samplerate = info.sample_rate;
                *channels = (unsigned char)info.channels;
            }
            free(pcm);
        }
        faad_decoder_close(&probe);
    }
    return 0;
}

char NeAACDecInit2(NeAACDecHandle h, unsigned char *pBuffer, unsigned long size,
                   unsigned long *samplerate, unsigned char *channels)
{
    shim_t *s = h;
    faad_stream_info info = { .struct_size = sizeof(info) };

    if (!s || !pBuffer || size < 2 || !samplerate || !channels)
        return -1;

    free(s->asc);
    s->asc = malloc(size);
    if (!s->asc)
        return -1;
    memcpy(s->asc, pBuffer, size);
    s->asc_len = (uint32_t)size;
    s->stream = FAAD_STREAM_RAW;
    s->frame = 0;
    if (shim_reopen(s) != 0)
        return -1;
    if (faad_decoder_get_info(s->dec, &info) != FAAD_OK || !info.format_known)
        return -1;
    *samplerate = info.sample_rate;
    /* FAAD2 reported the channels of the core, which is mono for PS streams;
     * the frames then carry two. */
    *channels = (unsigned char)(info.object_type == FAAD_OBJ_HE_AAC_V2 ? 1 : info.channels);
    return 0;
}

char NeAACDecInitDRM(NeAACDecHandle *h, unsigned long samplerate, unsigned char channels)
{
    (void)h;
    (void)samplerate;
    (void)channels;
    return 1;
}

void NeAACDecPostSeekReset(NeAACDecHandle h, long frame)
{
    shim_t *s = h;
    if (!s)
        return;
    /* A seek discards decoder history anyway, so this is where a changed
     * output format or downmix can take effect mid-stream. */
    if (s->dec) {
        if (s->dirty && (s->cfg.outputFormat != s->fmt || s->cfg.downMatrix != s->downmix_on))
            shim_reopen(s);
        else
            faad_decoder_flush(s->dec);
    }
    if (frame != -1)
        s->frame = frame;
}

void NeAACDecClose(NeAACDecHandle h)
{
    shim_t *s = h;
    if (!s)
        return;
    shim_release(s);
    free(s->asc);
    free(s->out);
    free(s);
}

char NeAACDecAudioSpecificConfig(unsigned char *pBuffer, unsigned long size,
                                 mp4AudioSpecificConfig *asc)
{
    faad_decoder *d = NULL;
    faad_stream_info info = { .struct_size = sizeof(info) };
    unsigned i;

    if (!pBuffer || size < 2 || !asc)
        return -1;
    memset(asc, 0, sizeof(*asc));
    if (shim_open_dec(NULL, FAAD_STREAM_RAW, T_I16, pBuffer, (uint32_t)size, &d) != FAAD_OK)
        return -1;
    if (faad_decoder_get_info(d, &info) != FAAD_OK || !info.format_known) {
        faad_decoder_close(&d);
        return -1;
    }
    faad_decoder_close(&d);

    /* FAAD2 reported the object type the ASC starts with when SBR is signalled
     * explicitly (AOT 5 or 29 is wrapped around an AAC-LC core, so 2), and 5
     * when the SBR or PS extension follows an AOT 2 core. */
    {
        unsigned first = pBuffer[0] >> 3;
        int hev2 = info.object_type == FAAD_OBJ_HE_AAC_V2;
        asc->objectTypeIndex = (unsigned char)((first == 5 || first == 29) ? LC :
                               hev2 ? HE_AAC : info.object_type);
        asc->sbr_present_flag = info.object_type != FAAD_OBJ_LC ? 1 : -1;
        asc->ps_present_flag = hev2 ? 1 : -1;
    }
    asc->samplingFrequency = info.sample_rate;
    asc->channelsConfiguration = (unsigned char)(info.object_type == FAAD_OBJ_HE_AAC_V2 ? 1 : info.channels);
    asc->frameLengthFlag = (info.frame_samples % 960) == 0 && (info.frame_samples % 1024) != 0;
    {
        unsigned long core = asc->sbr_present_flag > 0 ? info.sample_rate / 2 : info.sample_rate;
        for (i = 0; i < 12; i++)
            if (sr_table[i] == core)
                break;
        asc->samplingFrequencyIndex = (unsigned char)(i < 12 ? i : 15);
    }
    return 0;
}

/* FAAD2 position code for each bit of a WAVE channel mask. */
static unsigned mask_bit_code(unsigned bit)
{
    switch (bit) {
    case 0: return FRONT_CHANNEL_LEFT;
    case 1: return FRONT_CHANNEL_RIGHT;
    case 2: return FRONT_CHANNEL_CENTER;
    case 3: return LFE_CHANNEL;
    case 4: return BACK_CHANNEL_LEFT;
    case 5: return BACK_CHANNEL_RIGHT;
    case 8: return BACK_CHANNEL_CENTER;
    case 9: return SIDE_CHANNEL_LEFT;
    case 10: return SIDE_CHANNEL_RIGHT;
    default: return UNKNOWN_CHANNEL;
    }
}

/* FAAD2 emits channels in bitstream order, which is ascending position code
 * (C L R, side, back, back centre, LFE). faad3 emits ascending WAVE mask bit.
 * perm[j] is the faad3 channel that becomes output channel j. Returns 1 when
 * the order is not the identity. */
static int build_perm(uint32_t mask, uint32_t n, uint8_t *perm, uint8_t *pos)
{
    uint8_t code[SHIM_MAX_CHANNELS];
    uint32_t k = 0, i, j;
    int moved = 0;

    for (i = 0; i < n; i++)
        perm[i] = (uint8_t)i, pos[i] = UNKNOWN_CHANNEL;
    if (!mask)
        return 0;
    for (i = 0; i < 32 && k < n; i++)
        if (mask & (1u << i))
            code[k++] = (uint8_t)mask_bit_code(i);
    if (k != n)
        return 0;
    for (i = 0; i < n; i++)
        if (code[i] == UNKNOWN_CHANNEL)
            return 0;
    for (i = 1; i < n; i++) {   /* stable insertion sort by code */
        uint8_t c = code[i], p = perm[i];
        for (j = i; j > 0 && code[j - 1] > c; j--) {
            code[j] = code[j - 1];
            perm[j] = perm[j - 1];
        }
        code[j] = c;
        perm[j] = p;
    }
    for (i = 0; i < n; i++) {
        pos[i] = code[i];
        moved |= perm[i] != i;
    }
    return moved;
}

static size_t out_sample_bytes(unsigned fmt)
{
    switch (fmt) {
    case FAAD_FMT_16BIT: return 2;
    case FAAD_FMT_DOUBLE: return 8;
    default: return 4;
    }
}

static void put_sample(unsigned fmt, uint8_t *dst, size_t idx, double v)
{
    switch (fmt) {
    case FAAD_FMT_16BIT: {
        double x = v * 32768.0;
        x = x >= 32767.0 ? 32767.0 : x <= -32768.0 ? -32768.0 : x;
        ((int16_t *)dst)[idx] = (int16_t)lrint(x);
        break;
    }
    case FAAD_FMT_24BIT: {
        double x = v * 8388608.0;
        x = x >= 8388607.0 ? 8388607.0 : x <= -8388608.0 ? -8388608.0 : x;
        ((int32_t *)dst)[idx] = (int32_t)lrint(x);
        break;
    }
    case FAAD_FMT_32BIT: {
        double x = v * 2147483648.0;
        x = x >= 2147483647.0 ? 2147483647.0 : x <= -2147483648.0 ? -2147483648.0 : x;
        ((int32_t *)dst)[idx] = (int32_t)lrint(x);
        break;
    }
    case FAAD_FMT_FLOAT:
        ((float *)dst)[idx] = (float)v;
        break;
    default:
        ((double *)dst)[idx] = v;
        break;
    }
}

static int ensure_out(shim_t *s, size_t need)
{
    if (s->out_cap < need) {
        uint8_t *p = realloc(s->out, need);
        if (!p)
            return -1;
        s->out = p;
        s->out_cap = need;
    }
    return 0;
}

#define DM_MUL 0.3203772410170407
#define RSQRT2 0.7071067811865475244

/* Map a faad3 error to the nearest FAAD2 error number. */
static unsigned char map_status(faad_status st)
{
    switch (st) {
    case FAAD_OK: return ERR_NONE;
    case FAAD_ERR_SYNC_LOST: return ERR_NO_SYNC;
    case FAAD_ERR_NEED_MORE_DATA: return ERR_INPUT_SMALL;
    case FAAD_ERR_OUTPUT_TOO_SMALL: return ERR_OUTPUT_SMALL;
    case FAAD_ERR_UNSUPPORTED: return ERR_NOT_ALLOWED;
    case FAAD_ERR_DECODE_FAILED: return ERR_BITSTREAM;
    default: return ERR_NOT_ALLOWED;
    }
}

/* FAAD2 normalised a stream that carries DRC data but no programme reference
 * level as if the reference were 0 dBFS, so those streams came out 20 dB quieter.
 * Callers may have worked around that, so it is reproduced. */
static void attenuate_missing_reference(void *pcm, pcm_type t, size_t count)
{
    const float g = exp2f(-(float)DRC_TARGET / 24.0f);
    size_t i;

    for (i = 0; i < count; i++) {
        if (t == T_I16) ((int16_t *)pcm)[i] = (int16_t)lrintf(((int16_t *)pcm)[i] * g);
        else if (t == T_I32) ((int32_t *)pcm)[i] = (int32_t)lrintf(((int32_t *)pcm)[i] * g);
        else ((float *)pcm)[i] *= g;
    }
}

static void *shim_decode(shim_t *s, NeAACDecFrameInfo *fi, const uint8_t *buf, unsigned long size,
                         void *dest, unsigned long dest_size)
{
    faad_stream_info info = { .struct_size = sizeof(info) };
    uint32_t used = 0, wr = 0, flags = 0, n, frames, i, c, out_ch, skipped = 0;
    uint8_t perm[SHIM_MAX_CHANNELS], pos[SHIM_MAX_CHANNELS];
    unsigned fmt = s->fmt;
    size_t esz, osz, need;
    int moved, downmix;
    faad_status st;
    uint8_t *src, *out;

    if (!fi)
        return NULL;
    memset(fi, 0, sizeof(*fi));
    if (!s->dec || !buf || size == 0) {
        fi->error = ERR_INPUT_SMALL;
        return NULL;
    }
    if (s->dirty && !s->decoded && shim_reopen(s) != 0) {
        fi->error = ERR_NOT_ALLOWED;
        return NULL;
    }

    /* FAAD2 searches up to 768 bytes for an ADTS syncword inside one call and
     * then decodes the frame behind the junk; callers rely on that. */
    for (;;) {
        st = faad_decode_frame(s->dec, buf + skipped, (uint32_t)(size - skipped), &used,
                               s->pcm, s->pcm_cap, &wr, &flags);
        if (st != FAAD_ERR_SYNC_LOST || used == 0 || skipped + used >= size ||
            skipped + used >= SYNC_SEARCH)
            break;
        skipped += used;
    }
    used += skipped;
    fi->bytesconsumed = used;
    if (st != FAAD_OK) {
        fi->error = map_status(st);
        /* FAAD2 consumed nothing on an error and callers step through the
         * stream themselves; claiming a frame length could jump over real
         * frames when the header was a false sync. */
        fi->bytesconsumed = 0;
        return NULL;
    }
    if (flags & FAAD_FRAME_CONCEALED) {
        /* FAAD2 reported corrupt core data as an error and returned no samples;
         * its callers and their resync logic expect that. The claimed length
         * of a corrupt ADTS frame cannot be trusted either. */
        fi->error = ERR_BITSTREAM;
        fi->bytesconsumed = 0;
        return NULL;
    }
    s->decoded = 1;
    s->frame++;
    fi->header_type = s->stream == FAAD_STREAM_ADTS ? ADTS : RAW;
    if (wr == 0)
        return NULL;
    if (faad_decoder_get_info(s->dec, &info) != FAAD_OK || info.channels == 0 ||
        info.channels > SHIM_MAX_CHANNELS) {
        fi->error = ERR_BAD_CHANNELS;
        return NULL;
    }

    n = info.channels;
    esz = s->type == T_I16 ? 2 : 4;
    frames = wr / (uint32_t)(esz * n);
    if ((flags & FAAD_FRAME_DRC_INFO) && !info.prog_ref_level_present)
        attenuate_missing_reference(s->pcm, s->type, (size_t)frames * n);
    downmix = s->downmix_on && (n == 5 || n == 6);
    out_ch = downmix ? 2 : n;
    moved = build_perm(info.channel_mask, n, perm, pos);
    osz = out_sample_bytes(fmt);
    need = (size_t)frames * out_ch * osz;

    src = s->pcm;
    if (!downmix && !moved &&
        ((s->type == T_I16 && fmt == FAAD_FMT_16BIT) ||
         (s->type == T_I32 && fmt == FAAD_FMT_24BIT) ||
         (s->type == T_F32 && fmt == FAAD_FMT_FLOAT))) {
        out = src;
    } else {
        if (dest) {
            if (dest_size < need) {
                fi->error = ERR_OUTPUT_SMALL;
                return NULL;
            }
            out = dest;
        } else {
            if (ensure_out(s, need) != 0) {
                fi->error = ERR_NOT_ALLOWED;
                return NULL;
            }
            out = s->out;
        }
        if (downmix) {
            const float *f = (const float *)src;
            for (i = 0; i < frames; i++) {
                const float *x = f + (size_t)i * n;
                double cen = x[perm[0]], l = x[perm[1]], r = x[perm[2]];
                double bl = x[perm[3]], br = x[perm[4]];
                put_sample(fmt, out, (size_t)i * 2, DM_MUL * (l + cen * RSQRT2 + bl * RSQRT2));
                put_sample(fmt, out, (size_t)i * 2 + 1, DM_MUL * (r + cen * RSQRT2 + br * RSQRT2));
            }
        } else if (s->type == T_F32) {
            const float *f = (const float *)src;
            for (i = 0; i < frames; i++)
                for (c = 0; c < n; c++)
                    put_sample(fmt, out, (size_t)i * n + c, f[(size_t)i * n + perm[c]]);
        } else if (s->type == T_I16) {
            const int16_t *p = (const int16_t *)src;
            for (i = 0; i < frames; i++)
                for (c = 0; c < n; c++)
                    ((int16_t *)out)[(size_t)i * n + c] = p[(size_t)i * n + perm[c]];
        } else {
            const int32_t *p = (const int32_t *)src;
            for (i = 0; i < frames; i++)
                for (c = 0; c < n; c++)
                    ((int32_t *)out)[(size_t)i * n + c] = p[(size_t)i * n + perm[c]];
        }
    }
    if (out == src && dest) {
        if (dest_size < need) {
            fi->error = ERR_OUTPUT_SMALL;
            return NULL;
        }
        memcpy(dest, src, need);
        out = dest;
    }

    fi->channels = (unsigned char)out_ch;
    fi->samples = (unsigned long)frames * out_ch;
    fi->samplerate = info.sample_rate;
    fi->object_type = info.object_type == FAAD_OBJ_LC ? LC : HE_AAC;
    fi->sbr = (flags & FAAD_FRAME_SBR) ? SBR_UPSAMPLED : NO_SBR;
    fi->ps = (flags & FAAD_FRAME_PS) ? 1 : 0;
    if (downmix) {
        fi->num_front_channels = 2;
        fi->channel_position[0] = FRONT_CHANNEL_LEFT;
        fi->channel_position[1] = FRONT_CHANNEL_RIGHT;
    } else {
        for (c = 0; c < n; c++) {
            fi->channel_position[c] = pos[c];
            switch (pos[c]) {
            case FRONT_CHANNEL_CENTER:
            case FRONT_CHANNEL_LEFT:
            case FRONT_CHANNEL_RIGHT: fi->num_front_channels++; break;
            case SIDE_CHANNEL_LEFT:
            case SIDE_CHANNEL_RIGHT: fi->num_side_channels++; break;
            case BACK_CHANNEL_LEFT:
            case BACK_CHANNEL_RIGHT:
            case BACK_CHANNEL_CENTER: fi->num_back_channels++; break;
            case LFE_CHANNEL: fi->num_lfe_channels++; break;
            default: break;
            }
        }
    }
    /* FAAD2 decodes the first frame but hands back no samples for it. */
    if (s->frame <= 1)
        fi->samples = 0;
    return out;
}

void *NeAACDecDecode(NeAACDecHandle h, NeAACDecFrameInfo *hInfo, unsigned char *buffer,
                     unsigned long buffer_size)
{
    return shim_decode(h, hInfo, buffer, buffer_size, NULL, 0);
}

void *NeAACDecDecode2(NeAACDecHandle h, NeAACDecFrameInfo *hInfo, unsigned char *buffer,
                      unsigned long buffer_size, void **sample_buffer,
                      unsigned long sample_buffer_size)
{
    if (!sample_buffer || !*sample_buffer || sample_buffer_size == 0) {
        if (hInfo) {
            memset(hInfo, 0, sizeof(*hInfo));
            hInfo->error = ERR_OUTPUT_SMALL;
        }
        return NULL;
    }
    return shim_decode(h, hInfo, buffer, buffer_size, *sample_buffer, sample_buffer_size);
}
