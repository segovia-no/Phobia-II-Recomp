// DirectInput emulation (keyboard device; state from SDL scancodes).
#include "runtime.h"
#include "win32.h"

#define DI_OK 0
#define E_NOINTERFACE 0x80004002u

typedef struct { int kind; } Dev;   /* 0 keyboard, 1 mouse, 2 other */

static void DEV_QueryInterface(CPU *c) { WR32(ARG(2), ARG(0)); STDRET(3, DI_OK); }
static void DEV_AddRef(CPU *c) { STDRET(1, 1); }
static void DEV_Release(CPU *c) { STDRET(1, 0); }
static void DEV_GetCapabilities(CPU *c) { STDRET(2, DI_OK); }
static void DEV_EnumObjects(CPU *c) { STDRET(4, DI_OK); }
static void DEV_GetProperty(CPU *c) { STDRET(3, DI_OK); }
static void DEV_SetProperty(CPU *c) { STDRET(3, DI_OK); }
static void DEV_Acquire(CPU *c) { STDRET(1, DI_OK); }
static void DEV_Unacquire(CPU *c) { STDRET(1, DI_OK); }
static void DEV_GetDeviceState(CPU *c) {
    Dev *d = com_host(ARG(0)); uint32_t n = ARG(1), p = ARG(2);
    win32_pump_if_main();
    if (d && d->kind == 0) {
        memcpy(M + p, g_dik_state, n < 256 ? n : 256);
    } else memset(M + p, 0, n);
    STDRET(3, DI_OK);
}
static void DEV_GetDeviceData(CPU *c) { WR32(ARG(3), 0); STDRET(5, DI_OK); }
static void DEV_SetDataFormat(CPU *c) { STDRET(2, DI_OK); }
static void DEV_SetEventNotification(CPU *c) { STDRET(2, DI_OK); }
static void DEV_SetCooperativeLevel(CPU *c) { STDRET(3, DI_OK); }
static void DEV_GetObjectInfo(CPU *c) { STDRET(4, 0x80004001u); }
static void DEV_GetDeviceInfo(CPU *c) { STDRET(2, 0x80004001u); }
static void DEV_RunControlPanel(CPU *c) { STDRET(3, DI_OK); }
static void DEV_Initialize(CPU *c) { STDRET(4, DI_OK); }
static const GuestFn k_dev_vt[] = { DEV_QueryInterface, DEV_AddRef, DEV_Release, DEV_GetCapabilities, DEV_EnumObjects,
    DEV_GetProperty, DEV_SetProperty, DEV_Acquire, DEV_Unacquire, DEV_GetDeviceState, DEV_GetDeviceData, DEV_SetDataFormat,
    DEV_SetEventNotification, DEV_SetCooperativeLevel, DEV_GetObjectInfo, DEV_GetDeviceInfo, DEV_RunControlPanel, DEV_Initialize };
static const char *const k_dev_names[] = { "DEV_QI", "DEV_AddRef", "DEV_Release", "DEV_GetCapabilities", "DEV_EnumObjects",
    "DEV_GetProperty", "DEV_SetProperty", "DEV_Acquire", "DEV_Unacquire", "DEV_GetDeviceState", "DEV_GetDeviceData", "DEV_SetDataFormat",
    "DEV_SetEventNotification", "DEV_SetCooperativeLevel", "DEV_GetObjectInfo", "DEV_GetDeviceInfo", "DEV_RunControlPanel", "DEV_Initialize" };

/* GUID_SysKeyboard = 6F1D2B61-D5A0-11CF-BFC7-444553540000, GUID_SysMouse = 6F1D2B60-... */
static void DI_QueryInterface(CPU *c) { WR32(ARG(2), ARG(0)); STDRET(3, DI_OK); }
static void DI_AddRef(CPU *c) { STDRET(1, 1); }
static void DI_Release(CPU *c) { STDRET(1, 0); }
static void DI_CreateDevice(CPU *c) {
    uint32_t g = ARG(1), out = ARG(2);
    Dev *d = calloc(1, sizeof *d);
    uint32_t d1 = RD32(g);
    d->kind = d1 == 0x6F1D2B61u ? 0 : d1 == 0x6F1D2B60u ? 1 : 2;
    rt_log("DirectInput CreateDevice(%08x) kind=%d", d1, d->kind);
    WR32(out, com_make_object(k_dev_vt, k_dev_names, 18, 0, d));
    STDRET(4, DI_OK);
}
static void DI_EnumDevices(CPU *c) { STDRET(5, DI_OK); }
static void DI_GetDeviceStatus(CPU *c) { STDRET(2, DI_OK); }
static void DI_RunControlPanel(CPU *c) { STDRET(3, DI_OK); }
static void DI_Initialize(CPU *c) { STDRET(3, DI_OK); }
static const GuestFn k_di_vt[] = { DI_QueryInterface, DI_AddRef, DI_Release, DI_CreateDevice, DI_EnumDevices, DI_GetDeviceStatus, DI_RunControlPanel, DI_Initialize };
static const char *const k_di_names[] = { "DI_QI", "DI_AddRef", "DI_Release", "DI_CreateDevice", "DI_EnumDevices", "DI_GetDeviceStatus", "DI_RunControlPanel", "DI_Initialize" };

void IMP_DINPUT_DirectInputCreateA(CPU *c) {
    uint32_t out = ARG(2);
    WR32(out, com_make_object(k_di_vt, k_di_names, 8, 0, NULL));
    rt_log("DirectInputCreateA(version=%x)", ARG(1));
    STDRET(4, DI_OK);
}
