#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct CPU {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint8_t cf, zf, sf, of, pf, af, df, _pad;
    uint32_t fs_base, gs_base;
    /* x87 */
    double st[8];
    int ftop;
    uint16_t fcw, fsw;
    /* debugging */
    uint32_t retaddr;
    uint32_t smc_delta;   /* heap copy address - original address for self-modifying routines */
    int thread_index;
} CPU;

/* Flat 32-bit guest address space, host base pointer. */
extern uint8_t *M;

static inline uint8_t  RD8 (uint32_t a) { return M[a]; }
static inline uint16_t RD16(uint32_t a) { uint16_t v; memcpy(&v, M + a, 2); return v; }
static inline uint32_t RD32(uint32_t a) { uint32_t v; memcpy(&v, M + a, 4); return v; }
static inline uint64_t RD64(uint32_t a) { uint64_t v; memcpy(&v, M + a, 8); return v; }
static inline void WR8 (uint32_t a, uint8_t v)  { M[a] = v; }
static inline void WR16(uint32_t a, uint16_t v) { memcpy(M + a, &v, 2); }
static inline void WR32(uint32_t a, uint32_t v) { memcpy(M + a, &v, 4); }
static inline void WR64(uint32_t a, uint64_t v) { memcpy(M + a, &v, 8); }
static inline float  RDF32(uint32_t a) { float v;  memcpy(&v, M + a, 4); return v; }
static inline double RDF64(uint32_t a) { double v; memcpy(&v, M + a, 8); return v; }
static inline void WRF32(uint32_t a, double v) { float f = (float)v; memcpy(M + a, &f, 4); }
static inline void WRF64(uint32_t a, double v) { memcpy(M + a, &v, 8); }
double RDF80(uint32_t a);
void WRF80(uint32_t a, double v);

#define PARITY(x) ((uint8_t)(!__builtin_parity((uint32_t)(uint8_t)(x))))

/* x87 stack */
#define ST(i) (c->st[(c->ftop + (i)) & 7])
#define FPUSH(v) do { double v__ = (v); c->ftop = (c->ftop - 1) & 7; c->st[c->ftop] = v__; } while (0)
#define FPOP() do { c->ftop = (c->ftop + 1) & 7; } while (0)

void fpu_compare(CPU *c, double a, double b);
uint16_t fpu_status(CPU *c);
double fpu_round(CPU *c, double v);
int16_t fpu_to_int16(CPU *c, double v);
int32_t fpu_to_int32(CPU *c, double v);
int64_t fpu_to_int64(CPU *c, double v);
void fpu_init(CPU *c);
void fpu_fxam(CPU *c);
void fpu_save(CPU *c, uint32_t a);
void fpu_restore(CPU *c, uint32_t a);

/* runtime services used by generated code */
void dispatch(CPU *c, uint32_t target);
void rt_bad(CPU *c, uint32_t addr) __attribute__((noreturn));
void rt_unimpl(CPU *c, uint32_t addr, const char *what) __attribute__((noreturn));
void rt_trap(CPU *c, uint32_t addr, const char *what) __attribute__((noreturn));
void rt_int(CPU *c, uint32_t addr, int n);
void rt_divzero(CPU *c, uint32_t addr) __attribute__((noreturn));
void rt_cpuid(CPU *c);
uint32_t rt_getflags(CPU *c);
void rt_setflags(CPU *c, uint32_t f);
void rt_movs(CPU *c, int sz);
void rt_stos(CPU *c, int sz);
void rt_retmismatch(CPU *c, uint32_t expected);

#ifdef RECOMP_CHECK
#define RET_HOOK(c) ((c)->retaddr = RD32((c)->esp))
#define CHECK_RET(c, exp) do { if ((c)->retaddr != (exp)) rt_retmismatch(c, exp); } while (0)
#else
#define RET_HOOK(c) ((void)0)
#define CHECK_RET(c, exp) ((void)0)
#endif

/* stdcall helpers for host shims */
#define ARG(n) RD32(c->esp + 4 + 4 * (n))
#define STDRET(nargs, val) do { uint32_t v___ = (uint32_t)(val); RET_HOOK(c); c->eax = v___; c->esp += 4 + 4 * (nargs); } while (0)

#ifdef RECOMP_TRACE
void rt_trace(CPU *c, uint32_t addr);
extern uint32_t g_tr_lo, g_tr_hi;
#define TR(a) do { if ((a) >= g_tr_lo && (a) < g_tr_hi) rt_trace(c, (a)); } while (0)
#else
#define TR(a) ((void)0)
#endif
extern int g_trace;
void rt_trace_imp(CPU *c, const char *name);
#define TRACE_IMP(n) do { if (g_trace) rt_trace_imp(c, n); } while (0)
