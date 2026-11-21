// DirectDraw emulation: surfaces live in guest memory; primary flips present through SDL.
#include "runtime.h"
#include "win32.h"
#include <SDL.h>

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_NOTFOUND 0x887600FFu
#define E_NOINTERFACE 0x80004002u
#define DDERR_UNSUPPORTED 0x80004001u

#define DDSD_CAPS 0x1
#define DDSD_HEIGHT 0x2
#define DDSD_WIDTH 0x4
#define DDSD_PITCH 0x8
#define DDSD_BACKBUFFERCOUNT 0x20
#define DDSD_LPSURFACE 0x800
#define DDSD_PIXELFORMAT 0x1000
#define DDSD_CKSRCBLT 0x10000
#define DDSCAPS_BACKBUFFER 0x4
#define DDSCAPS_COMPLEX 0x8
#define DDSCAPS_FLIP 0x10
#define DDSCAPS_FRONTBUFFER 0x20
#define DDSCAPS_OFFSCREENPLAIN 0x40
#define DDSCAPS_PRIMARYSURFACE 0x200
#define DDSCAPS_SYSTEMMEMORY 0x800
#define DDSCAPS_VIDEOMEMORY 0x4000
#define DDSCAPS_LOCALVIDMEM 0x10000000

typedef struct Surface {
    uint32_t obj;          /* guest COM object */
    int w, h, bpp, pitch;
    uint32_t mem;          /* guest pixel memory */
    uint32_t caps;
    int primary;
    struct Surface *back;  /* attached back buffer (primary flip chain) */
    struct Surface *front;
    uint32_t palette_obj;
    int has_ck; uint32_t ck_lo, ck_hi;
    int refs;
    uint32_t clipper;
} Surface;
typedef struct { uint32_t obj; uint32_t entries[256]; int refs; } Palette;

static int g_mode_w = 640, g_mode_h = 480, g_mode_bpp = 8;
static Surface *g_primary;
static uint32_t g_dd_obj, g_dd2_obj;
static int g_force_bpp;

/* ---------------------------------------------------------------- present */
static uint32_t g_last_present;
static void present_primary(int force) {
    if (!g_primary) return;
    uint32_t now = SDL_GetTicks();
    if (!force && now - g_last_present < 4) return;
    g_last_present = now;
    Surface *s = g_primary;
    static uint32_t gray[256];
    const uint32_t *pal = gray;
    if (s->palette_obj) { Palette *p = com_host(s->palette_obj); if (p) pal = p->entries; }
    else for (int i = 0; i < 256; i++) gray[i] = 0xFF000000u | (i << 16) | (i << 8) | i;
    video_present(M + s->mem, s->w, s->h, s->bpp, s->pitch, pal);
}
static uint64_t g_next_vbl_us;
static uint64_t now_us(void) { return SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency(); }
static void wait_vblank(void) {
    uint64_t t = now_us();
    const uint64_t period = 1000000ull / 60;
    if (g_next_vbl_us == 0 || t > g_next_vbl_us + period) g_next_vbl_us = t;
    if (g_next_vbl_us > t) SDL_Delay((uint32_t)((g_next_vbl_us - t) / 1000));
    while (now_us() < g_next_vbl_us) {}
    g_next_vbl_us += period;
}

/* ---------------------------------------------------------------- forward decls */
static uint32_t make_surface(int w, int h, int bpp, uint32_t caps, Surface **out);
static uint32_t make_palette(const uint32_t *init, int n);

/* ---------------------------------------------------------------- IUnknown helpers */
static void fill_pixel_format(uint32_t pf, int bpp) {
    memset(M + pf, 0, 32);
    WR32(pf, 32);
    if (bpp == 8) { WR32(pf + 4, 0x20 | 0x40); WR32(pf + 12, 8); }      /* DDPF_PALETTEINDEXED8 | DDPF_RGB */
    else if (bpp == 16) { WR32(pf + 4, 0x40); WR32(pf + 12, 16); WR32(pf + 16, 0xF800); WR32(pf + 20, 0x07E0); WR32(pf + 24, 0x001F); }
    else { WR32(pf + 4, 0x40); WR32(pf + 12, 32); WR32(pf + 16, 0xFF0000); WR32(pf + 20, 0xFF00); WR32(pf + 24, 0xFF); }
}
static void fill_desc(uint32_t d, Surface *s) {
    uint32_t sz = RD32(d); if (sz < 108) sz = 108;
    memset(M + d, 0, sz); WR32(d, sz);
    WR32(d + 4, DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT | DDSD_LPSURFACE);
    WR32(d + 8, s->h); WR32(d + 12, s->w); WR32(d + 16, s->pitch);
    if (s->back) { WR32(d + 20, 1); WR32(d + 4, RD32(d + 4) | DDSD_BACKBUFFERCOUNT); }
    WR32(d + 36, s->mem);
    if (s->has_ck) { WR32(d + 64, s->ck_lo); WR32(d + 68, s->ck_hi); WR32(d + 4, RD32(d + 4) | DDSD_CKSRCBLT); }
    fill_pixel_format(d + 72, s->bpp);
    WR32(d + 104, s->caps);
}

