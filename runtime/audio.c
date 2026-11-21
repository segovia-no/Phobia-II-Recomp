// Audio: SDL output device; mixes DirectSound buffers and the waveOut stream.
#include "runtime.h"
#include "win32.h"
#include <SDL.h>
#include <pthread.h>

#define OUT_RATE 44100
#define DS_OK 0
#define DSERR_GENERIC 0x80004005u
#define E_NOINTERFACE 0x80004002u

static SDL_AudioDeviceID g_dev;
static SDL_mutex *g_mix;
static int g_dev_frames = 512;          /* device chunk size */
static uint64_t g_last_cb_us;           /* time of last mixing callback */
static uint64_t audio_now_us(void) { return SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency(); }

/* ------------------------------------------------------------------ DirectSound buffers */
typedef struct DSBuf {
    uint32_t obj;
    uint32_t mem, size;          /* guest memory */
    int channels, bits, rate, align;
    int primary;
    int playing, looping;
    uint64_t pos_fp;             /* byte-frame position in 32.32 fixed point frames */
    uint32_t freq;               /* playback frequency */
    int32_t volume, pan;
    float gl, gr;
    int refs;
    uint64_t mixed_from_fp;      /* position at start of the last mixed chunk */
    struct DSBuf *next;
} DSBuf;
static DSBuf *g_bufs;

static float db_to_gain(int32_t hundredths) { if (hundredths <= -10000) return 0; return powf(10.0f, hundredths / 2000.0f); }
static void update_gain(DSBuf *b) {
    float v = db_to_gain(b->volume);
    float l = 1, r = 1;
    if (b->pan < 0) r = db_to_gain(b->pan); else if (b->pan > 0) l = db_to_gain(-b->pan);
    b->gl = v * l; b->gr = v * r;
}

/* ------------------------------------------------------------------ waveOut stream */
typedef struct { int channels, bits, rate, align; uint32_t queue[256]; int qh, qt; uint32_t cur_off; uint64_t frac; int open; } WaveOut;
static WaveOut g_wo;
static unsigned g_underruns, g_cb_count;

static inline float sample_at(uint32_t mem, uint32_t frame, int ch, int channels, int bits) {
    if (bits == 16) { int16_t v; memcpy(&v, M + mem + (frame * channels + ch) * 2, 2); return v / 32768.0f; }
    return (M[mem + frame * channels + ch] - 128) / 128.0f;
}

static void mix_waveout(float *out, int frames) {
    WaveOut *w = &g_wo;
    if (!w->open) return;
    g_cb_count++;
    if (w->qh == w->qt) g_underruns++;
    uint64_t step = ((uint64_t)w->rate << 32) / OUT_RATE;
    int i = 0;
    while (i < frames && w->qh != w->qt) {
        uint32_t hdr = w->queue[w->qh];
        uint32_t data = RD32(hdr), len = RD32(hdr + 4);
        uint32_t nframes = len / w->align;
        while (i < frames) {
            uint32_t f = w->cur_off + (uint32_t)(w->frac >> 32);
            if (f >= nframes) break;
            float l = sample_at(data, f, 0, w->channels, w->bits);
            float r = w->channels > 1 ? sample_at(data, f, 1, w->channels, w->bits) : l;
            out[2 * i] += l; out[2 * i + 1] += r;
            i++;
            w->frac += step;
        }
        uint32_t f = w->cur_off + (uint32_t)(w->frac >> 32);
        if (f >= nframes && w->qh == (w->qt + 255) % 256 && i < frames) g_underruns++;
        if (f >= nframes) {
            /* buffer done */
            WR32(hdr + 16, (RD32(hdr + 16) & ~0x10u) | 0x1u);
            w->qh = (w->qh + 1) % 256;
            w->frac &= 0xFFFFFFFFull;
            w->cur_off = 0;
            /* carry over the fractional overshoot (f - nframes) */
            w->cur_off = f - nframes;
        }
    }
}

