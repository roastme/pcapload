/* pcapload - portable save data: registry redirection.
 *
 * The plugin games store their settings in the registry under
 * HKCU\SOFTWARE\PopCap\... (SfxVolume, GameContinue, ChallengeState, ...),
 * i.e. on C:. To make pcapload installs portable, the game dll's
 * advapi32 imports are IAT-patched (see main.c PatchGameIAT) and any key
 * under that prefix is mapped onto plain files below
 *
 *     savedata\<game>\registry\<subkey path>\<value name>
 *
 * Each value file is: [4-byte LE type][data]. The default value (NULL or ""
 * name) is stored as "(default)". Keys that are not ours (other roots,
 * other prefixes, real handles) pass through to the real advapi32.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "registry.h"

/* ---------------- state ---------------- */

#define PORTK_MAGIC 0x31504B52 /* "RKP1" */

typedef struct PortKey {
    DWORD magic;
    char  dir[MAX_PATH];          /* this key's directory */
    struct PortKey* next;
} PortKey;

static char   g_base[MAX_PATH] = "";  /* ...\savedata\<game>\registry */
static BOOL   g_active = FALSE;
static PortKey* g_keys = NULL;
static CRITICAL_SECTION g_cs;
static BOOL g_csInit = FALSE;
static FILE* g_trace = NULL;

static void RTrace(const char* fmt, ...)
{
    va_list ap;
    char buf[512];
    if (!g_active) return;
    if (!g_trace) {
        char path[MAX_PATH];
        _snprintf(path, sizeof(path) - 1, "%s\\..\\registry-trace.log", g_base);
        path[sizeof(path) - 1] = 0;
        g_trace = fopen(path, "a");
        if (!g_trace) return;
    }
    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    buf[sizeof(buf) - 1] = 0;
    va_end(ap);
    fputs(buf, g_trace);
    fputc('\n', g_trace);
    fflush(g_trace);
}

static LONG PortOpenInternal(HKEY root, const char* subkey, PHKEY phk,
                             BOOL create, LPDWORD disposition);

/* passthrough to the real advapi32 */
static void* RealApi(const char* name)
{
    HMODULE adv = GetModuleHandleA("advapi32.dll");
    if (!adv) adv = LoadLibraryA("advapi32.dll");
    return adv ? (void*)GetProcAddress(adv, name) : NULL;
}

/* ---------------- helpers ---------------- */

static BOOL IsPortKey(HKEY hk)
{
    PortKey* k = (PortKey*)hk;
    /* predefined root handles (HKLM, HKCU, ... = 0x8000000x) and any other
     * kernel-side value are never ours — don't dereference them! */
    if (!k || (ULONG_PTR)k >= 0x80000000) return FALSE;
    return k->magic == PORTK_MAGIC;
}

/* keep alnum, space and -_.()[]#; hex-escape everything else */
static void Sanitize(const char* in, char* out, int outsz)
{
    int o = 0;
    for (; *in && o < outsz - 4; in++) {
        unsigned char c = (unsigned char)*in;
        if (isalnum(c) || strchr(" -_.()[]#", c))
            out[o++] = (char)c;
        else
            o += _snprintf(out + o, 4, "%%%02X", c);
    }
    out[o] = 0;
}

/* full path of a key dir: g_base\<subkey with '\' kept, chars sanitized> */
static void KeyDir(const char* subkey, char* out, int outsz)
{
    char clean[MAX_PATH * 2];
    int o = 0, i;
    for (i = 0; subkey[i] && o < (int)sizeof(clean) - 2; i++) {
        unsigned char c = (unsigned char)subkey[i];
        if (c == '\\' || c == '/') {
            clean[o++] = '\\';
        } else if (isalnum(c) || strchr(" -_.()[]#", c)) {
            clean[o++] = (char)c;
        } else {
            o += _snprintf(clean + o, 4, "%%%02X", c);
        }
    }
    clean[o] = 0;
    _snprintf(out, outsz - 1, "%s\\%s", g_base, clean);
    out[outsz - 1] = 0;
}

