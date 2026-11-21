// USER32 / GDI32 shims and SDL-backed window, message queue, video presentation.
#include "runtime.h"
#include "win32.h"
#include <SDL.h>
#include <pthread.h>
#include <ctype.h>

#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202

uint32_t g_hwnd_main, g_wndproc_main;
static uint32_t g_class_wndproc;
static SDL_Window *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture *g_tex;
static pthread_t g_main_thread;
static int g_quit_posted;

/* ------------------------------------------------------------------ message queue */
typedef struct { uint32_t hwnd, msg, wp, lp, time; } Msg;
#define QCAP 1024
static Msg g_q[QCAP];
static int g_qh, g_qt;
static pthread_mutex_t g_qlock = PTHREAD_MUTEX_INITIALIZER;

void win32_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    pthread_mutex_lock(&g_qlock);
    int n = (g_qt + 1) % QCAP;
    if (n != g_qh) { g_q[g_qt] = (Msg){hwnd, msg, wp, lp, host_ticks()}; g_qt = n; }
    pthread_mutex_unlock(&g_qlock);
}
static int queue_pop(Msg *m, int remove) {
    pthread_mutex_lock(&g_qlock);
    if (g_qh == g_qt) { pthread_mutex_unlock(&g_qlock); return 0; }
    *m = g_q[g_qh];
    if (remove) g_qh = (g_qh + 1) % QCAP;
    pthread_mutex_unlock(&g_qlock);
    return 1;
}