static void mix_ds(float *out, int frames) {
    for (DSBuf *b = g_bufs; b; b = b->next) {
        if (!b->playing || b->primary || !b->mem) continue;
        uint32_t nframes = b->size / b->align;
        if (!nframes) continue;
        uint64_t step = ((uint64_t)b->freq << 32) / OUT_RATE;
        b->mixed_from_fp = b->pos_fp;
        for (int i = 0; i < frames; i++) {
            uint32_t f = (uint32_t)(b->pos_fp >> 32);
            if (f >= nframes) {
                if (b->looping) { b->pos_fp -= (uint64_t)nframes << 32; f = (uint32_t)(b->pos_fp >> 32); if (f >= nframes) { b->pos_fp = 0; f = 0; } }
                else { b->playing = 0; b->pos_fp = 0; break; }
            }
            float l = sample_at(b->mem, f, 0, b->channels, b->bits);
            float r = b->channels > 1 ? sample_at(b->mem, f, 1, b->channels, b->bits) : l;
            out[2 * i] += l * b->gl; out[2 * i + 1] += r * b->gr;
            b->pos_fp += step;
        }
    }
}

static void audio_cb(void *ud, Uint8 *stream, int len) {
    float *out = (float *)stream;
    int frames = len / 8;
    memset(out, 0, len);
    SDL_LockMutex(g_mix);
    g_last_cb_us = audio_now_us();
    mix_waveout(out, frames);
    mix_ds(out, frames);
    SDL_UnlockMutex(g_mix);
    if (getenv("PHOBIA_AUDIO_STATS") && g_cb_count && g_cb_count % 200 == 0) rt_log("audio: callbacks=%u underruns=%u queued=%d", g_cb_count, g_underruns, (g_wo.qt - g_wo.qh + 256) % 256);
    for (int i = 0; i < frames * 2; i++) { float v = out[i]; out[i] = v > 1 ? 1 : v < -1 ? -1 : v; }
    static FILE *wav; static int wav_init;
    if (!wav_init) { wav_init = 1; const char *p = getenv("PHOBIA_WAV"); if (p) wav = fopen(p, "wb"); }
    if (wav) fwrite(out, 8, frames, wav);   /* raw float32 stereo 44.1 kHz */
}

void audio_init(void) {
    if (g_dev) return;
    g_mix = SDL_CreateMutex();
    SDL_AudioSpec want = {0}, have;
    want.freq = OUT_RATE; want.format = AUDIO_F32SYS; want.channels = 2; want.samples = (Uint16)g_dev_frames; want.callback = audio_cb;
    g_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_dev) { rt_log("audio: SDL_OpenAudioDevice failed: %s", SDL_GetError()); return; }
    g_dev_frames = have.samples;
    rt_log("audio: %d Hz, %d-frame device buffer", have.freq, have.samples);
    SDL_PauseAudioDevice(g_dev, 0);
}

