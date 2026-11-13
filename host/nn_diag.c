/* nn::diag: the game's own log lines and aborts. */
#include "hos.h"
#include <pthread.h>
#include <stdio.h>
#include <link.h>

void hos_backtrace(void);

/* nn::diag::detail::PutImpl(LogMetaData const&, char const* text, unsigned long length) */
HLE(_ZN2nn4diag6detail7PutImplERKNS0_11LogMetaDataEPKcm) {
    uint64_t n = ARG(2) > 4000 ? 4000 : ARG(2);
    char buf[4001];
    memcpy(buf, GP(ARG(1)), n);
    buf[n] = 0;
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    hos_log("game [lr=%#llx]: %s", (unsigned long long)c->x[30], buf);
    if (strstr(buf, "Failed MagicOrchestra_Wwise")) {
        hos_backtrace();
    }
}

/* nn::diag::detail::AbortImpl() */
HLE(_ZN2nn4diag6detail9AbortImplEv) { hos_fatal(c, "the game aborted (nn::diag::detail::AbortImpl)"); }
/* nn::diag::detail::AbortImpl(nn::Result const*) */
HLE(_ZN2nn4diag6detail9AbortImplEPKNS_6ResultE) {
    hos_fatal(c, "the game aborted (nn::diag::detail::AbortImpl, result %#x)", ARG(0) ? G32(ARG(0)) : 0u);
}

/* nn::detail::UnexpectedDefaultImpl(): a switch reached a case the SDK says cannot happen */
HLE(_ZN2nn6detail21UnexpectedDefaultImplEv) { hos_fatal(c, "nn::detail::UnexpectedDefaultImpl"); }

/* nn::diag::GetBacktrace(unsigned long*, int) -> int: none */
HLE(_ZN2nn4diag12GetBacktraceEPmi) { RET(0); }