/* ------------------------------------------------------------------ input mapping */
uint8_t g_dik_state[256];
int sdl_to_dik(int sc) {
    switch (sc) {
        case SDL_SCANCODE_ESCAPE: return 0x01;
        case SDL_SCANCODE_1: return 0x02; case SDL_SCANCODE_2: return 0x03; case SDL_SCANCODE_3: return 0x04;
        case SDL_SCANCODE_4: return 0x05; case SDL_SCANCODE_5: return 0x06; case SDL_SCANCODE_6: return 0x07;
        case SDL_SCANCODE_7: return 0x08; case SDL_SCANCODE_8: return 0x09; case SDL_SCANCODE_9: return 0x0A;
        case SDL_SCANCODE_0: return 0x0B; case SDL_SCANCODE_MINUS: return 0x0C; case SDL_SCANCODE_EQUALS: return 0x0D;
        case SDL_SCANCODE_BACKSPACE: return 0x0E; case SDL_SCANCODE_TAB: return 0x0F;
        case SDL_SCANCODE_Q: return 0x10; case SDL_SCANCODE_W: return 0x11; case SDL_SCANCODE_E: return 0x12;
        case SDL_SCANCODE_R: return 0x13; case SDL_SCANCODE_T: return 0x14; case SDL_SCANCODE_Y: return 0x15;
        case SDL_SCANCODE_U: return 0x16; case SDL_SCANCODE_I: return 0x17; case SDL_SCANCODE_O: return 0x18;
        case SDL_SCANCODE_P: return 0x19; case SDL_SCANCODE_LEFTBRACKET: return 0x1A; case SDL_SCANCODE_RIGHTBRACKET: return 0x1B;
        case SDL_SCANCODE_RETURN: return 0x1C; case SDL_SCANCODE_LCTRL: return 0x1D;
        case SDL_SCANCODE_A: return 0x1E; case SDL_SCANCODE_S: return 0x1F; case SDL_SCANCODE_D: return 0x20;
        case SDL_SCANCODE_F: return 0x21; case SDL_SCANCODE_G: return 0x22; case SDL_SCANCODE_H: return 0x23;
        case SDL_SCANCODE_J: return 0x24; case SDL_SCANCODE_K: return 0x25; case SDL_SCANCODE_L: return 0x26;
        case SDL_SCANCODE_SEMICOLON: return 0x27; case SDL_SCANCODE_APOSTROPHE: return 0x28; case SDL_SCANCODE_GRAVE: return 0x29;
        case SDL_SCANCODE_LSHIFT: return 0x2A; case SDL_SCANCODE_BACKSLASH: return 0x2B;
        case SDL_SCANCODE_Z: return 0x2C; case SDL_SCANCODE_X: return 0x2D; case SDL_SCANCODE_C: return 0x2E;
        case SDL_SCANCODE_V: return 0x2F; case SDL_SCANCODE_B: return 0x30; case SDL_SCANCODE_N: return 0x31;
        case SDL_SCANCODE_M: return 0x32; case SDL_SCANCODE_COMMA: return 0x33; case SDL_SCANCODE_PERIOD: return 0x34;
        case SDL_SCANCODE_SLASH: return 0x35; case SDL_SCANCODE_RSHIFT: return 0x36; case SDL_SCANCODE_KP_MULTIPLY: return 0x37;
        case SDL_SCANCODE_LALT: return 0x38; case SDL_SCANCODE_SPACE: return 0x39; case SDL_SCANCODE_CAPSLOCK: return 0x3A;
        case SDL_SCANCODE_F1: return 0x3B; case SDL_SCANCODE_F2: return 0x3C; case SDL_SCANCODE_F3: return 0x3D;
        case SDL_SCANCODE_F4: return 0x3E; case SDL_SCANCODE_F5: return 0x3F; case SDL_SCANCODE_F6: return 0x40;
        case SDL_SCANCODE_F7: return 0x41; case SDL_SCANCODE_F8: return 0x42; case SDL_SCANCODE_F9: return 0x43;
        case SDL_SCANCODE_F10: return 0x44; case SDL_SCANCODE_NUMLOCKCLEAR: return 0x45; case SDL_SCANCODE_SCROLLLOCK: return 0x46;
        case SDL_SCANCODE_KP_7: return 0x47; case SDL_SCANCODE_KP_8: return 0x48; case SDL_SCANCODE_KP_9: return 0x49;
        case SDL_SCANCODE_KP_MINUS: return 0x4A; case SDL_SCANCODE_KP_4: return 0x4B; case SDL_SCANCODE_KP_5: return 0x4C;
        case SDL_SCANCODE_KP_6: return 0x4D; case SDL_SCANCODE_KP_PLUS: return 0x4E; case SDL_SCANCODE_KP_1: return 0x4F;
        case SDL_SCANCODE_KP_2: return 0x50; case SDL_SCANCODE_KP_3: return 0x51; case SDL_SCANCODE_KP_0: return 0x52;
        case SDL_SCANCODE_KP_PERIOD: return 0x53; case SDL_SCANCODE_F11: return 0x57; case SDL_SCANCODE_F12: return 0x58;
        case SDL_SCANCODE_KP_ENTER: return 0x9C; case SDL_SCANCODE_RCTRL: return 0x9D; case SDL_SCANCODE_KP_DIVIDE: return 0xB5;
        case SDL_SCANCODE_RALT: return 0xB8; case SDL_SCANCODE_HOME: return 0xC7; case SDL_SCANCODE_UP: return 0xC8;
        case SDL_SCANCODE_PAGEUP: return 0xC9; case SDL_SCANCODE_LEFT: return 0xCB; case SDL_SCANCODE_RIGHT: return 0xCD;
        case SDL_SCANCODE_END: return 0xCF; case SDL_SCANCODE_DOWN: return 0xD0; case SDL_SCANCODE_PAGEDOWN: return 0xD1;
        case SDL_SCANCODE_INSERT: return 0xD2; case SDL_SCANCODE_DELETE: return 0xD3;
        default: return 0;
    }
}
int sdl_to_vk(int sym, int sc) {
    if (sym >= 'a' && sym <= 'z') return sym - 32;
    if (sym >= '0' && sym <= '9') return sym;
    switch (sc) {
        case SDL_SCANCODE_ESCAPE: return 0x1B; case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 0x0D;
        case SDL_SCANCODE_SPACE: return 0x20; case SDL_SCANCODE_BACKSPACE: return 0x08; case SDL_SCANCODE_TAB: return 0x09;
        case SDL_SCANCODE_LEFT: return 0x25; case SDL_SCANCODE_UP: return 0x26; case SDL_SCANCODE_RIGHT: return 0x27; case SDL_SCANCODE_DOWN: return 0x28;
        case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return 0x10;
        case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return 0x11;
        case SDL_SCANCODE_LALT: case SDL_SCANCODE_RALT: return 0x12;
        case SDL_SCANCODE_PAGEUP: return 0x21; case SDL_SCANCODE_PAGEDOWN: return 0x22; case SDL_SCANCODE_END: return 0x23; case SDL_SCANCODE_HOME: return 0x24;
        case SDL_SCANCODE_INSERT: return 0x2D; case SDL_SCANCODE_DELETE: return 0x2E;
        case SDL_SCANCODE_KP_0: return 0x60; case SDL_SCANCODE_KP_1: return 0x61; case SDL_SCANCODE_KP_2: return 0x62;
        case SDL_SCANCODE_KP_3: return 0x63; case SDL_SCANCODE_KP_4: return 0x64; case SDL_SCANCODE_KP_5: return 0x65;
        case SDL_SCANCODE_KP_6: return 0x66; case SDL_SCANCODE_KP_7: return 0x67; case SDL_SCANCODE_KP_8: return 0x68; case SDL_SCANCODE_KP_9: return 0x69;
        case SDL_SCANCODE_F1: return 0x70; case SDL_SCANCODE_F2: return 0x71; case SDL_SCANCODE_F3: return 0x72; case SDL_SCANCODE_F4: return 0x73;
        case SDL_SCANCODE_F5: return 0x74; case SDL_SCANCODE_F6: return 0x75; case SDL_SCANCODE_F7: return 0x76; case SDL_SCANCODE_F8: return 0x77;
        case SDL_SCANCODE_F9: return 0x78; case SDL_SCANCODE_F10: return 0x79; case SDL_SCANCODE_F11: return 0x7A; case SDL_SCANCODE_F12: return 0x7B;
        case SDL_SCANCODE_MINUS: return 0xBD; case SDL_SCANCODE_EQUALS: return 0xBB; case SDL_SCANCODE_COMMA: return 0xBC; case SDL_SCANCODE_PERIOD: return 0xBE;
        default: return 0;
    }
}
static uint8_t g_vk_down[256];