/* ================================================================== IDirectSoundBuffer */
#define BUF(o) ((DSBuf *)com_host(o))
static void B_QueryInterface(CPU *c) { WR32(ARG(2), ARG(0)); STDRET(3, DS_OK); }
static void B_AddRef(CPU *c) { STDRET(1, ++BUF(ARG(0))->refs); }
static void B_Release(CPU *c) {
    DSBuf *b = BUF(ARG(0));
    int r = b->refs > 0 ? --b->refs : 0;
    if (r == 0) { SDL_LockMutex(g_mix); b->playing = 0; SDL_UnlockMutex(g_mix); }
    STDRET(1, r);
}
static void B_GetCaps(CPU *c) {
    DSBuf *b = BUF(ARG(0)); uint32_t p = ARG(1);
    WR32(p + 4, b->primary ? 1 : 0x2 | 0x80 | 0x40 | 0x20); WR32(p + 8, b->size); WR32(p + 12, 0); WR32(p + 16, 0);
    STDRET(2, DS_OK);
}
static void B_GetCurrentPosition(CPU *c) {
    /* Write cursor = end of what the device already consumed (everything before it is in flight);
       play cursor = start of the last chunk advanced by elapsed time (what is audible now). */
    DSBuf *b = BUF(ARG(0)); uint32_t pp = ARG(1), pw = ARG(2);
    uint32_t play = 0, wr = 0;
    SDL_LockMutex(g_mix);
    if (!b->primary && b->size && b->align) {
        uint32_t nframes = b->size / b->align;
        uint64_t end = b->pos_fp >> 32, start = b->mixed_from_fp >> 32;
        if (!b->playing) start = end;
        if (end < start) end += nframes;   /* wrapped during the last chunk */
        uint64_t el = audio_now_us() - g_last_cb_us;
        uint64_t adv = el * b->freq / 1000000ull;
        uint64_t p = start + adv; if (p > end) p = end;
        play = (uint32_t)(p % nframes) * b->align;
        wr = (uint32_t)(end % nframes) * b->align;
    }
    SDL_UnlockMutex(g_mix);
    if (pp) WR32(pp, play);
    if (pw) WR32(pw, wr);
    STDRET(3, DS_OK);
}
static void B_GetFormat(CPU *c) {
    DSBuf *b = BUF(ARG(0)); uint32_t p = ARG(1), sz = ARG(2), pw = ARG(3);
    if (p && sz >= 16) {
        WR16(p, 1); WR16(p + 2, b->channels); WR32(p + 4, b->rate); WR32(p + 8, b->rate * b->align);
        WR16(p + 12, b->align); WR16(p + 14, b->bits); if (sz >= 18) WR16(p + 16, 0);
    }
    if (pw) WR32(pw, 18);
    STDRET(4, DS_OK);
}
static void B_GetVolume(CPU *c) { WR32(ARG(1), BUF(ARG(0))->volume); STDRET(2, DS_OK); }
static void B_GetPan(CPU *c) { WR32(ARG(1), BUF(ARG(0))->pan); STDRET(2, DS_OK); }
static void B_GetFrequency(CPU *c) { WR32(ARG(1), BUF(ARG(0))->freq); STDRET(2, DS_OK); }
static void B_GetStatus(CPU *c) {
    DSBuf *b = BUF(ARG(0));
    SDL_LockMutex(g_mix);
    uint32_t s = b->playing ? (1 | (b->looping ? 4 : 0)) : 0;
    SDL_UnlockMutex(g_mix);
    WR32(ARG(1), s);
    STDRET(2, DS_OK);
}
static void B_Initialize(CPU *c) { STDRET(3, DS_OK); }
static void B_Lock(CPU *c) {
    DSBuf *b = BUF(ARG(0)); uint32_t off = ARG(1), n = ARG(2), p1 = ARG(3), n1 = ARG(4), p2 = ARG(5), n2 = ARG(6), flags = ARG(7);
    if (!b->mem) { b->size = 65536; b->mem = galloc(b->size); }
    if (flags & 0x2) n = b->size;            /* DSBLOCK_ENTIREBUFFER */
    if (flags & 0x1) {                      /* DSBLOCK_FROMWRITECURSOR */
        off = (uint32_t)((b->pos_fp >> 32) * b->align) % b->size;
    }
    if (n > b->size) n = b->size;
    off %= b->size;
    uint32_t first = n, second = 0;
    if (off + n > b->size) { first = b->size - off; second = n - first; }
    WR32(p1, b->mem + off); WR32(n1, first);
    if (p2) WR32(p2, second ? b->mem : 0);
    if (n2) WR32(n2, second);
    STDRET(8, DS_OK);
}
static void B_Play(CPU *c) {
    DSBuf *b = BUF(ARG(0));
    SDL_LockMutex(g_mix); b->playing = 1; b->looping = (ARG(3) & 1) != 0; SDL_UnlockMutex(g_mix);
    STDRET(4, DS_OK);
}
static void B_SetCurrentPosition(CPU *c) {
    DSBuf *b = BUF(ARG(0));
    SDL_LockMutex(g_mix); b->pos_fp = (uint64_t)(ARG(1) / (b->align ? b->align : 1)) << 32; SDL_UnlockMutex(g_mix);
    STDRET(2, DS_OK);
}
static void B_SetFormat(CPU *c) {
    DSBuf *b = BUF(ARG(0)); uint32_t f = ARG(1);
    if (f) { b->channels = RD16(f + 2); b->rate = RD32(f + 4); b->bits = RD16(f + 14); b->align = b->channels * b->bits / 8; }
    rt_log("DS primary SetFormat %d Hz %d bit %d ch", b->rate, b->bits, b->channels);
    STDRET(2, DS_OK);
}
static void B_SetVolume(CPU *c) { DSBuf *b = BUF(ARG(0)); SDL_LockMutex(g_mix); b->volume = (int32_t)ARG(1); update_gain(b); SDL_UnlockMutex(g_mix); STDRET(2, DS_OK); }
static void B_SetPan(CPU *c) { DSBuf *b = BUF(ARG(0)); SDL_LockMutex(g_mix); b->pan = (int32_t)ARG(1); update_gain(b); SDL_UnlockMutex(g_mix); STDRET(2, DS_OK); }
static void B_SetFrequency(CPU *c) {
    DSBuf *b = BUF(ARG(0)); uint32_t f = ARG(1);
    SDL_LockMutex(g_mix); b->freq = f ? f : (uint32_t)b->rate; SDL_UnlockMutex(g_mix);
    STDRET(2, DS_OK);
}
static void B_Stop(CPU *c) { DSBuf *b = BUF(ARG(0)); SDL_LockMutex(g_mix); b->playing = 0; SDL_UnlockMutex(g_mix); STDRET(1, DS_OK); }
static void B_Unlock(CPU *c) { STDRET(5, DS_OK); }
static void B_Restore(CPU *c) { STDRET(1, DS_OK); }
static const GuestFn k_buf_vt[] = { B_QueryInterface, B_AddRef, B_Release, B_GetCaps, B_GetCurrentPosition, B_GetFormat,
    B_GetVolume, B_GetPan, B_GetFrequency, B_GetStatus, B_Initialize, B_Lock, B_Play, B_SetCurrentPosition, B_SetFormat,
    B_SetVolume, B_SetPan, B_SetFrequency, B_Stop, B_Unlock, B_Restore };
