// Host entry: set up SDL, guest memory, load the recompiled image and run the original entry point.
#include "runtime.h"
#include "win32.h"
#include <SDL.h>
#include <unistd.h>
#include <libgen.h>
#include <sys/stat.h>

void image_load(void);
extern void cpu_bind_thread(CPU *c);

static int dir_has_game(const char *d) {
    char p[HOST_PATH_MAX + 64]; struct stat st;
    snprintf(p, sizeof p, "%s/pics", d);
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}
static void find_game_dir(void) {
    const char *env = getenv("PHOBIA_DATA");
    if (env && dir_has_game(env)) { snprintf(g_game_dir, sizeof g_game_dir, "%s", env); return; }
    char *base = SDL_GetBasePath();   /* folder of the program, portable (macOS, Linux) */
    if (base) {
        char d[HOST_PATH_MAX]; snprintf(d, sizeof d, "%s", base); SDL_free(base);
        size_t l = strlen(d); if (l > 1 && d[l - 1] == '/') d[l - 1] = 0;
        const char *cands[] = { "%s", "%s/game", "%s/../Resources/game", "%s/../game", "%s/../../game" };
        for (unsigned i = 0; i < sizeof cands / sizeof cands[0]; i++) {
            char p[HOST_PATH_MAX + 64]; snprintf(p, sizeof p, cands[i], d);
            char r[HOST_PATH_MAX];
            if (realpath(p, r) && dir_has_game(r)) { snprintf(g_game_dir, sizeof g_game_dir, "%s", r); return; }
        }
    }
    if (dir_has_game(".")) { getcwd(g_game_dir, sizeof g_game_dir); return; }
    if (dir_has_game("game")) { getcwd(g_game_dir, sizeof g_game_dir); strcat(g_game_dir, "/game"); return; }
    fprintf(stderr, "Cannot find game data directory (with pics/, sounds/, dat/). Set PHOBIA_DATA.\n");
    exit(1);
}

int main(int argc, char **argv) {
    if (getenv("PHOBIA_TRACE")) g_trace = 1;
    find_game_dir();
    chdir(g_game_dir);
    rt_log("Phobia II static recompilation — data: %s", g_game_dir);
    SDL_SetHint(SDL_HINT_MAC_CTRL_CLICK_EMULATE_RIGHT_CLICK, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    module_load();
    mem_init();
    image_load();
    kernel32_init();
    user32_init();
    CPU *c = cpu_new_thread_context(G->stack_reserve > 0x400000 ? G->stack_reserve : 0x400000);
    cpu_bind_thread(c);
    /* Win32 process entry: return address on stack goes to ExitThread-ish marker */
    c->esp -= 4; WR32(c->esp, 0xFFFEFFF0u);
    dispatch(c, G->entry_point);
    rt_log("entry point returned eax=%u", c->eax);
    SDL_Quit();
    return (int)c->eax;
}