/* ================================================================= IDirectDrawPalette */
static void PAL_QueryInterface(CPU *c) { STDRET(3, E_NOINTERFACE); }
static void PAL_AddRef(CPU *c) { Palette *p = com_host(ARG(0)); STDRET(1, ++p->refs); }
static void PAL_Release(CPU *c) { Palette *p = com_host(ARG(0)); STDRET(1, p->refs > 0 ? --p->refs : 0); }
static void PAL_GetCaps(CPU *c) { WR32(ARG(1), 0x4 | 0x40); STDRET(2, DD_OK); }
static void PAL_GetEntries(CPU *c) {
    Palette *p = com_host(ARG(0)); uint32_t start = ARG(2), n = ARG(3), e = ARG(4);
    for (uint32_t i = 0; i < n && start + i < 256; i++) {
        uint32_t v = p->entries[start + i];
        M[e + 4 * i] = (v >> 16) & 255; M[e + 4 * i + 1] = (v >> 8) & 255; M[e + 4 * i + 2] = v & 255; M[e + 4 * i + 3] = 0;
    }
    STDRET(5, DD_OK);
}
static void PAL_Initialize(CPU *c) { STDRET(4, DD_OK); }
static void PAL_SetEntries(CPU *c) {
    Palette *p = com_host(ARG(0)); uint32_t start = ARG(2), n = ARG(3), e = ARG(4);
    for (uint32_t i = 0; i < n && start + i < 256; i++)
        p->entries[start + i] = 0xFF000000u | (M[e + 4 * i] << 16) | (M[e + 4 * i + 1] << 8) | M[e + 4 * i + 2];
    if (g_primary && g_primary->palette_obj == ARG(0)) present_primary(1);
    STDRET(5, DD_OK);
}
static const GuestFn k_pal_vt[] = { PAL_QueryInterface, PAL_AddRef, PAL_Release, PAL_GetCaps, PAL_GetEntries, PAL_Initialize, PAL_SetEntries };
static const char *const k_pal_names[] = { "PAL_QueryInterface", "PAL_AddRef", "PAL_Release", "PAL_GetCaps", "PAL_GetEntries", "PAL_Initialize", "PAL_SetEntries" };
static uint32_t make_palette(const uint32_t *init, int n) {
    Palette *p = calloc(1, sizeof *p); p->refs = 1;
    for (int i = 0; i < n && i < 256; i++) p->entries[i] = init[i];
    p->obj = com_make_object(k_pal_vt, k_pal_names, 7, 0, p);
    return p->obj;
}

/* ================================================================= IDirectDrawClipper */
static void CLIP_QueryInterface(CPU *c) { STDRET(3, E_NOINTERFACE); }
static void CLIP_AddRef(CPU *c) { STDRET(1, 1); }
static void CLIP_Release(CPU *c) { STDRET(1, 0); }
static void CLIP_GetClipList(CPU *c) { STDRET(4, DDERR_UNSUPPORTED); }
static void CLIP_GetHWnd(CPU *c) { WR32(ARG(1), g_hwnd_main); STDRET(2, DD_OK); }
static void CLIP_Initialize(CPU *c) { STDRET(3, DD_OK); }
static void CLIP_IsClipListChanged(CPU *c) { WR32(ARG(1), 0); STDRET(2, DD_OK); }
static void CLIP_SetClipList(CPU *c) { STDRET(3, DD_OK); }
static void CLIP_SetHWnd(CPU *c) { STDRET(3, DD_OK); }
static const GuestFn k_clip_vt[] = { CLIP_QueryInterface, CLIP_AddRef, CLIP_Release, CLIP_GetClipList, CLIP_GetHWnd, CLIP_Initialize, CLIP_IsClipListChanged, CLIP_SetClipList, CLIP_SetHWnd };
static const char *const k_clip_names[] = { "CLIP_QI", "CLIP_AddRef", "CLIP_Release", "CLIP_GetClipList", "CLIP_GetHWnd", "CLIP_Initialize", "CLIP_IsClipListChanged", "CLIP_SetClipList", "CLIP_SetHWnd" };

