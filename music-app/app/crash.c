/* Crash reports in music.log.
 *
 * Before this a crash left nothing behind: the supervisor restarted Libra and
 * the only evidence was a gap in the log, so a crash like the frozen-queue one
 * had to be worked out from indirect clues. Now a fatal signal writes what it
 * can to music.log first -- the signal, the faulting address, the program
 * counter, the return address, the code addresses found on the stack, the
 * executable mappings to place them in, and what Libra was doing -- and then
 * dies of the same signal, so the supervisor restarts it exactly as before.
 * (The idea and its shape come from compas-player's crash handler.)
 *
 * Everything in the handler is async-signal-safe: raw open/read/write, no
 * stdio, no malloc, fixed static buffers. The addresses are resolved later,
 * off the device, against an unstripped build of the same source
 * (build_standalone.sh keeps one as library_standalone.debug). */
#define _GNU_SOURCE
#include "crash.h"

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef CRASH_LOG
#define CRASH_LOG "/usr/data/music.log"
#endif
#define STACK_WORDS 2048          /* 8 KB scanned from sp, at most */
#define STACK_HITS  24            /* code addresses reported from it */
#define MAX_MAPS    96

/* What Libra was doing, copied in by the owners; read (never written) by the
 * handler, so a torn copy costs one garbled line at worst. */
static char note_track[256];
static char note_scan[256];
static volatile const char *const volatile *note_phase;

void crash_note_track(const char *path) {
    if (!path) path = "";
    strncpy(note_track, path, sizeof(note_track) - 1);
}

void crash_note_scan(const char *path) {
    if (!path) path = "";
    strncpy(note_scan, path, sizeof(note_scan) - 1);
}

void crash_note_phase(volatile const char *const volatile *phase) { note_phase = phase; }

/* ---- async-signal-safe output --------------------------------------- */

static char out[2048];
static size_t out_n;
static int out_fd = -1;

static void flush_out(void) {
    size_t off = 0;
    while (out_fd >= 0 && off < out_n) {
        ssize_t w = write(out_fd, out + off, out_n - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    out_n = 0;
}

static void put(const char *s) {
    while (*s) {
        if (out_n == sizeof(out)) flush_out();
        out[out_n++] = *s++;
    }
}

static void put_n(const char *s, size_t n) {
    for (size_t i = 0; i < n && s[i]; i++) {
        if (out_n == sizeof(out)) flush_out();
        out[out_n++] = s[i];
    }
}

static void put_hex(uintptr_t v) {
    char b[2 + sizeof(v) * 2 + 1];
    int i = (int)sizeof(b) - 1;
    b[i] = '\0';
    do { b[--i] = "0123456789abcdef"[v & 0xF]; v >>= 4; } while (v && i > 2);
    b[--i] = 'x';
    b[--i] = '0';
    put(b + i);
}

static void put_dec(long v, int width, char pad) {
    char b[24];
    int i = (int)sizeof(b) - 1, neg = v < 0;
    unsigned long u = neg ? (unsigned long)-v : (unsigned long)v;
    b[i] = '\0';
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    while ((int)sizeof(b) - 1 - i < width) b[--i] = pad;
    put(b + i);
}

/* "[  1234.567] [crash] " -- the same stamp mlog/alog use, so the report
 * sorts into the log with everything around it. */
static void line_start(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    put("[");
    put_dec((long)ts.tv_sec, 6, ' ');
    put(".");
    put_dec(ts.tv_nsec / 1000000L, 3, '0');
    put("] [crash] ");
}

/* ---- /proc/self/maps, read in the handler ---------------------------- */

typedef struct { uintptr_t lo, hi; int exec; } map_t;
static map_t maps[MAX_MAPS];
static int maps_n;
static char maps_buf[16384];
static size_t maps_len;

static uintptr_t parse_hex(const char **p) {
    uintptr_t v = 0;
    for (;;) {
        char c = **p;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return v;
        v = (v << 4) | (uintptr_t)d;
        (*p)++;
    }
}

static void read_maps(void) {
    maps_n = 0;
    maps_len = 0;
    int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    ssize_t r;
    while (maps_len < sizeof(maps_buf) - 1 &&
           (r = read(fd, maps_buf + maps_len, sizeof(maps_buf) - 1 - maps_len)) > 0)
        maps_len += (size_t)r;
    close(fd);
    maps_buf[maps_len] = '\0';
    const char *p = maps_buf;
    while (*p && maps_n < MAX_MAPS) {
        const char *line = p;
        uintptr_t lo = parse_hex(&p);
        if (*p == '-') p++;
        uintptr_t hi = parse_hex(&p);
        if (*p == ' ') p++;
        /* Code only: executable and not writable. This kernel maps the
         * stack and heap rwx, and their addresses are not return addresses. */
        int exec = p[0] && p[1] && p[1] != 'w' && p[2] == 'x';
        if (hi > lo) { maps[maps_n].lo = lo; maps[maps_n].hi = hi; maps[maps_n].exec = exec; maps_n++; }
        while (*p && *p != '\n') p++;
        if (*p) p++;
        (void)line;
    }
}

static int in_exec_map(uintptr_t a) {
    for (int i = 0; i < maps_n; i++)
        if (maps[i].exec && a >= maps[i].lo && a < maps[i].hi) return 1;
    return 0;
}

/* The range to scan from sp: its own mapping, or -- when sp is unmapped,
 * which is what a stack overflow looks like, sp just past the end of the
 * stack -- the mapping directly above it, if one starts within 64 KB. */
static int stack_range(uintptr_t sp, uintptr_t *from, uintptr_t *to) {
    int above = -1;
    for (int i = 0; i < maps_n; i++) {
        if (sp >= maps[i].lo && sp < maps[i].hi) { *from = sp; *to = maps[i].hi; return 1; }
        if (maps[i].lo > sp && maps[i].lo - sp <= 64 * 1024 &&
            (above < 0 || maps[i].lo < maps[above].lo)) above = i;
    }
    if (above < 0) return 0;   /* nothing near: scan nothing rather than fault again */
    *from = maps[above].lo;
    *to = maps[above].hi;
    return 1;
}

/* The executable mappings, one line each: enough to place every address above
 * in its library and turn it into an offset. */
static void put_exec_maps(void) {
    const char *p = maps_buf;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n') e++;
        /* "lo-hi r-xp off dev inode path": the x is the 3rd permission char */
        const char *perm = p;
        while (perm < e && *perm != ' ') perm++;
        if (perm + 3 < e && perm[2] != 'w' && perm[3] == 'x') {
            line_start();
            put("map ");
            put_n(p, (size_t)(e - p));
            put("\n");
        }
        p = *e ? e + 1 : e;
    }
}

/* ---- the handler ------------------------------------------------------ */

static const char *sig_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        case SIGABRT: return "SIGABRT";
        case SIGSYS:  return "SIGSYS";
        case SIGTRAP: return "SIGTRAP";
        default:      return "signal";
    }
}

