// KERNEL32 shims (stdcall). Guest handles index a host handle table.
#include "runtime.h"
#include "win32.h"
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <strings.h>
#include <time.h>
#include <SDL.h>

static uint32_t g_last_error;

/* ------------------------------------------------------------------ handle table */
#define MAX_HANDLES 1024
typedef struct {
    int type;
    int fd;
    /* events */
    pthread_mutex_t mu; pthread_cond_t cv; int manual, signaled;
    /* threads */
    pthread_t th; int finished;
    /* find */
    DIR *dir; char dirpath[HOST_PATH_MAX]; char pattern[260];
} Handle;
static Handle *g_handles[MAX_HANDLES];
static pthread_mutex_t g_handles_lock = PTHREAD_MUTEX_INITIALIZER;

uint32_t handle_new(int type) {
    pthread_mutex_lock(&g_handles_lock);
    for (int i = 8; i < MAX_HANDLES; i++) {
        if (!g_handles[i]) {
            Handle *h = calloc(1, sizeof(Handle)); h->type = type; h->fd = -1;
            pthread_mutex_init(&h->mu, NULL); pthread_cond_init(&h->cv, NULL);
            g_handles[i] = h;
            pthread_mutex_unlock(&g_handles_lock);
            return (uint32_t)(i * 4);
        }
    }
    pthread_mutex_unlock(&g_handles_lock);
    rt_fatal("out of handles");
}
static Handle *handle_get(uint32_t h) {
    uint32_t i = h / 4;
    if (h & 3 || i >= MAX_HANDLES) return NULL;
    return g_handles[i];
}
static void handle_free(uint32_t h) {
    uint32_t i = h / 4;
    if (i < MAX_HANDLES && g_handles[i]) {
        Handle *x = g_handles[i];
        if (x->fd > 2) close(x->fd);
        if (x->dir) closedir(x->dir);
        g_handles[i] = NULL;
        if (x->type != HT_THREAD) free(x);   /* thread structs referenced by running thread */
    }
}
static void init_std_handles(void) {
    for (int i = 0; i < 3; i++) {
        Handle *h = calloc(1, sizeof(Handle)); h->type = HT_FILE; h->fd = i;
        g_handles[1 + i] = h;   /* handles 4, 8, 12 */
    }
}

