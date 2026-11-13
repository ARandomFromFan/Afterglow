/* nn::codec: the hardware Opus decoder, on libopus (third_party/opus).

   Input packets come as on the hardware decoder: an 8-byte header {u32 big-endian payload size, u32 big-endian
   final range} then the Opus packet. DecodeOpusInterleavedWithHardware decodes one packet into interleaved
   16-bit PCM and says how many bytes it consumed (header + payload) and how many samples (per channel) it made.
   Each HardwareOpusDecoderType gets our handle in its first 8 bytes. */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define CODEC_MAGIC 0x4F505553u   /* 'OPUS' */
#define MAX_DECODERS 512

#define QMAX 64   /* decoded frames a queued decoder holds */
typedef struct {
    int16_t *pcm;
    int samples;   /* per channel */
} Frame;
typedef struct {
    OpusDecoder *dec;
    int rate, channels;
    Frame q[QMAX];
    int head, count;
} Dec;
static Dec decs[MAX_DECODERS];
static volatile LONG decs_lock;

static void dlock(void) { while (InterlockedExchange(&decs_lock, 1)) hos_sleep_ns(1000); }
static void dunlock(void) { InterlockedExchange(&decs_lock, 0); }

static Dec *dec_of(uint64_t obj) {
    if (!obj) return NULL;
    const uint64_t h = G64(obj);
    if ((uint32_t)(h >> 32) != CODEC_MAGIC) return NULL;
    const uint32_t i = (uint32_t)h;
    return i < MAX_DECODERS && decs[i].dec ? &decs[i] : NULL;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static void queue_clear(Dec *d);

/* GetHardwareOpusDecoderWorkBufferSize(int sampleRate, int channelCount, ...): the game allocates it; we keep our
   state on the host */
HLE(_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii) { RET(0x1000); }

/* InitializeHardwareOpusDecoder(HardwareOpusDecoderType*, int sampleRate, int channelCount, void* workBuffer,
                                 size_t workBufferSize, int option) -> Result */
HLE(_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi) {
    const int rate = (int)ARG(1), channels = (int)ARG(2);
    int err = 0;
    OpusDecoder *d = opus_decoder_create(rate, channels, &err);
    if (!d || err != OPUS_OK) {
        hos_log("codec: Opus decoder %d Hz %d channels not created (%d)", rate, channels, err);
        RET(0x1F0);   /* (any failure result) */
        return;
    }
    dlock();
    int i = 0;
    while (i < MAX_DECODERS && decs[i].dec) i++;
    if (i == MAX_DECODERS) {
        dunlock();
        opus_decoder_destroy(d);
        RET(0x1F0);
        return;
    }
    decs[i].dec = d;
    decs[i].rate = rate;
    decs[i].channels = channels;
    G64(ARG(0)) = (uint64_t)CODEC_MAGIC << 32 | (uint32_t)i;
    dunlock();
    static int logged;
    if (logged++ < 8) hos_log("codec: Opus decoder %d: %d Hz, %d channels", i, rate, channels);
    RET(0);
}
HLE(_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE) {
    dlock();
    Dec *d = dec_of(ARG(0));
    if (d) {
        queue_clear(d);
        opus_decoder_destroy(d->dec);
        d->dec = NULL;
        G64(ARG(0)) = 0;
    }
    dunlock();
}
HLE(_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE) {
    dlock();
    Dec *d = dec_of(ARG(0));
    if (d) {
        queue_clear(d);
        opus_decoder_ctl(d->dec, OPUS_RESET_STATE);
    }
    dunlock();
    RET(0);
}
HLE(_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE) {
    Dec *d = dec_of(ARG(0));
    RET(d ? d->rate : 48000);
}
HLE(_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE) {
    Dec *d = dec_of(ARG(0));
    RET(d ? d->channels : 2);
}

/* GetOpusPacketSize(const uint8* data, size_t size): the size of the packet the header describes, header included */
HLE(_ZN2nn5codec17GetOpusPacketSizeEPKhm) {
    const uint8_t *p = (const uint8_t *)GP(ARG(0));
    const uint64_t size = ARG(1);
    const uint64_t r = size >= 8 ? 8 + (uint64_t)be32(p) : 0;
    static int logged;
    if (logged++ < 6) hos_log("codec: GetOpusPacketSize(%#llx, %llu): header %08x %08x -> %llu", (unsigned long long)ARG(0),
                              (unsigned long long)size, be32(p), be32(p + 4), (unsigned long long)r);
    RET(r);
}

/* DecodeOpusInterleavedWithHardware(HardwareOpusDecoderType*, size_t* consumed, int* sampleCount, int16* out,
                                     size_t outSize, const void* in, size_t inSize) -> Result */
HLE(_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm) {
    Dec *d = dec_of(ARG(0));
    const uint64_t out_size = ARG(4), in_size = ARG(6);
    const uint8_t *in = (const uint8_t *)GP(ARG(5));
    if (!d || in_size < 8) { RET(0x1F0); return; }
    const uint32_t len = be32(in);
    if (len > in_size - 8) { RET(0x1F0); return; }
    const int frames = (int)(out_size / (2 * (uint64_t)d->channels));
    const int n = opus_decode(d->dec, in + 8, (opus_int32)len, (opus_int16 *)GP(ARG(3)), frames, 0);
    static int logged;
    if (logged++ < 6)
        hos_log("codec: decode %u bytes (of %llu) -> %d samples (room for %d)", len, (unsigned long long)in_size, n, frames);
    if (n < 0) { RET(0x1F0); return; }
    if (ARG(1)) G64(ARG(1)) = 8 + (uint64_t)len;
    if (ARG(2)) G32(ARG(2)) = (uint32_t)n;
    RET(0);
}

HLE(_ZN2nn5codec43GetHardwareOpusDecoderManagerWorkBufferSizeEiiNS_8TimeSpanEii) {
    RET(0x10000);
}

extern void hos_system_event_init(uint64_t key, int autoclear);
extern void hos_system_event_signal(uint64_t key);
#define MAX_MANAGERS 8
static struct { uint64_t mgr, event; } managers[MAX_MANAGERS];   /* the completion event each manager was given */

HLE(_ZN2nn5codec36InitializeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeEPNS_2os15SystemEventTypeEiiNS_8TimeSpanEiiPvm) {
    G64(ARG(0)) = (uint64_t)CODEC_MAGIC << 32 | 0x8000;
    if (ARG(1)) {
        hos_system_event_init(ARG(1), 1);
        for (int i = 0; i < MAX_MANAGERS; i++)
            if (!managers[i].mgr || managers[i].mgr == ARG(0)) { managers[i].mgr = ARG(0), managers[i].event = ARG(1); break; }
    }
    RET(0);
}

HLE(_ZN2nn5codec34FinalizeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeE) {
    for (int i = 0; i < MAX_MANAGERS; i++)
        if (managers[i].mgr == ARG(0)) managers[i].mgr = managers[i].event = 0;
    G64(ARG(0)) = 0;
}

HLE(_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiNS_8TimeSpanEPNS0_30HardwareOpusDecoderManagerTypeE) {
    const int rate = (int)ARG(1), channels = (int)ARG(2);
    int err = 0;
    OpusDecoder *d = opus_decoder_create(rate, channels, &err);
    if (!d || err != OPUS_OK) {
        RET(0x1F0);
        return;
    }
    dlock();
    int i = 0;
    while (i < MAX_DECODERS && decs[i].dec) i++;
    if (i == MAX_DECODERS) {
        dunlock();
        opus_decoder_destroy(d);
        RET(0x1F0);
        return;
    }
    decs[i].dec = d;
    decs[i].rate = rate;
    decs[i].channels = channels;
    G64(ARG(0)) = (uint64_t)CODEC_MAGIC << 32 | (uint32_t)i;
    dunlock();
    RET(0);
}

/* (afterglow) the queued decoder (Switch 2 SDK): Wwise enqueues one packet a call ({be32 size, be32 range} + payload)
   and dequeues one 960-sample frame a call. Decoded here at enqueue, so nothing ever waits or executes. */
static void queue_clear(Dec *d) {
    while (d->count) {
        free(d->q[d->head].pcm);
        d->head = (d->head + 1) % QMAX, d->count--;
    }
    d->head = 0;
}
HLE(_ZN2nn5codec46GetHardwareOpusDecoderManagerFreeIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE) {
    RET(32);
}

/* Enqueue already decoded: the run completes at once, and its event says so as the hardware's would */
HLE(_ZN2nn5codec34ExecuteHardwareOpusDecoderDecodingEPNS0_30HardwareOpusDecoderManagerTypeE) {
    for (int i = 0; i < MAX_MANAGERS; i++)
        if (managers[i].mgr == ARG(0) && managers[i].event) hos_system_event_signal(managers[i].event);
    RET(0);
}

/* EnqueueOpusDataToHardwareOpusDecoder(HardwareOpusDecoderType*, size_t* consumed, const void* data, size_t size) */
HLE(_ZN2nn5codec36EnqueueOpusDataToHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPmPKvm) {
    const uint64_t size = ARG(3);
    const uint8_t *in = (const uint8_t *)GP(ARG(2));
    if (ARG(1)) G64(ARG(1)) = 0;
    dlock();
    Dec *d = dec_of(ARG(0));
    if (!d || size < 8 || be32(in) > size - 8 || d->count == QMAX) { dunlock(); RET(0x1F0); return; }
    const uint32_t len = be32(in);
    const int room = d->rate * 120 / 1000;   /* (the longest Opus packet: 120 ms) */
    int16_t *pcm = (int16_t *)malloc((size_t)room * d->channels * 2);
    const int n = pcm ? opus_decode(d->dec, in + 8, (opus_int32)len, pcm, room, 0) : -1;
    static int logged;
    if (logged++ < 6) hos_log("codec: queued decode %u bytes -> %d samples", len, n);
    if (n < 0) { free(pcm); dunlock(); RET(0x1F0); return; }
    Frame *f = &d->q[(d->head + d->count) % QMAX];
    f->pcm = pcm, f->samples = n;
    d->count++;
    dunlock();
    if (ARG(1)) G64(ARG(1)) = 8 + (uint64_t)len;
    RET(0);
}

/* DequeuePcmInterleavedDataFromHardwareOpusDecoder(HardwareOpusDecoderType*, int* sampleCount, int16* out, size_t size) */
HLE(_ZN2nn5codec48DequeuePcmInterleavedDataFromHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPiPsm) {
    if (ARG(1)) G32(ARG(1)) = 0;
    dlock();
    Dec *d = dec_of(ARG(0));
    if (!d || !d->count) { dunlock(); RET(d ? 0 : 0x1F0); return; }
    Frame *f = &d->q[d->head];
    const uint64_t fit = ARG(3) / (2 * (uint64_t)d->channels);
    const int n = (uint64_t)f->samples < fit ? f->samples : (int)fit;
    memcpy(GP(ARG(2)), f->pcm, (size_t)n * d->channels * 2);
    free(f->pcm);
    d->head = (d->head + 1) % QMAX, d->count--;
    dunlock();
    if (ARG(1)) G32(ARG(1)) = (uint32_t)n;
    RET(0);
}

HLE(_ZN2nn5codec47GetHardwareOpusDecoderWaitingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE) {
    RET(0);
}

HLE(_ZN2nn5codec49GetHardwareOpusDecoderExecutingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE) {
    RET(0);
}

HLE(_ZN2nn5codec49GetHardwareOpusDecoderCompletedDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE) {
    dlock();
    Dec *d = dec_of(ARG(0));
    const int n = d ? d->count : 0;
    dunlock();
    RET(n);
}