/* ------------------------------------------------------------------ SDL pump */
extern void joystick_handle_event(SDL_Event *e);
static void toggle_fullscreen(void) {
    Uint32 f = SDL_GetWindowFlags(g_win);
    SDL_SetWindowFullscreen(g_win, (f & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
}
/* test automation: PHOBIA_KEYS="ms:KEY[:hold_ms],..." injects key presses; PHOBIA_SHOTS="ms:file.bmp,..." dumps frames */
typedef struct { uint32_t t; int sc; uint32_t hold; int state; } AutoKey;
static AutoKey g_auto[256]; static int g_nauto = -1;
typedef struct { uint32_t t; char path[512]; int done; } AutoShot;
static AutoShot g_shots[64]; static int g_nshots = -1;
static void auto_init(void) {
    g_nauto = 0; g_nshots = 0;
    const char *k = getenv("PHOBIA_KEYS");
    if (k) {
        char buf[4096]; snprintf(buf, sizeof buf, "%s", k);
        char *save, *tok = strtok_r(buf, ",", &save);
        while (tok && g_nauto < 256) {
            char name[64] = {0}; unsigned t = 0, hold = 80;
            if (sscanf(tok, "%u:%63[^:]:%u", &t, name, &hold) >= 2) {
                SDL_Scancode sc = SDL_GetScancodeFromName(name);
                if (sc != SDL_SCANCODE_UNKNOWN) g_auto[g_nauto++] = (AutoKey){t, sc, hold, 0};
                else rt_log("PHOBIA_KEYS: unknown key %s", name);
            }
            tok = strtok_r(NULL, ",", &save);
        }
    }
    const char *s = getenv("PHOBIA_SHOTS");
    if (s) {
        char buf[4096]; snprintf(buf, sizeof buf, "%s", s);
        char *save, *tok = strtok_r(buf, ",", &save);
        while (tok && g_nshots < 64) {
            unsigned t; char path[512];
            if (sscanf(tok, "%u:%511s", &t, path) == 2) { g_shots[g_nshots].t = t; snprintf(g_shots[g_nshots].path, 512, "%s", path); g_nshots++; }
            tok = strtok_r(NULL, ",", &save);
        }
    }
}
static uint32_t g_start_ticks;
static void auto_step(void) {
    if (g_nauto < 0) auto_init();
    uint32_t now = SDL_GetTicks() - g_start_ticks;
    for (int i = 0; i < g_nauto; i++) {
        AutoKey *a = &g_auto[i];
        SDL_Event e = {0};
        if (a->state == 0 && now >= a->t) { e.type = SDL_KEYDOWN; a->state = 1; }
        else if (a->state == 1 && now >= a->t + a->hold) { e.type = SDL_KEYUP; a->state = 2; }
        else continue;
        e.key.keysym.scancode = a->sc; e.key.keysym.sym = SDL_GetKeyFromScancode(a->sc);
        e.key.state = e.type == SDL_KEYDOWN ? SDL_PRESSED : SDL_RELEASED;
        SDL_PushEvent(&e);
    }
}
static void pump_sdl(void) {
    SDL_Event e;
    auto_step();
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                if (g_hwnd_main) win32_post_message(g_hwnd_main, WM_CLOSE, 0, 0);
                else exit(0);
                break;
            case SDL_KEYDOWN: case SDL_KEYUP: {
                int down = e.type == SDL_KEYDOWN;
                int sc = e.key.keysym.scancode;
                if (down && sc == SDL_SCANCODE_RETURN && (e.key.keysym.mod & (KMOD_ALT | KMOD_GUI))) { toggle_fullscreen(); break; }
                if (down && sc == SDL_SCANCODE_F && (e.key.keysym.mod & KMOD_GUI) && (e.key.keysym.mod & KMOD_CTRL)) { toggle_fullscreen(); break; }
                int dik = sdl_to_dik(sc);
                if (dik) g_dik_state[dik] = down ? 0x80 : 0;
                int vk = sdl_to_vk(e.key.keysym.sym, sc);
                if (vk) {
                    uint32_t lp = 1 | ((uint32_t)(dik & 0x7f) << 16) | ((dik & 0x80) ? (1u << 24) : 0);
                    if (!down) lp |= 0xC0000000u;
                    else if (e.key.repeat) lp |= 0x40000000u;
                    g_vk_down[vk] = down;
                    if (g_hwnd_main) win32_post_message(g_hwnd_main, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
                }
                break;
            }
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) memset(g_dik_state, 0, sizeof g_dik_state);
                break;
            default:
                joystick_handle_event(&e);
                break;
        }
    }
}
void win32_pump_if_main(void) {
    if (pthread_equal(pthread_self(), g_main_thread)) pump_sdl();
}