/* ------------------------------------------------------------------ process / module */
void IMP_KERNEL32_GetModuleHandleA(CPU *c) {
    uint32_t name = ARG(0);
    if (!name) { STDRET(1, IMAGE_BASE_ADDR); return; }
    const char *n = HS(name);
    if (!strncasecmp(n, "game", 4)) { STDRET(1, IMAGE_BASE_ADDR); return; }
    if (!strncasecmp(n, "kernel32", 8)) { STDRET(1, 0x7FF00000u); return; }
    STDRET(1, 0);
}
static uint32_t g_cmdline, g_cmdlinew, g_env;
void IMP_KERNEL32_GetCommandLineA(CPU *c) { if (!g_cmdline) g_cmdline = gstrdup("Game.exe"); STDRET(0, g_cmdline); }
void IMP_KERNEL32_GetCommandLineW(CPU *c) {
    if (!g_cmdlinew) { const char *s = "Game.exe"; g_cmdlinew = galloc(64); for (int i = 0; s[i]; i++) WR16(g_cmdlinew + 2 * i, s[i]); }
    STDRET(0, g_cmdlinew);
}
void IMP_KERNEL32_GetEnvironmentStrings(CPU *c) {
    if (!g_env) {
        static const char env[] = "PATH=C:\\WINDOWS\0TEMP=C:\\TEMP\0\0";
        g_env = galloc(sizeof env); memcpy(M + g_env, env, sizeof env);
    }
    STDRET(0, g_env);
}
void IMP_KERNEL32_FreeEnvironmentStringsA(CPU *c) { STDRET(1, 1); }
void IMP_KERNEL32_SetEnvironmentVariableA(CPU *c) { STDRET(2, 1); }
void IMP_KERNEL32_SetEnvironmentVariableW(CPU *c) { STDRET(2, 1); }
void IMP_KERNEL32_GetVersion(CPU *c) { STDRET(0, 0xC0000A04u); }  /* Windows 98 */
void IMP_KERNEL32_GetVersionExA(CPU *c) {
    uint32_t p = ARG(0);
    WR32(p + 4, 4); WR32(p + 8, 10); WR32(p + 12, 0x040A07CE); WR32(p + 16, 1);
    memset(M + p + 20, 0, 128);
    STDRET(1, 1);
}
static const char *k_exe_path = "C:\\PHOBIA2\\Game.exe";
void IMP_KERNEL32_GetModuleFileNameA(CPU *c) {
    uint32_t buf = ARG(1), n = ARG(2);
    uint32_t l = (uint32_t)strlen(k_exe_path);
    if (l >= n) l = n - 1;
    memcpy(M + buf, k_exe_path, l); M[buf + l] = 0;
    STDRET(3, l);
}
void IMP_KERNEL32_GetModuleFileNameW(CPU *c) {
    uint32_t buf = ARG(1), n = ARG(2);
    uint32_t l = (uint32_t)strlen(k_exe_path);
    if (l >= n) l = n - 1;
    for (uint32_t i = 0; i < l; i++) WR16(buf + 2 * i, k_exe_path[i]);
    WR16(buf + 2 * l, 0);
    STDRET(3, l);
}
void IMP_KERNEL32_GetCurrentProcessId(CPU *c) { STDRET(0, 0x1000); }
void IMP_KERNEL32_GetCurrentThreadId(CPU *c) { STDRET(0, RD32(c->fs_base + 0x24)); }
void IMP_KERNEL32_GetCurrentThread(CPU *c) { STDRET(0, 0xFFFFFFFEu); }
void IMP_KERNEL32_ExitProcess(CPU *c) { rt_log("ExitProcess(%u)", ARG(0)); SDL_Quit(); exit((int)ARG(0)); }
void IMP_KERNEL32_SetUnhandledExceptionFilter(CPU *c) { STDRET(1, 0); }
void IMP_KERNEL32_UnhandledExceptionFilter(CPU *c) { STDRET(1, 1); }
void IMP_KERNEL32_OutputDebugStringA(CPU *c) { rt_log("[debug] %s", HS(ARG(0))); STDRET(1, 0); }
void IMP_KERNEL32_GetLastError(CPU *c) { STDRET(0, g_last_error); }
void IMP_KERNEL32_SetLastError(CPU *c) { g_last_error = ARG(0); STDRET(1, 0); }
void IMP_KERNEL32_GetACP(CPU *c) { STDRET(0, 1252); }
void IMP_KERNEL32_GetOEMCP(CPU *c) { STDRET(0, 437); }
void IMP_KERNEL32_GetCPInfo(CPU *c) {
    uint32_t p = ARG(1);
    memset(M + p, 0, 20); WR32(p, 1); M[p + 4] = '?';
    STDRET(2, 1);
}
void IMP_KERNEL32_MultiByteToWideChar(CPU *c) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); uint32_t dn = ARG(5);
    if (n < 0) n = (int32_t)strlen(HS(src)) + 1;
    if (!dn) { STDRET(6, n); return; }
    int32_t k = 0;
    for (; k < n && (uint32_t)k < dn; k++) WR16(dst + 2 * k, M[src + k]);
    STDRET(6, k);
}
void IMP_KERNEL32_WideCharToMultiByte(CPU *c) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); uint32_t dn = ARG(5);
    if (n < 0) { n = 0; while (RD16(src + 2 * n)) n++; n++; }
    if (!dn) { STDRET(8, n); return; }
    int32_t k = 0;
    for (; k < n && (uint32_t)k < dn; k++) { uint16_t w = RD16(src + 2 * k); M[dst + k] = w < 256 ? (uint8_t)w : '?'; }
    STDRET(8, k);
}

