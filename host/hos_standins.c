#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

void hos_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    const uint64_t lo = addr & ~0xFFFFULL, hi = (addr + size + 0xFFFF) & ~0xFFFFULL;
    if (!VirtualAlloc(GP(lo), (SIZE_T)(hi - lo), MEM_COMMIT, PAGE_READWRITE)) {
        const DWORD err = GetLastError();
        hos_log("mem: commit of %#llx bytes at %#llx failed (error %lu)",
                (unsigned long long)(hi - lo), (unsigned long long)lo, err);
    }
}

static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS *info) {
    EXCEPTION_RECORD *rec = info->ExceptionRecord;
    if (rec->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || rec->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    const uint8_t *p = (const uint8_t *)rec->ExceptionInformation[1];
    if (p < A64_MEM || p >= A64_MEM + HOS_GUEST_SPACE) return EXCEPTION_CONTINUE_SEARCH;
    const uint64_t guest = (uint64_t)(p - A64_MEM);
    if (guest < 0x100000) return EXCEPTION_CONTINUE_SEARCH;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_RESERVE) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG touched;
    if (InterlockedIncrement(&touched) <= 16)
        hos_log("mem: %#llx committed on first touch (nothing handed it out)", (unsigned long long)guest);
    return VirtualAlloc(GP(guest & ~0xFFFFULL), 0x10000, MEM_COMMIT, PAGE_READWRITE) ? EXCEPTION_CONTINUE_EXECUTION
                                                                                      : EXCEPTION_CONTINUE_SEARCH;
}

static uint64_t peek_guest(uint64_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (a >= HOS_GUEST_SPACE - 8 || !VirtualQuery(GP(a), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    return G64(a);
}

void hos_thread_backtrace(HosThread *t, char *buf, size_t cap) {
    if (!buf || cap == 0) return;
    buf[0] = 0;
    Ctx *c = hos_thread_ctx(t);
    if (!c) { snprintf(buf, cap, "(no ctx)"); return; }
    int n = snprintf(buf, cap, "lr %#llx", (unsigned long long)c->x[30]);
    uint64_t fp = c->x[29];
    for (int i = 0; i < 40 && fp && !(fp & 15) && n < (int)cap - 24; i++) {
        const uint64_t lr = peek_guest(fp + 8), next = peek_guest(fp);
        if (!lr) break;
        n += snprintf(buf + n, cap - n, " %#llx", (unsigned long long)lr);
        if (next <= fp) break;
        fp = next;
    }
}

void hos_backtrace(void) {
    char bt[1024];
    hos_thread_backtrace(hos_self(), bt, sizeof bt);
    fprintf(stderr, "[hos] guest backtrace: %s\n", bt);
    fflush(stderr);
}

void hos_commit_init(void) {
    AddVectoredExceptionHandler(1, commit_on_touch);
}

void hos_mem_checkpoint(const char *what) { (void)what; }
int hos_game_cores(void) { return 0; }
int hos_blank_cached_icon(const char *guest, const char *host) { (void)guest; (void)host; return 0; }
int hos_ultrawide_redirect(const char *guest, char *out, size_t cap) { (void)guest; (void)out; (void)cap; return 0; }
uint64_t hos_title_input(uint64_t buttons) { return buttons; }
uint64_t hos_frame_step_buttons(uint64_t buttons) { return buttons; }
int hos_gpu_async(void) { return 0; }
void hos_gpu_submit(int channel_fd, const uint64_t *entries, uint32_t n) {
    (void)channel_fd; (void)entries; (void)n;
}
void hos_gpu_queue_submit(int channel_fd, const uint64_t *entries, uint32_t n, int incr_id) {
    (void)incr_id;
    hos_gpu_submit(channel_fd, entries, n);
}
void hos_host_thread_register_self(const char *name) { (void)name; }