/* ------------------------------------------------------------------ video */
static uint32_t *g_rgba;
static int g_tex_w, g_tex_h;
static pthread_mutex_t g_video_lock = PTHREAD_MUTEX_INITIALIZER;
void video_init(void) {
    if (g_win) return;
    g_main_thread = pthread_self();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    g_win = SDL_CreateWindow("Phobia II", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_win) rt_fatal("SDL_CreateWindow: %s", SDL_GetError());
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, 0);
    SDL_RenderSetLogicalSize(g_ren, 640, 480);
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren); SDL_RenderPresent(g_ren);
    if (getenv("PHOBIA_FULLSCREEN")) toggle_fullscreen();
}
void video_present(const uint8_t *px, int w, int h, int bpp, int pitch, const uint32_t *pal) {
    if (!pthread_equal(pthread_self(), g_main_thread)) {
        static int warned; if (!warned++) rt_log("WARN: present from non-main thread ignored");
        return;
    }
    pthread_mutex_lock(&g_video_lock);
    if (!g_tex || g_tex_w != w || g_tex_h != h) {
        if (g_tex) SDL_DestroyTexture(g_tex);
        g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        g_rgba = realloc(g_rgba, (size_t)w * h * 4);
        g_tex_w = w; g_tex_h = h;
        SDL_RenderSetLogicalSize(g_ren, w, h);
    }
    for (int y = 0; y < h; y++) {
        const uint8_t *s = px + (size_t)y * pitch;
        uint32_t *d = g_rgba + (size_t)y * w;
        if (bpp == 8) for (int x = 0; x < w; x++) d[x] = pal[s[x]];
        else if (bpp == 16) for (int x = 0; x < w; x++) {
            uint16_t v = (uint16_t)(s[2 * x] | (s[2 * x + 1] << 8));
            uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
            d[x] = 0xFF000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        }
        else if (bpp == 32) memcpy(d, s, (size_t)w * 4);
    }
    if (g_nshots > 0) {
        uint32_t now = SDL_GetTicks() - g_start_ticks;
        for (int i = 0; i < g_nshots; i++) if (!g_shots[i].done && now >= g_shots[i].t) {
            g_shots[i].done = 1;
            SDL_Surface *ss = SDL_CreateRGBSurfaceWithFormatFrom(g_rgba, w, h, 32, w * 4, SDL_PIXELFORMAT_ARGB8888);
            SDL_SaveBMP(ss, g_shots[i].path); SDL_FreeSurface(ss);
            rt_log("saved frame %s", g_shots[i].path);
        }
    }
    SDL_UpdateTexture(g_tex, NULL, g_rgba, w * 4);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, NULL);
    SDL_RenderPresent(g_ren);
    pthread_mutex_unlock(&g_video_lock);
    pump_sdl();
}

