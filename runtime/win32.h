#pragma once
#include "runtime.h"
#define IMAGE_BASE_ADDR 0x400000u
enum { HT_FILE = 1, HT_EVENT, HT_THREAD, HT_FIND };
uint32_t handle_new(int type);
uint32_t host_ticks(void);
void win32_pump_if_main(void);     /* pump SDL events when called on main thread */
void kernel32_init(void);
void user32_init(void);
/* window/message system */
void win32_post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
extern uint32_t g_hwnd_main, g_wndproc_main;
/* video / input / audio subsystems */
void video_init(void);
void video_present(const uint8_t *pixels, int w, int h, int bpp, int pitch, const uint32_t *palette_rgba);
void audio_init(void);
void input_init(void);
extern uint8_t g_dik_state[256];      /* DirectInput keyboard state (0x80 = down) */
int sdl_to_dik(int scancode);
int sdl_to_vk(int sym, int scancode);
