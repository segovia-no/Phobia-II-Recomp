// Core runtime: guest memory, image loading, dispatcher, thunks, x87 helpers, threads, faults.
#include "runtime.h"
#include "meta.h"
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <execinfo.h>
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>

uint8_t *M;
int g_trace;
char g_game_dir[1024];

/* ------------------------------------------------------------------ logging */
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
void rt_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    pthread_mutex_lock(&g_log_lock);
    vfprintf(stderr, fmt, ap); fputc('\n', stderr); fflush(stderr);
    pthread_mutex_unlock(&g_log_lock);
    va_end(ap);
}
static void dump_cpu(CPU *c) {
    if (!c) return;
    fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x\n  esp=%08x ebp=%08x esi=%08x edi=%08x\n",
            c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
    if (c->esp > 0x10000 && c->esp < 0xF0000000u) {
        fprintf(stderr, "  stack:");
        for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", RD32(c->esp + 4 * i));
        fprintf(stderr, "\n");
    }
}
static void host_backtrace(void) {
    void *bt[64]; int n = backtrace(bt, 64);
    backtrace_symbols_fd(bt, n, 2);
}
void rt_fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "FATAL: "); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
    dump_cpu(cpu_current());
    host_backtrace();
    _exit(1);
}
void rt_bad(CPU *c, uint32_t addr) { rt_fatal("execution reached undecodable code at %08x", addr); }
void rt_unimpl(CPU *c, uint32_t addr, const char *what) { rt_fatal("unimplemented instruction at %08x: %s", addr, what); }
void rt_trap(CPU *c, uint32_t addr, const char *what) { rt_fatal("trap %s at %08x", what, addr); }
void rt_int(CPU *c, uint32_t addr, int n) { rt_fatal("software interrupt int 0x%x at %08x", n, addr); }
void rt_divzero(CPU *c, uint32_t addr) { rt_fatal("integer divide by zero at %08x", addr); }
void rt_missing_import(CPU *c, const char *dll, const char *name) {
    rt_fatal("unimplemented import %s!%s (called from %08x)", dll, name, RD32(c->esp));
}
void rt_retmismatch(CPU *c, uint32_t expected) {
    rt_log("WARN: return address mismatch: expected %08x got %08x", expected, c->retaddr);
}
void rt_cpuid(CPU *c) {
    uint32_t leaf = c->eax;
    if (leaf == 0) { c->eax = 1; c->ebx = 0x756e6547; c->edx = 0x49656e69; c->ecx = 0x6c65746e; }
    else { c->eax = 0x543; c->ebx = 0; c->ecx = 0; c->edx = 0x8001bf; }  /* Pentium MMX-ish, FPU+TSC+CX8+CMOV+MMX */
}
uint32_t rt_getflags(CPU *c) {
    return 0x202 | c->cf | (c->pf << 2) | (c->af << 4) | (c->zf << 6) | (c->sf << 7) | (c->df << 10) | (c->of << 11);
}
void rt_setflags(CPU *c, uint32_t f) {
    c->cf = f & 1; c->pf = (f >> 2) & 1; c->af = (f >> 4) & 1; c->zf = (f >> 6) & 1;
    c->sf = (f >> 7) & 1; c->df = (f >> 10) & 1; c->of = (f >> 11) & 1;
}
void rt_movs(CPU *c, int sz) {
    uint32_t n = c->ecx * sz, s = c->esi, d = c->edi;
    if (n) {
        if (d > s && d < s + n) { for (uint32_t i = 0; i < n; i++) M[d + i] = M[s + i]; }
        else memmove(M + d, M + s, n);
    }
    c->esi += n; c->edi += n; c->ecx = 0;
}
void rt_stos(CPU *c, int sz) {
    uint32_t n = c->ecx, d = c->edi;
    if (sz == 1) memset(M + d, (uint8_t)c->eax, n);
    else for (uint32_t i = 0; i < n; i++) WR32(d + 4 * i, c->eax);
    c->edi += n * sz; c->ecx = 0;
}

