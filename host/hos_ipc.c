/* host IPC services: session handle -> service name, and the CMIF services the host answers (ntc) */
#include "hos.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <time.h>

#define NTC_MAX 256
enum { NTC_NONE, NTC_ROOT, NTC_ENSURE, NTC_NSD };

static volatile LONG ntc_spin;
static uint32_t ntc_handles[NTC_MAX];
static uint8_t ntc_kinds[NTC_MAX];
static char ntc_names[NTC_MAX][9];
static volatile LONG ntc_next;

static void ntc_lock(void) { while (InterlockedCompareExchange(&ntc_spin, 1, 0) != 0) Sleep(0); }
static void ntc_unlock(void) { InterlockedExchange(&ntc_spin, 0); }

static int ntc_slot(uint32_t h) {
    for (int i = 0; i < NTC_MAX; i++) if (ntc_handles[i] == h) return i;
    return -1;
}
/* the slot for h, taking a free one if it is new; -1 when the table is full */
static int ntc_slot_for(uint32_t h) {
    int slot = ntc_slot(h);
    if (slot >= 0) return slot;
    for (int i = 0; i < NTC_MAX; i++) if (!ntc_handles[i]) { ntc_handles[i] = h; return i; }
    return -1;
}
static void ntc_add(uint32_t h, int kind) {
    int slot = ntc_slot_for(h);
    if (slot >= 0) ntc_kinds[slot] = (uint8_t)kind;
}
static void ntc_set_name(uint32_t h, const char *name) {
    int slot = ntc_slot_for(h);
    if (slot < 0) return;
    memset(ntc_names[slot], 0, 9);
    strncpy(ntc_names[slot], name, 8);
}

static const char *ntc_kind_name(int kind) {
    return kind == NTC_ROOT ? "root" : kind == NTC_NSD ? "nsd" : "ensure";
}

static void ntc_log_once(int kind, uint32_t cmd) {
    static uint8_t seen[4][64];
    if (kind > NTC_NSD || cmd >= 64 || seen[kind][cmd]) return;
    seen[kind][cmd] = 1;
    hos_log("ntc: %s cmd %u", ntc_kind_name(kind), cmd);
}

/* CMIF reply: header, optional handle descriptor, then 'SFCO' and the output words at byte 16 */
/* the reply's raw data words from byte 16 (nsd's 0x100-byte Fqdn needs more than the usual 16) */
static void ntc_reply_words(uint8_t *tls, int handle, uint32_t desc, uint32_t h, const uint32_t *out, int nout,
                            uint32_t words) {
    memset(tls, 0, 0x100);
    uint32_t *m = (uint32_t *)tls;
    m[0] = 0;
    m[1] = words | (handle ? 0x80000000u : 0);
    if (handle) { m[2] = desc; m[3] = h; }
    uint32_t *d = (uint32_t *)(tls + 16);
    d[0] = 0x4F434653u;   /* 'SFCO' */
    d[1] = 0;             /* version */
    d[2] = 0;             /* result */
    d[3] = 0;             /* token */
    for (int i = 0; i < nout; i++) d[4 + i] = out[i];
}
static void ntc_reply(uint8_t *tls, int handle, uint32_t desc, uint32_t h, const uint32_t *out, int nout) {
    ntc_reply_words(tls, handle, desc, h, out, nout, 16);
}


/* nsd:a requests carry the Fqdn in HIPC buffer descriptors (CMIF layout: header at word 0, then num_A send triples,
   then num_B receive triples, then the raw data words). A triple is {size[31:0], addr[31:0], word2}; word2 packs the
   high bits: bits 0-1 mode, bits 2-4 addr[38:36], bits 24-27 size[35:32], bits 28-31 addr[35:32]. The game's buffers
   live at 0x10xxxxxxxx (the heap above 4 GB), so the low 32 bits alone are not the address. */
static int nsd_descs(uint8_t *tls, uint64_t *addr, uint64_t *size, int max) {
    const uint32_t *w = (const uint32_t *)tls;
    const int na = (int)((w[0] >> 20) & 0xF), nb = (int)((w[0] >> 24) & 0xF);
    int n = 0;
    for (int k = 0; k < na + nb && n < max; k++) {
        const uint32_t *d = w + 2 + 3 * k, w2 = d[2];
        addr[n] = (uint64_t)d[1] | ((uint64_t)((w2 >> 28) & 0xF) << 32) | ((uint64_t)((w2 >> 2) & 7) << 36);
        size[n] = (uint64_t)d[0] | ((uint64_t)((w2 >> 24) & 0xF) << 32);
        n++;
    }
    return n;
}

/* Resolve(out, in): the A triples are the client's send buffers (the Fqdn it passes in), the B triples its receive
   buffers. Read the Fqdn from A[0]. */
static int nsd_input(uint8_t *tls, char *out, int cap) {
    uint64_t a[4], sz[4];
    out[0] = 0;
    const int nd = nsd_descs(tls, a, sz, 4);
    if (nd < 1 || !a[0]) return 0;
    uint64_t len = sz[0] < (uint64_t)cap ? sz[0] : (uint64_t)cap - 1;
    int n = 0;
    for (; n < (int)len; n++) {
        const uint8_t ch = G8(a[0] + (uint64_t)n);
        if (!ch) break;
        if (ch < 0x20 || ch > 0x7E) { out[0] = 0; return 0; }
        out[n] = (char)ch;
    }
    out[n] = 0;
    return n > 2;
}