/* ================================================================= IDirectDrawSurface */
#define SURF(o) ((Surface *)com_host(o))
static void S_QueryInterface(CPU *c) {
    /* all surface interface versions share the same vtable prefix; return self */
    uint32_t out = ARG(2);
    WR32(out, ARG(0)); SURF(ARG(0))->refs++;
    STDRET(3, DD_OK);
}
static void S_AddRef(CPU *c) { STDRET(1, ++SURF(ARG(0))->refs); }
static void S_Release(CPU *c) { Surface *s = SURF(ARG(0)); STDRET(1, s->refs > 0 ? --s->refs : 0); }
static void S_AddAttachedSurface(CPU *c) { STDRET(2, DD_OK); }
static void S_AddOverlayDirtyRect(CPU *c) { STDRET(2, DD_OK); }

static int rect_read(uint32_t r, int *x0, int *y0, int *x1, int *y1, Surface *s) {
    if (!r) { *x0 = 0; *y0 = 0; *x1 = s->w; *y1 = s->h; return 1; }
    *x0 = (int32_t)RD32(r); *y0 = (int32_t)RD32(r + 4); *x1 = (int32_t)RD32(r + 8); *y1 = (int32_t)RD32(r + 12);
    return 1;
}
static inline uint32_t getpx(Surface *s, int x, int y) {
    uint32_t a = s->mem + y * s->pitch + x * (s->bpp / 8);
    return s->bpp == 8 ? M[a] : s->bpp == 16 ? RD16(a) : RD32(a);
}
static inline void putpx(Surface *s, int x, int y, uint32_t v) {
    uint32_t a = s->mem + y * s->pitch + x * (s->bpp / 8);
    if (s->bpp == 8) M[a] = (uint8_t)v; else if (s->bpp == 16) WR16(a, (uint16_t)v); else WR32(a, v);
}
static void blit(Surface *d, int dx0, int dy0, int dx1, int dy1, Surface *s, int sx0, int sy0, int sx1, int sy1, int keysrc) {
    int dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int bpp = d->bpp / 8;
    if (dw == sw && dh == sh && !keysrc && s->bpp == d->bpp) {
        /* clip */
        if (dx0 < 0) { sx0 -= dx0; dx0 = 0; } if (dy0 < 0) { sy0 -= dy0; dy0 = 0; }
        if (dx1 > d->w) dx1 = d->w; if (dy1 > d->h) dy1 = d->h;
        int w = dx1 - dx0;
        for (int y = dy0; y < dy1; y++) {
            int sy = sy0 + (y - dy0);
            if (sy < 0 || sy >= s->h || w <= 0) continue;
            memmove(M + d->mem + y * d->pitch + dx0 * bpp, M + s->mem + sy * s->pitch + sx0 * bpp, (size_t)w * bpp);
        }
        return;
    }
    for (int y = dy0; y < dy1; y++) {
        if (y < 0 || y >= d->h) continue;
        int sy = sy0 + (int)((int64_t)(y - dy0) * sh / dh);
        if (sy < 0 || sy >= s->h) continue;
        for (int x = dx0; x < dx1; x++) {
            if (x < 0 || x >= d->w) continue;
            int sx = sx0 + (int)((int64_t)(x - dx0) * sw / dw);
            if (sx < 0 || sx >= s->w) continue;
            uint32_t v = getpx(s, sx, sy);
            if (keysrc && v >= s->ck_lo && v <= s->ck_hi) continue;
            putpx(d, x, y, v);
        }
    }
}
static void after_draw(Surface *d) { if (d == g_primary) present_primary(0); }

