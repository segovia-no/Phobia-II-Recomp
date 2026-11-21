// WINMM: joystick via SDL GameController, MCI MIDI sequencer via macOS AudioToolbox MusicPlayer.
#include "runtime.h"
#include "win32.h"
#include <SDL.h>
#include <pthread.h>
#include <strings.h>
#include <unistd.h>
#include <AudioToolbox/AudioToolbox.h>

/* ------------------------------------------------------------------ joystick */
static SDL_GameController *g_pad;
void joystick_handle_event(SDL_Event *e) {
    if (e->type == SDL_CONTROLLERDEVICEADDED && !g_pad) g_pad = SDL_GameControllerOpen(e->cdevice.which);
    if (e->type == SDL_CONTROLLERDEVICEREMOVED && g_pad) {
        SDL_Joystick *j = SDL_GameControllerGetJoystick(g_pad);
        if (SDL_JoystickInstanceID(j) == e->cdevice.which) { SDL_GameControllerClose(g_pad); g_pad = NULL; }
    }
}
void IMP_WINMM_joyGetNumDevs(CPU *c) { STDRET(0, 16); }
void IMP_WINMM_joyGetDevCapsA(CPU *c) {
    uint32_t id = ARG(0), p = ARG(1), sz = ARG(2);
    if (id != 0 || !g_pad) { STDRET(3, 167); return; }   /* JOYERR_UNPLUGGED */
    memset(M + p, 0, sz);
    WR16(p, 1); WR16(p + 2, 1);
    snprintf((char *)M + p + 4, 32, "%s", SDL_GameControllerName(g_pad) ? SDL_GameControllerName(g_pad) : "Gamepad");
    WR32(p + 36, 0); WR32(p + 40, 65535); WR32(p + 44, 0); WR32(p + 48, 65535); WR32(p + 52, 0); WR32(p + 56, 65535);
    WR32(p + 60, 4); WR32(p + 64, 10); WR32(p + 68, 1000);
    STDRET(3, 0);
}
void IMP_WINMM_joyGetPos(CPU *c) {
    uint32_t id = ARG(0), p = ARG(1);
    win32_pump_if_main();
    if (id != 0 || !g_pad) { STDRET(2, 167); return; }
    int ax = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
    int ay = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT)) ax = -32768;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) ax = 32767;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP)) ay = -32768;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN)) ay = 32767;
    uint32_t btn = 0;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A)) btn |= 1;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_B)) btn |= 2;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_X)) btn |= 4;
    if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_Y)) btn |= 8;
    WR32(p, (uint32_t)(ax + 32768)); WR32(p + 4, (uint32_t)(ay + 32768)); WR32(p + 8, 32768); WR32(p + 12, btn);
    STDRET(2, 0);
}

/* ------------------------------------------------------------------ MCI sequencer */
static MusicPlayer g_player;
static MusicSequence g_seq;
static MusicTimeStamp g_seq_len;
static int g_playing, g_notify;
static pthread_mutex_t g_midi_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_midi_thread;

static void midi_close(void) {
    if (g_player) { MusicPlayerStop(g_player); DisposeMusicPlayer(g_player); g_player = NULL; }
    if (g_seq) { DisposeMusicSequence(g_seq); g_seq = NULL; }
    g_playing = 0; g_notify = 0;
}
static int midi_open(const char *gpath) {
    char hp[1024]; host_path(gpath, hp, sizeof hp);
    midi_close();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)hp, (CFIndex)strlen(hp), false);
    if (NewMusicSequence(&g_seq) != noErr) return 0;
    OSStatus st = MusicSequenceFileLoad(g_seq, url, kMusicSequenceFile_MIDIType, 0);
    CFRelease(url);
    if (st != noErr) { rt_log("MIDI load failed (%d): %s", (int)st, hp); DisposeMusicSequence(g_seq); g_seq = NULL; return 0; }
    UInt32 n = 0; MusicSequenceGetTrackCount(g_seq, &n);
    g_seq_len = 0;
    for (UInt32 i = 0; i < n; i++) {
        MusicTrack t; MusicTimeStamp len = 0; UInt32 sz = sizeof len;
        MusicSequenceGetIndTrack(g_seq, i, &t);
        MusicTrackGetProperty(t, kSequenceTrackProperty_TrackLength, &len, &sz);
        if (len > g_seq_len) g_seq_len = len;
    }
    NewMusicPlayer(&g_player);
    MusicPlayerSetSequence(g_player, g_seq);
    MusicPlayerPreroll(g_player);
    rt_log("MIDI opened %s (%.1f beats)", hp, g_seq_len);
    return 1;
}
static void *midi_watch(void *arg) {
    for (;;) {
        usleep(100000);
        pthread_mutex_lock(&g_midi_lock);
        if (g_player && g_playing) {
            MusicTimeStamp t = 0; MusicPlayerGetTime(g_player, &t);
            if (t >= g_seq_len) {
                MusicPlayerStop(g_player);
                g_playing = 0;
                if (g_notify) { g_notify = 0; win32_post_message(g_hwnd_main, 0x3B9, 1, 1); }  /* MM_MCINOTIFY, SUCCESSFUL */
            }
        }
        pthread_mutex_unlock(&g_midi_lock);
    }
    return NULL;
}
void IMP_WINMM_mciSendStringA(CPU *c) {
    const char *cmd = HS(ARG(0)); uint32_t ret = ARG(1), retlen = ARG(2);
    rt_log("mciSendString(\"%s\")", cmd);
    if (ret && retlen) M[ret] = 0;
    if (getenv("PHOBIA_NOMIDI")) { STDRET(4, 0); return; }
    if (!g_midi_thread) pthread_create(&g_midi_thread, NULL, midi_watch, NULL);
    pthread_mutex_lock(&g_midi_lock);
    uint32_t rc = 0;
    char buf[512]; snprintf(buf, sizeof buf, "%s", cmd);
    char *save, *verb = strtok_r(buf, " ", &save);
    if (verb && !strcasecmp(verb, "open")) {
        char *file = strtok_r(NULL, " ", &save);
        if (!file || !midi_open(file)) rc = 275;   /* MCIERR_FILE_NOT_FOUND */
    } else if (verb && !strcasecmp(verb, "close")) {
        midi_close();
    } else if (verb && !strcasecmp(verb, "play")) {
        int from0 = strcasestr(cmd, "from 0") != NULL;
        g_notify = strcasestr(cmd, "notify") != NULL;
        if (g_player) {
            if (from0) MusicPlayerSetTime(g_player, 0);
            MusicTimeStamp t = 0; MusicPlayerGetTime(g_player, &t);
            if (t >= g_seq_len) MusicPlayerSetTime(g_player, 0);
            MusicPlayerStart(g_player); g_playing = 1;
        } else rc = 263;
    } else if (verb && (!strcasecmp(verb, "stop") || !strcasecmp(verb, "pause"))) {
        if (g_player) MusicPlayerStop(g_player);
        g_playing = 0;
    }
    pthread_mutex_unlock(&g_midi_lock);
    STDRET(4, rc);
}