/* the client's receive buffer B[0]: fill it before reply clears tls descriptors */
static void nsd_out_buffer(uint64_t addr, uint64_t size, const char *name) {
    if (!addr || !size) return;
    uint64_t n = (uint64_t)strlen(name) + 1;
    if (n > size) n = size;
    if (n > 0x100) n = 0x100;
    for (uint64_t i = 0; i < n; i++) G8(addr + i) = (uint8_t)name[i];
}

/* the Fqdn's "%" is the environment placeholder: the identifier is "lp1" (nn_applet.c GetEnvironmentIdentifier) */
static void nsd_map(const char *in, char *out) {
    int o = 0;
    for (int i = 0; in[i] && o < 0xFD; i++) {
        if (in[i] == '%') { memcpy(out + o, "lp1", 3); o += 3; }
        else out[o++] = in[i];
    }
    out[o] = 0;
}

void hos_ipc_service_opened(uint32_t h, const char *name) {
    ntc_lock();
    ntc_set_name(h, name);
    if (hos_force_online() && strcmp(name, "ntc") == 0) ntc_add(h, NTC_ROOT);
    else if (strncmp(name, "nsd", 3) == 0) ntc_add(h, NTC_NSD);   /* nsd:a Resolve/ResolveEx */
    ntc_unlock();
}

const char *hos_ipc_service_name(uint32_t h) {
    ntc_lock();
    const int slot = ntc_slot(h);
    const char *name = slot >= 0 && ntc_names[slot][0] ? ntc_names[slot] : "?";
    ntc_unlock();
    return name;
}

int hos_ipc_dispatch(uint32_t h, uint32_t cmd, uint8_t *tls) {
    ntc_lock();
    const int slot = ntc_slot(h);
    const int kind = slot < 0 ? NTC_NONE : ntc_kinds[slot];
    ntc_unlock();
    if (kind == NTC_NONE) return 0;
    ntc_log_once(kind, cmd);
    if (kind == NTC_ROOT) {
        if (cmd == 0) {   /* OpenEnsureNetworkClockAvailabilityService -> 1 move handle */
            uint32_t nh = 0x7E100000u | (uint32_t)InterlockedIncrement(&ntc_next);
            ntc_lock();
            ntc_add(nh, NTC_ENSURE);
            ntc_set_name(nh, "ntc:ens");
            ntc_unlock();
            ntc_reply(tls, 1, 1u << 5, nh, NULL, 0);
            return 1;
        }
        ntc_reply(tls, 0, 0, 0, NULL, 0);
        return 1;
    }
    if (kind == NTC_NSD) {
        static uint8_t seen[64];
        char in[0x101], out[0x101];
        uint32_t words[65];
        if (cmd < 64 && !seen[cmd]) {
            const uint32_t *w = (const uint32_t *)tls;
            seen[cmd] = 1;
            hos_log("nsd: cmd %u request %08x %08x %08x %08x %08x %08x %08x %08x", cmd, w[0], w[1], w[2], w[3],
                    w[4], w[5], w[6], w[7]);
        }
        const int have = nsd_input(tls, in, (int)sizeof in);
        if (cmd == 20 || cmd == 21) {
            nsd_map(in, out);
            uint64_t da[4], ds[4];
            const int nd = nsd_descs(tls, da, ds, 4);   /* (before the reply: ntc_reply_words clears the request) */
            if (nd >= 2 && da[1]) nsd_out_buffer(da[1], ds[1], out);
            memset(words, 0, sizeof words);
            memcpy(words, out, 0x100);
            if (cmd == 21) words[64] = 0;   /* ResolveEx: the trailing inner Result is 0 */
            ntc_reply_words(tls, 0, 0, 0, words, cmd == 21 ? 65 : 64, cmd == 21 ? 69 : 68);
            hos_log("nsd: resolve%s %s -> %s (send buffer %s; A %#llx/%llu B %#llx/%llu)", cmd == 21 ? "Ex" : "",
                    in[0] ? in : "(none)", out, have ? "read" : "not found",
                    nd > 0 ? (unsigned long long)da[0] : 0ull, nd > 0 ? (unsigned long long)ds[0] : 0ull,
                    nd > 1 ? (unsigned long long)da[1] : 0ull, nd > 1 ? (unsigned long long)ds[1] : 0ull);

            return 1;
        }
        hos_log("nsd: cmd %u: answered empty", cmd);
        ntc_reply(tls, 0, 0, 0, NULL, 0);
        return 1;
    }
    if (cmd == 1) {   /* GetFinishNotificationEvent -> 1 copy handle, already signaled */
        ntc_reply(tls, 1, 1u << 1, HOS_HANDLE_SIGNALED, NULL, 0);
        return 1;
    }
    uint32_t out[2];
    int nout = 0;
    if (cmd == 4) { out[0] = 0; nout = 1; }          /* IsProcessing -> false */
    else if (cmd == 5) {                             /* GetServerTime -> PosixTime (s64) */
        const int64_t t = (int64_t)time(NULL);
        out[0] = (uint32_t)t;
        out[1] = (uint32_t)((uint64_t)t >> 32);
        nout = 2;
    }
    ntc_reply(tls, 0, 0, 0, out, nout);
    return 1;
}