static void Mkdirs(const char* path)
{
    char tmp[MAX_PATH * 2];
    char* p;
    _snprintf(tmp, sizeof(tmp) - 1, "%s", path);
    tmp[sizeof(tmp) - 1] = 0;
    for (p = tmp + 3; *p; p++) {
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
}

static BOOL DirExists(const char* path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL IsOurPrefix(HKEY root, const char* subkey)
{
    if (root != HKEY_CURRENT_USER || !subkey) return FALSE;
    return !_strnicmp(subkey, "SOFTWARE\\POPCAP", 15);
}

static PortKey* AllocKey(const char* dir)
{
    PortKey* k = (PortKey*)malloc(sizeof(PortKey));
    if (!k) return NULL;
    k->magic = PORTK_MAGIC;
    lstrcpynA(k->dir, dir, sizeof(k->dir));
    EnterCriticalSection(&g_cs);
    k->next = g_keys;
    g_keys = k;
    LeaveCriticalSection(&g_cs);
    return k;
}

static PortKey* FindKey(HKEY hk)
{
    PortKey* k;
    /* compare pointer VALUES only — hk may be a real registry handle (a
     * small integer) and must never be dereferenced here */
    EnterCriticalSection(&g_cs);
    for (k = g_keys; k; k = k->next)
        if ((HKEY)k == hk) break;
    LeaveCriticalSection(&g_cs);
    return k;
}

static void FreeKey(PortKey* k)
{
    PortKey** p;
    EnterCriticalSection(&g_cs);
    for (p = &g_keys; *p; p = &(*p)->next)
        if (*p == k) { *p = k->next; break; }
    LeaveCriticalSection(&g_cs);
    free(k);
}

/* value file name for a value name (default -> "(default)") */
static void ValueFile(const char* dir, const char* name, char* out, int outsz)
{
    char vname[128];
    Sanitize(name && name[0] ? name : "(default)", vname, sizeof(vname));
    _snprintf(out, outsz - 1, "%s\\%s", dir, vname);
    out[outsz - 1] = 0;
}

/* ---------------- open / create ---------------- */

static LONG PortOpenInternal(HKEY root, const char* subkey, PHKEY phk,
                             BOOL create, LPDWORD disposition)
{
    char dir[MAX_PATH];
    char sub[MAX_PATH];
    PortKey* parent;
    PortKey* k;
    BOOL existed;

    if (!phk) return ERROR_INVALID_PARAMETER;

    if (IsPortKey(root)) {
        /* relative open under one of our keys: always ours */
        parent = (PortKey*)root;
        if (!subkey || !subkey[0]) {
            k = AllocKey(parent->dir);
            if (!k) return ERROR_NOT_ENOUGH_MEMORY;
            *phk = (HKEY)k;
            if (disposition) *disposition = REG_OPENED_EXISTING_KEY;
            return ERROR_SUCCESS;
        }
        _snprintf(dir, sizeof(dir) - 1, "%s\\%s", parent->dir, subkey);
        dir[sizeof(dir) - 1] = 0;
    } else if (IsOurPrefix(root, subkey)) {
        KeyDir(subkey, dir, sizeof(dir));
    } else {
        return PORT_NOT_OURS;
    }

    existed = DirExists(dir);
    if (!create && !existed) return ERROR_FILE_NOT_FOUND;
    if (create && !existed) Mkdirs(dir);

    k = AllocKey(dir);
    if (!k) return ERROR_NOT_ENOUGH_MEMORY;
    *phk = (HKEY)k;
    if (disposition) *disposition = existed ? REG_OPENED_EXISTING_KEY
                                            : REG_CREATED_NEW_KEY;
    (void)sub;
    return ERROR_SUCCESS;
}

/* ---------------- query / set / delete ---------------- */

static LONG PortQueryValue(PortKey* k, const char* name, LPDWORD type,
                           LPBYTE data, LPDWORD cbData)
{
    char path[MAX_PATH * 2];
    FILE* f;
    DWORD ftype = REG_SZ;
    long fsize;

    if (!k) return ERROR_INVALID_PARAMETER;
    ValueFile(k->dir, name, path, sizeof(path));
    f = fopen(path, "rb");
    if (!f) return ERROR_FILE_NOT_FOUND;
    if (fread(&ftype, 1, 4, f) != 4) { fclose(f); return ERROR_FILE_NOT_FOUND; }
    fseek(f, 0, SEEK_END);
    fsize = ftell(f) - 4;
    if (fsize < 0) fsize = 0;

    if (type) *type = ftype;
    if (!data) {
        if (cbData) *cbData = (DWORD)fsize;
        fclose(f);
        return ERROR_SUCCESS;
    }
    if (!cbData) { fclose(f); return ERROR_INVALID_PARAMETER; }
    if (*cbData < (DWORD)fsize) {
        *cbData = (DWORD)fsize;
        fclose(f);
        return ERROR_MORE_DATA;
    }
    if (fsize > 0) {
        fseek(f, 4, SEEK_SET);
        if (fread(data, 1, fsize, f) != (size_t)fsize) {
            fclose(f);
            return ERROR_FILE_NOT_FOUND;
        }
    }
    *cbData = (DWORD)fsize;
    fclose(f);
    return ERROR_SUCCESS;
}

static LONG PortSetValue(PortKey* k, const char* name, DWORD type,
                         const BYTE* data, DWORD cb)
{
    char path[MAX_PATH * 2];
    FILE* f;
    if (!k || (cb > 0 && !data)) return ERROR_INVALID_PARAMETER;
    ValueFile(k->dir, name, path, sizeof(path));
    Mkdirs(k->dir);
    f = fopen(path, "wb");
    if (!f) return ERROR_ACCESS_DENIED;
    fwrite(&type, 1, 4, f);
    if (cb > 0) fwrite(data, 1, cb, f);
    fclose(f);
    return ERROR_SUCCESS;
}

static LONG PortDeleteValue(PortKey* k, const char* name)
{
    char path[MAX_PATH * 2];
    if (!k) return ERROR_INVALID_PARAMETER;
    ValueFile(k->dir, name, path, sizeof(path));
    if (!DeleteFileA(path)) return GetLastError() == ERROR_FILE_NOT_FOUND
                                 ? ERROR_FILE_NOT_FOUND : ERROR_ACCESS_DENIED;
    return ERROR_SUCCESS;
}

static BOOL EmptyDir(const char* path)
{
    char spec[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    _snprintf(spec, sizeof(spec) - 1, "%s\\*", path);
    spec[sizeof(spec) - 1] = 0;
    h = FindFirstFileA(spec, &fd);
    if (h == INVALID_HANDLE_VALUE) return TRUE;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) {
            FindClose(h);
            return FALSE;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return TRUE;
}

static void RmDirTree(const char* path)
{
    char spec[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    if (!DirExists(path)) return;
    _snprintf(spec, sizeof(spec) - 1, "%s\\*", path);
    spec[sizeof(spec) - 1] = 0;
    h = FindFirstFileA(spec, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char sub[MAX_PATH * 2];
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
                continue;
            _snprintf(sub, sizeof(sub) - 1, "%s\\%s", path, fd.cFileName);
            sub[sizeof(sub) - 1] = 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RmDirTree(sub);
            else
                DeleteFileA(sub);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
}

static LONG PortDeleteKey(HKEY root, const char* subkey)
{
    char dir[MAX_PATH];
    PortKey* parent;
    if (IsPortKey(root)) {
        parent = (PortKey*)root;
        if (!subkey || !subkey[0]) return ERROR_INVALID_PARAMETER;
        _snprintf(dir, sizeof(dir) - 1, "%s\\%s", parent->dir, subkey);
        dir[sizeof(dir) - 1] = 0;
    } else if (IsOurPrefix(root, subkey)) {
        KeyDir(subkey, dir, sizeof(dir));
    } else {
        return PORT_NOT_OURS;
    }
    if (!DirExists(dir)) return ERROR_FILE_NOT_FOUND;
    if (!EmptyDir(dir)) return ERROR_ACCESS_DENIED;
    RmDirTree(dir);
    return ERROR_SUCCESS;
}

/* ---------------- exported hooks (real signatures) ---------------- */

LONG __stdcall Hook_RegOpenKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions,
                                  REGSAM samDesired, PHKEY phkResult)
{
    LONG r;
    RTrace("OpenKey root=%p sub='%s'", hKey, lpSubKey ? lpSubKey : "(null)");
    r = PortOpenInternal(hKey, lpSubKey, phkResult, FALSE, NULL);
    if (r != PORT_NOT_OURS) { RTrace("  -> %ld hk=%p", r, phkResult ? *phkResult : 0); return r; }
    {
        typedef LONG (__stdcall *Fn)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
        Fn f = (Fn)RealApi("RegOpenKeyExA");
        return f ? f(hKey, lpSubKey, ulOptions, samDesired, phkResult)
                 : ERROR_PROC_NOT_FOUND;
    }
}

LONG __stdcall Hook_RegCreateKeyExA(HKEY hKey, LPCSTR lpSubKey, DWORD Reserved,
                                    LPSTR lpClass, DWORD dwOptions,
                                    REGSAM samDesired,
                                    SECURITY_ATTRIBUTES* sa, PHKEY phkResult,
                                    LPDWORD lpdwDisposition)
{
    LONG r;
    RTrace("CreateKey root=%p sub='%s'", hKey, lpSubKey ? lpSubKey : "(null)");
    r = PortOpenInternal(hKey, lpSubKey, phkResult, TRUE,
                              lpdwDisposition);
    if (r != PORT_NOT_OURS) { RTrace("  -> %ld hk=%p", r, phkResult ? *phkResult : 0); return r; }
    {
        typedef LONG (__stdcall *Fn)(HKEY, LPCSTR, DWORD, LPSTR, DWORD,
                                     REGSAM, SECURITY_ATTRIBUTES*, PHKEY,
                                     LPDWORD);
        Fn f = (Fn)RealApi("RegCreateKeyExA");
        return f ? f(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired,
                     sa, phkResult, lpdwDisposition)
                 : ERROR_PROC_NOT_FOUND;
    }
}

LONG __stdcall Hook_RegQueryValueExA(HKEY hKey, LPCSTR lpValueName,
                                     LPDWORD lpReserved, LPDWORD lpType,
                                     LPBYTE lpData, LPDWORD lpcbData)
{
    PortKey* k = FindKey(hKey);
    if (k) { LONG r = PortQueryValue(k, lpValueName, lpType, lpData, lpcbData);
             RTrace("Query hk=%p val='%s' -> %ld (cb=%lu)", hKey, lpValueName ? lpValueName : "(default)", r, lpcbData ? *lpcbData : 0);
             return r; }
    {
        typedef LONG (__stdcall *Fn)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE,
                                     LPDWORD);
        Fn f = (Fn)RealApi("RegQueryValueExA");
        return f ? f(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData)
                 : ERROR_PROC_NOT_FOUND;
    }
}

LONG __stdcall Hook_RegSetValueExA(HKEY hKey, LPCSTR lpValueName,
                                   DWORD Reserved, DWORD dwType,
                                   const BYTE* lpData, DWORD cbData)
{
    PortKey* k = FindKey(hKey);
    if (k) { LONG r = PortSetValue(k, lpValueName, dwType, lpData, cbData);
             RTrace("Set hk=%p val='%s' type=%lu cb=%lu -> %ld", hKey, lpValueName ? lpValueName : "(default)", dwType, cbData, r);
             return r; }
    {
        typedef LONG (__stdcall *Fn)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*,
                                     DWORD);
        Fn f = (Fn)RealApi("RegSetValueExA");
        return f ? f(hKey, lpValueName, Reserved, dwType, lpData, cbData)
                 : ERROR_PROC_NOT_FOUND;
    }
}

LONG __stdcall Hook_RegCloseKey(HKEY hKey)
{
    PortKey* k = FindKey(hKey);
    if (k) { RTrace("Close hk=%p", hKey); FreeKey(k); return ERROR_SUCCESS; }
    {
        typedef LONG (__stdcall *Fn)(HKEY);
        Fn f = (Fn)RealApi("RegCloseKey");
        return f ? f(hKey) : ERROR_PROC_NOT_FOUND;
    }
}

LONG __stdcall Hook_RegDeleteValueA(HKEY hKey, LPCSTR lpValueName)
{
    PortKey* k = FindKey(hKey);
    if (k) return PortDeleteValue(k, lpValueName);
    {
        typedef LONG (__stdcall *Fn)(HKEY, LPCSTR);
        Fn f = (Fn)RealApi("RegDeleteValueA");
        return f ? f(hKey, lpValueName) : ERROR_PROC_NOT_FOUND;
    }
}

LONG __stdcall Hook_RegDeleteKeyA(HKEY hKey, LPCSTR lpSubKey)
{
    LONG r = PortDeleteKey(hKey, lpSubKey);
    if (r != PORT_NOT_OURS) return r;
    {
        typedef LONG (__stdcall *Fn)(HKEY, LPCSTR);
        Fn f = (Fn)RealApi("RegDeleteKeyA");
        return f ? f(hKey, lpSubKey) : ERROR_PROC_NOT_FOUND;
    }
}

/* ---------------- lifecycle ---------------- */

void PortRegInit(const char* baseDir)
{
    if (!g_csInit) {
        InitializeCriticalSection(&g_cs);
        g_csInit = TRUE;
    }
    EnterCriticalSection(&g_cs);
    _snprintf(g_base, sizeof(g_base) - 1, "%s\\registry", baseDir);
    g_base[sizeof(g_base) - 1] = 0;
    g_active = TRUE;
    LeaveCriticalSection(&g_cs);
    Mkdirs(g_base);
}

void PortRegShutdown(void)
{
    EnterCriticalSection(&g_cs);
    while (g_keys) {
        PortKey* k = g_keys;
        g_keys = k->next;
        free(k);
    }
    g_active = FALSE;
    LeaveCriticalSection(&g_cs);
}
