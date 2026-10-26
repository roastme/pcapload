#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include "cab.h"

/* optional trace (wired to the same log as main.c when enabled) */
extern void pcaplog(const char* fmt, ...);
#define CLOG pcaplog

/* ---- FDI (cabinet.dll) interface ---- */

typedef void* HFDI;

typedef struct {
    int  erfOper;
    int  erfType;
    BOOL fError;
} ERF;

typedef enum {
    /* Empirically verified against Windows cabinet.dll (xp->win10):
       the notification codes do NOT match the old Diamond headers. */
    fdintCABINET_INFO   = 0,   /* once, right after the header is read */
    fdintUNKNOWN1       = 1,   /* not observed yet */
    fdintCOPY_FILE      = 2,   /* open+return handle, 0=skip, -1=abort */
    fdintCLOSE_FILE_INFO= 3,   /* close, set timestamps, TRUE=success */
    fdintUNKNOWN4       = 4,   /* not observed yet */
    fdintENUMERATE      = 5,   /* once after CABINET_INFO; 1=continue */
    fdintUNKNOWN6       = 6
} FDINOTIFICATIONTYPE;

typedef struct {
    long cbSize;
    char* psz1;             /* file name inside cabinet */
    char* psz2;
    char* psz3;
    void* pv;
    int   hf;               /* file handle (as returned by our COPY_FILE) */
    unsigned short date;
    unsigned short time;
    unsigned short attribs;
    short setID;
    short iCabinet;
    int   fdie;
} FDINOTIFICATION, *PFDINOTIFICATION;

typedef HFDI (__cdecl *FNFDICreate)(
    void* (__cdecl *pfnalloc)(unsigned long),
    void  (__cdecl *pfnfree)(void*),
    int   (__cdecl *pfnopen)(char*, int, int),
    unsigned int (__cdecl *pfnread)(int, void*, unsigned int),
    unsigned int (__cdecl *pfnwrite)(int, void*, unsigned int),
    int   (__cdecl *pfnclose)(int),
    long  (__cdecl *pfnseek)(int, long, int),
    int   cpuType,
    ERF*  perf);

typedef BOOL (__cdecl *FNFDICopy)(
    HFDI hfdi,
    char* pszCabinetName,
    char* pszCabPath,
    int   flags,
    int   (__cdecl *pfnNotify)(FDINOTIFICATIONTYPE, PFDINOTIFICATION),
    void  (__cdecl *pfnDecryptDummy)(void), /* unused, pass NULL */
    void* pvUser);

typedef BOOL (__cdecl *FNFDIDestroy)(HFDI);

/* ---- per-extraction state ---- */

static char g_destDir[MAX_PATH];
static BOOL g_failed;

static void mkparents(char* path)
{
    char* p;
    for (p = path; *p; p++) {
        if (*p == '\\' || *p == '/') {
            char c = *p;
            *p = 0;
            CreateDirectoryA(path, NULL);
            *p = c;
        }
    }
}

/* Build "<g_destDir>\<member name>" from a cabinet member name.
 * Returns FALSE for absolute paths, drive specs, ".." components or
 * anything that would overflow "out" — a cab is untrusted input. */
static BOOL build_dest(char* out, size_t outsz, const char* name)
{
    const char* s;
    const char* p;
    size_t n;

    if (!name || !*name || !outsz) return FALSE;
    /* ignore leading separators; everything below stays relative */
    while (*name == '\\' || *name == '/') name++;
    if (name[0] && name[1] == ':') return FALSE;   /* C:... */
    if (strchr(name, ':')) return FALSE;
    /* reject any ".." path component */
    for (p = name; *p; ) {
        const char* q = p;
        while (*q && *q != '\\' && *q != '/') q++;
        if (q - p == 2 && p[0] == '.' && p[1] == '.') return FALSE;
        p = (*q) ? q + 1 : q;
    }

    n = strlen(g_destDir);
    if (n + 2 >= outsz) return FALSE;
    memcpy(out, g_destDir, n);
    if (n && out[n-1] != '\\' && out[n-1] != '/') out[n++] = '\\';
    for (s = name; *s; s++) {
        if (n + 2 >= outsz) return FALSE;
        out[n++] = (*s == '/') ? '\\' : *s;
    }
    out[n] = 0;
    return TRUE;
}

/* ---- FDI callbacks (cdecl, CRT-file based) ---- */

static void* __cdecl cb_alloc(unsigned long cb) { return malloc(cb); }
static void  __cdecl cb_free(void* pv)           { free(pv); }

static int __cdecl cb_open(char* pszFile, int oflag, int pmode)
{
    int fd = _open(pszFile, oflag, pmode);
    CLOG("fdi: open(\"%s\", oflag=%04X) -> %d (errno=%d)", pszFile, oflag, fd, errno);
    return fd;
}

static unsigned int __cdecl cb_read(int hf, void* pv, unsigned int cb)
{
    return _read(hf, pv, cb);
}

static unsigned int __cdecl cb_write(int hf, void* pv, unsigned int cb)
{
    return _write(hf, pv, cb);
}

static int __cdecl cb_close(int hf)
{
    return _close(hf);
}