static void S_Blt(CPU *c) {
    Surface *d = SURF(ARG(0)); uint32_t dr = ARG(1), so = ARG(2), sr = ARG(3), flags = ARG(4), fx = ARG(5);
    int dx0, dy0, dx1, dy1; rect_read(dr, &dx0, &dy0, &dx1, &dy1, d);
    if (flags & 0x400) {  /* DDBLT_COLORFILL */
        uint32_t col = RD32(fx + 80);
        if (dx0 < 0) dx0 = 0; if (dy0 < 0) dy0 = 0; if (dx1 > d->w) dx1 = d->w; if (dy1 > d->h) dy1 = d->h;
        for (int y = dy0; y < dy1; y++) for (int x = dx0; x < dx1; x++) putpx(d, x, y, col);
        after_draw(d);
        STDRET(6, DD_OK); return;
    }
    if (!so) { STDRET(6, DD_OK); return; }
    Surface *s = SURF(so);
    int sx0, sy0, sx1, sy1; rect_read(sr, &sx0, &sy0, &sx1, &sy1, s);
    blit(d, dx0, dy0, dx1, dy1, s, sx0, sy0, sx1, sy1, (flags & 0x8000) && s->has_ck);
    after_draw(d);
    STDRET(6, DD_OK);
}
static void S_BltBatch(CPU *c) { STDRET(4, DDERR_UNSUPPORTED); }
static void S_BltFast(CPU *c) {
    Surface *d = SURF(ARG(0)); int x = (int)ARG(1), y = (int)ARG(2); uint32_t so = ARG(3), sr = ARG(4), tr = ARG(5);
    Surface *s = SURF(so);
    int sx0, sy0, sx1, sy1; rect_read(sr, &sx0, &sy0, &sx1, &sy1, s);
    blit(d, x, y, x + (sx1 - sx0), y + (sy1 - sy0), s, sx0, sy0, sx1, sy1, (tr & 1) && s->has_ck);
    after_draw(d);
    STDRET(6, DD_OK);
}
static void S_DeleteAttachedSurface(CPU *c) { STDRET(3, DD_OK); }
static void S_EnumAttachedSurfaces(CPU *c) {
    Surface *s = SURF(ARG(0)); uint32_t ctx = ARG(1), cb = ARG(2);
    if (s->back) {
        uint32_t desc = galloc(108); WR32(desc, 108); fill_desc(desc, s->back);
        s->back->refs++;
        uint32_t args[3] = { s->back->obj, desc, ctx };
        uint32_t sp = c->esp; call_guest(c, cb, 3, args); c->esp = sp;
    }
    STDRET(3, DD_OK);
}
static void S_EnumOverlayZOrders(CPU *c) { STDRET(4, DD_OK); }
static void S_Flip(CPU *c) {
    Surface *s = SURF(ARG(0));
    if (s->primary && s->back) {
        /* swap memory between front and back so the game draws into the new back buffer */
        uint32_t t = s->mem; s->mem = s->back->mem; s->back->mem = t;
        present_primary(1);
        wait_vblank();
    }
    STDRET(3, DD_OK);
}
static void S_GetAttachedSurface(CPU *c) {
    Surface *s = SURF(ARG(0)); uint32_t caps = ARG(1), out = ARG(2);
    if (s->back && (RD32(caps) & DDSCAPS_BACKBUFFER)) { s->back->refs++; WR32(out, s->back->obj); STDRET(3, DD_OK); return; }
    WR32(out, 0); STDRET(3, DDERR_NOTFOUND);
}
static void S_GetBltStatus(CPU *c) { STDRET(2, DD_OK); }
static void S_GetCaps(CPU *c) { WR32(ARG(1), SURF(ARG(0))->caps); STDRET(2, DD_OK); }
static void S_GetClipper(CPU *c) { WR32(ARG(1), SURF(ARG(0))->clipper); STDRET(2, SURF(ARG(0))->clipper ? DD_OK : DDERR_NOTFOUND); }
static void S_GetColorKey(CPU *c) { Surface *s = SURF(ARG(0)); WR32(ARG(2), s->ck_lo); WR32(ARG(2) + 4, s->ck_hi); STDRET(3, s->has_ck ? DD_OK : DDERR_NOTFOUND); }
/* DCs: hdc = 0x50000 + surface host index; we keep a small table */
static Surface *g_dc_surf[64];
static void S_GetDC(CPU *c) {
    Surface *s = SURF(ARG(0));
    for (int i = 0; i < 64; i++) if (!g_dc_surf[i] || g_dc_surf[i] == s) { g_dc_surf[i] = s; WR32(ARG(1), 0x50000 + i); STDRET(2, DD_OK); return; }
    STDRET(2, DDERR_GENERIC);
}
static void S_ReleaseDC(CPU *c) { after_draw(SURF(ARG(0))); STDRET(2, DD_OK); }
static uint32_t rgb_to_pixel(Surface *s, uint32_t rgb) {
    int r = rgb & 255, g = (rgb >> 8) & 255, b = (rgb >> 16) & 255;
    if (s->bpp == 16) return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    if (s->bpp == 32) return (r << 16) | (g << 8) | b;
    /* 8-bit: nearest palette entry (primary palette) */
    Palette *p = (g_primary && g_primary->palette_obj) ? com_host(g_primary->palette_obj) : NULL;
    if (s->palette_obj) p = com_host(s->palette_obj);
    if (!p) return 0;
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        uint32_t e = p->entries[i];
        int dr = ((e >> 16) & 255) - r, dg = ((e >> 8) & 255) - g, db = (e & 255) - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bd) { bd = d; best = i; }
    }
    return (uint32_t)best;
}
int ddraw_dc_setpixel(uint32_t hdc, int x, int y, uint32_t rgb) {
    uint32_t i = hdc - 0x50000; if (i >= 64 || !g_dc_surf[i]) return 0;
    Surface *s = g_dc_surf[i];
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    putpx(s, x, y, rgb_to_pixel(s, rgb)); return 1;
}
int ddraw_dc_getpixel(uint32_t hdc, int x, int y, uint32_t *rgb) {
    uint32_t i = hdc - 0x50000; if (i >= 64 || !g_dc_surf[i]) return 0;
    Surface *s = g_dc_surf[i];
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0;
    uint32_t v = getpx(s, x, y);
    if (s->bpp == 16) { *rgb = ((v >> 11) << 3) | (((v >> 5) & 63) << 10) | ((v & 31) << 19); }
    else if (s->bpp == 8) {
        Palette *p = (g_primary && g_primary->palette_obj) ? com_host(g_primary->palette_obj) : NULL;
        uint32_t e = p ? p->entries[v & 255] : 0;
        *rgb = ((e >> 16) & 255) | (e & 0xFF00) | ((e & 255) << 16);
    } else *rgb = ((v >> 16) & 255) | (v & 0xFF00) | ((v & 255) << 16);
    return 1;
}
static void S_GetFlipStatus(CPU *c) { STDRET(2, DD_OK); }
static void S_GetOverlayPosition(CPU *c) { STDRET(3, DDERR_UNSUPPORTED); }
static void S_GetPalette(CPU *c) { Surface *s = SURF(ARG(0)); WR32(ARG(1), s->palette_obj); STDRET(2, s->palette_obj ? DD_OK : DDERR_NOTFOUND); }
static void S_GetPixelFormat(CPU *c) { fill_pixel_format(ARG(1), SURF(ARG(0))->bpp); STDRET(2, DD_OK); }
static void S_GetSurfaceDesc(CPU *c) { fill_desc(ARG(1), SURF(ARG(0))); STDRET(2, DD_OK); }
static void S_Initialize(CPU *c) { STDRET(3, DD_OK); }
static void S_IsLost(CPU *c) { STDRET(1, DD_OK); }
static void S_Lock(CPU *c) {
    Surface *s = SURF(ARG(0)); uint32_t r = ARG(1), d = ARG(2);
    fill_desc(d, s);
    if (r) {
        int x0 = (int32_t)RD32(r), y0 = (int32_t)RD32(r + 4);
        WR32(d + 36, s->mem + y0 * s->pitch + x0 * (s->bpp / 8));
    }
    STDRET(5, DD_OK);
}
static void S_ReleaseDC_(CPU *c);
static void S_Restore(CPU *c) { STDRET(1, DD_OK); }
static void S_SetClipper(CPU *c) { SURF(ARG(0))->clipper = ARG(1); STDRET(2, DD_OK); }
static void S_SetColorKey(CPU *c) {
    Surface *s = SURF(ARG(0)); uint32_t flags = ARG(1), k = ARG(2);
    if (flags & 0x8) {   /* DDCKEY_SRCBLT */
        if (k) { s->has_ck = 1; s->ck_lo = RD32(k); s->ck_hi = RD32(k + 4); if (!(flags & 0x1) && s->ck_hi < s->ck_lo) s->ck_hi = s->ck_lo; }
        else s->has_ck = 0;
    }
    STDRET(3, DD_OK);
}
static void S_SetOverlayPosition(CPU *c) { STDRET(3, DDERR_UNSUPPORTED); }
static void S_SetPalette(CPU *c) {
    Surface *s = SURF(ARG(0)); s->palette_obj = ARG(1);
    if (s->back) s->back->palette_obj = ARG(1);
    if (s == g_primary) present_primary(1);
    STDRET(2, DD_OK);
}
static void S_Unlock(CPU *c) { after_draw(SURF(ARG(0))); STDRET(2, DD_OK); }
static void S_UpdateOverlay(CPU *c) { STDRET(6, DDERR_UNSUPPORTED); }
static void S_UpdateOverlayDisplay(CPU *c) { STDRET(2, DDERR_UNSUPPORTED); }
static void S_UpdateOverlayZOrder(CPU *c) { STDRET(3, DDERR_UNSUPPORTED); }
static void S_GetDDInterface(CPU *c) { WR32(ARG(1), g_dd_obj); STDRET(2, DD_OK); }
static void S_PageLock(CPU *c) { STDRET(2, DD_OK); }
static void S_PageUnlock(CPU *c) { STDRET(2, DD_OK); }
static void S_SetSurfaceDesc(CPU *c) { STDRET(3, DDERR_UNSUPPORTED); }