static volatile sig_atomic_t crashing_tid;

static void on_crash(int sig, siginfo_t *si, void *ctx) {
    long tid = (long)syscall(SYS_gettid);
    if (crashing_tid) {
        /* The report itself faulted: die now rather than try again. */
        if (crashing_tid == tid) { signal(sig, SIG_DFL); raise(sig); _exit(128 + sig); }
        /* Another thread crashing while the first is still writing: let the
         * first finish; this one is taken down with the process. */
        for (;;) pause();
    }
    crashing_tid = (sig_atomic_t)tid;

    out_fd = open(CRASH_LOG, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);

    char comm[32] = "";
    int cfd = open("/proc/thread-self/comm", O_RDONLY | O_CLOEXEC);
    if (cfd >= 0) {
        ssize_t n = read(cfd, comm, sizeof(comm) - 1);
        close(cfd);
        if (n > 0) { comm[n] = '\0'; char *nl = strchr(comm, '\n'); if (nl) *nl = '\0'; }
    }

    line_start();
    put(sig_name(sig)); put(" ("); put_dec(sig, 0, ' '); put(") code ");
    put_dec(si ? si->si_code : 0, 0, ' ');
    put(" addr "); put_hex(si ? (uintptr_t)si->si_addr : 0);
    put(" in thread "); put_dec(tid, 0, ' ');
    if (comm[0]) { put(" '"); put(comm); put("'"); }
    put("\n");

    uintptr_t pc = 0, ra = 0, sp = 0;
    if (ctx) {
        ucontext_t *uc = (ucontext_t *)ctx;
#if defined(__mips__)
        pc = (uintptr_t)uc->uc_mcontext.pc;
        ra = (uintptr_t)uc->uc_mcontext.gregs[31];
        sp = (uintptr_t)uc->uc_mcontext.gregs[29];
#elif defined(__aarch64__)
        pc = (uintptr_t)uc->uc_mcontext.pc;
        ra = (uintptr_t)uc->uc_mcontext.regs[30];
        sp = (uintptr_t)uc->uc_mcontext.sp;
#elif defined(__x86_64__)
        pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
        sp = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
#endif
    }
    line_start();
    put("pc "); put_hex(pc); put(" ra "); put_hex(ra); put(" sp "); put_hex(sp); put("\n");

    read_maps();

    /* Return addresses left on the stack: every word from sp up to the end of
     * its mapping that points into executable code. Not a real unwind -- some
     * are stale -- but on MIPS, without frame pointers, it is what there is,
     * and the real callers are among them. */
    uintptr_t from, end;
    if (sp && stack_range(sp, &from, &end)) {
        if (end > from + STACK_WORDS * sizeof(uintptr_t)) end = from + STACK_WORDS * sizeof(uintptr_t);
        int hits = 0;
        line_start();
        put("stack");
        for (uintptr_t a = from & ~(uintptr_t)(sizeof(uintptr_t) - 1);
             a + sizeof(uintptr_t) <= end && hits < STACK_HITS; a += sizeof(uintptr_t)) {
            uintptr_t w = *(const uintptr_t *)a;
            if (in_exec_map(w)) { put(" "); put_hex(w); hits++; }
        }
        if (!hits) put(" (no code addresses)");
        put("\n");
    }

    if (note_phase && *note_phase) { line_start(); put("phase "); put((const char *)*note_phase); put("\n"); }
    if (note_track[0]) { line_start(); put("track "); put(note_track); put("\n"); }
    if (note_scan[0])  { line_start(); put("scanning "); put(note_scan); put("\n"); }

    put_exec_maps();
    line_start(); put("end of report, exiting for the supervisor to restart\n");
    flush_out();
    if (out_fd >= 0) { fsync(out_fd); close(out_fd); }

    /* Die of the same signal, as if nothing had caught it. */
    signal(sig, SIG_DFL);
    raise(sig);
    _exit(128 + sig);
}

/* The main thread's handler stack. A stack overflow faults on the thread's own
 * stack, so the handler needs one of its own there to run at all; the other
 * threads report on their own stacks, which covers everything but an
 * overflow on one of them. */
static char alt_stack[64 * 1024];

void crash_install(void) {
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = alt_stack;
    ss.ss_size = sizeof(alt_stack);
    sigaltstack(&ss, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGSYS, SIGTRAP };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
}