static long __cdecl cb_seek(int hf, long dist, int seektype)
{
    return _lseek(hf, dist, seektype);
}

static int __cdecl cb_notify(int fdint, PFDINOTIFICATION pfdin)
{
    CLOG("fdi: notify fdint=%d", fdint);
    switch (fdint) {
    case 0: /* CABINET_INFO */
        CLOG("fdi: CABINET_INFO %s", pfdin->psz1 ? pfdin->psz1 : "");
        return 1; /* continue */

    case 2: { /* COPY_FILE */
        char path[MAX_PATH];
        int fd;
        CLOG("fdi: COPY_FILE \"%s\"", pfdin->psz1 ? pfdin->psz1 : "(null)");
        if (!pfdin->psz1 || !pfdin->psz1[0]) return 0; /* skip */
        if (!build_dest(path, sizeof(path), pfdin->psz1)) {
            /* never write outside the destination directory */
            CLOG("fdi: unsafe member name \"%s\", aborting", pfdin->psz1);
            g_failed = TRUE;
            return -1; /* abort */
        }
        mkparents(path);
        fd = _open(path, _O_BINARY | _O_WRONLY | _O_CREAT | _O_TRUNC,
                   _S_IREAD | _S_IWRITE);
        if (fd < 0) {
            CLOG("fdi: open(\"%s\") failed err=%d", path, errno);
            g_failed = TRUE;
            return -1; /* abort */
        }
        return fd;
    }

    case 3: { /* CLOSE_FILE_INFO */
        FILETIME ft, lft;
        SYSTEMTIME st;
        HANDLE h;
        WORD d = pfdin->date, t = pfdin->time;
        st.wYear  = ((d >> 9) & 0x7f) + 1980;
        st.wMonth = (d >> 5) & 0x0f;
        st.wDay   = d & 0x1f;
        st.wHour  = (t >> 11) & 0x1f;
        st.wMinute = (t >> 5) & 0x3f;
        st.wSecond = (t & 0x1f) * 2;
        st.wMilliseconds = 0;
        h = (HANDLE)_get_osfhandle(pfdin->hf);
        if (h != INVALID_HANDLE_VALUE &&
            SystemTimeToFileTime(&st, &lft) &&
            LocalFileTimeToFileTime(&lft, &ft))
            SetFileTime(h, &ft, &ft, &ft);
        _close(pfdin->hf);
        return TRUE;
    }

    default: /* 1/4/5/6: enumerate-ish and unknown — keep going */
        return 1;
    }
}

int cab_extract(const char* cabPath, const char* destDir)
{
    static FNFDICreate  pFDICreate  = NULL;
    static FNFDICopy    pFDICopy    = NULL;
    static FNFDIDestroy pFDIDestroy = NULL;
    static BOOL loaded = FALSE;

    HMODULE hCab;
    HFDI hfdi;
    ERF erf;
    char drive[_MAX_DRIVE], dir2[_MAX_DIR], fname[_MAX_FNAME], ext[_MAX_EXT];
    char cabDir[MAX_PATH], cabName[_MAX_FNAME + _MAX_EXT];
    BOOL ok;
    char* p;

    if (!loaded) {
        hCab = LoadLibraryA("cabinet.dll");
        if (!hCab) return 1;
        pFDICreate  = (FNFDICreate)GetProcAddress(hCab, "FDICreate");
        pFDICopy    = (FNFDICopy)GetProcAddress(hCab, "FDICopy");
        pFDIDestroy = (FNFDIDestroy)GetProcAddress(hCab, "FDIDestroy");
        if (!pFDICreate || !pFDICopy || !pFDIDestroy) {
            FreeLibrary(hCab);
            return 1;
        }
        loaded = TRUE; /* keep cabinet.dll loaded for the process lifetime */
    }

    _splitpath(cabPath, drive, dir2, fname, ext);
    _makepath(cabDir, drive, dir2, NULL, NULL);
    /* FDI concatenates pszCabPath + cabinet name verbatim, so the path
       must end with a separator */
    p = cabDir + strlen(cabDir);
    if (p > cabDir && p[-1] != '\\' && p[-1] != '/')
        *p++ = '\\';
    *p = 0;
    _makepath(cabName, NULL, NULL, fname, ext);

    strncpy(g_destDir, destDir, MAX_PATH - 1);
    g_destDir[MAX_PATH - 1] = 0;
    /* ensure destination exists */
    mkparents(g_destDir);
    CreateDirectoryA(g_destDir, NULL);
    g_failed = FALSE;

    hfdi = pFDICreate(cb_alloc, cb_free, cb_open, cb_read, cb_write,
                      cb_close, cb_seek, 0, &erf);
    CLOG("FDICreate -> %p (erf %d/%d)", hfdi, erf.erfOper, erf.erfType);
    if (!hfdi) return 2;

    ok = pFDICopy(hfdi, cabName, cabDir, 0, cb_notify, NULL, NULL);
    CLOG("FDICopy(\"%s\", \"%s\") -> %d (erfOper=%d)", cabName, cabDir,
         ok, erf.erfOper);
    pFDIDestroy(hfdi);

    if (!ok || g_failed) return 3;
    return 0;
}
