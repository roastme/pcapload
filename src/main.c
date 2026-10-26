/*
 * pcapload - a standalone loader for PopCap "Sexy" web-plugin game DLLs.
 *
 * Historically these games ran inside a browser via the NPAPI plugin
 * (nppopcaploader.dll), which downloaded a signed .cab, extracted it,
 * loaded the game DLL and drove it through the Sexy* exports.
 *
 * This exe implements the same host side:
 *   - creates the game window and pumps messages into SexyProcessMsg
 *   - calls SexyUpdateState once per frame
 *   - provides the host hooks the game resolves by name:
 *       getparam, setdrawparams, drawtodc, notify, getdllhandle, urlnavigate
 *   - can extract the original .cab files with cabinet.dll
 *
 * Games are described by .json files in the "games" directory next to the
 * exe (see README.md). Nothing about a specific game is hardcoded here.
 *
 * Interface reversed from nppopcaploader.dll / Zuma.dll:
 *   void*  SexyCreateGame(HWND hwnd, const char* gameDir, DWORD flags,
 *                         void* hostCtx,
 *                         void* (__cdecl *resolve)(void* ctx, const char* name));
 *   void   SexyDeleteGame(void* game);
 *   void   SexyGetDimensions(void* game, int* w, int* h);
 *   BOOL   SexyProcessMsg(void* game, UINT msg, WPARAM wp, LPARAM lp,
 *                         LRESULT* outResult);  // writes message result
 *   void   SexyReceiveNotification(void* game, const char* a, const char* b);
 *   int    SexyUpdateState(void* game);   // negative return = shut down
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <time.h>

#include "json.h"
#include "cab.h"
#include "registry.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

/* ---------------- config ---------------- */

#define MAX_GAMES   64
#define GAME_FRAME_MS 10   /* ~100 fps, matches the 10ms SexyApp frame */

typedef struct {
    char key[64];
    char val[256];
} Param;

typedef struct {
    char name[96];        /* short id (file name of the .json) */
    char display[128];    /* shown in the launcher / window title */
    char dll[MAX_PATH];   /* absolute path to the game dll */
    char dir[MAX_PATH];   /* absolute game base dir (passed to the dll) */
    char cab[MAX_PATH];   /* optional: cab to extract when dll is missing */
    Param params[48];
    int  nparams;
    /* optional: RVA (in the game dll) of the `call` to the game's internal
     * init inside the SexyCreateGame export, where a register still holds
     * the game object. A trampoline is patched over the call to stash that
     * pointer so pc_downloadresult can be delivered re-entrantly (games
     * like Bookworm Adventures block inside SexyCreateGame until the
     * extras-download result notification arrives). 0 = disabled. */
    DWORD createHookRva;
    char  createHookReg[8];
} GameCfg;

static GameCfg g_games[MAX_GAMES];
static int     g_ngames = 0;

/* ---------------- Sexy game dll interface ---------------- */

typedef void* (__cdecl *FnCreateGame)(HWND, const char*, DWORD, void*,
                                      void* (__stdcall*)(void*, const char*));
typedef void  (__cdecl *FnDeleteGame)(void*);
typedef void  (__cdecl *FnGetDimensions)(void*, int*, int*);
typedef int   (__cdecl *FnProcessMsg)(void*, UINT, WPARAM, LPARAM, LRESULT*);
/* NOTE: SexyProcessMsg takes a 5th, hidden argument: an out-pointer where
 * the game writes the message result (Zuma.dll+0x1E293 does `mov [arg],ebx`).
 * The original loader passed a pointer to its own result slot (then returned
 * 1 when the game claimed the message). */
typedef void  (__cdecl *FnReceiveNotification)(void*, const char*, const char*);
typedef int   (__cdecl *FnUpdateState)(void*);

typedef struct {
    FnCreateGame CreateGame;
    FnDeleteGame DeleteGame;
    FnGetDimensions GetDimensions;
    FnProcessMsg ProcessMsg;
    FnReceiveNotification ReceiveNotification;
    FnUpdateState UpdateState;
} GameFns;

typedef struct {
    HMODULE hDll;
    GameCfg* cfg;
    HWND hwnd;
    /* setdrawparams state, used by the drawtodc loading screen */
    float progress;
    char ds1[192], ds2[192], ds3[192];
    /* offscreen buffer for the loading screen (double buffering so that
     * per-frame progress/text updates don't make it flicker) */
    HDC     loadDC;
    HBITMAP loadBmp;
    int     loadW, loadH;
    DWORD   lastLoadTick;
    /* queued host->game notification (extras download results) */
    BOOL notifyPending;
    char notifyName[128], notifyVal[128];
    /* re-entrant delivery of pc_downloadresult (see createHookRva) */
    FnReceiveNotification ReceiveNotification;
    void** gamePtrSlot;       /* trampoline stash slot (NULL = disabled) */
    BOOL   lastDlOk;          /* result of the last pc_download */
} Host;

static int g_frames = 0;
static BOOL g_quiet = FALSE; /* suppress message boxes (probe mode) */
static void* g_gameObj = NULL;   /* live game object, for the wndproc */
static GameFns g_fns;            /* export pointers, shared with wndproc */
/* drive letter ("X:") mapped to the game dir for the current run, if any */
static char g_substDrive[8] = {0};
static char g_substDir[MAX_PATH] = {0};

static HDC     g_backDC = NULL;      /* game's presented frame (native size) */
static HBITMAP g_backBmp = NULL;
static HWND    g_frameWnd = NULL;    /* top-level window */
static HWND    g_viewWnd  = NULL;    /* child: shows the scaled image */
static HWND    g_gameWnd  = NULL;    /* child: native-size game render target */
static BOOL g_traceMsgs = FALSE; /* per-message tracing (verbose!) */

static void ExeDir(char* out, int outsz);
void pcaplog(const char* fmt, ...);   /* shared with cab.c */
#define TraceLog pcaplog
static BOOL g_logEnabled = FALSE;
/* ---- per-instance drive-letter ownership ------------------------------ *
 * Every RunGame maps the game dir to its OWN free drive letter and removes
 * only that mapping on exit. A lock file (next to the exe, keyed by a hash
 * of the game dir) records which pid owns which letter, so a mapping left
 * behind by a crashed run is reclaimed, while a mapping owned by a live
 * instance is left alone. (An earlier version reused an existing mapping
 * to the same dir, which let two instances share a letter — the first one
 * to exit unmounted it under the other.) */
static void ExeDir(char* out, int outsz);

static BOOL ProcessAlive(DWORD pid)
{
    HANDLE h;
    if (!pid) return FALSE;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    CloseHandle(h);
    return TRUE;
}

static void DriveLockPath(const char* dir, char* out, int outsz)
{
    char exeDir[MAX_PATH];
    const char* p;
    unsigned h = 2166136261u;
    ExeDir(exeDir, sizeof(exeDir));
    for (p = dir; *p; p++) {
        h ^= (unsigned)(BYTE)*p;
        h *= 16777619u;
    }
    _snprintf(out, outsz - 1, "%s\\pcapload_drive_%08x.lock", exeDir, h);
    out[outsz - 1] = 0;
}

/* remove one "letter pid" line from the lock file */
static void RemoveLockLine(const char* lockPath, char letter, DWORD pid)
{
    FILE* lf = fopen(lockPath, "rb");
    char keep[64][24];
    int n = 0, i;
    while (lf && n < 64) {
        char ltr[4];
        unsigned long lpid;
        if (fscanf(lf, "%3s %lu", ltr, &lpid) != 2) break;
        if (ltr[0] == letter && ltr[1] == ':' &&
            (pid == 0 || lpid == (unsigned long)pid))
            continue;
        _snprintf(keep[n], sizeof(keep[n]) - 1, "%s %lu", ltr, lpid);
        keep[n][sizeof(keep[n]) - 1] = 0;
        n++;
    }
    if (lf) fclose(lf);
    lf = fopen(lockPath, "wb");
    if (lf) {
        for (i = 0; i < n; i++) fprintf(lf, "%s\n", keep[i]);
        fclose(lf);
    }
}

static BOOL DriveAcquire(const char* dir, char* outDrive, int outSz)
{
    char drive[4], qbuf[MAX_PATH * 2], lockPath[MAX_PATH];
    int i;
    DriveLockPath(dir, lockPath, sizeof(lockPath));
    drive[1] = ':'; drive[2] = 0;

    /* pass 1: reclaim stale mappings of OUR dir (left by crashed runs) */
    for (i = 'Z'; i >= 'D'; i--) {
        drive[0] = (char)i;
        qbuf[0] = 0;
        if (QueryDosDeviceA(drive, qbuf, sizeof(qbuf) - 1) <= 0) continue;
        qbuf[sizeof(qbuf) - 1] = 0;
        {
            char* tgt = qbuf;
            FILE* lf;
            DWORD pid = 0, owner = 0;
            char ltr[4];
            if (!_strnicmp(tgt, "\\??\\", 4)) tgt += 4;
            if (strlen(tgt) != strlen(dir) || _strnicmp(tgt, dir, strlen(dir)))
                continue;                       /* someone else's letter */
            lf = fopen(lockPath, "rb");
            while (lf && fscanf(lf, "%3s %lu", ltr, &pid) == 2) {
                if (ltr[0] == drive[0] && ltr[1] == ':') {
                    owner = pid;
                    break;
                }
            }
            if (lf) fclose(lf);
            if (owner && owner != GetCurrentProcessId() && ProcessAlive(owner))
                continue;                       /* live instance owns it */
            /* stale: drop it (and its lock entry) */
            TraceLog("reclaiming stale drive mapping %s (owner pid %lu)",
                     drive, (unsigned long)owner);
            DefineDosDeviceA(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE,
                             drive, tgt);
            RemoveLockLine(lockPath, drive[0], owner);
        }
    }

    /* pass 2: take the first free letter */
    for (i = 'Z'; i >= 'D'; i--) {
        drive[0] = (char)i;
        if (QueryDosDeviceA(drive, qbuf, sizeof(qbuf) - 1) > 0)
            continue;                           /* in use */
        if (!DefineDosDeviceA(0, drive, dir)) continue;
        {
            FILE* lf = fopen(lockPath, "a");
            if (lf) {
                fprintf(lf, "%s %lu\n", drive,
                        (unsigned long)GetCurrentProcessId());
                fclose(lf);
            }
        }
        lstrcpynA(outDrive, drive, outSz);
        TraceLog("game dir mapped to %s (owner pid %lu)",
                 drive, (unsigned long)GetCurrentProcessId());
        return TRUE;
    }
    TraceLog("no free drive letter for the game dir");
    return FALSE;
}