static const char *const k_buf_names[] = { "B_QI", "B_AddRef", "B_Release", "B_GetCaps", "B_GetCurrentPosition", "B_GetFormat",
    "B_GetVolume", "B_GetPan", "B_GetFrequency", "B_GetStatus", "B_Initialize", "B_Lock", "B_Play", "B_SetCurrentPosition", "B_SetFormat",
    "B_SetVolume", "B_SetPan", "B_SetFrequency", "B_Stop", "B_Unlock", "B_Restore" };

static DSBuf *new_buf(void) {
    DSBuf *b = calloc(1, sizeof *b);
    b->refs = 1; b->channels = 2; b->bits = 16; b->rate = 22050; b->align = 4; b->freq = 22050;
    update_gain(b);
    b->obj = com_make_object(k_buf_vt, k_buf_names, 21, 0, b);
    SDL_LockMutex(g_mix); b->next = g_bufs; g_bufs = b; SDL_UnlockMutex(g_mix);
    return b;
}

/* ================================================================== IDirectSound */
static void DS_QueryInterface(CPU *c) { WR32(ARG(2), ARG(0)); STDRET(3, DS_OK); }
static void DS_AddRef(CPU *c) { STDRET(1, 1); }
static void DS_Release(CPU *c) { STDRET(1, 0); }
static void DS_CreateSoundBuffer(CPU *c) {
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t flags = RD32(d + 4), bytes = RD32(d + 8), fmt = RD32(d + 16);
    DSBuf *b = new_buf();
    if (flags & 1) { b->primary = 1; b->size = 0; }
    else {
        if (fmt) { b->channels = RD16(fmt + 2); b->rate = RD32(fmt + 4); b->bits = RD16(fmt + 14); b->align = b->channels * b->bits / 8; if (!b->align) b->align = 1; b->freq = b->rate; }
        b->size = bytes; b->mem = galloc(bytes ? bytes : 16);
    }
    if (g_trace || getenv("PHOBIA_AUDIO_STATS")) rt_log("DS CreateSoundBuffer flags=%x bytes=%u %dHz %dbit %dch", flags, bytes, b->rate, b->bits, b->channels);
    WR32(out, b->obj);
    STDRET(4, DS_OK);
}
static void DS_GetCaps(CPU *c) { uint32_t p = ARG(1); uint32_t sz = RD32(p); memset(M + p, 0, sz ? sz : 96); WR32(p, sz); WR32(p + 4, 0x1 | 0x4 | 0x8 | 0x10 | 0x400 | 0x800); STDRET(2, DS_OK); }
static void DS_DuplicateSoundBuffer(CPU *c) {
    DSBuf *s = BUF(ARG(1)); uint32_t out = ARG(2);
    DSBuf *b = new_buf();
    b->mem = s->mem; b->size = s->size; b->channels = s->channels; b->bits = s->bits; b->rate = s->rate; b->align = s->align; b->freq = s->freq;
    b->volume = s->volume; b->pan = s->pan; update_gain(b);
    WR32(out, b->obj);
    STDRET(3, DS_OK);
}
static void DS_SetCooperativeLevel(CPU *c) { STDRET(3, DS_OK); }
static void DS_Compact(CPU *c) { STDRET(1, DS_OK); }
static void DS_GetSpeakerConfig(CPU *c) { WR32(ARG(1), 4); STDRET(2, DS_OK); }
static void DS_SetSpeakerConfig(CPU *c) { STDRET(2, DS_OK); }
static void DS_Initialize(CPU *c) { STDRET(2, DS_OK); }
static const GuestFn k_ds_vt[] = { DS_QueryInterface, DS_AddRef, DS_Release, DS_CreateSoundBuffer, DS_GetCaps, DS_DuplicateSoundBuffer,
    DS_SetCooperativeLevel, DS_Compact, DS_GetSpeakerConfig, DS_SetSpeakerConfig, DS_Initialize };
