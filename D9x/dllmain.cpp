#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <stdarg.h>
#include "Shared.h"
#include "Direct3DDeviceProxy.h"

// ================= GLOBALS =================
HMODULE g_real = NULL;
FILE* g_log = NULL;

// ================= LOG FUNCTIONS =================
void Log(const char* msg)
{
    if (g_log) {
        fprintf(g_log, "%s\n", msg);
        fflush(g_log);
    }
}

void LogF(const char* fmt, ...)
{
    if (!g_log) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsprintf_s(buf, sizeof(buf), fmt, args);
    va_end(args);
    fprintf(g_log, "%s\n", buf);
    fflush(g_log);
}

// ================= GLOBAL VARIABLES =================
int g_drawCallsPerSec = 0;
int g_drawCallsPerFrame = 0;
int g_textDrawCallsPerSec = 0;
int g_textDrawCallsPerFrame = 0;
int g_confirmedTextDrawsTotal = 0;
int g_devicesCreated = 0;
int g_devicesAlive = 0;
int g_resets = 0;
int g_presentCalls = 0;
UINT g_maxPrim = 0;

int g_DIP = 0, g_DP = 0, g_RT = 0, g_TEX = 0;
int g_VS = 0, g_PS = 0, g_STREAM = 0, g_IDX = 0;

// ================= TYPEDEFS =================
typedef IDirect3D9* (WINAPI* tDirect3DCreate9)(UINT);
typedef HRESULT(WINAPI* tDirect3DCreate9Ex)(UINT, IDirect3D9Ex**);
typedef HRESULT(WINAPI* tCreateDevice)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);

// ================= POINTERS =================
tDirect3DCreate9   oDirect3DCreate9 = NULL;
tDirect3DCreate9Ex oDirect3DCreate9Ex = NULL;
tCreateDevice      oCreateDevice = NULL;

// ================= LOAD ORIGINAL =================
void LoadOriginal()
{
    if (g_real) return;
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    strcat_s(path, MAX_PATH, "\\d3d9.dll");
    g_real = LoadLibraryA(path);
    if (!g_real) { Log("[ERROR] LoadLibrary failed"); return; }
    oDirect3DCreate9 = (tDirect3DCreate9)GetProcAddress(g_real, "Direct3DCreate9");
    oDirect3DCreate9Ex = (tDirect3DCreate9Ex)GetProcAddress(g_real, "Direct3DCreate9Ex");
    Log("[INIT] Original d3d9.dll loaded");
}

// ================= HOOK CreateDevice =================
HRESULT WINAPI hkCreateDevice(IDirect3D9* self, UINT Adapter, D3DDEVTYPE Type, HWND hWnd,
    DWORD Flags, D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** dev)
{
    Log("[HOOK] CreateDevice called");
    if (!oCreateDevice) return E_FAIL;
    HRESULT hr = oCreateDevice(self, Adapter, Type, hWnd, Flags, pp, dev);
    if (SUCCEEDED(hr) && dev && *dev) {
        *dev = new HookedIDirect3DDevice9(*dev);
        LogF("[HOOK] Wrapped Device: %p", *dev);
    }
    return hr;
}

// ================= VTable Hook =================
void HookVTable(IDirect3D9* pD3D)
{
    if (!pD3D) return;
    void** vtbl = *(void***)pD3D;
    DWORD old;
    VirtualProtect(&vtbl[16], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    oCreateDevice = (tCreateDevice)vtbl[16];
    vtbl[16] = (void*)hkCreateDevice;
    VirtualProtect(&vtbl[16], sizeof(void*), old, &old);
    Log("[HOOK] VTable patched");
}

// ================= EXPORT Direct3DCreate9 =================
extern "C" IDirect3D9* WINAPI Direct3DCreate9(UINT SDKVersion)
{
    if (!oDirect3DCreate9) LoadOriginal();
    if (!oDirect3DCreate9) return NULL;
    LogF("[CALL] Direct3DCreate9 (SDK: %u)", SDKVersion);
    IDirect3D9* d3d = oDirect3DCreate9(SDKVersion);
    if (d3d) HookVTable(d3d);
    return d3d;
}

// ================= EXPORT Direct3DCreate9Ex =================
extern "C" HRESULT WINAPI Direct3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppD3D)
{
    if (!oDirect3DCreate9Ex) LoadOriginal();
    if (!oDirect3DCreate9Ex) return E_FAIL;
    LogF("[CALL] Direct3DCreate9Ex (SDK: %u)", SDKVersion);
    return oDirect3DCreate9Ex(SDKVersion, ppD3D);
}

