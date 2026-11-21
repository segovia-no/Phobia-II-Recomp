// Finding the translated game code: linked in (static build) or loaded from gamecode.so / gamecode.dylib.
//
// Module builds (PHOBIA_GAME_MODULE): the program contains no code translated from Game.exe. The module,
// built on the player's machine by recompiler/build_gamecode.py, exports one PhobiaModule descriptor; its
// own undefined symbols (M, dispatch, rt_*, fpu_*) resolve against this program, which exports them.
// Import shims go the other way: the module calls IMP_<DLL>_<Name> thunks that jump through
// import_slots[], filled here by looking the shims up by name in this program.
#include "runtime.h"
#include <string.h>
#include <SDL.h>

const PhobiaModule *G;

#ifndef PHOBIA_GAME_MODULE
extern const PhobiaModule phobia_module;
const PhobiaModule *module_load(void) {
    G = &phobia_module;
    return G;
}
#else
#include <dlfcn.h>
#include <sys/stat.h>

#ifdef __APPLE__
#define MODULE_FILE "gamecode.dylib"
#else
#define MODULE_FILE "gamecode.so"
#endif

static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* The module path: $PHOBIA_GAME_MODULE, next to the program, or in the data folder's gamecode/ cache. */
static const char *module_path(char *buf, size_t n) {
    const char *env = getenv("PHOBIA_GAME_MODULE");
    if (env && file_exists(env)) { snprintf(buf, n, "%s", env); return buf; }
    char *base = SDL_GetBasePath();
    if (base) {
        snprintf(buf, n, "%s%s", base, MODULE_FILE);
        if (file_exists(buf)) { SDL_free(base); return buf; }
        snprintf(buf, n, "%sgamecode/%s", base, MODULE_FILE);
        if (file_exists(buf)) { SDL_free(base); return buf; }
        SDL_free(base);
    }
    snprintf(buf, n, "%s/gamecode/%s", g_game_dir, MODULE_FILE);
    if (file_exists(buf)) return buf;
    return NULL;
}

const PhobiaModule *module_load(void) {
    char path[2048];
    if (!module_path(path, sizeof path))
        rt_fatal("game code module %s not found. Build it from your Game.exe with "
                 "recompiler/build_gamecode.py (or set PHOBIA_GAME_MODULE).", MODULE_FILE);
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) rt_fatal("cannot load %s: %s", path, dlerror());
    const PhobiaModule *m = dlsym(h, "phobia_module");
    if (!m) rt_fatal("%s has no phobia_module descriptor", path);
    if (m->version != PHOBIA_MODULE_VERSION)
        rt_fatal("%s is version %u, this program needs %u: rebuild it with recompiler/build_gamecode.py",
                 path, m->version, PHOBIA_MODULE_VERSION);
    /* Fill import slots with this program's shims. The module's own thunks are hidden, so a lookup in
       the main program can only find the real shim; a missing one stays NULL (the thunk reports it). */
    void *self = dlopen(NULL, RTLD_NOW);
    unsigned found = 0;
    for (unsigned i = 0; i < m->imports_count; i++) {
        char sym[256];
        snprintf(sym, sizeof sym, "IMP_%s_%s", m->imports[i].dll, m->imports[i].name);
        GuestFn f = (GuestFn)dlsym(self, sym);
        m->import_slots[i] = f;
        if (f) found++;
    }
    rt_log("game code: %s (%u functions, %u of %u import shims, %s)", path, m->functable_count, found,
           m->imports_count, m->translator_id ? m->translator_id : "?");
    G = m;
    return G;
}
#endif