/* ------------------------------------------------------------------ USER32 */
static void send_message(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!g_wndproc_main) return;
    uint32_t args[4] = { hwnd, msg, wp, lp };
    uint32_t sv_eax = c->eax, sv_ecx = c->ecx, sv_edx = c->edx;
    call_guest(c, g_wndproc_main, 4, args);
    uint32_t r = c->eax;
    c->eax = sv_eax; c->ecx = sv_ecx; c->edx = sv_edx;
    (void)r;
}
static uint32_t send_message_r(CPU *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!g_wndproc_main) return 0;
    uint32_t args[4] = { hwnd, msg, wp, lp };
    call_guest(c, g_wndproc_main, 4, args);
    return c->eax;
}
void IMP_USER32_RegisterClassExA(CPU *c) {
    uint32_t wc = ARG(0);
    g_class_wndproc = RD32(wc + 8);
    rt_log("RegisterClassExA(%s) wndproc=%08x", HS(RD32(wc + 40)) ? HS(RD32(wc + 40)) : "?", g_class_wndproc);
    STDRET(1, 0xC001);
}
void IMP_USER32_CreateWindowExA(CPU *c) {
    rt_log("CreateWindowExA(class=%s, title=%s, %dx%d)", HS(ARG(1)) ? HS(ARG(1)) : "", HS(ARG(2)) ? HS(ARG(2)) : "", ARG(6), ARG(7));
    video_init();
    if (HS(ARG(2))) SDL_SetWindowTitle(g_win, HS(ARG(2)));
    g_hwnd_main = 0x00010010u;
    g_wndproc_main = g_class_wndproc;
    /* CREATESTRUCT for WM_CREATE */
    uint32_t cs = galloc(48);
    WR32(cs + 0, ARG(11)); WR32(cs + 4, ARG(10)); WR32(cs + 8, ARG(9)); WR32(cs + 12, ARG(8));
    WR32(cs + 16, ARG(7)); WR32(cs + 20, ARG(6)); WR32(cs + 24, ARG(5)); WR32(cs + 28, ARG(4));
    WR32(cs + 32, ARG(3)); WR32(cs + 36, ARG(2)); WR32(cs + 40, ARG(1)); WR32(cs + 44, ARG(0));
    uint32_t sp = c->esp;
    uint32_t hwnd = g_hwnd_main;
    send_message(c, hwnd, WM_CREATE, 0, cs);
    c->esp = sp;
    STDRET(12, hwnd);
}
void IMP_USER32_ShowWindow(CPU *c) {
    win32_post_message(g_hwnd_main, WM_ACTIVATEAPP, 1, 0);
    win32_post_message(g_hwnd_main, WM_ACTIVATE, 1, 0);
    win32_post_message(g_hwnd_main, WM_SETFOCUS, 0, 0);
    STDRET(2, 0);
}
void IMP_USER32_UpdateWindow(CPU *c) { STDRET(1, 1); }
void IMP_USER32_DestroyWindow(CPU *c) {
    uint32_t h = ARG(0);
    static int destroyed;
    if (!destroyed++) { uint32_t sp = c->esp; send_message(c, h, WM_DESTROY, 0, 0); c->esp = sp; }
    STDRET(1, 1);
}
void IMP_USER32_DefWindowProcA(CPU *c) {
    uint32_t h = ARG(0), msg = ARG(1);
    if (msg == WM_CLOSE) { uint32_t sp = c->esp; uint32_t a[1] = { h }; (void)a; c->esp = sp; win32_post_message(h, 0x7FFF, 0, 0); }
    STDRET(4, 0);
}
void IMP_USER32_PostQuitMessage(CPU *c) { win32_post_message(0, WM_QUIT, ARG(0), 0); g_quit_posted = 1; STDRET(1, 0); }
static void write_msg(uint32_t p, Msg *m) {
    WR32(p, m->hwnd); WR32(p + 4, m->msg); WR32(p + 8, m->wp); WR32(p + 12, m->lp); WR32(p + 16, m->time);
    WR32(p + 20, 320); WR32(p + 24, 240);
}
/* internal message 0x7FFF: DefWindowProc(WM_CLOSE) -> DestroyWindow */
static int handle_internal(CPU *c, Msg *m) {
    if (m->msg == 0x7FFF) {
        static int destroyed;
        if (!destroyed++) { uint32_t sp = c->esp; send_message(c, m->hwnd, WM_DESTROY, 0, 0); c->esp = sp; }
        return 1;
    }
    return 0;
}
void IMP_USER32_PeekMessageA(CPU *c) {
    uint32_t p = ARG(0), remove = ARG(4) & 1;
    pump_sdl();
    Msg m;
    for (;;) {
        if (!queue_pop(&m, remove)) { STDRET(5, 0); return; }
        if (remove && handle_internal(c, &m)) continue;
        break;
    }
    write_msg(p, &m);
    STDRET(5, 1);
}
void IMP_USER32_GetMessageA(CPU *c) {
    uint32_t p = ARG(0);
    Msg m;
    for (;;) {
        pump_sdl();
        if (queue_pop(&m, 1)) {
            if (handle_internal(c, &m)) continue;
            break;
        }
        SDL_WaitEventTimeout(NULL, 10);
    }
    write_msg(p, &m);
    STDRET(4, m.msg == WM_QUIT ? 0 : 1);
}
void IMP_USER32_TranslateMessage(CPU *c) {
    uint32_t p = ARG(0);
    if (RD32(p + 4) == WM_KEYDOWN) {
        uint32_t vk = RD32(p + 8); int ch = 0;
        int shift = g_vk_down[0x10];
        if (vk >= 'A' && vk <= 'Z') ch = shift ? (int)vk : (int)vk + 32;
        else if (vk >= '0' && vk <= '9') ch = (int)vk;
        else if (vk == 0x20 || vk == 0x08 || vk == 0x0D || vk == 0x1B) ch = (int)vk;
        else if (vk == 0xBD) ch = shift ? '_' : '-';
        else if (vk == 0xBE) ch = '.';
        else if (vk == 0xBC) ch = ',';
        if (ch) win32_post_message(RD32(p), WM_CHAR, (uint32_t)ch, RD32(p + 12));
    }
    STDRET(1, 0);
}
void IMP_USER32_DispatchMessageA(CPU *c) {
    uint32_t p = ARG(0);
    uint32_t hwnd = RD32(p), msg = RD32(p + 4), wp = RD32(p + 8), lp = RD32(p + 12);
    uint32_t sp = c->esp;
    uint32_t r = send_message_r(c, hwnd, msg, wp, lp);
    c->esp = sp;
    STDRET(1, r);
}
void IMP_USER32_GetForegroundWindow(CPU *c) { STDRET(0, g_hwnd_main); }
void IMP_USER32_GetSystemMetrics(CPU *c) {
    uint32_t i = ARG(0);
    STDRET(1, i == 0 ? 640 : i == 1 ? 480 : 0);
}
void IMP_USER32_LoadCursorA(CPU *c) { STDRET(2, 0x20001); }
void IMP_USER32_LoadIconA(CPU *c) { STDRET(2, 0x20002); }
void IMP_USER32_LoadImageA(CPU *c) {
    rt_log("LoadImageA(%s, type=%u, flags=%x) -> unsupported (0)", ARG(1) > 0xFFFF ? HS(ARG(1)) : "#res", ARG(2), ARG(5));
    STDRET(6, 0);
}
static int g_cursor_count;
void IMP_USER32_ShowCursor(CPU *c) {
    g_cursor_count += ARG(0) ? 1 : -1;
    SDL_ShowCursor(g_cursor_count >= 0 ? SDL_ENABLE : SDL_DISABLE);
    STDRET(1, (uint32_t)g_cursor_count);
}
void IMP_USER32_MessageBoxA(CPU *c) {
    const char *text = HS(ARG(1)) ? HS(ARG(1)) : "", *cap = HS(ARG(2)) ? HS(ARG(2)) : "";
    rt_log("MessageBoxA(%s: %s)", cap, text);
    if (!getenv("PHOBIA_NO_MSGBOX")) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, cap, text, g_win);
    STDRET(4, 1);
}
void IMP_USER32_CharUpperBuffA(CPU *c) {
    uint32_t s = ARG(0), n = ARG(1);
    for (uint32_t i = 0; i < n; i++) M[s + i] = (uint8_t)toupper(M[s + i]);
    STDRET(2, n);
}

