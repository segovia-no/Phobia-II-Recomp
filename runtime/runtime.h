#pragma once
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

typedef void (*GuestFn)(CPU *c);
typedef struct { uint32_t addr; GuestFn fn; } FuncEntry;
typedef struct { uint32_t slot; const char *dll; const char *name; GuestFn fn; } ImportEntry;

typedef struct { uint32_t start, end; const unsigned char *mask; GuestFn fn; } SmcRoutine;

/* Everything the runtime needs from the translated game code, in one exported descriptor.
   Static builds link it (phobia_module in the generated module.c); module builds load it from
   gamecode.so / gamecode.dylib (runtime/module.c). Bump the version when the layout changes. */
#define PHOBIA_MODULE_VERSION 1
typedef struct {
    uint32_t version;
    uint32_t image_base, image_size, entry_point, code_lo, code_hi, stack_reserve;
    const unsigned char *image;
    const FuncEntry *functable; unsigned functable_count;
    const ImportEntry *imports; unsigned imports_count;
    const SmcRoutine *smc; unsigned smc_count;
    GuestFn *import_slots;          /* module builds: the host fills these with its shims (NULL: static) */
    const char *translator_id;      /* identifies recompiler + Game.exe, for cache checks */
} PhobiaModule;
extern const PhobiaModule *G;       /* set by module_load() */
const PhobiaModule *module_load(void);

/* magic host-thunk address space */
#define THUNK_BASE 0xFFF00000u
uint32_t thunk_register(GuestFn fn, const char *name);
uint32_t thunk_for_import(const char *dll, const char *name);   /* GetProcAddress */

/* guest memory */
void mem_init(void);
uint32_t galloc(uint32_t size);            /* zeroed, 16-aligned */
void gfree(uint32_t addr);
uint32_t gsize(uint32_t addr);
uint32_t grealloc(uint32_t addr, uint32_t size);
uint32_t gvirtual_alloc(uint32_t addr, uint32_t size);
static inline void *H(uint32_t a) { return a ? (void *)(M + a) : NULL; }
static inline const char *HS(uint32_t a) { return a ? (const char *)(M + a) : NULL; }
uint32_t gstrdup(const char *s);

/* threads */
CPU *cpu_new_thread_context(uint32_t stack_size);
CPU *cpu_current(void);
void call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);  /* stdcall/cdecl agnostic: restores esp */

/* misc */
void rt_missing_import(CPU *c, const char *dll, const char *name) __attribute__((noreturn));
void rt_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int g_trace;
const char *host_path(const char *guest_path, char *buf, size_t n);  /* map DOS path -> host */
#define HOST_PATH_MAX 4096   /* glibc realpath() and fortified builds expect PATH_MAX buffers */
extern char g_game_dir[HOST_PATH_MAX];

/* COM helper: build object with vtable of host thunks; returns guest pointer to object */
uint32_t com_make_object(const GuestFn *methods, const char *const *names, int nmethods, uint32_t extra_bytes, void *host);
void *com_host(uint32_t obj);   /* host pointer stored in object */