/* ------------------------------------------------------------------ x87 */
void fpu_init(CPU *c) { c->ftop = 0; c->fcw = 0x037f; c->fsw = 0; for (int i = 0; i < 8; i++) c->st[i] = 0; }
void fpu_compare(CPU *c, double a, double b) {
    c->fsw &= ~(0x0100 | 0x0400 | 0x4000);
    if (a != a || b != b) c->fsw |= 0x0100 | 0x0400 | 0x4000;
    else if (a < b) c->fsw |= 0x0100;
    else if (a == b) c->fsw |= 0x4000;
}
void fpu_fxam(CPU *c) {
    double v = ST(0);
    c->fsw &= ~(0x0100 | 0x0200 | 0x0400 | 0x4000);
    if (signbit(v)) c->fsw |= 0x0200;
    if (v != v) c->fsw |= 0x0100;
    else if (isinf(v)) c->fsw |= 0x0500;
    else if (v == 0) c->fsw |= 0x4000;
    else c->fsw |= 0x0400;
}
uint16_t fpu_status(CPU *c) { return (uint16_t)((c->fsw & ~0x3800) | ((c->ftop & 7) << 11)); }
double fpu_round(CPU *c, double v) {
    switch ((c->fcw >> 10) & 3) {
        case 0: return nearbyint(v);
        case 1: return floor(v);
        case 2: return ceil(v);
        default: return trunc(v);
    }
}
int16_t fpu_to_int16(CPU *c, double v) { double r = fpu_round(c, v); if (r != r || r > 32767 || r < -32768) return (int16_t)0x8000; return (int16_t)r; }
int32_t fpu_to_int32(CPU *c, double v) { double r = fpu_round(c, v); if (r != r || r > 2147483647.0 || r < -2147483648.0) return (int32_t)0x80000000; return (int32_t)r; }
int64_t fpu_to_int64(CPU *c, double v) { double r = fpu_round(c, v); if (r != r || r > 9.2e18 || r < -9.2e18) return (int64_t)0x8000000000000000ull; return (int64_t)r; }
double RDF80(uint32_t a) {
    uint64_t mant = RD64(a); uint16_t se = RD16(a + 8);
    int sign = se >> 15; int exp = se & 0x7fff;
    if (exp == 0 && mant == 0) return sign ? -0.0 : 0.0;
    if (exp == 0x7fff) return (mant << 1) ? NAN : (sign ? -INFINITY : INFINITY);
    double v = ldexp((double)mant, exp - 16383 - 63);
    return sign ? -v : v;
}
void WRF80(uint32_t a, double v) {
    uint64_t mant = 0; uint16_t se = 0;
    if (signbit(v)) { se = 0x8000; v = -v; }
    if (v == 0) {}
    else if (isinf(v)) { se |= 0x7fff; mant = 0x8000000000000000ull; }
    else if (v != v) { se |= 0x7fff; mant = 0xC000000000000000ull; }
    else { int e; double m = frexp(v, &e); mant = (uint64_t)ldexp(m, 64); se |= (uint16_t)(e - 1 + 16383); }
    WR64(a, mant); WR16(a + 8, se);
}
void fpu_save(CPU *c, uint32_t a) {
    memset(M + a, 0, 108);
    WR16(a, c->fcw); WR16(a + 4, fpu_status(c));
    for (int i = 0; i < 8; i++) WRF80(a + 28 + 10 * i, ST(i));
    fpu_init(c);
}
void fpu_restore(CPU *c, uint32_t a) {
    c->fcw = RD16(a); uint16_t sw = RD16(a + 4); c->fsw = sw & ~0x3800; c->ftop = (sw >> 11) & 7;
    for (int i = 0; i < 8; i++) ST(i) = RDF80(a + 28 + 10 * i);
}

/* ------------------------------------------------------------------ guest heap */
#define HEAP_LO 0x10000000u
#define HEAP_HI 0xE0000000u
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_heap_top = HEAP_LO;
#define NCLASS 24
static uint32_t g_free[NCLASS];   /* singly linked free lists by power-of-two class (block incl. header) */

static int size_class(uint32_t total) { int k = 5; while ((1u << k) < total) k++; return k; }