/* step35: a sequence node holds a NULL child (diagnosis only, prints once per node) */
void hos_null_child_report(Ctx *c, uint64_t node, uint64_t idx) {
    static uint64_t seen[8];
    static int nseen;
    for (int i = 0; i < nseen; i++) if (seen[i] == node) return;
    if (nseen < 8) seen[nseen++] = node;
    char tname[24];
    if (pthread_getname_np(pthread_self(), tname, sizeof tname) != 0) tname[0] = 0;
    const uint64_t size = recomp_load64(c, node + 88);
    fprintf(stderr, "seq: null child node %#llx vtbl %#llx idx %llu size %llu thread %s\n",
            (unsigned long long)node, (unsigned long long)recomp_load64(c, node),
            (unsigned long long)idx, (unsigned long long)size, tname[0] ? tname : "?");
    for (uint64_t i = 0; i < size && i < 16; i++) {
        const uint64_t ch = recomp_load64(c, node + 16 + i * 8);
        if (ch) fprintf(stderr, "seq: child[%llu] %#llx vtbl %#llx\n", (unsigned long long)i,
                        (unsigned long long)ch, (unsigned long long)recomp_load64(c, ch));
        else fprintf(stderr, "seq: child[%llu] 0x0\n", (unsigned long long)i);
    }
    uint64_t fp = c->x[29];
    fprintf(stderr, "seq: guest frames (fp %#llx)", (unsigned long long)fp);
    for (int i = 0; i < 10 && fp; i++) {
        fprintf(stderr, " lr %#llx", (unsigned long long)recomp_load64(c, fp + 8));
        fp = recomp_load64(c, fp);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}
/* step36/38: f_81adbfa0 entered with a NULL or wild param (first 3 hits) */
static int diag_main_phdr(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    if (info->dlpi_name && info->dlpi_name[0]) return 0;
    uint64_t *r = (uint64_t *)data; /* lo, hi */
    r[0] = ~0ULL; r[1] = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        const uint64_t lo = (uint64_t)info->dlpi_addr + ph->p_vaddr;
        const uint64_t hi = lo + ph->p_memsz;
        if (lo < r[0]) r[0] = lo;
        if (hi > r[1]) r[1] = hi;
    }
    return 1;
}
void hos_null_param_report(Ctx *c) {
    static int hits;
    if (hits >= 3) return;
    hits++;
    char tname[24];
    if (pthread_getname_np(pthread_self(), tname, sizeof tname) != 0) tname[0] = 0;
    const uint64_t x0 = c->x[0], x1 = c->x[1], sp = c->x[31], fp0 = c->x[29];
    uint64_t bin[2] = {0, 0}, stk[2] = {0, 0};
    dl_iterate_phdr(diag_main_phdr, bin);
    pthread_attr_t at;
    if (pthread_getattr_np(pthread_self(), &at) == 0) {
        void *sa = 0; size_t ss = 0;
        if (pthread_attr_getstack(&at, &sa, &ss) == 0) { stk[0] = (uint64_t)(uintptr_t)sa; stk[1] = stk[0] + ss; }
        pthread_attr_destroy(&at);
    }
    const int in_bin = (x1 >= bin[0] && x1 < bin[1]);
    const int in_stk = (stk[1] > stk[0] && x1 >= stk[0] && x1 < stk[1]);
    fprintf(stderr, "seq: bad param lr %#llx x0 %#llx x1 %#llx sp %#llx fp %#llx thread %s\n",
            (unsigned long long)c->x[30], (unsigned long long)x0, (unsigned long long)x1,
            (unsigned long long)sp, (unsigned long long)fp0, tname[0] ? tname : "?");
    fprintf(stderr, "seq: x1 %s (binary %#llx-%#llx stack %#llx-%#llx) guest window %#llx-%#llx\n",
            in_bin ? "IN HOST BINARY" : in_stk ? "IN HOST STACK" : "NEITHER",
            (unsigned long long)bin[0], (unsigned long long)bin[1],
            (unsigned long long)stk[0], (unsigned long long)stk[1],
            (unsigned long long)A64_HOST_BASE, (unsigned long long)(A64_HOST_BASE + 0x100000000ULL));
    fprintf(stderr, "seq: bad param node %#llx vtbl %#llx n %u cap %llu\n",
            (unsigned long long)x0, (unsigned long long)recomp_load64(c, x0),
            (unsigned)recomp_load32(c, x0 + 12), (unsigned long long)recomp_load64(c, x0 + 88));
    fprintf(stderr, "seq: args x2 %#llx x3 %#llx x4 %#llx x5 %#llx x6 %#llx x7 %#llx x19 %#llx x20 %#llx x21 %#llx x22 %#llx\n",
            (unsigned long long)c->x[2], (unsigned long long)c->x[3], (unsigned long long)c->x[4],
            (unsigned long long)c->x[5], (unsigned long long)c->x[6], (unsigned long long)c->x[7],
            (unsigned long long)c->x[19], (unsigned long long)c->x[20], (unsigned long long)c->x[21], (unsigned long long)c->x[22]);
    uint64_t fp = fp0;
    fprintf(stderr, "seq: guest frames (fp %#llx)", (unsigned long long)fp);
    for (int i = 0; i < 12 && fp; i++) {
        fprintf(stderr, " lr %#llx", (unsigned long long)recomp_load64(c, fp + 8));
        fp = recomp_load64(c, fp);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* step39: the slot-3 ICALL in f_81adbfa0 (lr 0x81adc000) returned with a callee-saved register changed */
void hos_callee_clobber_report(Ctx *c, uint64_t child, uint64_t target, uint64_t s19, uint64_t s20,
                               uint64_t s21, uint64_t s22, uint64_t ssp) {
    static int hits;
    if (hits >= 3) return;
    hits++;
    const char *hos_hostcall_name(uint64_t addr);
    char tname[24];
    if (pthread_getname_np(pthread_self(), tname, sizeof tname) != 0) tname[0] = 0;
    const char *hn = hos_hostcall_name(target);
    const A64Fn fn = a64_lookup(target);
    fprintf(stderr, "seq: callee clobber target %#llx f_%08llx kind %s lookup %p host %s\n",
            (unsigned long long)target, (unsigned long long)(uint32_t)target,
            hn ? "host HLE" : fn ? "lifted" : "UNKNOWN", (void *)fn, hn ? hn : "-");
    fprintf(stderr, "seq: callee clobber child %#llx vtbl %#llx (x0 was the child, now the return value %#llx)\n",
            (unsigned long long)child, (unsigned long long)recomp_load64(c, child),
            (unsigned long long)c->x[0]);
    fprintf(stderr, "seq: callee clobber x19 %#llx -> %#llx%s\n", (unsigned long long)s19,
            (unsigned long long)c->x[19], c->x[19] != s19 ? "  CHANGED" : "");
    fprintf(stderr, "seq: callee clobber x20 %#llx -> %#llx x21 %#llx -> %#llx x22 %#llx -> %#llx%s%s%s\n",
            (unsigned long long)s20, (unsigned long long)c->x[20], (unsigned long long)s21,
            (unsigned long long)c->x[21], (unsigned long long)s22, (unsigned long long)c->x[22],
            c->x[20] != s20 ? "  x20 CHANGED" : "", c->x[21] != s21 ? "  x21 CHANGED" : "",
            c->x[22] != s22 ? "  x22 CHANGED" : "");
    fprintf(stderr, "seq: callee clobber sp %#llx -> %#llx%s thread %s\n", (unsigned long long)ssp,
            (unsigned long long)c->x[31], c->x[31] != ssp ? "  CHANGED" : "", tname[0] ? tname : "?");
    uint64_t fp = c->x[29];
    fprintf(stderr, "seq: guest frames (fp %#llx)", (unsigned long long)fp);
    for (int i = 0; i < 12 && fp && fp < HOS_GUEST_SPACE; i++) {
        fprintf(stderr, " lr %#llx", (unsigned long long)recomp_load64(c, fp + 8));
        fp = recomp_load64(c, fp);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* step39: f_81adbfa0 reached its loop head with a wild node. via_prologue: this call ran the prologue (1) or the
   loop head was entered some other way (0). Ctx has no field that says whether the context was resumed. */
void hos_bad_head_report(Ctx *c, int via_prologue) {
    static int hits;
    if (hits >= 3) return;
    hits++;
    char tname[24];
    if (pthread_getname_np(pthread_self(), tname, sizeof tname) != 0) tname[0] = 0;
    const uint64_t x19 = c->x[19], x20 = c->x[20], sp = c->x[31], fp0 = c->x[29];
    fprintf(stderr, "seq: bad head x19 %#llx x20 %#llx sp %#llx fp %#llx lr %#llx thread %s prologue %d\n",
            (unsigned long long)x19, (unsigned long long)x20, (unsigned long long)sp,
            (unsigned long long)fp0, (unsigned long long)c->x[30], tname[0] ? tname : "?", via_prologue);
    if (x20 && x20 < HOS_GUEST_SPACE)
        fprintf(stderr, "seq: bad head node x20 %#llx vtbl %#llx n %u cap %llu\n", (unsigned long long)x20,
                (unsigned long long)recomp_load64(c, x20), (unsigned)recomp_load32(c, x20 + 12),
                (unsigned long long)recomp_load64(c, x20 + 88));
    else fprintf(stderr, "seq: bad head x20 %#llx outside the guest window (no loads)\n", (unsigned long long)x20);
    uint64_t fp = fp0;
    fprintf(stderr, "seq: guest frames (fp %#llx)", (unsigned long long)fp);
    for (int i = 0; i < 12 && fp && fp < HOS_GUEST_SPACE; i++) {
        fprintf(stderr, " lr %#llx", (unsigned long long)recomp_load64(c, fp + 8));
        fp = recomp_load64(c, fp);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* step40: the prologue save slot for x19 (this frame's sp+56) changed value during the slot-3 ICALL.
   Reported by the innermost f_81adbfa0 frame that saw it change, so `target`/`child` name the direct
   child call during which the byte was written. Diagnosis only. */
void hos_slot_clobber_report(Ctx *c, uint64_t slot_addr, uint64_t oldv, uint64_t newv, uint64_t target, uint64_t child) {
    static int hits;
    if (hits >= 5) return;
    hits++;
    const char *hos_hostcall_name(uint64_t addr);
    char tname[24];
    if (pthread_getname_np(pthread_self(), tname, sizeof tname) != 0) tname[0] = 0;
    const char *hn = hos_hostcall_name(target);
    const A64Fn fn = a64_lookup(target);
    fprintf(stderr, "seq: slot clobber #%d slot %#llx old %#llx new %#llx xor %#llx bytes",
            hits, (unsigned long long)slot_addr, (unsigned long long)oldv, (unsigned long long)newv,
            (unsigned long long)(oldv ^ newv));
    for (int b = 0; b < 8; b++)
        if (((oldv ^ newv) >> (8 * b)) & 0xffULL)
            fprintf(stderr, " b%d %02x->%02x", b, (unsigned)((oldv >> (8 * b)) & 0xffULL),
                    (unsigned)((newv >> (8 * b)) & 0xffULL));
    fprintf(stderr, "\n");
    fprintf(stderr, "seq: slot clobber target %#llx f_%08llx kind %s lookup %p host %s sp %#llx lr %#llx\n",
            (unsigned long long)target, (unsigned long long)(uint32_t)target,
            hn ? "host HLE" : fn ? "lifted" : "UNKNOWN", (void *)fn, hn ? hn : "-",
            (unsigned long long)c->x[31], (unsigned long long)c->x[30]);
    const int child_ok = child && child < HOS_GUEST_SPACE;
    fprintf(stderr, "seq: slot clobber child %#llx vtbl %#llx child_x0_now %#llx thread %s\n",
            (unsigned long long)child,
            (unsigned long long)(child_ok ? recomp_load64(c, child) : 0),
            (unsigned long long)c->x[0], tname[0] ? tname : "?");
    for (int i = 0; i < 6; i++) {
        const uint64_t v = child_ok ? recomp_load64(c, child + (uint64_t)i * 8) : 0;
        const char *ihn = v ? hos_hostcall_name(v) : 0;
        fprintf(stderr, "seq: slot clobber vslot[%d] %#llx lookup %p host %s\n", i, (unsigned long long)v,
                (void *)(v ? a64_lookup(v) : 0), ihn ? ihn : "-");
    }
    uint64_t fp = c->x[29];
    fprintf(stderr, "seq: guest frames (fp %#llx)", (unsigned long long)fp);
    for (int i = 0; i < 12 && fp && fp < HOS_GUEST_SPACE; i++) {
        fprintf(stderr, " lr %#llx", (unsigned long long)recomp_load64(c, fp + 8));
        fp = recomp_load64(c, fp);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* ---- step42 (diagnosis only): the flag store at f_81dcb488+0xec (guest 0x81dcb574, L_81dcb56c). It stores 1 into
   the byte that obj->[8] points at; the step41 watch named this store as the writer of the 0x100000001100540 value in
   f_81adbfa0's saved-x19 slot. This reports every pass (max 20): the target, whether it looks like a dead frame of
   this thread's stack (inside 1 MiB above the current guest sp), the object, and the guest frame chain at the store. */
static volatile uint64_t flag_site_lo, flag_site_hi;   /* (host-rip window around the store, for the watch handler) */
void hos_flag_store_report(Ctx *c, uint64_t entry_lr) {
    static int hits;
    const uint64_t ra = (uint64_t)(uintptr_t)__builtin_return_address(0);
    flag_site_lo = ra > 0x200 ? ra - 0x200 : 0;   /* (before the 20-hit cap: the watch uses it on every pass) */
    flag_site_hi = ra + 0x200;
    if (hits >= 20) return;
    hits++;
    char tname[24];
    if (pthread_getname_np(pthread_self(), tname, sizeof tname) != 0) tname[0] = 0;
    const uint64_t tgt = c->x[8], sp = c->x[31], obj = c->x[19], fp0 = c->x[29], x20 = c->x[20];
    const int above = tgt > sp && (tgt - sp) < (1ULL << 20);
    const int instk = tgt < HOS_GUEST_SPACE;
    const int obj_ok = obj && obj < HOS_GUEST_SPACE;
    fprintf(stderr, "flag:#%d target %#llx sp %#llx %s (+%#llx) cur8 %02x x20 %#llx thread %s entry_lr %#llx\n",
            hits, (unsigned long long)tgt, (unsigned long long)sp,
            above ? "DEAD-STACK?" : "not-above-sp", (unsigned long long)(tgt - sp),
            (unsigned)(instk ? recomp_load8(c, tgt) : 0), (unsigned long long)x20,
            tname[0] ? tname : "?", (unsigned long long)entry_lr);
    fprintf(stderr, "flag: obj %#llx vtbl %#llx x19 %s obj+8 %#llx\n",
            (unsigned long long)obj,
            (unsigned long long)(obj_ok ? recomp_load64(c, obj) : 0),
            obj_ok ? "ok" : "BAD", (unsigned long long)(obj_ok ? recomp_load64(c, obj + 8) : 0));
    fprintf(stderr, "flag: obj fields");
    for (int i = 0; i < 8; i++)
        fprintf(stderr, " [%d]%#llx", i, (unsigned long long)(obj_ok ? recomp_load64(c, obj + (uint64_t)i * 8) : 0));
    fprintf(stderr, "\n");
    uint64_t fp = fp0;
    fprintf(stderr, "flag: guest frames (fp %#llx)", (unsigned long long)fp);
    for (int i = 0; i < 12 && fp && fp < HOS_GUEST_SPACE; i++) {
        fprintf(stderr, " lr %#llx", (unsigned long long)recomp_load64(c, fp + 8));
        fp = recomp_load64(c, fp);
    }
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* ---- step43 (diagnosis only): up to three x86 hardware write watchpoints on guest addresses, each "addr:len".
   Purpose: name the instruction on the *start* side of the step42 use-after-return - the store that wrote
   0x11004ff into the object's +8 field, and the first write of the flag byte itself (done=false at start time,
   then done=true at completion). HOS_WATCH / HOS_WATCH2 / HOS_WATCH3 = "<hex guest address>:<len>", len 1|2|4|8
   (default 8); HOS_WATCH_VAL = <hex>: every hit whose new value equals it is logged with a 12-level guest fp walk.
   Per watch only the first 16 distinct host rips are logged (plus every value match) - the flag byte is rewritten
   by every frame that reuses the stack slot, so an unfiltered log would drown in noise. The interrupting rip is
   the instruction after the store; symbolize offline with `nm -n` (the binary keeps its f_XXXXXXXX symbols) as
   f_<sym>+<x>. Every watch is write-only and armed on all threads of the process. */
#if defined(__linux__)
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#ifndef TRAP_PERF   /* (the kernel's since 5.13; glibc names it from 2.36) */
#define TRAP_PERF 6
#endif

enum { WATCH_N = 3, WATCH_MAXFD = 1024, WATCH_DISTINCT = 16, WATCH_SIGMAGIC = 0x57544348 };
struct Watch {
    uint64_t host, guest, prev;
    int len, hits, distinct;
    int mapped;                  /* (was the page there when we armed? then prev is meaningful) */
    uint64_t rips[WATCH_DISTINCT];
};
static struct Watch watch[WATCH_N];
static int watch_count, watch_fd[WATCH_MAXFD], watch_nfd;
static uint64_t watch_bin_base, watch_val;
static int watch_val_set;
static __thread Ctx *watch_ctx;

/* (async-signal-safe: no printf, one write(2) per line) */
static char *w_app(char *p, char *e, const char *s) { while (*s && p < e) *p++ = *s++; return p; }
static char *w_hex(char *p, char *e, uint64_t v) {
    char t[17];
    int n = 0;
    if (!v) t[n++] = '0';
    while (v && n < 17) { const int d = (int)(v & 15); t[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10); v >>= 4; }
    if (p < e) *p++ = '0';
    if (p < e) *p++ = 'x';
    while (n && p < e) *p++ = t[--n];
    return p;
}
static char *w_dec(char *p, char *e, long long v) {
    char t[24];
    int n = 0;
    unsigned long long u = v < 0 ? (unsigned long long)(-v) : (unsigned long long)v;
    if (!u) t[n++] = '0';
    while (u && n < 23) { t[n++] = (char)('0' + (int)(u % 10)); u /= 10; }
    if (v < 0 && p < e) *p++ = '-';
    while (n && p < e) *p++ = t[--n];
    return p;
}
static uint64_t w_load(uint64_t a, int len) {
    const void *p = (const void *)(uintptr_t)a;
    if (len == 1) return *(const volatile uint8_t *)p;
    if (len == 2) { uint16_t v; memcpy(&v, p, 2); return v; }
    if (len == 4) { uint32_t v; memcpy(&v, p, 4); return v; }
    { uint64_t v; memcpy(&v, p, 8); return v; }
}
static void w_emit(int i, const char *tag, uint64_t rip, uint64_t val, uint64_t prev, Ctx *c) {
    char b[512], *p = b, *e = b + sizeof b - 2;
    p = w_app(p, e, "watch"); p = w_dec(p, e, i + 1); p = w_app(p, e, ": ");
    p = w_app(p, e, tag);
    p = w_app(p, e, " off "); p = w_hex(p, e, rip - watch_bin_base);
    p = w_app(p, e, " rip "); p = w_hex(p, e, rip);
    p = w_app(p, e, " val "); p = w_hex(p, e, val);
    p = w_app(p, e, " prev "); p = w_hex(p, e, prev);
    p = w_app(p, e, " lr "); p = w_hex(p, e, c ? c->x[30] : 0);
    p = w_app(p, e, " sp "); p = w_hex(p, e, c ? c->x[31] : 0);
    p = w_app(p, e, " tid "); p = w_dec(p, e, (long long)syscall(SYS_gettid));
    p = w_app(p, e, " hit "); p = w_dec(p, e, watch[i].hits);
    p = w_app(p, e, "\n");
    (void)!write(2, b, (size_t)(p - b));
}
/* (the guest fp chain of the interrupted thread: {fp, lr} pairs down the guest stack) */
static void w_fpwalk(int i, Ctx *c) {
    char b[640], *p = b, *e = b + sizeof b - 2;
    p = w_app(p, e, "watch"); p = w_dec(p, e, i + 1); p = w_app(p, e, " fp:");
    uint64_t fp = c ? c->x[29] : 0;
    for (int k = 0; k < 12 && fp && fp < HOS_GUEST_SPACE; k++) {
        p = w_app(p, e, " lr "); p = w_hex(p, e, recomp_load64(c, fp + 8));
        fp = recomp_load64(c, fp);
    }
    p = w_app(p, e, "\n");
    (void)!write(2, b, (size_t)(p - b));
}
/* (this glibc does not expose si_perf_data; the kernel's 128-byte siginfo block still carries our sig_data) */
static int w_sigindex(siginfo_t *si) {
    uint64_t q;
    for (int k = 0; k < 16; k++) {
        memcpy(&q, (const char *)(const void *)si + k * 8, 8);
        if ((uint32_t)(q & 0xffffffffu) == (uint32_t)WATCH_SIGMAGIC) return (int)(q >> 32);
    }
    return -1;
}
static void on_watch(int sig, siginfo_t *si, void *uctx) {
    (void)sig;
    if (si->si_code != TRAP_PERF) return;   /* (not ours: a debugger's or the game's int3) */
    const uint64_t rip = (uint64_t)((ucontext_t *)uctx)->uc_mcontext.gregs[REG_RIP];
    Ctx *c = watch_ctx;
    uint64_t nv[WATCH_N];
    int hit[WATCH_N] = {0, 0, 0}, any = 0;
    const int sel = w_sigindex(si);
    if (sel >= 0 && sel < watch_count) { hit[sel] = 1; any = 1; }
    for (int i = 0; i < watch_count; i++) {
        nv[i] = w_load(watch[i].host, watch[i].len);
        if (!any && nv[i] != watch[i].prev) { hit[i] = 1; any = 1; }   /* (fallback: the one that changed) */
    }
    for (int i = 0; i < watch_count; i++) {
        if (!hit[i]) { if (nv[i] != watch[i].prev) watch[i].prev = nv[i]; continue; }
        const uint64_t val = nv[i], prev = watch[i].prev;
        watch[i].hits++;
        watch[i].prev = val;
        int seen = 0;
        for (int k = 0; k < watch[i].distinct; k++) if (watch[i].rips[k] == rip) { seen = 1; break; }
        int logit = 0;
        if (!seen && watch[i].distinct < WATCH_DISTINCT) { watch[i].rips[watch[i].distinct++] = rip; logit = 1; }
        const int valmatch = watch_val_set && val == watch_val;   /* (exact: the 8-byte pointer value, step43) */
        const int changed = val != prev;                           /* (a real transition: done false->true etc.) */
        if (valmatch || changed) logit = 1;
        if (!logit) continue;
        w_emit(i, valmatch ? "VALMATCH" : changed ? "CHG" : "hit", rip, val, prev, c);
        if (valmatch) w_fpwalk(i, c);
    }
}
static int w_mapped(uint64_t a, int len) {   /* (so the arm-time "prev" read cannot fault if the heap slot is late) */
    const long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) return 0;
    const uint64_t end = a + (uint64_t)len;
    for (uint64_t x = a & ~((uint64_t)ps - 1); x < end; x += (uint64_t)ps) {
        unsigned char v;
        if (mincore((void *)(uintptr_t)x, (size_t)ps, &v) != 0) return 0;
    }
    return 1;
}
static int watch_arm_tid(int i, int tid) {
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.type = PERF_TYPE_BREAKPOINT;
    a.size = sizeof a;
    a.bp_type = HW_BREAKPOINT_W;
    a.bp_addr = watch[i].host;
    a.bp_len = watch[i].len == 1 ? HW_BREAKPOINT_LEN_1 : watch[i].len == 2 ? HW_BREAKPOINT_LEN_2
             : watch[i].len == 4 ? HW_BREAKPOINT_LEN_4 : HW_BREAKPOINT_LEN_8;
    a.sample_period = 1;
    a.exclude_kernel = 1;
    a.exclude_hv = 1;
    a.sigtrap = 1;
    a.remove_on_exec = 1;
    a.sig_data = (uint64_t)WATCH_SIGMAGIC | ((uint64_t)i << 32);   /* (the handler recovers the index from it) */
    const int fd = (int)syscall(SYS_perf_event_open, &a, tid, -1, -1, PERF_FLAG_FD_CLOEXEC);
    if (fd < 0) return -errno;
    (void)ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
    if (watch_nfd < WATCH_MAXFD) watch_fd[watch_nfd++] = fd;
    return 0;
}
static long long watch_paranoid(void) {
    FILE *f = fopen("/proc/sys/kernel/perf_event_paranoid", "r");
    long long v = -999;
    if (f) { if (fscanf(f, "%lld", &v) != 1) v = -998; fclose(f); }
    return v;
}
void hos_watch_arm(Ctx *c) {
    watch_ctx = c;   /* (per thread: the handler prints its guest lr/sp/fp) */
    static int armed;
    if (armed) return;
    const char *names[WATCH_N] = {"HOS_WATCH", "HOS_WATCH2", "HOS_WATCH3"};
    int want = 0;
    for (int i = 0; i < WATCH_N; i++) { const char *e = getenv(names[i]); if (e && *e) want = 1; }
    if (!want) return;
    armed = 1;
    uint64_t bin[2] = {0, 0};
    dl_iterate_phdr(diag_main_phdr, bin);
    watch_bin_base = bin[0];
    const char *vs = getenv("HOS_WATCH_VAL");
    if (vs && *vs) { watch_val = strtoull(vs, NULL, 16); watch_val_set = 1; }
    for (int i = 0; i < WATCH_N; i++) {
        const char *e = getenv(names[i]);
        if (!e || !*e) continue;
        struct Watch *w = &watch[watch_count];
        memset(w, 0, sizeof *w);
        char *end = NULL;
        w->guest = strtoull(e, &end, 16);
        w->len = (end && *end == ':') ? atoi(end + 1) : 8;
        if (w->len != 1 && w->len != 2 && w->len != 4 && w->len != 8) w->len = 8;
        w->host = (uint64_t)(uintptr_t)A64_PTR(w->guest);
        w->mapped = w_mapped(w->host, w->len);
        w->prev = w->mapped ? w_load(w->host, w->len) : 0;
        watch_count++;
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_watch;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    struct sigaction cur;
    if (sigaction(SIGTRAP, &sa, &cur) != 0)
        hos_log("watch: sigaction(SIGTRAP) failed: %s", strerror(errno));
    else if (cur.sa_sigaction && cur.sa_sigaction != on_watch)
        hos_log("watch: replaced an existing SIGTRAP handler %p", (void *)cur.sa_sigaction);
    for (int i = 0; i < watch_count; i++)
        hos_log("watch: watch%d host %#llx guest %#llx len %d prev %#llx %s", i + 1,
                (unsigned long long)watch[i].host, (unsigned long long)watch[i].guest, watch[i].len,
                (unsigned long long)watch[i].prev, watch[i].mapped ? "(mapped)" : "(NOT MAPPED at arm: prev unknown)");
    if (watch_val_set) hos_log("watch: HOS_WATCH_VAL %#llx (value matches also get an fp walk, no distinct cap)",
                               (unsigned long long)watch_val);
    int ok = 0, failed = 0, err = 0, okc[WATCH_N] = {0, 0, 0};
    DIR *d = opendir("/proc/self/task");
    if (d) {
        for (struct dirent *de; (de = readdir(d));) {
            const int tid = atoi(de->d_name);
            if (tid <= 0) continue;
            for (int i = 0; i < watch_count; i++) {
                const int r = watch_arm_tid(i, tid);
                if (r == 0) ok++, okc[i]++;
                else failed++, err = -r;
            }
        }
        closedir(d);
    }
    char per[96], *pp = per, *pe = per + sizeof per - 2;
    per[0] = 0;
    for (int i = 0; i < watch_count; i++) {
        pp = w_app(pp, pe, i ? " w" : "w"); pp = w_dec(pp, pe, i + 1); pp = w_app(pp, pe, ":");
        pp = w_dec(pp, pe, okc[i]);
    }
    *pp = 0;
    hos_log("watch: armed %d breakpoints on %d threads each (%s; %d failed, %s); base %#llx-%#llx paranoid %lld",
            watch_count, watch_count ? ok : 0, per, failed, failed ? strerror(err) : "-",
            (unsigned long long)bin[0], (unsigned long long)bin[1], watch_paranoid());
    fflush(stderr);
}

/* ---- step44 (diagnosis only): report the construction of a vtable-0x857734a0 object.
   f_81dca9ec (the ctor) copies obj+8 from arg0->+8, and obj+8 is the pointer whose target byte gets
   written 0x01 by f_81dcb488, so arg0 is what owns the bool. Entered before the prologue store: x0 is
   arg0, x30 is the caller, x31/x29 are still the caller's sp/fp. For each hit: arg0, its first six
   words, the pointee byte, and a 12-level guest fp walk with each frame's [fp, parent fp) range; the
   level whose range contains the pointee is flagged, as is "the pointee is a local of the caller
   itself" (sp <= p < fp). Prints at most 20 hits. (Static findings, no judgement: the offsets and the
   owner's lr are what matter.) */
static uint64_t crd(Ctx *c, uint64_t a, int len) {   /* (guest read that cannot fault) */
    if (!a || a >= HOS_GUEST_SPACE) return 0;
    if (!w_mapped((uint64_t)(uintptr_t)A64_PTR(a), len)) return 0;
    return w_load((uint64_t)(uintptr_t)A64_PTR(a), len);
}
void hos_ctor_report(Ctx *c) {
    static int hits;
    if (hits >= 20) return;
    hits++;
    char tname[24];
    if (pthread_getname_np(pthread_self(), tname, sizeof tname) != 0) tname[0] = 0;
    const uint64_t lr = c->x[30], x0 = c->x[0], sp = c->x[31], fp0 = c->x[29];
    const uint64_t p = crd(c, x0 + 8, 8);
    fprintf(stderr, "ctor:#%d lr %#llx x0 %#llx sp %#llx fp %#llx thread %s\n", hits,
            (unsigned long long)lr, (unsigned long long)x0, (unsigned long long)sp,
            (unsigned long long)fp0, tname[0] ? tname : "?");
    fprintf(stderr, "ctor: arg0 fields");
    for (int i = 0; i < 6; i++)
        fprintf(stderr, " [+%d]%#llx", i * 8, (unsigned long long)crd(c, x0 + (uint64_t)i * 8, 8));
    fprintf(stderr, "\n");
    const uint64_t pbyte = crd(c, p, 1);
    const int self_owns = p >= sp && p < fp0;
    fprintf(stderr, "ctor: p %#llx byte %02x in_guest %d vs_sp %s%#llx self_sp<=p<fp %d\n",
            (unsigned long long)p, (unsigned)(pbyte & 0xff), (int)(p < HOS_GUEST_SPACE),
            p >= sp ? "+" : "-", (unsigned long long)(p >= sp ? p - sp : sp - p), self_owns);
    uint64_t fp = fp0;
    for (int k = 0; k < 12 && fp && fp < HOS_GUEST_SPACE; k++) {
        const uint64_t nfp = crd(c, fp, 8), flr = crd(c, fp + 8, 8);
        const int owns = nfp > fp && p >= fp && p < nfp;
        fprintf(stderr, "ctor: fp[%d] fp %#llx lr %#llx range [%#llx,%#llx) owns_bool %d\n", k,
                (unsigned long long)fp, (unsigned long long)flr, (unsigned long long)fp,
                (unsigned long long)nfp, owns);
        fp = nfp;
    }
    fflush(stderr);
}
/* step45: TBI experiment - report and mask a top-byte-corrupted element pointer (logs once) */
void hos_tbi_note(Ctx *c) {
    static int hits;
    if (hits >= 5) return;
    hits++;
    fprintf(stderr, "tbi: masked x19 %#llx in f_81adbfa0 (sp %#llx fp %#llx lr %#llx)\n",
            (unsigned long long)c->x[19], (unsigned long long)c->x[31],
            (unsigned long long)c->x[29], (unsigned long long)c->x[30]);
    fflush(stderr);
}
#else
void hos_watch_arm(Ctx *c) { (void)c; }
#endif