static const char *const k_ds_names[] = { "DS_QI", "DS_AddRef", "DS_Release", "DS_CreateSoundBuffer", "DS_GetCaps", "DS_DuplicateSoundBuffer",
    "DS_SetCooperativeLevel", "DS_Compact", "DS_GetSpeakerConfig", "DS_SetSpeakerConfig", "DS_Initialize" };

void IMP_DSOUND_DirectSoundCreate(CPU *c) {
    audio_init();
    if (getenv("PHOBIA_NOSOUND")) { STDRET(3, DSERR_GENERIC); return; }
    WR32(ARG(1), com_make_object(k_ds_vt, k_ds_names, 11, 0, NULL));
    rt_log("DirectSoundCreate ok");
    STDRET(3, DS_OK);
}

/* ================================================================== WINMM waveOut */
void IMP_WINMM_waveOutGetNumDevs(CPU *c) { STDRET(0, getenv("PHOBIA_NOSOUND") ? 0 : 1); }
void IMP_WINMM_waveOutOpen(CPU *c) {
    uint32_t ph = ARG(0), fmt = ARG(2), flags = ARG(5);
    audio_init();
    int ch = RD16(fmt + 2), rate = (int)RD32(fmt + 4), bits = RD16(fmt + 14);
    rt_log("waveOutOpen(%d Hz, %d bit, %d ch, flags=%x)", rate, bits, ch, flags);
    if (flags & 1) { STDRET(6, 0); return; }   /* WAVE_FORMAT_QUERY */
    SDL_LockMutex(g_mix);
    g_wo.channels = ch; g_wo.rate = rate; g_wo.bits = bits; g_wo.align = ch * bits / 8; g_wo.qh = g_wo.qt = 0; g_wo.cur_off = 0; g_wo.frac = 0; g_wo.open = 1;
    SDL_UnlockMutex(g_mix);
    if (ph) WR32(ph, 0xA0001);
    STDRET(6, 0);
}
void IMP_WINMM_waveOutPrepareHeader(CPU *c) { uint32_t h = ARG(1); WR32(h + 16, RD32(h + 16) | 0x2); STDRET(3, 0); }
void IMP_WINMM_waveOutUnprepareHeader(CPU *c) { uint32_t h = ARG(1); WR32(h + 16, RD32(h + 16) & ~0x2u); STDRET(3, 0); }
void IMP_WINMM_waveOutWrite(CPU *c) {
    uint32_t h = ARG(1);
    SDL_LockMutex(g_mix);
    WR32(h + 16, (RD32(h + 16) & ~0x1u) | 0x10);
    int n = (g_wo.qt + 1) % 256;
    if (n != g_wo.qh) { g_wo.queue[g_wo.qt] = h; g_wo.qt = n; }
    SDL_UnlockMutex(g_mix);
    STDRET(3, 0);
}
void IMP_WINMM_waveOutReset(CPU *c) {
    SDL_LockMutex(g_mix);
    while (g_wo.qh != g_wo.qt) { uint32_t h = g_wo.queue[g_wo.qh]; WR32(h + 16, (RD32(h + 16) & ~0x10u) | 0x1); g_wo.qh = (g_wo.qh + 1) % 256; }
    g_wo.cur_off = 0; g_wo.frac = 0;
    SDL_UnlockMutex(g_mix);
    STDRET(1, 0);
}
void IMP_WINMM_waveOutClose(CPU *c) { SDL_LockMutex(g_mix); g_wo.open = 0; SDL_UnlockMutex(g_mix); STDRET(1, 0); }