uint32_t galloc(uint32_t size) {
    uint32_t total = size + 16; int k = size_class(total);
    pthread_mutex_lock(&g_heap_lock);
    uint32_t blk;
    if (k < NCLASS && g_free[k]) { blk = g_free[k]; g_free[k] = RD32(blk + 4); }
    else {
        uint32_t bs = 1u << k;
        uint32_t align = bs >= 4096 ? 4096 : 16;
        g_heap_top = (g_heap_top + align - 1) & ~(align - 1);
        if (align == 4096) g_heap_top += 4096 - 16;   /* so that user pointer is page aligned */
        blk = g_heap_top; g_heap_top += bs;
        if (g_heap_top >= HEAP_HI) rt_fatal("guest heap exhausted");
    }
    WR32(blk, (uint32_t)k); WR32(blk + 4, 0); WR32(blk + 8, size); WR32(blk + 12, 0x48454150);
    pthread_mutex_unlock(&g_heap_lock);
    memset(M + blk + 16, 0, size);
    return blk + 16;
}
void gfree(uint32_t p) {
    if (!p) return;
    uint32_t blk = p - 16;
    if (RD32(blk + 12) != 0x48454150) { rt_log("WARN: gfree of non-heap pointer %08x", p); return; }
    int k = (int)RD32(blk);
    pthread_mutex_lock(&g_heap_lock);
    WR32(blk + 12, 0);
    if (k < NCLASS) { WR32(blk + 4, g_free[k]); g_free[k] = blk; }
    pthread_mutex_unlock(&g_heap_lock);
}
uint32_t gsize(uint32_t p) { return p ? RD32(p - 16 + 8) : 0; }
uint32_t grealloc(uint32_t p, uint32_t size) {
    if (!p) return galloc(size);
    uint32_t old = gsize(p);
    uint32_t n = galloc(size);
    memcpy(M + n, M + p, old < size ? old : size);
    gfree(p);
    return n;
}
uint32_t gvirtual_alloc(uint32_t addr, uint32_t size) {
    if (addr) return addr;   /* commit of a reserved range: already backed */
    return galloc(size);
}
uint32_t gstrdup(const char *s) { uint32_t n = (uint32_t)strlen(s) + 1; uint32_t p = galloc(n); memcpy(M + p, s, n); return p; }

/* ------------------------------------------------------------------ thunks */
#define MAX_THUNKS 4096
static GuestFn g_thunk_fn[MAX_THUNKS];
static const char *g_thunk_name[MAX_THUNKS];
static int g_nthunks;
static pthread_mutex_t g_thunk_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t thunk_register(GuestFn fn, const char *name) {
    pthread_mutex_lock(&g_thunk_lock);
    for (int i = 0; i < g_nthunks; i++) if (g_thunk_fn[i] == fn) { pthread_mutex_unlock(&g_thunk_lock); return THUNK_BASE + 16 * i; }
    int i = g_nthunks++;
    if (i >= MAX_THUNKS) rt_fatal("too many thunks");
    g_thunk_fn[i] = fn; g_thunk_name[i] = name;
    pthread_mutex_unlock(&g_thunk_lock);
    return THUNK_BASE + 16 * i;
}
uint32_t thunk_for_import(const char *dll, const char *name) {
    for (unsigned i = 0; i < g_imports_count; i++) {
        if (!strcasecmp(g_imports[i].name, name)) return thunk_register(g_imports[i].fn, g_imports[i].name);
    }
    return 0;
}