static const GuestFn k_surf_vt[] = {
    S_QueryInterface, S_AddRef, S_Release, S_AddAttachedSurface, S_AddOverlayDirtyRect, S_Blt, S_BltBatch, S_BltFast,
    S_DeleteAttachedSurface, S_EnumAttachedSurfaces, S_EnumOverlayZOrders, S_Flip, S_GetAttachedSurface, S_GetBltStatus,
    S_GetCaps, S_GetClipper, S_GetColorKey, S_GetDC, S_GetFlipStatus, S_GetOverlayPosition, S_GetPalette, S_GetPixelFormat,
    S_GetSurfaceDesc, S_Initialize, S_IsLost, S_Lock, S_ReleaseDC, S_Restore, S_SetClipper, S_SetColorKey,
    S_SetOverlayPosition, S_SetPalette, S_Unlock, S_UpdateOverlay, S_UpdateOverlayDisplay, S_UpdateOverlayZOrder,
    S_GetDDInterface, S_PageLock, S_PageUnlock, S_SetSurfaceDesc };
static const char *const k_surf_names[] = {
    "S_QueryInterface", "S_AddRef", "S_Release", "S_AddAttachedSurface", "S_AddOverlayDirtyRect", "S_Blt", "S_BltBatch", "S_BltFast",
    "S_DeleteAttachedSurface", "S_EnumAttachedSurfaces", "S_EnumOverlayZOrders", "S_Flip", "S_GetAttachedSurface", "S_GetBltStatus",
    "S_GetCaps", "S_GetClipper", "S_GetColorKey", "S_GetDC", "S_GetFlipStatus", "S_GetOverlayPosition", "S_GetPalette", "S_GetPixelFormat",
    "S_GetSurfaceDesc", "S_Initialize", "S_IsLost", "S_Lock", "S_ReleaseDC", "S_Restore", "S_SetClipper", "S_SetColorKey",
    "S_SetOverlayPosition", "S_SetPalette", "S_Unlock", "S_UpdateOverlay", "S_UpdateOverlayDisplay", "S_UpdateOverlayZOrder",
    "S_GetDDInterface", "S_PageLock", "S_PageUnlock", "S_SetSurfaceDesc" };