static void DriveRelease(void)
{
    char lockPath[MAX_PATH];
    char ltr0;
    if (!g_substDrive[0]) return;
    ltr0 = g_substDrive[0];
    if (!DefineDosDeviceA(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE,
                          g_substDrive, g_substDir))
        DefineDosDeviceA(DDD_REMOVE_DEFINITION, g_substDrive, NULL);
    TraceLog("removed drive mapping %s", g_substDrive);
    DriveLockPath(g_substDir, lockPath, sizeof(lockPath));
    RemoveLockLine(lockPath, ltr0, GetCurrentProcessId());
    g_substDrive[0] = 0;
    g_substDir[0] = 0;
}


static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep)
{
    /* ignore benign first-chance exceptions (OutputDebugString etc.) */
    if ((ep->ExceptionRecord->ExceptionCode & 0xFFFFFFFF) < 0x80000000)
        return EXCEPTION_CONTINUE_SEARCH;
    g_logEnabled = TRUE;
    TraceLog("*** CRASH: code=0x%08lX addr=%p ctx-eip=%p",
        ep->ExceptionRecord->ExceptionCode,
        ep->ExceptionRecord->ExceptionAddress,
        (void*)ep->ContextRecord->Eip);
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---------------- utilities ---------------- */

static void ExeDir(char* out, int outsz);

/* exported so cab.c can trace into the same log */
void pcaplog(const char* fmt, ...)
{
    static FILE* lf;
    char buf[1024];
    va_list ap;
    if (!g_logEnabled) return;
    if (!lf) {
        char dir[MAX_PATH], path[MAX_PATH];
        ExeDir(dir, sizeof(dir));
        _snprintf(path, sizeof(path) - 1, "%s\\pcapload.log", dir);
        lf = fopen(path, "a");
        if (!lf) return;
    }
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    fputs(buf, lf);
    fputc('\n', lf);
    fflush(lf);
}

static void ShowErr(const char* title, const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    if (g_quiet) {
        pcaplog("ERROR: %s: %s", title, buf);
        return;
    }
    MessageBoxA(NULL, buf, title, MB_OK | MB_ICONERROR);
}

static BOOL FileExists(const char* p)
{
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void DirName(const char* path, char* out, int outsz)
{
    char drive[_MAX_DRIVE], dir[_MAX_DIR];
    int n;
    _splitpath(path, drive, dir, NULL, NULL);
    _snprintf(out, outsz - 1, "%s%s", drive, dir);
    out[outsz - 1] = 0;
    n = (int)strlen(out);
    while (n > 0 && (out[n - 1] == '\\' || out[n - 1] == '/'))
        out[--n] = 0;
}

/* resolve 'p' relative to 'base' if not absolute */
static void ResolvePath(const char* base, const char* p, char* out, int outsz)
{
    char* q;
    if (!p || !p[0]) { out[0] = 0; return; }
    if (p[1] == ':' || p[0] == '\\' || p[0] == '/')
        _snprintf(out, outsz - 1, "%s", p);
    else
        _snprintf(out, outsz - 1, "%s\\%s", base, p);
    out[outsz - 1] = 0;
    for (q = out; *q; q++)
        if (*q == '/') *q = '\\';
}

static void ExeDir(char* out, int outsz)
{
    char* slash;
    GetModuleFileNameA(NULL, out, outsz);
    slash = strrchr(out, '\\');
    if (slash) *slash = 0;
    out[outsz - 1] = 0;
}

static void CenterWindow(HWND hwnd, int w, int h)
{
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd, NULL, (sx - w) / 2, (sy - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---------------- host hooks ---------------- */

/*
 * Host hooks. The game calls these __stdcall (callee cleans the stack),
 * with the host context (SexyCreateGame arg4) as the first argument.
 * (Verified against nppopcaploader.dll: every hook implementation ends in
 *  "ret N", while the Sexy* exports themselves are __cdecl.)
 */

static int __stdcall hook_getparam(void* ctx, const char* name,
                                   char* buf, int bufMax)
{
    GameCfg* cfg = ((Host*)ctx)->cfg;
    const char* val = NULL;
    int i;
    size_t n;
    TraceLog("hook getparam(\"%s\", buf=%p, max=%d)", name ? name : "(null)", buf, bufMax);
    if (name)
        for (i = 0; i < cfg->nparams; i++)
            if (!lstrcmpiA(cfg->params[i].key, name)) {
                val = cfg->params[i].val;
                break;
            }
    if (!val) val = "";
    n = strlen(val);
    if (buf && bufMax > 0) {
        size_t c = n < (size_t)(bufMax - 1) ? n : (size_t)(bufMax - 1);
        memcpy(buf, val, c);
        buf[c] = 0;
    }
    return (int)n + 1;
}

static void __stdcall hook_setdrawparams(void* ctx, float progress,
                                         const char* s1, const char* s2,
                                         const char* s3)
{
    Host* h = (Host*)ctx;
    TraceLog("hook setdrawparams(%g, \"%s\", \"%s\", \"%s\")", progress,
        s1 ? s1 : "", s2 ? s2 : "", s3 ? s3 : "");
    h->progress = progress < 0.f ? 0.f : (progress > 1.f ? 1.f : progress);
    if (s1) { strncpy(h->ds1, s1, sizeof(h->ds1) - 1); h->ds1[sizeof(h->ds1) - 1] = 0; }
    if (s2) { strncpy(h->ds2, s2, sizeof(h->ds2) - 1); h->ds2[sizeof(h->ds2) - 1] = 0; }
    if (s3) { strncpy(h->ds3, s3, sizeof(h->ds3) - 1); h->ds3[sizeof(h->ds3) - 1] = 0; }
}

/* The plugin drew its logo/loading art here; we draw progress + strings. */
static void __stdcall hook_drawtodc(void* ctx, HDC hdc)
{
    Host* h = (Host*)ctx;
    TraceLog("hook drawtodc(hdc=%p)", hdc);
    RECT rc;
    int cw, ch, bw, bh, bx, by, fill;
    HBRUSH bg, bar;
    HPEN penOld;
    HFONT fnt, fntOld;
    LOGFONTA lf;
    char pct[64];
    HDC hdcDraw;   /* everything is rendered here, blitted to hdc at the end */

    if (!h->hwnd || !GetClientRect(h->hwnd, &rc))
        SetRect(&rc, 0, 0, 640, 480);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;

    /* (re)create the offscreen buffer when the client size changed */
    if (h->hwnd && (h->loadW != cw || h->loadH != ch) && h->loadDC) {
        DeleteDC(h->loadDC);
        DeleteObject(h->loadBmp);
        h->loadDC = NULL;
        h->loadBmp = NULL;
    }
    if (h->hwnd && !h->loadDC) {
        HDC wnd = GetDC(h->hwnd);
        h->loadDC = CreateCompatibleDC(wnd);
        h->loadBmp = CreateCompatibleBitmap(wnd, cw, ch);
        ReleaseDC(h->hwnd, wnd);
        SelectObject(h->loadDC, h->loadBmp);
        h->loadW = cw;
        h->loadH = ch;
    }
    hdcDraw = h->loadDC ? h->loadDC : hdc;

    bg = CreateSolidBrush(RGB(16, 24, 66));
    FillRect(hdcDraw, &rc, bg);
    DeleteObject(bg);

    SetBkMode(hdcDraw, TRANSPARENT);
    if (h->ds1[0]) {
        SetTextColor(hdcDraw, RGB(235, 235, 235));
        memset(&lf, 0, sizeof(lf));
        lf.lfHeight = -16;
        lf.lfWeight = FW_BOLD;
        lstrcpyA(lf.lfFaceName, "Arial");
        fnt = CreateFontIndirectA(&lf);
        fntOld = (HFONT)SelectObject(hdcDraw, fnt);
        SetTextAlign(hdcDraw, TA_CENTER);
        TextOutA(hdcDraw, cw / 2, ch / 2 - 48, h->ds1, lstrlenA(h->ds1));
        if (h->ds2[0])
            TextOutA(hdcDraw, cw / 2, ch / 2 - 24, h->ds2, lstrlenA(h->ds2));
        SetTextAlign(hdcDraw, TA_LEFT);
        SelectObject(hdcDraw, fntOld);
        DeleteObject(fnt);
    }

    bw = cw * 3 / 4;
    bh = 18;
    bx = (cw - bw) / 2;
    by = ch / 2 + 12;
    penOld = (HPEN)SelectObject(hdcDraw, GetStockObject(WHITE_PEN));
    Rectangle(hdcDraw, bx - 1, by - 1, bx + bw + 1, by + bh + 1);
    SelectObject(hdcDraw, penOld);
    fill = (int)(bw * h->progress + 0.5f);
    if (fill > 0) {
        RECT fr;
        SetRect(&fr, bx, by, bx + fill, by + bh);
        bar = CreateSolidBrush(RGB(240, 150, 30));
        FillRect(hdcDraw, &fr, bar);
        DeleteObject(bar);
    }
    wsprintfA(pct, "%d%%", (int)(h->progress * 100.f + 0.5f));
    SetTextColor(hdcDraw, RGB(200, 200, 200));
    SetTextAlign(hdcDraw, TA_CENTER);
    TextOutA(hdcDraw, cw / 2, by + bh + 6, pct, lstrlenA(pct));
    if (h->ds3[0]) {
        SetTextColor(hdcDraw, RGB(150, 150, 160));
        TextOutA(hdcDraw, cw / 2, ch - 24, h->ds3, lstrlenA(h->ds3));
    }
    SetTextAlign(hdcDraw, TA_LEFT);

    /* one blit into the DC the game handed us — that is where the loading
     * art belongs (it may be the game's own backbuffer, which it then
     * presents composited). If it is the game window's DC (which our hooks
     * intercept), detour into the backbuffer instead. */
    if (g_backDC && WindowFromDC(hdc) == g_gameWnd)
        BitBlt(g_backDC, 0, 0, cw, ch, hdcDraw, 0, 0, SRCCOPY);
    else
        BitBlt(hdc, 0, 0, cw, ch, hdcDraw, 0, 0, SRCCOPY);
    /* The game hands us the DC it presents into (its window DC — see the
     * IsGameDestDC note). Draw the loading art into our backbuffer there;
     * for any other DC (e.g. the game's own memory buffer), draw directly. */
    if (g_backDC && (WindowFromDC(hdc) == g_gameWnd ||
                     WindowFromDC(hdc) == g_frameWnd))
        BitBlt(g_backDC, 0, 0, cw, ch, hdcDraw, 0, 0, SRCCOPY);
    else
        BitBlt(hdc, 0, 0, cw, ch, hdcDraw, 0, 0, SRCCOPY);
    h->lastLoadTick = timeGetTime();
}

/* Queue a notification to deliver to the game on the next main-loop pass
 * ( SexyReceiveNotification is the plugin's way to push events to the game).
 * Delivered from RunGame to avoid re-entrancy inside a game callback. */
static void HostQueueNotify(Host* h, const char* name, const char* val)
{
    strncpy(h->notifyName, name, sizeof(h->notifyName) - 1);
    h->notifyName[sizeof(h->notifyName) - 1] = 0;
    strncpy(h->notifyVal, val, sizeof(h->notifyVal) - 1);
    h->notifyVal[sizeof(h->notifyVal) - 1] = 0;
    h->notifyPending = TRUE;
}

/* Patch the `call` to the game's internal init inside its SexyCreateGame
 * export with a trampoline that stashes the game-object pointer (held in a
 * register at that point) into a slot we can read from the hooks. Needed to
 * push pc_downloadresult back into games that block inside SexyCreateGame. */
static BOOL InstallCreateHook(Host* h)
{
    static const char* regs[] = { "eax","ecx","edx","ebx","esp","ebp","esi","edi" };
    GameCfg* cfg = h->cfg;
    BYTE *site, *target, *page, *code;
    DWORD oldProt;
    int r;

    if (!cfg->createHookRva) return TRUE;
    for (r = 0; r < 8; r++)
        if (!lstrcmpiA(cfg->createHookReg, regs[r])) break;
    if (r == 8) {
        TraceLog("createHook: unknown register \"%s\"", cfg->createHookReg);
        return FALSE;
    }
    site = (BYTE*)h->hDll + cfg->createHookRva;
    if (site[0] != 0xE8) {
        TraceLog("createHook: no call at dll+0x%08x (found %02x)",
                 cfg->createHookRva, site[0]);
        return FALSE;
    }
    target = site + 5 + *(int*)(site + 1);
    page = (BYTE*)VirtualAlloc(NULL, 4096,
                               MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    if (!page) {
        TraceLog("createHook: VirtualAlloc failed (%d)", GetLastError());
        return FALSE;
    }
    code = page;
    if (r == 0) {                     /* mov [slot], eax */
        *code++ = 0xA3;
    } else {                          /* mov [slot], r32 */
        *code++ = 0x89;
        *code++ = (BYTE)(0x05 | (r << 3));
    }
    *(DWORD*)code = (DWORD)(page + 0x400);
    code += 4;
    *code++ = 0xE9;                   /* jmp back to the real init */
    *(DWORD*)code = (DWORD)((DWORD)target - (DWORD)(code + 4));
    code += 4;

    if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
        TraceLog("createHook: VirtualProtect failed (%d)", GetLastError());
        VirtualFree(page, 0, MEM_RELEASE);
        return FALSE;
    }
    site[0] = 0xE8;
    *(DWORD*)(site + 1) = (DWORD)page - (DWORD)(site + 5);
    VirtualProtect(site, 5, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), site, 5);
    FlushInstructionCache(GetCurrentProcess(), page, 32);

    *(void**)(page + 0x400) = NULL;
    h->gamePtrSlot = (void**)(page + 0x400);
    TraceLog("createHook: hooked dll+0x%08x (init=%p, reg=%s), slot=%p",
             cfg->createHookRva, target, cfg->createHookReg, h->gamePtrSlot);
    return TRUE;
}

/*
 * The game asks the host to fetch an extras .cab (notify "pc_download") and
 * then just waits: only "pc_downloadresult" ("1" ok / "0" failed), pushed
 * back through SexyReceiveNotification, lets it continue past its loading
 * screen. (Bejeweled 2's main-menu art, sounds, ... ship in the
 * bej2extras*.cab files, so without this the game hangs at 100%.)
 *
 * The original plugin downloaded the cab from the web page it lived on and
 * extracted it into the game dir. We have no page — instead look for the cab
 * next to the game dir (as archived: Plugin\<game>_1_2\ + Plugin\<name>.cab),
 * then inside the game dir, extract it there and write the marker file the
 * game polls for ("extras<i>.txt" containing the cab name, same convention
 * as the original loader — Bejeweled2.dll:0x1001cfd1 checks it).
 */
static void HandlePcDownload(Host* h, const char* value)
{
    char parent[MAX_PATH], cand[2][MAX_PATH], cab[MAX_PATH];
    char marker[MAX_PATH], idxName[16];
    const char* base;
    int i, n, ok = 0, idx = -1;

    /* parent of the game dir */
    strncpy(parent, h->cfg->dir, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = 0;
    n = (int)strlen(parent);
    while (n > 0 && (parent[n-1] == '\\' || parent[n-1] == '/'))
        parent[--n] = 0;
    {
        char* slash = strrchr(parent, '\\');
        if (slash) *slash = 0;
    }

    strncpy(cand[0], parent, MAX_PATH - 1); cand[0][MAX_PATH-1] = 0;
    strncpy(cand[1], h->cfg->dir, MAX_PATH - 1); cand[1][MAX_PATH-1] = 0;

    TraceLog("hook pc_download(\"%s\")", value);
    for (i = 0; i < 2 && !ok; i++) {
        _snprintf(cab, sizeof(cab) - 1, "%s\\%s", cand[i], value);
        cab[sizeof(cab) - 1] = 0;
        if (FileExists(cab)) {
            int r = cab_extract(cab, h->cfg->dir);
            TraceLog("extras cab_extract(\"%s\") -> %d", cab, r);
            ok = (r == 0);
        }
    }

    /* marker file "extras<i>.txt" with the cab name as content; <i> is the
     * trailing digit(s) of the cab name ("bej2extras0.cab" -> extras0.txt) */
    base = strrchr(value, '\\');
    base = base ? base + 1 : value;
    n = (int)strlen(base);
    while (n > 0 && base[n-1] >= '0' && base[n-1] <= '9') n--;
    if (n > 0 && n < (int)strlen(base) && ok) {
        FILE* f;
        _snprintf(idxName, sizeof(idxName) - 1, "%s", base + n);
        idxName[sizeof(idxName) - 1] = 0;
        {
            char* dot = strrchr(idxName, '.');
            if (dot) *dot = 0;
        }
        _snprintf(marker, sizeof(marker) - 1, "%s\\extras%s.txt",
                  h->cfg->dir, idxName);
        marker[sizeof(marker) - 1] = 0;
        f = fopen(marker, "wb");
        if (f) {
            fputs(value, f);
            fclose(f);
            TraceLog("extras marker written: \"%s\" = \"%s\"", marker, value);
        }
    }

    h->lastDlOk = (ok != 0);
    TraceLog("pc_download done, ok=%d", ok);
}

static void __stdcall hook_notify(void* ctx, const char* name, const char* value)
{
    /* game -> host notification. On the web this became a JS callback
     * (PopcapNotification_*). "pc_download" (fetch an extras cab) is handled
     * here like the original loader did; everything else is just logged. */
    if (name && value && !lstrcmpiA(name, "pc_download")) {
        Host* h = (Host*)ctx;
        HandlePcDownload(h, value);
        if (h->gamePtrSlot && *h->gamePtrSlot) {
            /* the game requested the download from inside SexyCreateGame
             * (e.g. Bookworm Adventures): it blocks until the result
             * notification arrives, so deliver it re-entrantly right here —
             * the pump loop cannot run yet */
            TraceLog("direct SexyReceiveNotification(\"pc_downloadresult\", \"%s\")",
                     h->lastDlOk ? "1" : "0");
            if (h->ReceiveNotification)
                h->ReceiveNotification(*h->gamePtrSlot, "pc_downloadresult",
                                       h->lastDlOk ? "1" : "0");
        } else {
            HostQueueNotify(h, "pc_downloadresult", h->lastDlOk ? "1" : "0");
        }
        return;
    }
    {
        char buf[512];
        _snprintf(buf, sizeof(buf) - 1, "[pcapload] game notify: %s = %s\n",
                  name ? name : "(null)", value ? value : "(null)");
        buf[sizeof(buf) - 1] = 0;
        TraceLog("hook notify(\"%s\", \"%s\")", name ? name : "", value ? value : "");
        OutputDebugStringA(buf);
    }
    (void)ctx;
}

static void* __stdcall hook_getdllhandle(void* ctx)
{
    return (void*)((Host*)ctx)->hDll;
}

static void __stdcall hook_urlnavigate(void* ctx, const char* url,
                                        const char* target)
{
    (void)ctx; (void)target;
    if (url && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)))
        ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
}

static void* __stdcall HookResolve(void* ctx, const char* name)
{
    TraceLog("HookResolve(\"%s\")", name ? name : "(null)");
    if (!name) return NULL;
    if (!lstrcmpiA(name, "getparam"))      return (void*)hook_getparam;
    if (!lstrcmpiA(name, "setdrawparams")) return (void*)hook_setdrawparams;
    if (!lstrcmpiA(name, "drawtodc"))      return (void*)hook_drawtodc;
    if (!lstrcmpiA(name, "notify"))        return (void*)hook_notify;
    if (!lstrcmpiA(name, "getdllhandle"))  return (void*)hook_getdllhandle;
    if (!lstrcmpiA(name, "urlnavigate"))   return (void*)hook_urlnavigate;
    return NULL;
}

/* ---------------- configuration ---------------- */

static BOOL LoadGameJson(const char* jsonPath, GameCfg* out)
{
    FILE* f;
    long len;
    char* text;
    JVal* root;
    char buf[256], buf2[16];
    char jsonDir[MAX_PATH];
    int i;

    memset(out, 0, sizeof(*out));

    f = fopen(jsonPath, "rb");
    if (!f) return FALSE;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0 || len > (1 << 20)) { fclose(f); return FALSE; }
    text = (char*)malloc(len + 1);
    if (!text) { fclose(f); return FALSE; }
    if (fread(text, 1, len, f) != (size_t)len) { free(text); fclose(f); return FALSE; }
    text[len] = 0;
    fclose(f);

    root = json_parse(text);
    free(text);
    if (!root || root->t != JSON_OBJ) {
        json_free(root);
        return FALSE;
    }

    /* directory of the json file = base for relative paths */
    strncpy(jsonDir, jsonPath, MAX_PATH - 1);
    jsonDir[MAX_PATH - 1] = 0;
    {
        char* slash = strrchr(jsonDir, '\\');
        if (slash) *slash = 0;
    }

    /* name: "name" key or the json file name */
    {
        JVal* jn = json_get(root, "name");
        const char* nm = json_scalar_str(jn, buf, sizeof(buf));
        if (!nm[0]) {
            const char* base = strrchr(jsonPath, '\\');
            char* dot;
            base = base ? base + 1 : jsonPath;
            strncpy(out->name, base, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = 0;
            dot = strrchr(out->name, '.');
            if (dot) *dot = 0;
        } else {
            strncpy(out->name, nm, sizeof(out->name) - 1);
            out->name[sizeof(out->name) - 1] = 0;
        }
    }
    {
        JVal* jd = json_get(root, "displayName");
        const char* dn = json_scalar_str(jd, buf, sizeof(buf));
        strncpy(out->display, dn[0] ? dn : out->name, sizeof(out->display) - 1);
        out->display[sizeof(out->display) - 1] = 0;
    }

    /* dll (required) */
    {
        JVal* jl = json_get(root, "dll");
        const char* dl = json_scalar_str(jl, buf, sizeof(buf));
        if (!dl[0]) { json_free(root); return FALSE; }
        ResolvePath(jsonDir, dl, out->dll, sizeof(out->dll));
    }
    /* cab (optional) */
    {
        JVal* jc = json_get(root, "cab");
        const char* cb = json_scalar_str(jc, buf, sizeof(buf));
        if (cb[0])
            ResolvePath(jsonDir, cb, out->cab, sizeof(out->cab));
    }
    /* dir (optional; defaults to the dll's directory) */
    {
        JVal* jdr = json_get(root, "dir");
        const char* dr = json_scalar_str(jdr, buf, sizeof(buf));
        if (dr[0])
            ResolvePath(jsonDir, dr, out->dir, sizeof(out->dir));
        else
            DirName(out->dll, out->dir, sizeof(out->dir));
    }

    /* params (any string/scalar map) */
    {
        JVal* jp = json_get(root, "params");
        if (jp && jp->t == JSON_OBJ) {
            for (i = 0; i < jp->n && out->nparams < 48; i++) {
                Param* p = &out->params[out->nparams];
                const char* v = json_scalar_str(jp->items[i], buf, sizeof(buf));
                strncpy(p->key, jp->keys[i] ? jp->keys[i] : "", sizeof(p->key) - 1);
                p->key[sizeof(p->key) - 1] = 0;
                strncpy(p->val, v, sizeof(p->val) - 1);
                p->val[sizeof(p->val) - 1] = 0;
                out->nparams++;
            }
        }
    }

    /* optional create-game hook (see GameCfg.createHookRva) */
    {
        JVal* jh = json_get(root, "createHookRva");
        const char* hv = json_scalar_str(jh, buf, sizeof(buf));
        JVal* jr = json_get(root, "createHookReg");
        const char* rg = json_scalar_str(jr, buf2, sizeof(buf2));
        out->createHookRva = 0;
        out->createHookReg[0] = 0;
        if (hv[0]) {
            out->createHookRva = (DWORD)strtoul(hv, NULL, 0);
            strncpy(out->createHookReg, rg[0] ? rg : "esi",
                    sizeof(out->createHookReg) - 1);
            out->createHookReg[sizeof(out->createHookReg) - 1] = 0;
        }
    }

    json_free(root);
    return TRUE;
}

static void ScanGames(void)
{
    char exeDir[MAX_PATH], pattern[MAX_PATH + 32];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    ExeDir(exeDir, sizeof(exeDir));
    _snprintf(pattern, sizeof(pattern) - 1, "%s\\games\\*.json", exeDir);
    pattern[sizeof(pattern) - 1] = 0;

    g_ngames = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (g_ngames >= MAX_GAMES) break;
        _snprintf(path, sizeof(path) - 1, "%s\\games\\%s", exeDir, fd.cFileName);
        path[sizeof(path) - 1] = 0;
        if (LoadGameJson(path, &g_games[g_ngames]))
            g_ngames++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static GameCfg* FindGame(const char* arg)
{
    int i;
    if (!arg || !arg[0]) return NULL;
    /* explicit json path? */
    {
        const char* dot = strrchr(arg, '.');
        if (dot && !_stricmp(dot, ".json") && FileExists(arg)) {
            /* reload directly into a static slot */
            static GameCfg adhoc;
            if (LoadGameJson(arg, &adhoc))
                return &adhoc;
            return NULL;
        }
    }
    /* by name / by <name>.json in the games dir */
    for (i = 0; i < g_ngames; i++)
        if (!_stricmp(g_games[i].name, arg))
            return &g_games[i];
    return NULL;
}

/* ---------------- game window + main loop ---------------- */

/*
 * Window hierarchy:
 *
 *   g_frameWnd  (top-level, resizable / maximizable, what the user sees)
 *     g_gameWnd (child, always the game's NATIVE size, at client 0,0)
 *     g_viewWnd (child created later, covers the whole frame client)
 *
 * The game renders with GDI into g_gameWnd at its fixed native size and
 * knows nothing about resizing. Its final present blit (BitBlt /
 * StretchBlt / SetDIBitsToDevice, patched in the dll's import table) is
 * redirected into an offscreen backbuffer at native size, so the game
 * window itself never shows a half-drawn state. The main loop presents
 * the backbuffer into g_viewWnd: stretched by an integer factor,
 * centered, with black letterbox bars. Mouse input on the view is mapped
 * back to game coordinates and forwarded; the game window keeps keyboard
 * focus.
 */

#define WCL_GAME   "PCapLoaderGameWnd"
#define WCL_FRAME  "PCapLoaderFrameWnd"
#define WCL_VIEW   "PCapLoaderViewWnd"

/* g_frameWnd/g_viewWnd/g_gameWnd are declared near the top of the file */

static int  g_gameW = 0, g_gameH = 0;        /* native game size */
static RECT g_destRect = { 0, 0, 0, 0 };     /* scaled image rect in the view */
static HDC     g_compDC = NULL;              /* letterboxed composed image */
static HBITMAP g_compBmp = NULL;
static int     g_compW = 0, g_compH = 0;
static int     g_backSaved = 0;              /* backbuffer DC baseline state */


/* forward a message to the game; returns TRUE if the game handled it */
static BOOL ForwardToGameEx(UINT msg, WPARAM wp, LPARAM lp)
{
    LRESULT result = 0;
    if (g_gameObj && g_fns.ProcessMsg)
        return g_fns.ProcessMsg(g_gameObj, msg, wp, lp, &result);
    return FALSE;
}

static void ForwardToGame(UINT msg, WPARAM wp, LPARAM lp)
{
    ForwardToGameEx(msg, wp, lp);
}

/* keep the game's presented frame in an offscreen backbuffer at native
 * size (the game's present blit is redirected here — see GDI hooks) */
static void EnsureBackBuffer(void)
{
    HDC cdc;
    if (g_gameW <= 0 || g_gameH <= 0) return;
    if (g_backDC) {
        BITMAP bm;
        if (GetObjectA(g_backBmp, sizeof(bm), &bm) &&
            bm.bmWidth == g_gameW && bm.bmHeight == g_gameH)
            return;
        DeleteDC(g_backDC);
        DeleteObject(g_backBmp);
        g_backDC = NULL;
        g_backBmp = NULL;
    }
    cdc = GetDC(g_gameWnd);
    if (!cdc) return;
    g_backDC = CreateCompatibleDC(cdc);
    g_backBmp = CreateCompatibleBitmap(cdc, g_gameW, g_gameH);
    ReleaseDC(g_gameWnd, cdc);
    if (g_backDC && g_backBmp) {
        RECT rc = { 0, 0, g_gameW, g_gameH };
        SelectObject(g_backDC, g_backBmp);
        FillRect(g_backDC, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        g_backSaved = SaveDC(g_backDC); /* baseline state for DC handouts */
    }
}

/* reset the backbuffer DC to its baseline state before handing it to the
 * game (GetDC normally returns a DC with default attributes every time) */
static void BackDCReset(void)
{
    if (g_backSaved) {
        RestoreDC(g_backDC, -1);
        g_backSaved = SaveDC(g_backDC);
    }
}

/* ---- GDI present hooks ----
 *
 * The game composes every frame in a memory DC and presents it to its
 * window DC with a final blit (BitBlt / StretchBlt / SetDIBitsToDevice).
 * We patch those three imports in the game dll's import table so the
 * present lands in our own backbuffer (g_backDC) instead of on screen.
 * The game window then never shows half-drawn state and there are no
 * screen read-backs — the main loop just presents the backbuffer.
 */

static BOOL (WINAPI* Real_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
static BOOL (WINAPI* Real_StretchBlt)(HDC, int, int, int, int, HDC,
                                      int, int, int, int, DWORD);
static int  (WINAPI* Real_SetDIBitsToDevice)(HDC, int, int, DWORD, DWORD, int, int,
                                             UINT, UINT, const VOID*,
                                             const BITMAPINFO*, UINT);

static BOOL IsGameDestDC(HDC hdc)
{
    HWND w;
    if (!g_backDC || !hdc) return FALSE;
    /* the game obtains its DC for the game child window, but GDI resolves
     * child-window DCs to the top-level window — match both */
    w = WindowFromDC(hdc);
    return w && (w == g_gameWnd || w == g_frameWnd);
}

static void TraceHook(const char* fn, HDC dst, BOOL redirected);

static BOOL WINAPI Hooked_BitBlt(HDC dst, int x, int y, int w, int h,
                                 HDC src, int sx, int sy, DWORD rop)
{
    BOOL redir = IsGameDestDC(dst);
    TraceHook("BitBlt", dst, redir);
    if (redir)
        return Real_BitBlt(g_backDC, x, y, w, h, src, sx, sy, rop);
    return Real_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
}

static BOOL WINAPI Hooked_StretchBlt(HDC dst, int xd, int yd, int wd, int hd,
                                     HDC src, int xs, int ys, int ws, int hs,
                                     DWORD rop)
{
    BOOL redir = IsGameDestDC(dst);
    TraceHook("StretchBlt", dst, redir);
    if (redir)
        return Real_StretchBlt(g_backDC, xd, yd, wd, hd,
                               src, xs, ys, ws, hs, rop);
    return Real_StretchBlt(dst, xd, yd, wd, hd, src, xs, ys, ws, hs, rop);
}

static int WINAPI Hooked_SetDIBitsToDevice(HDC dst, int x, int y, DWORD w, DWORD h,
                                           int sx, int sy, UINT start, UINT lines,
                                           const VOID* bits, const BITMAPINFO* bmi,
                                           UINT use)
{
    BOOL redir = IsGameDestDC(dst);
    TraceHook("SetDIBitsToDevice", dst, redir);
    if (redir)
        return Real_SetDIBitsToDevice(g_backDC, x, y, w, h, sx, sy, start,
                                      lines, bits, bmi, use);
    return Real_SetDIBitsToDevice(dst, x, y, w, h, sx, sy, start,
                                  lines, bits, bmi, use);
}

/* diagnostic: log the first few hooked present calls */
static void TraceHook(const char* fn, HDC dst, BOOL redirected)
{
    static int n;
    if (n++ < 40 || (n % 200) == 0)
        TraceLog("hook %s #%d dst=%p redirected=%d", fn, n, dst, redirected);
}

/* ---- DC hooks ----
 *
 * Rather than chasing every GDI function the game might draw with, hand
 * the game our backbuffer DC directly: GetDC/BeginPaint on its window
 * return g_backDC, so EVERY draw (blits, fills, text) lands offscreen and
 * nothing the game does can hit the screen. GetDC normally returns a DC
 * with fresh default attributes, so the baseline state is restored before
 * each handout.
 */

static HDC (WINAPI* Real_GetDC)(HWND);
static int (WINAPI* Real_ReleaseDC)(HWND, HDC);
static HDC (WINAPI* Real_BeginPaint)(HWND, LPPAINTSTRUCT);
static BOOL (WINAPI* Real_EndPaint)(HWND, const PAINTSTRUCT*);
static BOOL (WINAPI* Real_ScreenToClient)(HWND, LPPOINT);
static HWND (WINAPI* Real_WindowFromPoint)(POINT);
static BOOL (WINAPI* Real_GetCursorPos)(LPPOINT);
static HDC g_realPaintDC = NULL; /* DC BeginPaint really acquired */

static HDC WINAPI Hooked_GetDC(HWND hwnd)
{
    static int n;
    if (n++ < 60 || (n % 500) == 0)
        TraceLog("GetDC(hwnd=%p) game=%p frame=%p", hwnd, g_gameWnd, g_frameWnd);
    if (g_backDC && (hwnd == g_gameWnd || hwnd == g_frameWnd)) {
        BackDCReset();
        return g_backDC;
    }
    return Real_GetDC(hwnd);
}

static int WINAPI Hooked_ReleaseDC(HWND hwnd, HDC hdc)
{
    if (hdc == g_backDC) return 1; /* never release our backbuffer DC */
    return Real_ReleaseDC(hwnd, hdc);
}

static HDC WINAPI Hooked_BeginPaint(HWND hwnd, LPPAINTSTRUCT ps)
{
    HDC real = Real_BeginPaint(hwnd, ps); /* validates the update region */
    if (g_backDC && (hwnd == g_gameWnd || hwnd == g_frameWnd)) {
        g_realPaintDC = real;
        BackDCReset();
        ps->hdc = g_backDC;
        ps->fErase = FALSE;
        GetClientRect(hwnd, &ps->rcPaint);
        return g_backDC;
    }
    return real;
}

static BOOL WINAPI Hooked_EndPaint(HWND hwnd, const PAINTSTRUCT* ps)
{
    if (ps && ps->hdc == g_backDC && g_realPaintDC) {
        /* give EndPaint back the DC it really acquired so it can be freed */
        ((PAINTSTRUCT*)ps)->hdc = g_realPaintDC;
        g_realPaintDC = NULL;
    }
    return Real_EndPaint(hwnd, ps);
}

/* the game locates the window under the cursor with WindowFromPoint and
 * converts with ScreenToClient — that window is OUR view (it covers the
 * client), so report the game's own window to keep its polled mouse in
 * game coordinates */
static HWND WINAPI Hooked_WindowFromPoint(POINT pt)
{
    HWND w = Real_WindowFromPoint(pt);
    static int n;
    if (n++ < 60 || (n % 300) == 0)
        TraceLog("WindowFromPoint pt=(%d,%d) -> %p (game=%p view=%p)",
                 pt.x, pt.y, w, g_gameWnd, g_viewWnd);
    if (g_gameWnd && (w == g_viewWnd || w == g_frameWnd))
        return g_gameWnd;
    return w;
}

static BOOL WINAPI Hooked_GetCursorPos(LPPOINT pt)
{
    BOOL r = Real_GetCursorPos(pt);
    static int n;
    if (n++ < 60 || (n % 300) == 0)
        TraceLog("GetCursorPos -> (%d,%d)", pt ? pt->x : 0, pt ? pt->y : 0);
    return r;
}

/* the game polls the mouse with GetCursorPos + ScreenToClient on its
 * window; the window sits at the top-left at NATIVE size while the image
 * the user sees is scaled and letterboxed — map into game coordinates so
 * polled and message-driven input agree */
static BOOL WINAPI Hooked_ScreenToClient(HWND hwnd, LPPOINT pt)
{
    BOOL hit = pt && g_viewWnd && g_backDC && g_gameW > 0 && g_gameH > 0 &&
        (hwnd == g_gameWnd || hwnd == g_frameWnd || hwnd == g_viewWnd) &&
        g_destRect.right > g_destRect.left &&
        g_destRect.bottom > g_destRect.top;
    {
        static int n;
        int show = (n++ < 80 || (n % 200) == 0);
        if (show)
            TraceLog("ScreenToClient hwnd=%p pt=(%d,%d) mapped=%d", hwnd,
                     pt ? pt->x : 0, pt ? pt->y : 0, hit);
    }
    if (hit)
    {
        POINT org;
        int dw = g_destRect.right - g_destRect.left;
        int dh = g_destRect.bottom - g_destRect.top;
        int vx, vy, gx, gy;
        org.x = 0; org.y = 0;
        if (!ClientToScreen(g_viewWnd, &org))
            return Real_ScreenToClient(hwnd, pt);
        vx = pt->x - org.x;
        vy = pt->y - org.y;
        gx = (vx - g_destRect.left) * g_gameW / dw;
        gy = (vy - g_destRect.top) * g_gameH / dh;
        if (gx < 0) gx = 0;
        if (gy < 0) gy = 0;
        if (gx >= g_gameW) gx = g_gameW - 1;
        if (gy >= g_gameH) gy = g_gameH - 1;
        pt->x = gx;
        pt->y = gy;
        {
            static int m;
            if (m++ < 80 || (m % 200) == 0) {
                POINT vo = { 0, 0 };
                ClientToScreen(g_viewWnd, &vo);
                TraceLog("  -> (%d,%d) dest=[%d,%d,%d,%d] viewOrg=(%d,%d)",
                         gx, gy, g_destRect.left, g_destRect.top,
                         g_destRect.right, g_destRect.bottom, vo.x, vo.y);
            }
        }
        return TRUE;
    }
    return Real_ScreenToClient(hwnd, pt);
}

/* the game must not capture the mouse onto its (child) render window:
 * captured input would bypass the view's coordinate mapping and eat all
 * clicks on the frame window's title bar / buttons */
static HWND WINAPI Hooked_SetCapture(HWND hwnd)
{
    TraceLog("SetCapture(%p) suppressed", hwnd);
    return NULL;
}

static BOOL WINAPI Hooked_ReleaseCapture(void)
{
    return TRUE;
}

/* patch one named import in an import descriptor; returns 1 if patched */
static int PatchImport(HMODULE mod, IMAGE_IMPORT_DESCRIPTOR* desc,
                       const char* func, void* hook, void** orig)
{
    IMAGE_THUNK_DATA* nameThunk = desc->OriginalFirstThunk ?
        (IMAGE_THUNK_DATA*)((char*)mod + desc->OriginalFirstThunk) : NULL;
    IMAGE_THUNK_DATA* iat = (IMAGE_THUNK_DATA*)((char*)mod + desc->FirstThunk);
    if (!nameThunk) return 0;
    for (; nameThunk->u1.Function && iat->u1.Function; nameThunk++, iat++) {
        char* fn;
        DWORD oldProt;
        if (IMAGE_SNAP_BY_ORDINAL(nameThunk->u1.Ordinal)) continue;
        fn = (char*)((IMAGE_IMPORT_BY_NAME*)((char*)mod +
                     nameThunk->u1.AddressOfData))->Name;
        if (lstrcmpiA(fn, func) != 0) continue;
        if (orig && !*orig) *orig = (void*)iat->u1.Function;
        if (VirtualProtect(&iat->u1.Function, sizeof(void*),
                           PAGE_READWRITE, &oldProt)) {
            iat->u1.Function = (ULONG_PTR)hook;
            VirtualProtect(&iat->u1.Function, sizeof(void*), oldProt, &oldProt);
            TraceLog("IAT patched: %s -> hook", func);
            return 1;
        }
        TraceLog("IAT patch FAILED (protect): %s", func);
        return 0;
    }
    TraceLog("IAT patch: %s not found in descriptor", func);
    return 0;
}

/* redirect the game dll's gdi32 present blits into our backbuffer and
 * stop it from capturing the mouse */
static void PatchGameIAT(HMODULE mod)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((char*)mod + dos->e_lfanew);
    IMAGE_IMPORT_DESCRIPTOR* desc;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    if (!nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size)
        return;
    desc = (IMAGE_IMPORT_DESCRIPTOR*)((char*)mod +
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; desc->Name; desc++) {
        char* dll = (char*)mod + desc->Name;
        if (_strnicmp(dll, "gdi32", 5) == 0) {
            PatchImport(mod, desc, "BitBlt",
                        (void*)Hooked_BitBlt, (void**)&Real_BitBlt);
            PatchImport(mod, desc, "StretchBlt",
                        (void*)Hooked_StretchBlt, (void**)&Real_StretchBlt);
            PatchImport(mod, desc, "SetDIBitsToDevice",
                        (void*)Hooked_SetDIBitsToDevice,
                        (void**)&Real_SetDIBitsToDevice);
        }
        else if (_strnicmp(dll, "user32", 6) == 0) {
            PatchImport(mod, desc, "SetCapture",
                        (void*)Hooked_SetCapture, NULL);
            PatchImport(mod, desc, "ReleaseCapture",
                        (void*)Hooked_ReleaseCapture, NULL);
            PatchImport(mod, desc, "GetDC",
                        (void*)Hooked_GetDC, (void**)&Real_GetDC);
            PatchImport(mod, desc, "ReleaseDC",
                        (void*)Hooked_ReleaseDC, (void**)&Real_ReleaseDC);
            PatchImport(mod, desc, "BeginPaint",
                        (void*)Hooked_BeginPaint, (void**)&Real_BeginPaint);
            PatchImport(mod, desc, "EndPaint",
                        (void*)Hooked_EndPaint, (void**)&Real_EndPaint);
            PatchImport(mod, desc, "ScreenToClient",
                        (void*)Hooked_ScreenToClient,
                        (void**)&Real_ScreenToClient);
            PatchImport(mod, desc, "GetCursorPos",
                        (void*)Hooked_GetCursorPos, (void**)&Real_GetCursorPos);
            PatchImport(mod, desc, "WindowFromPoint",
                        (void*)Hooked_WindowFromPoint,
                        (void**)&Real_WindowFromPoint);
        }
        else if (_strnicmp(dll, "advapi32", 8) == 0) {
            /* portable save data: keep HKCU\SOFTWARE\PopCap... off C:\ */
            PatchImport(mod, desc, "RegOpenKeyExA",
                        (void*)Hook_RegOpenKeyExA, NULL);
            PatchImport(mod, desc, "RegCreateKeyExA",
                        (void*)Hook_RegCreateKeyExA, NULL);
            PatchImport(mod, desc, "RegQueryValueExA",
                        (void*)Hook_RegQueryValueExA, NULL);
            PatchImport(mod, desc, "RegSetValueExA",
                        (void*)Hook_RegSetValueExA, NULL);
            PatchImport(mod, desc, "RegCloseKey",
                        (void*)Hook_RegCloseKey, NULL);
            PatchImport(mod, desc, "RegDeleteValueA",
                        (void*)Hook_RegDeleteValueA, NULL);
            PatchImport(mod, desc, "RegDeleteKeyA",
                        (void*)Hook_RegDeleteKeyA, NULL);
        }
    }
}

/* draw the captured frame into the view: integer stretch + letterbox */
static void PresentFrame(void)
{
    RECT rc, full;
    int cw, ch, dw, dh, dx, dy;
    HDC vdc;
    if (!g_viewWnd || !g_backDC || g_gameW <= 0) return;
    if (!g_frameWnd || !IsWindowVisible(g_frameWnd)) return; /* hidden probe */
    if (!GetClientRect(g_viewWnd, &rc)) return;
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;

    if (cw >= g_gameW && ch >= g_gameH) {
        /* integer upscale, letterbox the remainder */
        int k = cw / g_gameW < ch / g_gameH ? cw / g_gameW : ch / g_gameH;
        if (k < 1) k = 1;
        dw = g_gameW * k;
        dh = g_gameH * k;
    } else {
        /* window smaller than the game: fit (rare; min size prevents it) */
        dw = cw;
        dh = MulDiv(cw, g_gameH, g_gameW);
        if (dh > ch) { dh = ch; dw = MulDiv(ch, g_gameW, g_gameH); }
    }
    dx = (cw - dw) / 2;
    dy = (ch - dh) / 2;

    /* compose offscreen: stretch into a view-sized buffer (recreate it /
     * refill the letterbox bars when the layout changed) */
    if (!g_compDC || g_compW != cw || g_compH != ch) {
        vdc = GetDC(g_viewWnd);
        if (!vdc) return;
        if (g_compDC) { DeleteDC(g_compDC); DeleteObject(g_compBmp); }
        g_compDC = CreateCompatibleDC(vdc);
        g_compBmp = CreateCompatibleBitmap(vdc, cw, ch);
        ReleaseDC(g_viewWnd, vdc);
        if (!g_compDC || !g_compBmp) { g_compDC = NULL; return; }
        SelectObject(g_compDC, g_compBmp);
        g_compW = cw;
        g_compH = ch;
        SetRect(&full, 0, 0, cw, ch);
        FillRect(g_compDC, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    if (g_destRect.left != dx || g_destRect.top != dy ||
        g_destRect.right != dx + dw || g_destRect.bottom != dy + dh) {
        SetRect(&full, 0, 0, cw, ch);
        FillRect(g_compDC, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));
    }
    g_destRect.left = dx;       g_destRect.top = dy;
    g_destRect.right = dx + dw; g_destRect.bottom = dy + dh;

    SetStretchBltMode(g_compDC, COLORONCOLOR);
    StretchBlt(g_compDC, dx, dy, dw, dh, g_backDC, 0, 0, g_gameW, g_gameH,
               SRCCOPY);

    /* one fast blit to the visible view — the only screen write we make */
    vdc = GetDC(g_viewWnd);
    if (!vdc) return;
    BitBlt(vdc, 0, 0, cw, ch, g_compDC, 0, 0, SRCCOPY);
    ReleaseDC(g_viewWnd, vdc);
}

/* view client coords -> game coords (via the last presented dest rect) */
static void MapViewToGame(int vx, int vy, int* gx, int* gy)
{
    int dw = g_destRect.right - g_destRect.left;
    int dh = g_destRect.bottom - g_destRect.top;
    int x = 0, y = 0;
    if (dw > 0 && dh > 0 && g_gameW > 0 && g_gameH > 0) {
        x = (vx - g_destRect.left) * g_gameW / dw;
        y = (vy - g_destRect.top) * g_gameH / dh;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x >= g_gameW) x = g_gameW - 1;
        if (y >= g_gameH) y = g_gameH - 1;
    }
    *gx = x;
    *gy = y;
}

/*
 * Every window message the game window receives goes through
 * SexyProcessMsg first — including SENT messages (WM_SETCURSOR,
 * WM_ACTIVATE/WM_ACTIVATEAPP, WM_SIZE, ...) which never pass through the
 * PeekMessage queue. The game needs these for the focus-pause overlay and
 * its own cursor management. This mirrors the original loader's subclass
 * proc: game handled -> return 1, else fall through to the normal handling.
 */
static LRESULT CALLBACK GameWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_gameObj && g_fns.ProcessMsg) {
        LRESULT result = 0;
        if (g_fns.ProcessMsg(g_gameObj, msg, wp, lp, &result))
            return result; /* the game handled it and wrote the result */
        if (g_traceMsgs)
            TraceLog("msg %04X not handled (wp=%X lp=%X)", msg, wp, lp);
    }

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        /* the game (or our capture pipeline) fully repaints the window;
         * erasing first only produces flicker */
        return 1;
    case WM_SETCURSOR:
        /* don't let the class cursor (arrow) fight the game's own cursor
         * management on the client area */
        if (LOWORD(lp) == HTCLIENT) {
            SetCursor(LoadCursor(NULL, IDC_ARROW));
            return 1;
        }
        break; /* non-client: let DefWindowProc size borders */
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* top-level window: chrome, focus routing, view layout */
static LRESULT CALLBACK FrameWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        TraceLog("frame WM_SIZE %dx%d maximized=%d", LOWORD(lp), HIWORD(lp),
                 IsZoomed(hwnd));
        if (g_viewWnd)
            MoveWindow(g_viewWnd, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        if (g_gameWnd && g_gameW > 0)
            SetWindowPos(g_gameWnd, NULL, 0, 0, g_gameW, g_gameH,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    case WM_GETMINMAXINFO: {
        /* never let the client get smaller than the game's native size */
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        if (g_gameW > 0 && g_gameH > 0) {
            RECT need = { 0, 0, g_gameW, g_gameH };
            AdjustWindowRect(&need, WS_OVERLAPPEDWINDOW, FALSE);
            mmi->ptMinTrackSize.x = need.right - need.left;
            mmi->ptMinTrackSize.y = need.bottom - need.top;
        }
        return 0;
    }
    case WM_ACTIVATE:
        /* keep the game's focus-pause logic working */
        ForwardToGame(WM_ACTIVATE, wp, lp);
        if (LOWORD(wp) != WA_INACTIVE && g_gameWnd)
            SetFocus(g_gameWnd);
        break;
    case WM_ACTIVATEAPP:
        ForwardToGame(WM_ACTIVATEAPP, wp, lp);
        break;
    case WM_SETFOCUS:
        if (g_gameWnd) SetFocus(g_gameWnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/* covers the frame client; shows the scaled game image and feeds input */
static LRESULT CALLBACK ViewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        int cw = ps.rcPaint.right - ps.rcPaint.left;
        int ch = ps.rcPaint.bottom - ps.rcPaint.top;
        if (g_compDC && g_compW == cw && g_compH == ch) {
            /* already-composed frame: single fast blit */
            BitBlt(hdc, 0, 0, cw, ch, g_compDC, 0, 0, SRCCOPY);
        } else {
            int dw = g_destRect.right - g_destRect.left;
            int dh = g_destRect.bottom - g_destRect.top;
            RECT rc;
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
            if (g_backDC && g_gameW > 0 && dw > 0 && dh > 0) {
                SetStretchBltMode(hdc, COLORONCOLOR);
                StretchBlt(hdc, g_destRect.left, g_destRect.top, dw, dh,
                           g_backDC, 0, 0, g_gameW, g_gameH, SRCCOPY);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        /* keep keyboard focus on the game window */
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            /* let the game pick its own cursor (hand over buttons etc.);
             * only fall back to the arrow when it doesn't handle it */
            if (!ForwardToGameEx(WM_SETCURSOR, (WPARAM)g_gameWnd,
                                 MAKELPARAM(HTCLIENT, HIWORD(lp))))
                SetCursor(LoadCursor(NULL, IDC_ARROW));
            return 1;
        }
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: {
        int gx, gy;
        MapViewToGame((short)LOWORD(lp), (short)HIWORD(lp), &gx, &gy);
        {
            static int n;
            if (n++ < 150 || (n % 200) == 0)
                TraceLog("fwd msg=%04X view=(%d,%d) -> game=(%d,%d)",
                         msg, (short)LOWORD(lp), (short)HIWORD(lp), gx, gy);
        }
        ForwardToGame(msg, wp, MAKELPARAM(gx, gy));
        return 0;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: {
        /* wheel messages carry SCREEN coordinates in lParam */
        POINT pt;
        int gx, gy;
        pt.x = (short)LOWORD(lp);
        pt.y = (short)HIWORD(lp);
        ScreenToClient(hwnd, &pt);
        MapViewToGame(pt.x, pt.y, &gx, &gy);
        ForwardToGame(msg, wp, MAKELPARAM(gx, gy));
        return 0;
    }
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

/*
 * Load and run one game. Returns TRUE if the game dll ran (even if the
 * player quit). hidden/probeMs are for smoke-testing (--probe).
 */
static BOOL RunGame(GameCfg* cfg, BOOL hidden, DWORD probeMs)
{
    static Host host;
    WNDCLASSA wc;
    HWND hwnd = NULL;
    HINSTANCE hInst = GetModuleHandleA(NULL);
    void* game = NULL;
    GameFns fn;
    char cmdline[MAX_PATH];
    DWORD style;
    RECT rc;
    int w = 640, h = 480;
    MSG msg;
    BOOL quit = FALSE, ok = FALSE;
    DWORD lastFrame, startTick;

    TraceLog("RunGame: dll=\"%s\" cab=\"%s\" dir=\"%s\"", cfg->dll, cfg->cab, cfg->dir);
    if (!FileExists(cfg->dll)) {
        TraceLog("dll missing");
        if (cfg->cab[0] && FileExists(cfg->cab)) {
            int r;
            TraceLog("extracting \"%s\" -> \"%s\"", cfg->cab, cfg->dir);
            r = cab_extract(cfg->cab, cfg->dir);
            TraceLog("cab_extract -> %d", r);
            if (r != 0) {
                ShowErr("PCap Loader",
                        "Failed to extract \"%s\" (error %d).", cfg->cab, r);
                return FALSE;
            }
        }
        if (!FileExists(cfg->dll)) {
            ShowErr("PCap Loader",
                    "Couldn't find game dll:\n%s\n\n"
                    "Set \"dll\" (and optionally \"cab\") in the game's .json.",
                    cfg->dll);
            return FALSE;
        }
    }

    host.hDll = LoadLibraryA(cfg->dll);
    if (!host.hDll) {
        ShowErr("PCap Loader", "Failed to load \"%s\" (error %d).",
                cfg->dll, GetLastError());
        return FALSE;
    }
    TraceLog("Loaded %s", cfg->dll);
    fn.CreateGame            = (FnCreateGame)GetProcAddress(host.hDll, "SexyCreateGame");
    fn.DeleteGame            = (FnDeleteGame)GetProcAddress(host.hDll, "SexyDeleteGame");
    fn.GetDimensions         = (FnGetDimensions)GetProcAddress(host.hDll, "SexyGetDimensions");
    fn.ProcessMsg            = (FnProcessMsg)GetProcAddress(host.hDll, "SexyProcessMsg");
    fn.ReceiveNotification   = (FnReceiveNotification)GetProcAddress(host.hDll, "SexyReceiveNotification");
    fn.UpdateState           = (FnUpdateState)GetProcAddress(host.hDll, "SexyUpdateState");

    if (!fn.CreateGame || !fn.DeleteGame || !fn.ProcessMsg || !fn.UpdateState) {
        ShowErr("PCap Loader",
                "\"%s\" is not a PopCap plugin game dll.\n"
                "Export missing: %s", cfg->dll,
                !fn.CreateGame ? "SexyCreateGame" :
                !fn.DeleteGame ? "SexyDeleteGame" :
                !fn.ProcessMsg ? "SexyProcessMsg" : "SexyUpdateState");
        FreeLibrary(host.hDll);
        host.hDll = NULL;
        return FALSE;
    }
    g_fns = fn;

    /* portable save data: redirect HKCU\SOFTWARE\PopCap... into
     * savedata\<game>\registry before the game can touch the registry */
    {
        char saveBase[MAX_PATH];
        ExeDir(saveBase, sizeof(saveBase));
        _snprintf(saveBase, sizeof(saveBase) - 1, "%s\\savedata\\%s",
                  saveBase, cfg->name);
        saveBase[sizeof(saveBase) - 1] = 0;
        PortRegInit(saveBase);
    }

    /* route the game's present blits into our backbuffer */
    PatchGameIAT(host.hDll);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = FrameWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = NULL;
    wc.lpszClassName = WCL_FRAME;
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = GameWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = WCL_GAME;
    RegisterClassA(&wc);

    memset(&wc, 0, sizeof(wc));
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = ViewWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = WCL_VIEW;
    RegisterClassA(&wc);

    style = WS_OVERLAPPEDWINDOW | WS_VISIBLE;
    SetRect(&rc, 0, 0, w, h);
    AdjustWindowRect(&rc, style, FALSE);
    hwnd = CreateWindowA(WCL_FRAME, cfg->display, style,
                         CW_USEDEFAULT, CW_USEDEFAULT,
                         rc.right - rc.left, rc.bottom - rc.top,
                         NULL, NULL, hInst, NULL);
    if (!hwnd) {
        ShowErr("PCap Loader", "Failed to create window (error %d).",
                GetLastError());
        FreeLibrary(host.hDll);
        host.hDll = NULL;
        return FALSE;
    }
    g_frameWnd = hwnd;

    /* native-size render target for the game (initial guess, corrected
     * after SexyGetDimensions below) */
    g_gameWnd = CreateWindowA(WCL_GAME, "", WS_CHILD | WS_VISIBLE,
                              0, 0, w, h, hwnd, NULL, hInst, NULL);
    /* the view covers the whole frame client and sits above the game
     * window (created later => higher z-order); we present the scaled
     * image into it */
    {
        RECT fc;
        GetClientRect(hwnd, &fc);
        g_viewWnd = CreateWindowA(WCL_VIEW, "", WS_CHILD | WS_VISIBLE,
                                  0, 0, fc.right, fc.bottom,
                                  hwnd, NULL, hInst, NULL);
    }
    if (!g_gameWnd || !g_viewWnd) {
        ShowErr("PCap Loader", "Failed to create game view (error %d).",
                GetLastError());
        g_frameWnd = g_viewWnd = g_gameWnd = NULL;
        DestroyWindow(hwnd);
        FreeLibrary(host.hDll);
        host.hDll = NULL;
        return FALSE;
    }
    /* the view must sit ABOVE the game window so that all mouse input goes
     * through our coordinate mapping at any scale (otherwise the game child
     * receives raw coordinates that are only correct at 1x) */
    SetWindowPos(g_viewWnd, g_gameWnd, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (hidden)
        ShowWindow(hwnd, SW_HIDE);
    UpdateWindow(hwnd);

    /* the game loads "resources.xml", "levels/levels.xml", ... relative to
     * this string, so it must be the game dir with a trailing separator.
     * Some games (Bookworm Adventures) concatenate absolute paths into a
     * fixed ~120-byte stack buffer without bounds checks — paths longer
     * than that overflow it and trip the dll's /GS cookie (crash
     * 0xC0000409 in BookwormAdventures.dll+0x3df90). Only for those games
     * do we need a SHORT path: map the game dir to a free drive letter
     * ("X:", like `subst`), falling back to the 8.3 short path, then the
     * real path. Ordinary-length dirs are passed through unchanged. */
#define PCAPLOAD_PATH_BUDGET 100
    {
        char tmp[MAX_PATH];
        DWORD n = (DWORD)strlen(cfg->dir);
        while (n > 0 && (cfg->dir[n-1] == '\\' || cfg->dir[n-1] == '/'))
            n--;
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, cfg->dir, n);
        tmp[n] = 0;

        g_substDrive[0] = 0;
        g_substDir[0] = 0;
        if (n > PCAPLOAD_PATH_BUDGET) {
            TraceLog("game dir is %d chars (> %d budget), shortening it",
                     (int)n, PCAPLOAD_PATH_BUDGET);
            if (DriveAcquire(tmp, g_substDrive, sizeof(g_substDrive)))
                lstrcpynA(g_substDir, tmp, sizeof(g_substDir));
        }

        if (g_substDrive[0]) {
            _snprintf(cmdline, sizeof(cmdline) - 1, "%s\\", g_substDrive);
        } else if (n > PCAPLOAD_PATH_BUDGET) {
            /* no free drive letter: try the 8.3 short path */
            char shortPath[MAX_PATH];
            DWORD r = GetShortPathNameA(tmp, shortPath, sizeof(shortPath) - 2);
            TraceLog("GetShortPathNameA(\"%s\") -> %d (\"%s\")", tmp, (int)r, shortPath);
            if (r > 0 && r < sizeof(shortPath) - 2 &&
                strlen(shortPath) < strlen(tmp))
                lstrcpynA(tmp, shortPath, sizeof(tmp) - 2);
            _snprintf(cmdline, sizeof(cmdline) - 1, "%s\\", tmp);
        } else {
            _snprintf(cmdline, sizeof(cmdline) - 1, "%s\\", tmp);
        }
    }
    cmdline[sizeof(cmdline) - 1] = 0;
    SetCurrentDirectoryA(cmdline[0] && cmdline[strlen(cmdline) - 1] == '\\'
                         ? cmdline : cfg->dir);

    {
        HMODULE hd = host.hDll;
        memset(&host, 0, sizeof(host));
        host.hDll = hd;
        host.cfg = cfg;
        host.hwnd = g_gameWnd;
        host.ReceiveNotification = fn.ReceiveNotification;
        InstallCreateHook(&host);
    }

    TraceLog("CreateGame(hwnd=%p, cmdline=\"%s\", flags=0, ctx, resolve)... dllbase=%p",
        g_gameWnd, cmdline, host.hDll);
    game = fn.CreateGame(g_gameWnd, cmdline, 0, &host, HookResolve);
    TraceLog("CreateGame returned %p", game);
    g_gameObj = game; /* forward window messages to the game from now on */
    if (!game) {
        ShowErr("PCap Loader", "Game load failed (SexyCreateGame returned NULL).");
        DestroyWindow(hwnd);
        FreeLibrary(host.hDll);
        host.hDll = NULL;
        DriveRelease();
        return FALSE;
    }

    if (fn.GetDimensions) {
        fn.GetDimensions(game, &w, &h);
        if (w <= 0 || h <= 0) { w = 640; h = 480; }
    }
    TraceLog("GetDimensions -> %dx%d", w, h);
    g_gameW = w;
    g_gameH = h;

    /* exact native size for the render target + presentation buffer */
    SetWindowPos(g_gameWnd, NULL, 0, 0, w, h,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    EnsureBackBuffer();
    {
        RECT fc;
        GetClientRect(hwnd, &fc);
        MoveWindow(g_viewWnd, 0, 0, fc.right, fc.bottom, FALSE);
    }

    SetRect(&rc, 0, 0, w, h);
    AdjustWindowRect(&rc, style, FALSE);
    CenterWindow(hwnd, rc.right - rc.left, rc.bottom - rc.top);
    if (!hidden)
        SetFocus(g_gameWnd);

    lastFrame = timeGetTime();
    startTick = lastFrame;

    while (!quit) {
        /* message pump — messages reach the game via GameWndProc */
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = TRUE; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (quit) break;

        /* don't render into a minimized window */
        if (IsIconic(hwnd)) {
            Sleep(GAME_FRAME_MS);
            lastFrame = timeGetTime();
            if (probeMs && timeGetTime() - startTick > probeMs) break;
            continue;
        }

        /* one SexyApp frame: update + render */
        {
            int ur;
            /* deliver queued host->game notifications first (extras results) */
            if (host.notifyPending) {
                host.notifyPending = FALSE;
                TraceLog("SexyReceiveNotification(\"%s\", \"%s\")",
                         host.notifyName, host.notifyVal);
                if (fn.ReceiveNotification)
                    fn.ReceiveNotification(game, host.notifyName, host.notifyVal);
            }
            ur = fn.UpdateState(game);
            if (g_frames++ < 30)
                TraceLog("UpdateState[%d] -> %d", g_frames, ur);
            if (ur < 0) {
                TraceLog("UpdateState negative, quitting loop");
                break;
            }

            /* present the backbuffer scaled with letterbox */
            PresentFrame();
        }

        if (probeMs && timeGetTime() - startTick > probeMs)
            break;

        /* pace to ~100 fps */
        {
            DWORD now = timeGetTime();
            DWORD dt = now - lastFrame;
            if (dt < GAME_FRAME_MS)
                Sleep(GAME_FRAME_MS - dt);
            lastFrame = timeGetTime();
        }
    }

    ok = TRUE;
    g_gameObj = NULL; /* stop routing messages to the dying game */
    if (host.loadDC) {
        DeleteDC(host.loadDC);
        DeleteObject(host.loadBmp);
        host.loadDC = NULL;
        host.loadBmp = NULL;
        host.loadW = host.loadH = 0;
    }
    if (g_backDC) {
        DeleteDC(g_backDC);
        DeleteObject(g_backBmp);
        g_backDC = NULL;
        g_backBmp = NULL;
    }
    if (g_compDC) {
        DeleteDC(g_compDC);
        DeleteObject(g_compBmp);
        g_compDC = NULL;
        g_compBmp = NULL;
        g_compW = g_compH = 0;
    }
    g_viewWnd = g_gameWnd = NULL;
    fn.DeleteGame(game);
    g_frameWnd = NULL;
    if (IsWindow(hwnd))
        DestroyWindow(hwnd);
    FreeLibrary(host.hDll);
    host.hDll = NULL;
    /* remove the drive-letter mapping of the game dir */
    DriveRelease();
    PortRegShutdown();
    return ok;
}

/* ---------------- launcher ---------------- */

#define WCL_LAUNCH  "PCapLoaderLaunchWnd"
#define IDC_LIST    1001
#define IDC_PLAY    1002
#define IDC_HINT    1003

static int g_pick = -1;

static LRESULT CALLBACK LaunchWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE hInst = ((LPCREATESTRUCTA)lp)->hInstance;
        CreateWindowA("STATIC",
                      "PopCap web-plugin games:",
                      WS_CHILD | WS_VISIBLE,
                      12, 10, 340, 20,
                      hwnd, (HMENU)IDC_HINT, hInst, NULL);
        CreateWindowA("LISTBOX", NULL,
                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_BORDER |
                      LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                      12, 34, 340, 220,
                      hwnd, (HMENU)IDC_LIST, hInst, NULL);
        CreateWindowA("BUTTON", "Play",
                      WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                      187, 262, 165, 28,
                      hwnd, (HMENU)IDC_PLAY, hInst, NULL);
        {
            int i;
            HWND lb = GetDlgItem(hwnd, IDC_LIST);
            for (i = 0; i < g_ngames; i++)
                SendMessageA(lb, LB_ADDSTRING, 0, (LPARAM)g_games[i].display);
            SendMessageA(lb, LB_SETCURSEL, 0, 0);
        }
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        MoveWindow(GetDlgItem(hwnd, IDC_LIST), 12, 34, w - 24, h - 96, TRUE);
        MoveWindow(GetDlgItem(hwnd, IDC_PLAY), w - 177, h - 46, 165, 28, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_PLAY ||
            (HIWORD(wp) == LBN_DBLCLK && LOWORD(wp) == IDC_LIST)) {
            int sel = (int)SendMessageA(GetDlgItem(hwnd, IDC_LIST),
                                        LB_GETCURSEL, 0, 0);
            if (sel >= 0 && sel < g_ngames) {
                g_pick = sel;
                DestroyWindow(hwnd);
            }
        }
        return 0;
    case WM_DESTROY:
        if (g_pick < 0)
            PostQuitMessage(0); /* window closed without a pick -> exit app */
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int LauncherRun(HINSTANCE hInst)
{
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = LaunchWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WCL_LAUNCH;
    RegisterClassA(&wc);

    hwnd = CreateWindowA(WCL_LAUNCH, "PCap Loader",
                         WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                         CW_USEDEFAULT, CW_USEDEFAULT, 380, 340,
                         NULL, NULL, hInst, NULL);
    if (!hwnd) return -1;
    CenterWindow(hwnd, 380, 340);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (g_pick >= 0) break;
    }
    return g_pick;
}

/* ---------------- entry point ---------------- */

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow)
{
    (void)hPrev; (void)cmdLine; (void)nShow;

    SetProcessDPIAware();
    timeBeginPeriod(1);
    AddVectoredExceptionHandler(0, CrashHandler);
    g_logEnabled = GetEnvironmentVariableA("PCAPLOAD_LOG", NULL, 0) != 0;
    g_traceMsgs = g_logEnabled;

    ScanGames();
    TraceLog("--- pcapload start, %d game(s) found", g_ngames);

    /* command line: pcapload.exe [gameName | path\to\game.json] */
    if (__argc >= 2 && __argv[1][0] != '-') {
        GameCfg* c = FindGame(__argv[1]);
        if (c) {
            RunGame(c, FALSE, 0);
            timeEndPeriod(1);
            return 0;
        }
        ShowErr("PCap Loader",
                "Game \"%s\" not found.\n\n"
                "Add a .json file to the \"games\" folder next to pcapload.exe.",
                __argv[1]);
    }
    /* hidden smoke test: pcapload.exe --probe <game> [ms] */
    else if (__argc >= 3 && !lstrcmpiA(__argv[1], "--probe")) {
        GameCfg* c = FindGame(__argv[2]);
        DWORD ms = __argc >= 4 ? (DWORD)atol(__argv[3]) : 3000;
        g_quiet = TRUE;
        BOOL ok = c && RunGame(c, TRUE, ms);
        timeEndPeriod(1);
        return ok ? 0 : 1;
    }

    if (g_ngames == 0) {
        ShowErr("PCap Loader",
                "No games found.\n\n"
                "Put a game description .json into the \"games\" folder next "
                "to pcapload.exe (see README.md).");
        timeEndPeriod(1);
        return 1;
    }

    /* single game -> run it directly, else show the launcher */
    if (g_ngames == 1) {
        RunGame(&g_games[0], FALSE, 0);
    } else {
        for (;;) {
            int pick = LauncherRun(hInst);
            if (pick < 0 || pick >= g_ngames) break;
            g_pick = -1;
            RunGame(&g_games[pick], FALSE, 0);
        }
    }

    timeEndPeriod(1);
    return 0;
}