/* ------------------------------------------------------------------ dispatcher */
static GuestFn *g_code;   /* indexed by addr - CODE_LO */
static void build_code_table(void) {
    g_code = calloc(CODE_HI - CODE_LO, sizeof(GuestFn));
    for (unsigned i = 0; i < g_functable_count; i++) g_code[g_functable[i].addr - CODE_LO] = g_functable[i].fn;
}
void dispatch(CPU *c, uint32_t t) {
    if (t >= CODE_LO && t < CODE_HI) {
        GuestFn f = g_code[t - CODE_LO];
        if (f) { f(c); return; }
        rt_fatal("dispatch: no compiled entry at %08x (add to recomp/tools/extra_entries.txt and regenerate); return address %08x", t, RD32(c->esp));
    }
    if (t >= THUNK_BASE) {
        uint32_t i = (t - THUNK_BASE) / 16;
        if (i < (uint32_t)g_nthunks) {
            if (g_trace) rt_log("[thunk] %s ret=%08x", g_thunk_name[i], RD32(c->esp));
            g_thunk_fn[i](c); return;
        }
    }
    if (t >= 0x10000 && t < THUNK_BASE) {
        /* heap copy of a self-modifying routine? match fixed opcode bytes */
        static uint32_t cache_addr[64]; static int cache_idx[64];
        unsigned h = (t >> 4) & 63;
        int idx = -1;
        if (cache_addr[h] == t) idx = cache_idx[h];
        else {
            for (unsigned i = 0; i < g_smc_count && idx < 0; i++) {
                const SmcRoutine *r = &g_smc[i]; uint32_t n = r->end - r->start; int ok = 1;
                for (uint32_t k = 0; k < n; k++) if (r->mask[k] && M[t + k] != M[r->start + k]) { ok = 0; break; }
                if (ok) idx = (int)i;
            }
            if (idx >= 0) { cache_addr[h] = t; cache_idx[h] = idx; rt_log("SMC: heap code %08x = copy of %08x", t, g_smc[idx].start); }
        }
        if (idx >= 0) { c->smc_delta = t - g_smc[idx].start; g_smc[idx].fn(c); return; }
    }
    if (t >= 0x10000 && t < 0xF0000000u) {
        fprintf(stderr, "bytes at %08x:", t);
        for (int i = 0; i < 32; i++) fprintf(stderr, " %02x", M[t + i]);
        fprintf(stderr, "\n");
    }
    rt_fatal("dispatch to invalid address %08x; return address %08x", t, RD32(c->esp));
}

/* ------------------------------------------------------------------ threads / TIB */
static __thread CPU *t_cpu;
static int g_thread_counter;
CPU *cpu_current(void) { return t_cpu; }
CPU *cpu_new_thread_context(uint32_t stack_size) {
    CPU *c = calloc(1, sizeof(CPU));
    if (stack_size < 0x100000) stack_size = 0x100000;
    uint32_t stk = galloc(stack_size + 64);
    c->esp = (stk + stack_size) & ~15u;
    uint32_t tib = galloc(0x1000);
    uint32_t tls = galloc(64 * 4);
    WR32(tib + 0x00, 0xFFFFFFFFu);  /* SEH chain end */
    WR32(tib + 0x04, stk + stack_size);
    WR32(tib + 0x08, stk);
    WR32(tib + 0x18, tib);
    WR32(tib + 0x20, 0x1000);
    c->thread_index = __sync_add_and_fetch(&g_thread_counter, 1);
    WR32(tib + 0x24, 0x100 + 4 * c->thread_index);
    WR32(tib + 0x2c, tls);
    c->fs_base = tib;
    fpu_init(c);
    t_cpu = c;
    return c;
}
void cpu_bind_thread(CPU *c) { t_cpu = c; }
void call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args) {
    uint32_t saved = c->esp;
    for (int i = nargs - 1; i >= 0; i--) { c->esp -= 4; WR32(c->esp, args[i]); }
    c->esp -= 4; WR32(c->esp, 0xFFFEFFF0u);
    dispatch(c, fn);
    c->esp = saved;
}

/* ------------------------------------------------------------------ COM objects */
/* layout: [0]=vtbl ptr, [4]=host handle index, [8..] extra; vtable follows object */
#define MAX_HOSTOBJ 4096
static void *g_hostobj[MAX_HOSTOBJ];
static int g_nhostobj = 1;
uint32_t com_make_object(const GuestFn *methods, const char *const *names, int n, uint32_t extra, void *host) {
    uint32_t obj = galloc(16 + extra + 4 * n);
    uint32_t vt = obj + 16 + extra;
    for (int i = 0; i < n; i++) WR32(vt + 4 * i, thunk_register(methods[i], names ? names[i] : "com"));
    WR32(obj, vt);
    pthread_mutex_lock(&g_thunk_lock);
    int h = g_nhostobj++;
    if (h >= MAX_HOSTOBJ) rt_fatal("too many host objects");
    g_hostobj[h] = host;
    pthread_mutex_unlock(&g_thunk_lock);
    WR32(obj + 4, (uint32_t)h);
    return obj;
}
void *com_host(uint32_t obj) {
    uint32_t h = RD32(obj + 4);
    return (h && h < MAX_HOSTOBJ) ? g_hostobj[h] : NULL;
}