/* ------------------------------------------------------------------ console / std */
void IMP_KERNEL32_GetStdHandle(CPU *c) {
    int32_t n = (int32_t)ARG(0);
    STDRET(1, n == -10 ? 4 : n == -11 ? 8 : n == -12 ? 12 : 0xFFFFFFFFu);
}
void IMP_KERNEL32_SetStdHandle(CPU *c) { STDRET(2, 1); }
void IMP_KERNEL32_GetFileType(CPU *c) {
    uint32_t h = ARG(0);
    Handle *x = handle_get(h);
    if (!x) { STDRET(1, 0); return; }
    STDRET(1, (x->fd >= 0 && x->fd <= 2) ? 2 : 1);
}
void IMP_KERNEL32_GetConsoleMode(CPU *c) { STDRET(2, 0); }
void IMP_KERNEL32_SetConsoleMode(CPU *c) { STDRET(2, 0); }
void IMP_KERNEL32_SetConsoleCtrlHandler(CPU *c) { STDRET(2, 1); }
void IMP_KERNEL32_ReadConsoleInputA(CPU *c) { WR32(ARG(3), 0); STDRET(4, 0); }
void IMP_KERNEL32_WriteConsoleA(CPU *c) {
    { const char *bw = getenv("PHOBIA_BREAK_WRITE");
      if (bw && memmem(M + ARG(1), ARG(2), bw, strlen(bw))) rt_fatal("break on write: %.*s", (int)ARG(2), (char *)M + ARG(1)); }
    fwrite(M + ARG(1), 1, ARG(2), stderr);
    if (ARG(3)) WR32(ARG(3), ARG(2));
    STDRET(5, 1);
}

