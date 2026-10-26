/* pcapload - portable save data: registry redirection (see registry.c).
 * Hooks (real signatures) to be IAT-patched into the game dll: */
#ifndef PCAPLOAD_REGISTRY_H
#define PCAPLOAD_REGISTRY_H

/* returned by the internal router when a call does not belong to us and
 * must be passed through to the real advapi32 */
#define PORT_NOT_OURS  -1000

void PortRegInit(const char* baseDir);   /* ...\savedata\<game> */
void PortRegShutdown(void);

LONG __stdcall Hook_RegOpenKeyExA(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
LONG __stdcall Hook_RegCreateKeyExA(HKEY, LPCSTR, DWORD, LPSTR, DWORD,
                                    REGSAM, SECURITY_ATTRIBUTES*, PHKEY,
                                    LPDWORD);
LONG __stdcall Hook_RegQueryValueExA(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE,
                                     LPDWORD);
LONG __stdcall Hook_RegSetValueExA(HKEY, LPCSTR, DWORD, DWORD, const BYTE*,
                                   DWORD);
LONG __stdcall Hook_RegCloseKey(HKEY);
LONG __stdcall Hook_RegDeleteValueA(HKEY, LPCSTR);
LONG __stdcall Hook_RegDeleteKeyA(HKEY, LPCSTR);

#endif