/* ------------------------------------------------------------------ paths */
static int resolve_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(dir[0] ? dir : ".");
    if (!d) return 0;
    struct dirent *e; int found = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s%s%s", dir, dir[0] ? "/" : "", e->d_name); found = 1; break; }
    }
    closedir(d);
    return found;
}
const char *host_path(const char *gp, char *buf, size_t n) {
    char tmp[1024]; size_t j = 0;
    const char *p = gp;
    if (isalpha((unsigned char)p[0]) && p[1] == ':') p += 2;   /* drop drive letter */
    for (; *p && j < sizeof(tmp) - 1; p++) tmp[j++] = (*p == '\\') ? '/' : *p;
    tmp[j] = 0;
    /* strip leading "./" and absolute game dir prefix if any */
    char *s = tmp;
    while (s[0] == '.' && s[1] == '/') s += 2;
    if (s[0] == '/') {
        /* absolute: if it starts with our fake cwd "/PHOBIA2" map to game dir */
        if (!strncasecmp(s, "/PHOBIA2", 8)) { s += 8; if (*s == '/') s++; }
        else { snprintf(buf, n, "%s", s); return buf; }
    }
    /* walk components case-insensitively, relative to game dir */
    char cur[1024]; snprintf(cur, sizeof cur, "%s", g_game_dir);
    char *save, *comp = strtok_r(s, "/", &save);
    while (comp) {
        char next[1024];
        if (!strcmp(comp, ".")) { comp = strtok_r(NULL, "/", &save); continue; }
        if (resolve_ci(cur, comp, next, sizeof next)) snprintf(cur, sizeof cur, "%s", next);
        else { size_t l = strlen(cur); snprintf(cur + l, sizeof cur - l, "/%s", comp); }
        comp = strtok_r(NULL, "/", &save);
    }
    snprintf(buf, n, "%s", cur);
    return buf;
}

/* ------------------------------------------------------------------ faults */
static void on_fault(int sig, siginfo_t *si, void *ctx) {
    uintptr_t a = (uintptr_t)si->si_addr;
    CPU *c = cpu_current();
    if (a >= (uintptr_t)M && a < (uintptr_t)M + 0x100000000ull)
        fprintf(stderr, "FATAL: guest memory fault at guest address %08x (signal %d)\n", (uint32_t)(a - (uintptr_t)M), sig);
    else
        fprintf(stderr, "FATAL: host fault at %p (signal %d)\n", (void *)a, sig);
    dump_cpu(c);
    host_backtrace();
    _exit(2);
}

void mem_init(void) {
    void *p = mmap(NULL, 0x100000000ull, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) { perror("mmap 4GB"); exit(1); }
    M = p;
    mprotect(M, 0x10000, PROT_NONE);   /* catch guest NULL dereferences */
    struct sigaction sa = {0};
    sa.sa_sigaction = on_fault; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
}

/* load image + patch IAT */
extern const unsigned char g_image[];
extern const unsigned g_image_size;
void image_load(void) {
    memcpy(M + IMAGE_BASE, g_image, g_image_size);
    for (unsigned i = 0; i < g_imports_count; i++)
        WR32(g_imports[i].slot, thunk_register(g_imports[i].fn, g_imports[i].name));
    build_code_table();
}

#ifdef RECOMP_TRACE
uint32_t g_tr_lo = 0xFFFFFFFFu, g_tr_hi = 0;
__attribute__((constructor)) static void tr_init(void) {
    const char *r = getenv("PHOBIA_TR");   /* "lo-hi" hex */
    if (r) sscanf(r, "%x-%x", &g_tr_lo, &g_tr_hi);
}
void rt_trace(CPU *c, uint32_t a) {
    fprintf(stderr, "%08x eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x c%d z%d s%d o%d\n",
            a, c->eax, c->ecx, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi, c->cf, c->zf, c->sf, c->of);
    fprintf(stderr, "    st0=%g st1=%g st2=%g st3=%g ftop=%d\n", ST(0), ST(1), ST(2), ST(3), c->ftop);
    static int nbt;
    if (getenv("PHOBIA_TR_BT") && nbt++ < 2) host_backtrace();
}
#endif
void rt_trace_imp(CPU *c, const char *name) {
    rt_log("[imp] %s(%08x, %08x, %08x, %08x) ret=%08x", name, RD32(c->esp + 4), RD32(c->esp + 8), RD32(c->esp + 12), RD32(c->esp + 16), RD32(c->esp));
}