// ================= FORWARD FUNCTIONS =================
#define FWD_VOID(name) extern "C" void WINAPI name() { static FARPROC f = NULL; if (!f && g_real) f = GetProcAddress(g_real, #name); if (f) ((void(WINAPI*)())f)(); }
#define FWD_INT(name) extern "C" int WINAPI name() { static FARPROC f = NULL; if (!f && g_real) f = GetProcAddress(g_real, #name); if (f) return ((int(WINAPI*)())f)(); return 0; }
#define FWD_DWORD(name) extern "C" DWORD WINAPI name() { static FARPROC f = NULL; if (!f && g_real) f = GetProcAddress(g_real, #name); if (f) return ((DWORD(WINAPI*)())f)(); return 0; }
#define FWD_BOOL(name) extern "C" BOOL WINAPI name() { static FARPROC f = NULL; if (!f && g_real) f = GetProcAddress(g_real, #name); if (f) return ((BOOL(WINAPI*)())f)(); return FALSE; }

FWD_VOID(D3D9GetSWInfo)
FWD_INT(D3DPERF_EndEvent)
FWD_DWORD(D3DPERF_GetStatus)
FWD_BOOL(D3DPERF_QueryRepeatFrame)

extern "C" void WINAPI D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR wszName) { static FARPROC f = NULL; if (!f && g_real) f = GetProcAddress(g_real, "D3DPERF_SetMarker"); if (f) ((void(WINAPI*)(D3DCOLOR, LPCWSTR))f)(color, wszName); }
extern "C" void WINAPI D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR wszName) { static FARPROC f = NULL; if (!f && g_real) f = GetProcAddress(g_real, "D3DPERF_SetRegion"); if (f) ((void(WINAPI*)(D3DCOLOR, LPCWSTR))f)(color, wszName); }
extern "C" void WINAPI D3DPERF_SetOptions(DWORD dwOptions) { static FARPROC f = NULL; if (!f && g_real) f = GetProcAddress(g_real, "D3DPERF_SetOptions"); if (f) ((void(WINAPI*)(DWORD))f)(dwOptions); }
extern "C" void WINAPI D3DPERF_SetOptionsEx(DWORD dwOptions, DWORD dwMask) { static FARPROC f = NULL; if (!f && g_real) f = GetProcAddress(g_real, "D3DPERF_SetOptionsEx"); if (f) ((void(WINAPI*)(DWORD, DWORD))f)(dwOptions, dwMask); }
extern "C" int WINAPI D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR wszName) { static FARPROC f = NULL; if (!f && g_real) f = GetProcAddress(g_real, "D3DPERF_BeginEvent"); if (f) return ((int(WINAPI*)(D3DCOLOR, LPCWSTR))f)(color, wszName); return 0; }
extern "C" HRESULT WINAPI Direct3D9EnableMaximizedWindowedModeShim(BOOL bEnable) { static FARPROC f = NULL; if (!f && g_real) f = GetProcAddress(g_real, "Direct3D9EnableMaximizedWindowedModeShim"); if (f) return ((HRESULT(WINAPI*)(BOOL))f)(bEnable); return S_OK; }

// ================= DLL MAIN =================
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hInst);
        fopen_s(&g_log, "d3d9_proxy_log.txt", "w");
        Log("╔════════════════════════════════════════════════════════════╗");
        Log("║     D3D9 PROXY DLL - Real Text Detection                   ║");
        Log("╚════════════════════════════════════════════════════════════╝");
        LoadOriginal();
    }
    else if (reason == DLL_PROCESS_DETACH) {
        LogF("[SHUTDOWN] Total confirmed glyphs: %d", g_confirmedTextDrawsTotal);
        if (g_log) fclose(g_log);
        if (g_real) FreeLibrary(g_real);
    }
    return TRUE;
}