/* ------------------------------------------------------------------ GDI32 (minimal; DDraw surface DCs) */
extern int ddraw_dc_getpixel(uint32_t hdc, int x, int y, uint32_t *rgb);
extern int ddraw_dc_setpixel(uint32_t hdc, int x, int y, uint32_t rgb);
void IMP_GDI32_CreateCompatibleDC(CPU *c) { STDRET(1, 0x30001); }
void IMP_GDI32_DeleteDC(CPU *c) { STDRET(1, 1); }
void IMP_GDI32_DeleteObject(CPU *c) { STDRET(1, 1); }
void IMP_GDI32_GetObjectA(CPU *c) { STDRET(3, 0); }
void IMP_GDI32_GetStockObject(CPU *c) { STDRET(1, 0x40000 + ARG(0)); }
void IMP_GDI32_SelectObject(CPU *c) { STDRET(2, 0x40000); }
void IMP_GDI32_StretchBlt(CPU *c) { rt_log("StretchBlt ignored"); STDRET(11, 1); }
void IMP_GDI32_GetPixel(CPU *c) { uint32_t v = 0; ddraw_dc_getpixel(ARG(0), (int)ARG(1), (int)ARG(2), &v); STDRET(3, v); }
void IMP_GDI32_SetPixel(CPU *c) { ddraw_dc_setpixel(ARG(0), (int)ARG(1), (int)ARG(2), ARG(3)); STDRET(4, ARG(3)); }

void user32_init(void) { g_main_thread = pthread_self(); g_start_ticks = SDL_GetTicks(); }
