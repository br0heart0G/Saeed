// ijl15.cpp — DLL Proxy Maphack for Warcraft III 1.26a
// Build: i686-w64-mingw32-g++ -O2 -shared -o ijl15.dll ijl15.cpp
//        -lkernel32 -luser32 -static-libgcc -static-libstdc++

#include <windows.h>
#include <cstring>

static void xor_decode(char* out, const unsigned char* enc, int len)
{
    for (int i = 0; i < len; i++)
        out[i] = (char)(enc[i] ^ 0x5A);
    out[len] = 0;
}

static const unsigned char ENC_GAME_DLL[] = {
    0x1D,0x2B,0x3F,0x1E,0x36,0x2E,0x3C,0x3C
};
static const unsigned char ENC_REAL_IJL[] = {
    0x13,0x30,0x3C,0x1B,0x0F,0x36,0x3C,0x3C,0x00
};

struct PatchEntry {
    DWORD  offset;
    BYTE   orig[8];
    BYTE   patch[8];
    SIZE_T len;
    BOOL   active;
};

static PatchEntry g_Patches[] = {
    { 0x0074C9F1, {}, {0x8B,0x14,0x24,0x90,0x52},  5, FALSE },
    { 0x003564B8, {}, {0x66,0x09,0xD0},             3, FALSE },
    { 0x003A15BA, {}, {0xEB},                        1, FALSE },
    { 0x003999F9, {}, {0x09,0xC3},                  2, FALSE },
    { 0x003A14C0, {}, {0xEB,0x30,0x90,0x90},        4, FALSE },
    { 0x00282A50, {}, {0x09,0xC2},                  2, FALSE },
    { 0x002851B2, {}, {0xEB},                        1, FALSE },
};
#define NUM_PATCHES (sizeof(g_Patches)/sizeof(g_Patches[0]))

#define FOG_START   0
#define FOG_END     3
#define ESP_START   4
#define ESP_END     5
#define CREEP_START 6
#define CREEP_END   6

static ULONG_PTR g_GameBase = 0;
static BOOL      g_Running  = TRUE;

static BOOL PatchMem(ULONG_PTR addr, const BYTE* bytes, SIZE_T len)
{
    DWORD old;
    if (!VirtualProtect((LPVOID)addr, len, PAGE_EXECUTE_READWRITE, &old))
        return FALSE;
    memcpy((LPVOID)addr, bytes, len);
    VirtualProtect((LPVOID)addr, len, old, &old);
    return TRUE;
}

static void SetGroup(int start, int end, BOOL enable)
{
    if (!g_GameBase) return;
    for (int i = start; i <= end; i++) {
        PatchEntry& p = g_Patches[i];
        ULONG_PTR addr = g_GameBase + p.offset;
        if (enable && !p.active) {
            memcpy(p.orig, (LPVOID)addr, p.len);
            PatchMem(addr, p.patch, p.len);
            p.active = TRUE;
        } else if (!enable && p.active) {
            PatchMem(addr, p.orig, p.len);
            p.active = FALSE;
        }
    }
}

static BOOL g_FogOn = FALSE;
static BOOL g_ESPOn = FALSE;
static BOOL g_AllOn = FALSE;

static void ToggleFog() { g_FogOn = !g_FogOn; SetGroup(FOG_START, FOG_END, g_FogOn); }
static void ToggleESP() {
    g_ESPOn = !g_ESPOn;
    SetGroup(ESP_START,   ESP_END,   g_ESPOn);
    SetGroup(CREEP_START, CREEP_END, g_ESPOn);
}
static void ToggleAll() {
    g_AllOn = !g_AllOn;
    SetGroup(FOG_START,   FOG_END,   g_AllOn);
    SetGroup(ESP_START,   ESP_END,   g_AllOn);
    SetGroup(CREEP_START, CREEP_END, g_AllOn);
    g_FogOn = g_ESPOn = g_AllOn;
}
static void RevertAll() {
    SetGroup(FOG_START,   FOG_END,   FALSE);
    SetGroup(ESP_START,   ESP_END,   FALSE);
    SetGroup(CREEP_START, CREEP_END, FALSE);
    g_FogOn = g_ESPOn = g_AllOn = FALSE;
}

static void ErasePEHeader(HMODULE hMod)
{
    DWORD old;
    if (VirtualProtect(hMod, 0x1000, PAGE_READWRITE, &old)) {
        memset(hMod, 0, 0x1000);
        VirtualProtect(hMod, 0x1000, old, &old);
    }
}