/* ------------------------------------------------------------------ files */
void IMP_KERNEL32_CreateFileA(CPU *c) {
    const char *gp = HS(ARG(0)); uint32_t access = ARG(1), disp = ARG(4);
    char hp[HOST_PATH_MAX]; host_path(gp, hp, sizeof hp);
    int fl = 0;
    if ((access & 0xC0000000u) == 0xC0000000u) fl = O_RDWR;
    else if (access & 0x40000000u) fl = O_WRONLY;
    else fl = O_RDONLY;
    switch (disp) {
        case 1: fl |= O_CREAT | O_EXCL; break;
        case 2: fl |= O_CREAT | O_TRUNC; break;
        case 3: break;
        case 4: fl |= O_CREAT; break;
        case 5: fl |= O_TRUNC; break;
    }
    int fd = open(hp, fl, 0644);
    if (g_trace) rt_log("CreateFileA(%s -> %s, %x, %u) = %d", gp, hp, access, disp, fd);
    if (fd < 0) { g_last_error = errno == ENOENT ? 2 : 5; STDRET(7, 0xFFFFFFFFu); return; }
    uint32_t h = handle_new(HT_FILE); handle_get(h)->fd = fd;
    g_last_error = 0;
    STDRET(7, h);
}
void IMP_KERNEL32_ReadFile(CPU *c) {
    Handle *x = handle_get(ARG(0)); uint32_t buf = ARG(1), n = ARG(2), pr = ARG(3);
    if (!x || x->fd < 0) { STDRET(5, 0); return; }
    ssize_t r = read(x->fd, M + buf, n);
    if (pr) WR32(pr, r < 0 ? 0 : (uint32_t)r);
    STDRET(5, r >= 0);
}
void IMP_KERNEL32_WriteFile(CPU *c) {
    Handle *x = handle_get(ARG(0)); uint32_t buf = ARG(1), n = ARG(2), pw = ARG(3);
    if (!x || x->fd < 0) { STDRET(5, 0); return; }
    if (x->fd == 2 || x->fd == 1) {
        const char *bw = getenv("PHOBIA_BREAK_WRITE");
        if (bw && memmem(M + buf, n, bw, strlen(bw))) rt_fatal("break on write: %.*s", (int)n, (char *)M + buf);
    }
    if ((x->fd == 1 || x->fd == 2) && !getenv("PHOBIA_CONSOLE")) {
        /* GUI-subsystem process: Windows has no console attached, std output goes nowhere */
        if (pw) WR32(pw, n);
        STDRET(5, 1); return;
    }
    ssize_t r = write(x->fd, M + buf, n);
    if (pw) WR32(pw, r < 0 ? 0 : (uint32_t)r);
    STDRET(5, r >= 0);
}
void IMP_KERNEL32_SetFilePointer(CPU *c) {
    Handle *x = handle_get(ARG(0)); int32_t lo = (int32_t)ARG(1); uint32_t phi = ARG(2), method = ARG(3);
    if (!x || x->fd < 0) { STDRET(4, 0xFFFFFFFFu); return; }
    off_t off = lo;
    if (phi) off = (off_t)(((uint64_t)RD32(phi) << 32) | (uint32_t)lo);
    off_t r = lseek(x->fd, off, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    if (phi) WR32(phi, (uint32_t)((uint64_t)r >> 32));
    STDRET(4, (uint32_t)r);
}
void IMP_KERNEL32_FlushFileBuffers(CPU *c) { STDRET(1, 1); }
void IMP_KERNEL32_CloseHandle(CPU *c) {
    uint32_t h = ARG(0);
    if (h >= 16) handle_free(h);
    STDRET(1, 1);
}
void IMP_KERNEL32_DeleteFileA(CPU *c) { char hp[HOST_PATH_MAX]; host_path(HS(ARG(0)), hp, sizeof hp); STDRET(1, unlink(hp) == 0); }
void IMP_KERNEL32_MoveFileA(CPU *c) {
    char a[HOST_PATH_MAX], b[HOST_PATH_MAX]; host_path(HS(ARG(0)), a, sizeof a); host_path(HS(ARG(1)), b, sizeof b);
    STDRET(2, rename(a, b) == 0);
}
void IMP_KERNEL32_GetFileAttributesA(CPU *c) {
    char hp[HOST_PATH_MAX]; host_path(HS(ARG(0)), hp, sizeof hp);
    struct stat st;
    if (stat(hp, &st)) { g_last_error = 2; STDRET(1, 0xFFFFFFFFu); return; }
    STDRET(1, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
}
void IMP_KERNEL32_GetFullPathNameA(CPU *c) {
    const char *n = HS(ARG(0)); uint32_t len = ARG(1), buf = ARG(2), fp = ARG(3);
    char tmp[HOST_PATH_MAX];
    if (n[0] && n[1] == ':') snprintf(tmp, sizeof tmp, "%s", n);
    else if (n[0] == '\\') snprintf(tmp, sizeof tmp, "C:%s", n);
    else snprintf(tmp, sizeof tmp, "C:\\PHOBIA2\\%s", n);
    uint32_t l = (uint32_t)strlen(tmp);
    if (l + 1 > len) { STDRET(4, l + 1); return; }
    memcpy(M + buf, tmp, l + 1);
    if (fp) { char *s = strrchr(tmp, '\\'); WR32(fp, buf + (uint32_t)(s ? s - tmp + 1 : 0)); }
    STDRET(4, l);
}
void IMP_KERNEL32_GetCurrentDirectoryA(CPU *c) {
    uint32_t n = ARG(0), buf = ARG(1);
    const char *d = "C:\\PHOBIA2";
    uint32_t l = (uint32_t)strlen(d);
    if (n < l + 1) { STDRET(2, l + 1); return; }
    memcpy(M + buf, d, l + 1);
    STDRET(2, l);
}
void IMP_KERNEL32_SetCurrentDirectoryA(CPU *c) { STDRET(1, 1); }
void IMP_KERNEL32__lopen(CPU *c) {
    char hp[HOST_PATH_MAX]; host_path(HS(ARG(0)), hp, sizeof hp);
    uint32_t mode = ARG(1) & 3;
    int fd = open(hp, mode == 0 ? O_RDONLY : mode == 1 ? O_WRONLY : O_RDWR);
    if (g_trace) rt_log("_lopen(%s) = %d", hp, fd);
    if (fd < 0) { STDRET(2, 0xFFFFFFFFu); return; }
    uint32_t h = handle_new(HT_FILE); handle_get(h)->fd = fd;
    STDRET(2, h);
}
void IMP_KERNEL32__lread(CPU *c) {
    Handle *x = handle_get(ARG(0));
    if (!x || x->fd < 0) { STDRET(3, 0xFFFFFFFFu); return; }
    ssize_t r = read(x->fd, M + ARG(1), ARG(2));
    STDRET(3, (uint32_t)r);
}
void IMP_KERNEL32__lclose(CPU *c) { handle_free(ARG(0)); STDRET(1, 0); }

/* file times: FILETIME = 100ns since 1601 */
static uint64_t unix_to_ft(time_t t) { return ((uint64_t)t + 11644473600ull) * 10000000ull; }
void IMP_KERNEL32_DosDateTimeToFileTime(CPU *c) {
    uint16_t d = (uint16_t)ARG(0), t = (uint16_t)ARG(1); uint32_t ft = ARG(2);
    struct tm tm = {0};
    tm.tm_year = ((d >> 9) & 0x7f) + 80; tm.tm_mon = ((d >> 5) & 15) - 1; tm.tm_mday = d & 31;
    tm.tm_hour = t >> 11; tm.tm_min = (t >> 5) & 63; tm.tm_sec = (t & 31) * 2;
    WR64(ft, unix_to_ft(timegm(&tm)));
    STDRET(3, 1);
}
void IMP_KERNEL32_FileTimeToDosDateTime(CPU *c) {
    uint64_t ft = RD64(ARG(0));
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tm; gmtime_r(&t, &tm);
    if (ARG(1)) WR16(ARG(1), (uint16_t)(((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday));
    if (ARG(2)) WR16(ARG(2), (uint16_t)((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2)));
    STDRET(3, 1);
}
void IMP_KERNEL32_FileTimeToLocalFileTime(CPU *c) { WR64(ARG(1), RD64(ARG(0))); STDRET(2, 1); }
void IMP_KERNEL32_LocalFileTimeToFileTime(CPU *c) { WR64(ARG(1), RD64(ARG(0))); STDRET(2, 1); }
void IMP_KERNEL32_GetTimeZoneInformation(CPU *c) { memset(M + ARG(0), 0, 172); STDRET(1, 0); }

/* find */
static int find_fill(Handle *x, uint32_t fd) {
    struct dirent *e;
    while ((e = readdir(x->dir))) {
        if (fnmatch(x->pattern, e->d_name, FNM_CASEFOLD) == 0 ||
            (!strcmp(x->pattern, "*.*"))) {
            char p[HOST_PATH_MAX * 2]; snprintf(p, sizeof p, "%s/%s", x->dirpath, e->d_name);
            struct stat st; stat(p, &st);
            memset(M + fd, 0, 320);
            WR32(fd, S_ISDIR(st.st_mode) ? 0x10 : 0x20);
            uint64_t ft = unix_to_ft(st.st_mtime);
            WR64(fd + 4, ft); WR64(fd + 12, ft); WR64(fd + 20, ft);
            WR32(fd + 32, 0); WR32(fd + 28, 0); WR32(fd + 32, (uint32_t)st.st_size);
            snprintf((char *)M + fd + 44, 260, "%s", e->d_name);
            return 1;
        }
    }
    return 0;
}
void IMP_KERNEL32_FindFirstFileA(CPU *c) {
    const char *gp = HS(ARG(0)); uint32_t fd = ARG(1);
    char hp[HOST_PATH_MAX]; host_path(gp, hp, sizeof hp);
    char *slash = strrchr(hp, '/');
    uint32_t h = handle_new(HT_FIND); Handle *x = handle_get(h);
    if (slash) { *slash = 0; snprintf(x->dirpath, sizeof x->dirpath, "%s", hp); snprintf(x->pattern, sizeof x->pattern, "%s", slash + 1); }
    else { snprintf(x->dirpath, sizeof x->dirpath, "."); snprintf(x->pattern, sizeof x->pattern, "%s", hp); }
    x->dir = opendir(x->dirpath);
    if (!x->dir || !find_fill(x, fd)) { handle_free(h); g_last_error = 2; STDRET(2, 0xFFFFFFFFu); return; }
    STDRET(2, h);
}
void IMP_KERNEL32_FindNextFileA(CPU *c) {
    Handle *x = handle_get(ARG(0));
    if (!x || !x->dir) { STDRET(2, 0); return; }
    int ok = find_fill(x, ARG(1));
    if (!ok) g_last_error = 18;
    STDRET(2, ok);
}
void IMP_KERNEL32_FindClose(CPU *c) { handle_free(ARG(0)); STDRET(1, 1); }

/* ------------------------------------------------------------------ memory */
void IMP_KERNEL32_GlobalAlloc(CPU *c) { STDRET(2, galloc(ARG(1) ? ARG(1) : 1)); }
void IMP_KERNEL32_GlobalFree(CPU *c) { gfree(ARG(0)); STDRET(1, 0); }
void IMP_KERNEL32_GlobalLock(CPU *c) { STDRET(1, ARG(0)); }
void IMP_KERNEL32_GlobalUnlock(CPU *c) { STDRET(1, 1); }
void IMP_KERNEL32_GlobalMemoryStatus(CPU *c) {
    uint32_t p = ARG(0);
    WR32(p, 32); WR32(p + 4, 10); WR32(p + 8, 256u << 20); WR32(p + 12, 192u << 20);
    WR32(p + 16, 512u << 20); WR32(p + 20, 512u << 20); WR32(p + 24, 1024u << 20); WR32(p + 28, 900u << 20);
    STDRET(1, 0);
}
void IMP_KERNEL32_VirtualAlloc(CPU *c) {
    uint32_t a = gvirtual_alloc(ARG(0), ARG(1));
    if (g_trace) rt_log("VirtualAlloc(%08x, %x, %x) = %08x", ARG(0), ARG(1), ARG(2), a);
    STDRET(4, a);
}
void IMP_KERNEL32_VirtualFree(CPU *c) { STDRET(3, 1); }
void IMP_KERNEL32_VirtualQuery(CPU *c) {
    uint32_t a = ARG(0), mbi = ARG(1);
    memset(M + mbi, 0, 28);
    WR32(mbi, a & ~0xFFFu); WR32(mbi + 4, a & ~0xFFFu); WR32(mbi + 8, 4);
    WR32(mbi + 12, 0x1000); WR32(mbi + 16, 0x1000); WR32(mbi + 20, 4); WR32(mbi + 24, 0x20000);
    STDRET(3, 28);
}

/* ------------------------------------------------------------------ libraries / resources */
void IMP_KERNEL32_LoadLibraryA(CPU *c) {
    const char *n = HS(ARG(0));
    uint32_t h = 0;
    if (!strncasecmp(n, "ddraw", 5)) h = 0x71000000u;
    else if (!strncasecmp(n, "dinput", 6)) h = 0x72000000u;
    else if (!strncasecmp(n, "dsound", 6)) h = 0x73000000u;
    else if (!strncasecmp(n, "kernel32", 8) || !strncasecmp(n, "user32", 6) || !strncasecmp(n, "gdi32", 5) || !strncasecmp(n, "winmm", 5)) h = 0x74000000u;
    rt_log("LoadLibraryA(%s) = %08x", n, h);
    STDRET(1, h);
}
void IMP_KERNEL32_FreeLibrary(CPU *c) { STDRET(1, 1); }
void IMP_KERNEL32_GetProcAddress(CPU *c) {
    uint32_t name = ARG(1);
    if (name < 0x10000) { STDRET(2, 0); return; }
    const char *n = HS(name);
    uint32_t t = thunk_for_import(NULL, n);
    rt_log("GetProcAddress(%s) = %08x", n, t);
    STDRET(2, t);
}
void IMP_KERNEL32_FindResourceA(CPU *c) { STDRET(3, 0); }
void IMP_KERNEL32_LoadResource(CPU *c) { STDRET(2, 0); }
void IMP_KERNEL32_LockResource(CPU *c) { STDRET(1, 0); }

/* ------------------------------------------------------------------ time */
static uint64_t g_t0_ms;
uint32_t host_ticks(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ms = (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    if (!g_t0_ms) g_t0_ms = ms - 100000;
    return (uint32_t)(ms - g_t0_ms);
}
void IMP_KERNEL32_GetTickCount(CPU *c) { STDRET(0, host_ticks()); }
void IMP_KERNEL32_Sleep(CPU *c) {
    uint32_t ms = ARG(0);
    win32_pump_if_main();
    if (ms) SDL_Delay(ms); else sched_yield();
    STDRET(1, 0);
}

/* ------------------------------------------------------------------ sync */
typedef struct { pthread_mutex_t m; } HostCS;
static HostCS *g_cs[8192];
static int g_ncs = 1;
static pthread_mutex_t g_cs_lock = PTHREAD_MUTEX_INITIALIZER;
static HostCS *cs_init(uint32_t cs) {
    HostCS *h = malloc(sizeof *h);
    pthread_mutexattr_t at; pthread_mutexattr_init(&at); pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&h->m, &at);
    pthread_mutex_lock(&g_cs_lock);
    int i = g_ncs++;
    if (i >= 8192) rt_fatal("too many critical sections");
    g_cs[i] = h;
    memset(M + cs, 0, 24);
    WR32(cs, 0x43530000u | (uint32_t)i);
    pthread_mutex_unlock(&g_cs_lock);
    return h;
}
void IMP_KERNEL32_InitializeCriticalSection(CPU *c) { cs_init(ARG(0)); STDRET(1, 0); }
static HostCS *cs_get(uint32_t cs) {
    uint32_t v = RD32(cs);
    if ((v & 0xFFFF0000u) != 0x43530000u) return cs_init(cs);
    return g_cs[v & 0xFFFF];
}
void IMP_KERNEL32_DeleteCriticalSection(CPU *c) { STDRET(1, 0); }
void IMP_KERNEL32_EnterCriticalSection(CPU *c) { HostCS *h = cs_get(ARG(0)); if (h) pthread_mutex_lock(&h->m); STDRET(1, 0); }
void IMP_KERNEL32_LeaveCriticalSection(CPU *c) { HostCS *h = cs_get(ARG(0)); if (h) pthread_mutex_unlock(&h->m); STDRET(1, 0); }

void IMP_KERNEL32_CreateEventA(CPU *c) {
    uint32_t h = handle_new(HT_EVENT); Handle *x = handle_get(h);
    x->manual = ARG(1) != 0; x->signaled = ARG(2) != 0;
    STDRET(4, h);
}
void IMP_KERNEL32_SetEvent(CPU *c) {
    Handle *x = handle_get(ARG(0));
    if (x) { pthread_mutex_lock(&x->mu); x->signaled = 1; pthread_cond_broadcast(&x->cv); pthread_mutex_unlock(&x->mu); }
    STDRET(1, 1);
}
void IMP_KERNEL32_WaitForSingleObject(CPU *c) {
    Handle *x = handle_get(ARG(0)); uint32_t ms = ARG(1);
    if (!x) { STDRET(2, 0xFFFFFFFFu); return; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t ns = (uint64_t)ts.tv_nsec + (uint64_t)(ms % 1000) * 1000000ull;
    ts.tv_sec += ms / 1000 + ns / 1000000000ull; ts.tv_nsec = ns % 1000000000ull;
    int rc = 0;
    pthread_mutex_lock(&x->mu);
    if (x->type == HT_THREAD) {
        while (!x->finished && rc == 0) rc = (ms == 0xFFFFFFFFu) ? pthread_cond_wait(&x->cv, &x->mu) : pthread_cond_timedwait(&x->cv, &x->mu, &ts);
        int ok = x->finished;
        pthread_mutex_unlock(&x->mu);
        STDRET(2, ok ? 0 : 0x102);
        return;
    }
    while (!x->signaled && rc == 0) rc = (ms == 0xFFFFFFFFu) ? pthread_cond_wait(&x->cv, &x->mu) : pthread_cond_timedwait(&x->cv, &x->mu, &ts);
    int ok = x->signaled;
    if (ok && !x->manual) x->signaled = 0;
    pthread_mutex_unlock(&x->mu);
    STDRET(2, ok ? 0 : 0x102);
}

/* TLS: per-thread slots in TIB tls array */
static uint32_t g_tls_used;
void IMP_KERNEL32_TlsAlloc(CPU *c) {
    for (int i = 0; i < 64; i++) if (!(g_tls_used & (1ull << i) ) && i < 32) { g_tls_used |= 1u << i; STDRET(0, i); return; }
    STDRET(0, 0xFFFFFFFFu);
}
void IMP_KERNEL32_TlsFree(CPU *c) { g_tls_used &= ~(1u << (ARG(0) & 31)); STDRET(1, 1); }
void IMP_KERNEL32_TlsGetValue(CPU *c) { STDRET(1, RD32(RD32(c->fs_base + 0x2c) + 4 * (ARG(0) & 63))); }
void IMP_KERNEL32_TlsSetValue(CPU *c) { WR32(RD32(c->fs_base + 0x2c) + 4 * (ARG(0) & 63), ARG(1)); STDRET(2, 1); }

/* threads */
typedef struct { CPU *cpu; uint32_t start, param, handle; } ThreadStart;
extern void cpu_bind_thread(CPU *c);
static __thread uint32_t t_thread_handle;
static void thread_finished(uint32_t h) {
    Handle *x = handle_get(h);
    if (x) { pthread_mutex_lock(&x->mu); x->finished = 1; pthread_cond_broadcast(&x->cv); pthread_mutex_unlock(&x->mu); }
}
static void *thread_main(void *p) {
    ThreadStart *ts = p;
    t_thread_handle = ts->handle;
    CPU *c = ts->cpu;
    cpu_bind_thread(c);
    rt_log("thread %d start: fn=%08x param=%08x", c->thread_index, ts->start, ts->param);
    uint32_t args[1] = { ts->param };
    call_guest(c, ts->start, 1, args);
    rt_log("thread %d returned %u", c->thread_index, c->eax);
    thread_finished(ts->handle);
    return NULL;
}
void IMP_KERNEL32_CreateThread(CPU *c) {
    uint32_t stack = ARG(1), start = ARG(2), param = ARG(3), ptid = ARG(5);
    CPU *self = cpu_current();
    ThreadStart *ts = calloc(1, sizeof *ts);
    ts->cpu = cpu_new_thread_context(stack);
    cpu_bind_thread(self);   /* cpu_new_thread_context binds; restore */
    ts->start = start; ts->param = param;
    uint32_t h = handle_new(HT_THREAD); ts->handle = h;
    Handle *x = handle_get(h);
    if (ptid) WR32(ptid, RD32(ts->cpu->fs_base + 0x24));
    pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8u << 20);
    pthread_create(&x->th, &at, thread_main, ts);
    STDRET(6, h);
}
void IMP_KERNEL32_ExitThread(CPU *c) {
    rt_log("ExitThread(%u) thread %d", ARG(0), c->thread_index);
    if (t_thread_handle) thread_finished(t_thread_handle);
    pthread_exit(NULL);
}
void IMP_KERNEL32_SetThreadPriority(CPU *c) { STDRET(2, 1); }

void kernel32_init(void) { init_std_handles(); host_ticks(); }