static uint32_t make_surface(int w, int h, int bpp, uint32_t caps, Surface **out) {
    Surface *s = calloc(1, sizeof *s);
    s->w = w; s->h = h; s->bpp = bpp; s->pitch = ((w * (bpp / 8)) + 3) & ~3; s->caps = caps; s->refs = 1;
    s->mem = galloc((uint32_t)(s->pitch * h + 64));
    s->obj = com_make_object(k_surf_vt, k_surf_names, 40, 0, s);
    if (out) *out = s;
    return s->obj;
}

/* ================================================================= IDirectDraw / IDirectDraw2 */
static void DD_QueryInterface(CPU *c);
static void DD_AddRef(CPU *c) { STDRET(1, 2); }
static void DD_Release(CPU *c) { STDRET(1, 1); }
static void DD_Compact(CPU *c) { STDRET(1, DD_OK); }
static void DD_CreateClipper(CPU *c) {
    uint32_t o = com_make_object(k_clip_vt, k_clip_names, 9, 0, NULL);
    WR32(ARG(2), o); STDRET(4, DD_OK);
}
static void DD_CreatePalette(CPU *c) {
    uint32_t flags = ARG(1), e = ARG(2), out = ARG(3);
    uint32_t init[256];
    int n = (flags & 0x4) ? 256 : (flags & 0x2) ? 16 : (flags & 0x1) ? 2 : 256;   /* DDPCAPS_8BIT=4 */
    for (int i = 0; i < n; i++) init[i] = 0xFF000000u | (M[e + 4 * i] << 16) | (M[e + 4 * i + 1] << 8) | M[e + 4 * i + 2];
    WR32(out, make_palette(init, n));
    STDRET(5, DD_OK);
}
static void DD_CreateSurface(CPU *c) {
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t flags = RD32(d + 4), caps = (flags & DDSD_CAPS) ? RD32(d + 104) : 0;
    int w = (flags & DDSD_WIDTH) ? (int)RD32(d + 12) : g_mode_w;
    int h = (flags & DDSD_HEIGHT) ? (int)RD32(d + 8) : g_mode_h;
    int bpp = g_mode_bpp;
    if (flags & DDSD_PIXELFORMAT) { int b = (int)RD32(d + 72 + 12); if (b) bpp = b; }
    Surface *s;
    if (caps & DDSCAPS_PRIMARYSURFACE) {
        w = g_mode_w; h = g_mode_h; bpp = g_mode_bpp;
        make_surface(w, h, bpp, caps | DDSCAPS_FRONTBUFFER | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM, &s);
        s->primary = 1;
        g_primary = s;
        if ((flags & DDSD_BACKBUFFERCOUNT) && RD32(d + 20) >= 1) {
            Surface *b;
            make_surface(w, h, bpp, DDSCAPS_BACKBUFFER | DDSCAPS_FLIP | DDSCAPS_COMPLEX | DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM, &b);
            s->back = b; b->front = s;
        }
        rt_log("CreateSurface primary %dx%dx%d backbuffers=%u", w, h, bpp, (flags & DDSD_BACKBUFFERCOUNT) ? RD32(d + 20) : 0);
    } else {
        make_surface(w, h, bpp, caps ? caps : DDSCAPS_OFFSCREENPLAIN, &s);
        if (!(s->caps & DDSCAPS_SYSTEMMEMORY)) s->caps |= DDSCAPS_VIDEOMEMORY | DDSCAPS_LOCALVIDMEM;
        if (flags & DDSD_CKSRCBLT) { s->has_ck = 1; s->ck_lo = RD32(d + 64); s->ck_hi = RD32(d + 68); }
        if (g_trace) rt_log("CreateSurface %dx%dx%d caps=%x", w, h, bpp, caps);
    }
    WR32(out, s->obj);
    STDRET(4, DD_OK);
}
static void DD_DuplicateSurface(CPU *c) { STDRET(3, DDERR_UNSUPPORTED); }
static const int k_modes[][3] = { {320, 200, 8}, {320, 240, 8}, {640, 400, 8}, {640, 480, 8}, {800, 600, 8}, {640, 480, 16}, {800, 600, 16} };
static void DD_EnumDisplayModes(CPU *c) {
    uint32_t ctx = ARG(3), cb = ARG(4);
    for (unsigned i = 0; i < sizeof k_modes / sizeof k_modes[0]; i++) {
        if (g_force_bpp && k_modes[i][2] != g_force_bpp) continue;
        uint32_t d = galloc(108);
        WR32(d, 108); WR32(d + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT);
        WR32(d + 8, k_modes[i][1]); WR32(d + 12, k_modes[i][0]); WR32(d + 16, k_modes[i][0] * k_modes[i][2] / 8);
        fill_pixel_format(d + 72, k_modes[i][2]);
        uint32_t args[2] = { d, ctx };
        uint32_t sp = c->esp; call_guest(c, cb, 2, args); uint32_t r = c->eax; c->esp = sp;
        gfree(d);
        if (!r) break;
    }
    STDRET(5, DD_OK);
}
static void DD_EnumSurfaces(CPU *c) { STDRET(5, DD_OK); }
static void DD_FlipToGDISurface(CPU *c) { STDRET(1, DD_OK); }
static void DD_GetCaps(CPU *c) {
    uint32_t hw = ARG(1), hel = ARG(2);
    if (hw) { uint32_t sz = RD32(hw); memset(M + hw, 0, sz ? sz : 316); WR32(hw, sz); WR32(hw + 4, 0x40 | 0x1000 | 0x200000); WR32(hw + 60, 32u << 20); WR32(hw + 64, 32u << 20); }
    if (hel) { uint32_t sz = RD32(hel); memset(M + hel, 0, sz ? sz : 316); WR32(hel, sz); WR32(hel + 4, 0x40); }
    STDRET(3, DD_OK);
}
static void DD_GetDisplayMode(CPU *c) {
    uint32_t d = ARG(1); memset(M + d, 0, 108); WR32(d, 108);
    WR32(d + 4, DDSD_HEIGHT | DDSD_WIDTH | DDSD_PITCH | DDSD_PIXELFORMAT);
    WR32(d + 8, g_mode_h); WR32(d + 12, g_mode_w); WR32(d + 16, g_mode_w * g_mode_bpp / 8);
    fill_pixel_format(d + 72, g_mode_bpp);
    STDRET(2, DD_OK);
}
static void DD_GetFourCCCodes(CPU *c) { WR32(ARG(1), 0); STDRET(3, DD_OK); }
static void DD_GetGDISurface(CPU *c) { WR32(ARG(1), g_primary ? g_primary->obj : 0); STDRET(2, g_primary ? DD_OK : DDERR_NOTFOUND); }
static void DD_GetMonitorFrequency(CPU *c) { WR32(ARG(1), 60); STDRET(2, DD_OK); }
static void DD_GetScanLine(CPU *c) { WR32(ARG(1), (SDL_GetTicks() * 31) % 525); STDRET(2, DD_OK); }
static void DD_GetVerticalBlankStatus(CPU *c) { WR32(ARG(1), 0); STDRET(2, DD_OK); }
static void DD_Initialize(CPU *c) { STDRET(2, DD_OK); }
static void DD_RestoreDisplayMode(CPU *c) { STDRET(1, DD_OK); }
static void DD_SetCooperativeLevel(CPU *c) { rt_log("SetCooperativeLevel(flags=%x)", ARG(2)); STDRET(3, DD_OK); }
static uint32_t set_mode(uint32_t w, uint32_t h, uint32_t bpp) {
    rt_log("SetDisplayMode(%u, %u, %u)", w, h, bpp);
    if (g_force_bpp && (int)bpp != g_force_bpp) return 0x88760000u + 0x253;   /* DDERR_INVALIDMODE */
    if (bpp != 8 && bpp != 16 && bpp != 32) return 0x88760000u + 0x253;
    g_mode_w = (int)w; g_mode_h = (int)h; g_mode_bpp = (int)bpp;
    return DD_OK;
}
static void DD_SetDisplayMode(CPU *c) { STDRET(4, set_mode(ARG(1), ARG(2), ARG(3))); }
static void DD2_SetDisplayMode(CPU *c) { STDRET(6, set_mode(ARG(1), ARG(2), ARG(3))); }
static void DD_WaitForVerticalBlank(CPU *c) { wait_vblank(); win32_pump_if_main(); STDRET(3, DD_OK); }
static void DD2_GetAvailableVidMem(CPU *c) { if (ARG(2)) WR32(ARG(2), 32u << 20); if (ARG(3)) WR32(ARG(3), 30u << 20); STDRET(4, DD_OK); }