static DWORD WINAPI HotkeyThread(LPVOID)
{
    char gameDllName[16] = {};
    xor_decode(gameDllName, ENC_GAME_DLL, 8);

    for (int tries = 0; tries < 150 && g_Running; tries++) {
        HMODULE hGame = GetModuleHandleA(gameDllName);
        if (hGame) { g_GameBase = (ULONG_PTR)hGame; break; }
        Sleep(200);
    }
    if (!g_GameBase) return 0;
    Sleep(8000);
    ToggleAll();

    while (g_Running) {
        if (GetAsyncKeyState(VK_F5) & 1) ToggleFog();
        if (GetAsyncKeyState(VK_F6) & 1) ToggleESP();
        if (GetAsyncKeyState(VK_F8) & 1) ToggleAll();
        if (GetAsyncKeyState(VK_F9) & 1) { RevertAll(); g_Running = FALSE; break; }
        Sleep(50);
    }
    return 0;
}

static HMODULE g_hSelf     = NULL;
static HANDLE  g_hHKThread = NULL;

static DWORD WINAPI MainThread(LPVOID hModule)
{
    ErasePEHeader((HMODULE)hModule);
    g_hHKThread = CreateThread(NULL, 0, HotkeyThread, NULL, 0, NULL);
    return 0;
}

// ── Real ijl15.dll proxy ──────────────────────────────────────────
static HMODULE g_hRealIjl = NULL;

typedef void* (__cdecl* pfnIjlInit_t)(void*);
typedef int   (__cdecl* pfnIjlFree_t)(void*);
typedef int   (__cdecl* pfnIjlRead_t)(void*, int);
typedef int   (__cdecl* pfnIjlWrite_t)(void*, int);

static pfnIjlInit_t  real_ijlInit  = NULL;
static pfnIjlFree_t  real_ijlFree  = NULL;
static pfnIjlRead_t  real_ijlRead  = NULL;
static pfnIjlWrite_t real_ijlWrite = NULL;

static void LoadRealIjl()
{
    char realName[16] = {};
    xor_decode(realName, ENC_REAL_IJL, 8);

    // FIX: search order:
    // 1. _real_ijl15.dll next to war3.exe (rename original here)
    // 2. System32
    // 3. game directory itself (in case Windows already loaded it)

    // Try _real_ijl15.dll first (user renames original)
    char renamed[32] = "_real_";
    lstrcat(renamed, realName);
    g_hRealIjl = LoadLibraryA(renamed);

    // Try System32
    if (!g_hRealIjl) {
        char sysPath[MAX_PATH];
        GetSystemDirectoryA(sysPath, MAX_PATH);
        char fullPath[MAX_PATH];
        wsprintfA(fullPath, "%s\\%s", sysPath, realName);
        g_hRealIjl = LoadLibraryA(fullPath);
    }

    // Try SysWOW64 (32-bit on 64-bit Windows)
    if (!g_hRealIjl) {
        char wowPath[MAX_PATH];
        GetWindowsDirectoryA(wowPath, MAX_PATH);
        char fullPath[MAX_PATH];
        wsprintfA(fullPath, "%s\\SysWOW64\\%s", wowPath, realName);
        g_hRealIjl = LoadLibraryA(fullPath);
    }

    if (g_hRealIjl) {
        real_ijlInit  = (pfnIjlInit_t) GetProcAddress(g_hRealIjl, "ijlInit");
        real_ijlFree  = (pfnIjlFree_t) GetProcAddress(g_hRealIjl, "ijlFree");
        real_ijlRead  = (pfnIjlRead_t) GetProcAddress(g_hRealIjl, "ijlRead");
        real_ijlWrite = (pfnIjlWrite_t)GetProcAddress(g_hRealIjl, "ijlWrite");
    }
}

// FIX: stub context so ijlInit never returns NULL
// Game.dll only checks the pointer, doesn't call methods at startup
static BYTE g_IjlStub[256] = {};

extern "C" {

__declspec(dllexport) void* __cdecl ijlInit(void* ctx)
{
    if (real_ijlInit) return real_ijlInit(ctx);
    // Return stub instead of NULL — prevents crash at 0x78B43A
    return (void*)g_IjlStub;
}

__declspec(dllexport) int __cdecl ijlFree(void* ctx)
{ return real_ijlFree ? real_ijlFree(ctx) : 1; }

__declspec(dllexport) int __cdecl ijlRead(void* ctx, int op)
{ return real_ijlRead ? real_ijlRead(ctx, op) : 1; }

__declspec(dllexport) int __cdecl ijlWrite(void* ctx, int op)
{ return real_ijlWrite ? real_ijlWrite(ctx, op) : 1; }

}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hInst);
        g_hSelf = hInst;
        LoadRealIjl();
        CreateThread(NULL, 0, MainThread, (LPVOID)hInst, 0, NULL);
        break;
    case DLL_PROCESS_DETACH:
        g_Running = FALSE;
        RevertAll();
        if (g_hRealIjl) FreeLibrary(g_hRealIjl);
        break;
    }
    return TRUE;
}