static const GuestFn k_dd_vt[] = {
    DD_QueryInterface, DD_AddRef, DD_Release, DD_Compact, DD_CreateClipper, DD_CreatePalette, DD_CreateSurface,
    DD_DuplicateSurface, DD_EnumDisplayModes, DD_EnumSurfaces, DD_FlipToGDISurface, DD_GetCaps, DD_GetDisplayMode,
    DD_GetFourCCCodes, DD_GetGDISurface, DD_GetMonitorFrequency, DD_GetScanLine, DD_GetVerticalBlankStatus,
    DD_Initialize, DD_RestoreDisplayMode, DD_SetCooperativeLevel, DD_SetDisplayMode, DD_WaitForVerticalBlank };
static const GuestFn k_dd2_vt[] = {
    DD_QueryInterface, DD_AddRef, DD_Release, DD_Compact, DD_CreateClipper, DD_CreatePalette, DD_CreateSurface,
    DD_DuplicateSurface, DD_EnumDisplayModes, DD_EnumSurfaces, DD_FlipToGDISurface, DD_GetCaps, DD_GetDisplayMode,
    DD_GetFourCCCodes, DD_GetGDISurface, DD_GetMonitorFrequency, DD_GetScanLine, DD_GetVerticalBlankStatus,
    DD_Initialize, DD_RestoreDisplayMode, DD_SetCooperativeLevel, DD2_SetDisplayMode, DD_WaitForVerticalBlank,
    DD2_GetAvailableVidMem };
static const char *const k_dd_names[] = {
    "DD_QueryInterface", "DD_AddRef", "DD_Release", "DD_Compact", "DD_CreateClipper", "DD_CreatePalette", "DD_CreateSurface",
    "DD_DuplicateSurface", "DD_EnumDisplayModes", "DD_EnumSurfaces", "DD_FlipToGDISurface", "DD_GetCaps", "DD_GetDisplayMode",
    "DD_GetFourCCCodes", "DD_GetGDISurface", "DD_GetMonitorFrequency", "DD_GetScanLine", "DD_GetVerticalBlankStatus",
    "DD_Initialize", "DD_RestoreDisplayMode", "DD_SetCooperativeLevel", "DD_SetDisplayMode", "DD_WaitForVerticalBlank",
    "DD2_GetAvailableVidMem" };

/* IIDs: first dword is enough to discriminate */
#define IID_IDirectDraw_D1   0x6C14DB80u
#define IID_IDirectDraw2_D1  0xB3A6F3E0u
static void DD_QueryInterface(CPU *c) {
    uint32_t iid = ARG(1), out = ARG(2);
    uint32_t d1 = RD32(iid);
    if (d1 == IID_IDirectDraw2_D1) {
        if (!g_dd2_obj) g_dd2_obj = com_make_object(k_dd2_vt, k_dd_names, 24, 0, NULL);
        WR32(out, g_dd2_obj); STDRET(3, DD_OK); return;
    }
    if (d1 == IID_IDirectDraw_D1) { WR32(out, g_dd_obj); STDRET(3, DD_OK); return; }
    rt_log("DD_QueryInterface: unknown IID %08x", d1);
    WR32(out, 0); STDRET(3, E_NOINTERFACE);
}

void IMP_DDRAW_DirectDrawCreate(CPU *c) {
    uint32_t out = ARG(1);
    if (getenv("PHOBIA_BPP")) g_force_bpp = atoi(getenv("PHOBIA_BPP"));
    if (!g_dd_obj) g_dd_obj = com_make_object(k_dd_vt, k_dd_names, 23, 0, NULL);
    rt_log("DirectDrawCreate -> %08x", g_dd_obj);
    WR32(out, g_dd_obj);
    STDRET(3, DD_OK);
}
