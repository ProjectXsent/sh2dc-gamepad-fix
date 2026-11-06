// dinput8 proxy -> exposes an SDL3 gamepad as a virtual DirectInput8 joystick.
// Build as 32-bit (sh2pc.exe is x86). Link: dxguid.lib, SDL3.lib
//
// Settings live in sh2input.ini next to this DLL (auto-created on first run,
// hot-reloaded while the game is running).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>

#ifndef DIDFT_OPTIONAL
#define DIDFT_OPTIONAL 0x80000000
#endif

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

// Our virtual device GUID
static const GUID GUID_SDLPad =
{ 0x5d1c3a10, 0x7e21, 0x4b6a, { 0x9a, 0x0c, 0x53, 0x44, 0x4c, 0x33, 0x50, 0x41 } };

static HMODULE          g_hModule = nullptr;
static CRITICAL_SECTION g_cs;

// =================================================================== config
static const int kNumButtons = 32;     // DI buttons exposed by the virtual device

struct BtnSrc { const char* key; SDL_GamepadButton sdl; int def; };
static const BtnSrc kBtnSrc[] = {
    { "A",             SDL_GAMEPAD_BUTTON_SOUTH,          1  },
    { "B",             SDL_GAMEPAD_BUTTON_EAST,           2  },
    { "X",             SDL_GAMEPAD_BUTTON_WEST,           0  },
    { "Y",             SDL_GAMEPAD_BUTTON_NORTH,          3  },
    { "LeftShoulder",  SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  4  },
    { "RightShoulder", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 5  },
    { "Back",          SDL_GAMEPAD_BUTTON_BACK,           6  },
    { "Start",         SDL_GAMEPAD_BUTTON_START,          7  },
    { "LeftStick",     SDL_GAMEPAD_BUTTON_LEFT_STICK,     8  },
    { "RightStick",    SDL_GAMEPAD_BUTTON_RIGHT_STICK,    9  },
    { "DPadUp",        SDL_GAMEPAD_BUTTON_DPAD_UP,        -1 },
    { "DPadDown",      SDL_GAMEPAD_BUTTON_DPAD_DOWN,      -1 },
    { "DPadLeft",      SDL_GAMEPAD_BUTTON_DPAD_LEFT,      -1 },
    { "DPadRight",     SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     -1 },
    { "Guide",         SDL_GAMEPAD_BUTTON_GUIDE,          12 },
    { "Misc1",         SDL_GAMEPAD_BUTTON_MISC1,          13 },
    { "Touchpad",      SDL_GAMEPAD_BUTTON_TOUCHPAD,       13 },
    { "LeftPaddle1",   SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,   14 },
    { "RightPaddle1",  SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,  15 },
    { "LeftPaddle2",   SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,   16 },
    { "RightPaddle2",  SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,  17 },
};
static const int kNumBtnSrc = (int)(sizeof(kBtnSrc) / sizeof(kBtnSrc[0]));

struct Config
{
    // General
    bool  log           = true;
    bool  hideReal      = true;
    bool  alwaysPresent = true;
    int   padIndex      = 0;

    // Sticks / triggers
    float lsDead = 0.10f, rsDead = 0.10f, trigDead = 0.05f, trigThresh = 0.5f, outer = 1.0f;
    bool  radial = true, honorGameDZ = false;
    bool  swapSticks = false, invLX = false, invLY = false, invRX = false, invRY = false;

    // Axis assignment (0..5 = X,Y,Z,Rx,Ry,Rz ; -1 = none)
    int   axLX = 0, axLY = 1, axRX = 3, axRY = 4;
    bool  combinedTriggers = true;
    int   axTrig = 2, axLT = 2, axRT = 5;

    // Buttons: DI button index (0..31) for each SDL source, -1 = disabled
    int   btn[32];
    int   btnLT = 10, btnRT = 11;
    bool  dpadPov = true;

    // Rumble
    bool  rumble = true;
    float rumbleStrength = 1.0f, rumbleLow = 1.0f, rumbleHigh = 1.0f;

    Config() { for (int i = 0; i < 32; ++i) btn[i] = -1; }
};

static Config g_cfg;
static char   g_iniPath[MAX_PATH] = "";
static char   g_logPath[MAX_PATH] = "sh2input.log";
static char   g_dllDir[MAX_PATH]  = "";
static bool   g_cfgLoaded = false;
static bool   g_reopenPad = false;
static FILETIME g_iniTime = {};
static DWORD  g_lastIniCheck = 0;

// ------------------------------------------------------------------ logging
static void Log(const char* fmt, ...)
{
    if (!g_cfg.log) return;
    static FILE* f = nullptr;
    if (!f) f = fopen(g_logPath, "w");
    if (!f) return;
    va_list va; va_start(va, fmt);
    vfprintf(f, fmt, va);
    va_end(va);
    fputc('\n', f);
    fflush(f);
}

static void LogGuid(const char* tag, REFGUID g)
{
    Log("%s {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}", tag, g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
}

// ------------------------------------------------------------------ ini helpers
static void IniStr(const char* sec, const char* key, const char* def, char* out, int n)
{
    GetPrivateProfileStringA(sec, key, def, out, n, g_iniPath);
}
static int IniInt(const char* sec, const char* key, int def)
{
    char b[64], d[32]; sprintf(d, "%d", def);
    IniStr(sec, key, d, b, sizeof(b));
    return atoi(b);
}
static float IniFloat(const char* sec, const char* key, float def)
{
    char b[64], d[32]; sprintf(d, "%f", def);
    IniStr(sec, key, d, b, sizeof(b));
    return (float)atof(b);
}
static bool IniBool(const char* sec, const char* key, bool def)
{
    return IniInt(sec, key, def ? 1 : 0) != 0;
}
static int IniAxis(const char* sec, const char* key, const char* def)
{
    char b[32]; IniStr(sec, key, def, b, sizeof(b));
    static const char* names[] = { "X", "Y", "Z", "Rx", "Ry", "Rz" };
    for (int i = 0; i < 6; ++i) if (!_stricmp(b, names[i])) return i;
    return -1;   // "None" or anything unrecognised
}
static float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static void WriteDefaultIni()
{
    FILE* f = fopen(g_iniPath, "w");
    if (!f) return;
    fputs(
R"(; sh2input.ini  -- changes are picked up automatically while the game runs.
; Lines starting with ; are comments. Do not put comments after a value.

[General]
; Write sh2input.log (useful for troubleshooting)
Logging=0
; Hide the physical DirectInput controllers so the game only sees the SDL pad
HideRealPads=1
; Report the virtual pad even when no controller is plugged in (allows hot-plugging)
AlwaysPresent=1
; Which SDL gamepad to use if several are connected (0 = first)
GamepadIndex=0
; Optional SDL controller mapping database (gamecontrollerdb.txt). Relative paths are
; relative to this DLL. Read once at startup, so restart the game after changing it.
MappingsFile=gamecontrollerdb.txt

[Buttons]
; Value = DirectInput button number the game sees (0-31), or -1 to disable.
; Several inputs may be mapped to the same number.
A=1
B=2
X=0
Y=3
LeftShoulder=4
RightShoulder=5
LeftTriggerButton=6
RightTriggerButton=7
Back=8
Start=9
LeftStick=10
RightStick=11
DPadUp=-1
DPadDown=-1
DPadLeft=-1
DPadRight=-1
Guide=12
Misc1=13
Touchpad=13
RightPaddle1=14
LeftPaddle1=15
RightPaddle2=16
LeftPaddle2=17

[DPad]
; 1 = also report the D-pad as a POV hat (buttons above still apply)
UsePOV=1

[Sticks]
; Deadzones are 0.0 - 1.0 of full stick travel
LeftDeadzone=0.10
RightDeadzone=0.10
TriggerDeadzone=0.05
; How far a trigger must be pulled (0-1) to count as a button press
TriggerThreshold=0.50
; Stick travel (0-1) at which output reaches maximum. Lower = reach max sooner.
OuterLimit=1.00
; 1 = circular deadzone, 0 = independent per-axis deadzone
RadialDeadzone=1
; Also apply the deadzone value the game requests via DirectInput (usually leave 0)
HonorGameDeadzone=0
; Swap left and right sticks
SwapSticks=0
InvertLeftX=0
InvertLeftY=0
InvertRightX=0
InvertRightY=0

[Axes]
; Which DirectInput axis each input is sent to: X, Y, Z, Rx, Ry, Rz or None
LeftStickX=X
LeftStickY=Y
RightStickX=Z
RightStickY=Rz
; 1 = one combined trigger axis (LT positive, RT negative, like Xbox DirectInput)
; 0 = separate axes (rest position = minimum)
CombinedTriggers=0
TriggerAxis=None
LeftTriggerAxis=None
RightTriggerAxis=None

[Rumble]
Enabled=1
; Overall multiplier (0.0 - 1.0+)
Strength=1.00
; Multipliers for the heavy (low frequency) and light (high frequency) motors. 0 disables a motor.
LowFrequencyScale=1.00
HighFrequencyScale=1.00
)", f);
    fclose(f);
}

static void LoadConfig()
{
    Config c;
    c.log           = IniBool("General", "Logging", true);
    c.hideReal      = IniBool("General", "HideRealPads", true);
    c.alwaysPresent = IniBool("General", "AlwaysPresent", true);
    c.padIndex      = std::max(0, IniInt("General", "GamepadIndex", 0));

    for (int i = 0; i < kNumBtnSrc; ++i) {
        int v = IniInt("Buttons", kBtnSrc[i].key, kBtnSrc[i].def);
        c.btn[i] = (v >= 0 && v < kNumButtons) ? v : -1;
    }
    int lt = IniInt("Buttons", "LeftTriggerButton", 10);
    int rt = IniInt("Buttons", "RightTriggerButton", 11);
    c.btnLT = (lt >= 0 && lt < kNumButtons) ? lt : -1;
    c.btnRT = (rt >= 0 && rt < kNumButtons) ? rt : -1;
    c.dpadPov = IniBool("DPad", "UsePOV", true);

    c.lsDead      = Clamp01(IniFloat("Sticks", "LeftDeadzone", 0.10f));
    c.rsDead      = Clamp01(IniFloat("Sticks", "RightDeadzone", 0.10f));
    c.trigDead    = Clamp01(IniFloat("Sticks", "TriggerDeadzone", 0.05f));
    c.trigThresh  = Clamp01(IniFloat("Sticks", "TriggerThreshold", 0.5f));
    c.outer       = Clamp01(IniFloat("Sticks", "OuterLimit", 1.0f));
    c.radial      = IniBool("Sticks", "RadialDeadzone", true);
    c.honorGameDZ = IniBool("Sticks", "HonorGameDeadzone", false);
    c.swapSticks  = IniBool("Sticks", "SwapSticks", false);
    c.invLX = IniBool("Sticks", "InvertLeftX", false);
    c.invLY = IniBool("Sticks", "InvertLeftY", false);
    c.invRX = IniBool("Sticks", "InvertRightX", false);
    c.invRY = IniBool("Sticks", "InvertRightY", false);

    c.axLX = IniAxis("Axes", "LeftStickX",  "X");
    c.axLY = IniAxis("Axes", "LeftStickY",  "Y");
    c.axRX = IniAxis("Axes", "RightStickX", "Rx");
    c.axRY = IniAxis("Axes", "RightStickY", "Ry");
    c.combinedTriggers = IniBool("Axes", "CombinedTriggers", true);
    c.axTrig = IniAxis("Axes", "TriggerAxis", "Z");
    c.axLT   = IniAxis("Axes", "LeftTriggerAxis", "Z");
    c.axRT   = IniAxis("Axes", "RightTriggerAxis", "Rz");

    c.rumble         = IniBool("Rumble", "Enabled", true);
    c.rumbleStrength = std::max(0.0f, IniFloat("Rumble", "Strength", 1.0f));
    c.rumbleLow      = std::max(0.0f, IniFloat("Rumble", "LowFrequencyScale", 1.0f));
    c.rumbleHigh     = std::max(0.0f, IniFloat("Rumble", "HighFrequencyScale", 1.0f));

    g_cfg = c;
    g_reopenPad = true;
    Log("Config loaded from %s", g_iniPath);
}

static void EnsureConfig()
{
    if (g_cfgLoaded) return;
    g_cfgLoaded = true;

    char dir[MAX_PATH];
    GetModuleFileNameA(g_hModule, dir, MAX_PATH);
    char* slash = strrchr(dir, '\\');
    if (slash) slash[1] = 0; else dir[0] = 0;

    strcpy(g_dllDir, dir);
    strcpy(g_iniPath, dir); strcat(g_iniPath, "sh2input.ini");
    strcpy(g_logPath, dir); strcat(g_logPath, "sh2input.log");

    if (GetFileAttributesA(g_iniPath) == INVALID_FILE_ATTRIBUTES) WriteDefaultIni();
    LoadConfig();

    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fa)) g_iniTime = fa.ftLastWriteTime;
}

static void CheckConfigReload()
{
    DWORD now = GetTickCount();
    if (now - g_lastIniCheck < 1000) return;
    g_lastIniCheck = now;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(g_iniPath, GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &g_iniTime) != 0) {
        g_iniTime = fa.ftLastWriteTime;
        LoadConfig();
    }
}

// =================================================================== SDL side
struct Snapshot
{
    float axis[6] = {};          // X,Y,Z,Rx,Ry,Rz in [-1,1]
    DWORD pov     = 0xFFFFFFFF;
    bool  btn[32] = {};
};

static SDL_Gamepad* g_pad = nullptr;
static bool         g_sdlReady = false;

static void UpdateRumble();      // defined after VirtualEffect

static void EnsureSDL()
{
    if (g_sdlReady) return;
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        Log("SDL_Init failed: %s", SDL_GetError());
    } else {
        char name[MAX_PATH];
        IniStr("General", "MappingsFile", "gamecontrollerdb.txt", name, sizeof(name));
        if (name[0]) {
            char path[MAX_PATH * 2];
            bool absolute = (name[0] == '\\' || name[0] == '/' || name[1] == ':');
            if (absolute) snprintf(path, sizeof(path), "%s", name);
            else          snprintf(path, sizeof(path), "%s%s", g_dllDir, name);
            int n = SDL_AddGamepadMappingsFromFile(path);
            if (n < 0) Log("Mappings file not loaded (%s): %s", path, SDL_GetError());
            else       Log("Loaded %d gamepad mappings from %s", n, path);
        }
    }
    g_sdlReady = true;
}

static void UpdatePad()
{
    EnsureConfig();
    EnsureSDL();
    CheckConfigReload();
    SDL_UpdateGamepads();

    if (g_pad && (!SDL_GamepadConnected(g_pad) || g_reopenPad)) {
        SDL_RumbleGamepad(g_pad, 0, 0, 0);
        SDL_CloseGamepad(g_pad);
        g_pad = nullptr;
    }
    g_reopenPad = false;

    if (!g_pad) {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids && n > 0) {
            int idx = g_cfg.padIndex < n ? g_cfg.padIndex : 0;
            g_pad = SDL_OpenGamepad(ids[idx]);
            if (g_pad) Log("Opened gamepad: %s", SDL_GetGamepadName(g_pad));
        }
        SDL_free(ids);
    }
    UpdateRumble();
}

static float Norm(Sint16 v) { return v < 0 ? v / 32768.0f : v / 32767.0f; }

static void ApplyStick(float& x, float& y, float dz)
{
    const Config& c = g_cfg;
    float outer = std::max(c.outer, dz + 0.01f);
    if (c.radial) {
        float m = sqrtf(x * x + y * y);
        if (m <= dz) { x = y = 0; return; }
        float k = fminf((m - dz) / (outer - dz), 1.0f) / m;
        x *= k; y *= k;
    } else {
        auto one = [&](float v) {
            float m = fabsf(v);
            if (m <= dz) return 0.0f;
            return copysignf(fminf((m - dz) / (outer - dz), 1.0f), v);
        };
        x = one(x); y = one(y);
    }
}

static void ReadPad(Snapshot& s)
{
    s = Snapshot();
    if (!g_pad) return;
    const Config& c = g_cfg;

    float lx = Norm(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTX));
    float ly = Norm(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFTY));   // up = negative (same as DI)
    float rx = Norm(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTX));
    float ry = Norm(SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHTY));
    ApplyStick(lx, ly, c.lsDead);
    ApplyStick(rx, ry, c.rsDead);
    if (c.swapSticks) { std::swap(lx, rx); std::swap(ly, ry); }
    if (c.invLX) lx = -lx;
    if (c.invLY) ly = -ly;
    if (c.invRX) rx = -rx;
    if (c.invRY) ry = -ry;

    float ltRaw = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  / 32767.0f;
    float rtRaw = SDL_GetGamepadAxis(g_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / 32767.0f;
    auto trig = [&](float v) {
        if (v <= c.trigDead) return 0.0f;
        return fminf((v - c.trigDead) / (1.0f - c.trigDead), 1.0f);
    };
    float lt = trig(ltRaw), rt = trig(rtRaw);

    auto put = [&](int a, float v) {
        if (a < 0 || a > 5) return;
        s.axis[a] = fmaxf(-1.0f, fminf(1.0f, s.axis[a] + v));
    };
    put(c.axLX, lx); put(c.axLY, ly);
    put(c.axRX, rx); put(c.axRY, ry);
    if (c.combinedTriggers) put(c.axTrig, lt - rt);
    else { put(c.axLT, lt * 2.0f - 1.0f); put(c.axRT, rt * 2.0f - 1.0f); }

    for (int i = 0; i < kNumBtnSrc; ++i)
        if (c.btn[i] >= 0 && SDL_GetGamepadButton(g_pad, kBtnSrc[i].sdl)) s.btn[c.btn[i]] = true;
    if (c.btnLT >= 0 && ltRaw > c.trigThresh) s.btn[c.btnLT] = true;
    if (c.btnRT >= 0 && rtRaw > c.trigThresh) s.btn[c.btnRT] = true;

    if (c.dpadPov) {
        bool u = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        bool d = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        bool l = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        bool r = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        if      (u && r) s.pov = 4500;
        else if (d && r) s.pov = 13500;
        else if (d && l) s.pov = 22500;
        else if (u && l) s.pov = 31500;
        else if (u)      s.pov = 0;
        else if (r)      s.pov = 9000;
        else if (d)      s.pov = 18000;
        else if (l)      s.pov = 27000;
    }
}

// =================================================================== force feedback -> rumble
struct EffDesc { const GUID* guid; DWORD type; const char* name; };
static const EffDesc kEffects[] = {
    { &GUID_ConstantForce, DIEFT_CONSTANTFORCE, "Constant Force" },
    { &GUID_RampForce,     DIEFT_RAMPFORCE,     "Ramp Force"     },
    { &GUID_Square,        DIEFT_PERIODIC,      "Square"         },
    { &GUID_Sine,          DIEFT_PERIODIC,      "Sine"           },
    { &GUID_Triangle,      DIEFT_PERIODIC,      "Triangle"       },
    { &GUID_SawtoothUp,    DIEFT_PERIODIC,      "Sawtooth Up"    },
    { &GUID_SawtoothDown,  DIEFT_PERIODIC,      "Sawtooth Down"  },
};

static const EffDesc* FindEffect(REFGUID g)
{
    for (auto& e : kEffects) if (IsEqualGUID(*e.guid, g)) return &e;
    return nullptr;
}

static void FillEffectInfo(DIEFFECTINFOA& i, const EffDesc& e)
{
    memset(&i, 0, sizeof(i));
    i.dwSize = sizeof(i);
    i.guid = *e.guid;
    i.dwEffType = e.type | DIEFT_FFATTACK | DIEFT_FFFADE;
    strncpy(i.tszName, e.name, MAX_PATH - 1);
}

class VirtualEffect;
static std::vector<VirtualEffect*> g_active;

class VirtualEffect : public IDirectInputEffect
{
public:
    LONG   m_ref = 1;
    GUID   m_guid;
    DWORD  m_duration = INFINITE;     // microseconds
    DWORD  m_gain = 10000;
    std::vector<BYTE> m_params;
    bool   m_playing = false;
    DWORD  m_iter = 1;
    ULONGLONG m_start = 0;

    VirtualEffect(REFGUID g, const DIEFFECT* e) : m_guid(g)
    {
        if (e) Apply(e, DIEP_DURATION | DIEP_GAIN | DIEP_TYPESPECIFICPARAMS);
        Log("Effect created: duration=%lu gain=%lu params=%zu", m_duration, m_gain, m_params.size());
    }
    ~VirtualEffect() { Deactivate(); }

    void Apply(const DIEFFECT* e, DWORD flags)
    {
        EnterCriticalSection(&g_cs);
        if (flags & DIEP_DURATION) m_duration = e->dwDuration;
        if (flags & DIEP_GAIN)     m_gain = e->dwGain;
        if ((flags & DIEP_TYPESPECIFICPARAMS) && e->lpvTypeSpecificParams && e->cbTypeSpecificParams)
            m_params.assign((BYTE*)e->lpvTypeSpecificParams,
                            (BYTE*)e->lpvTypeSpecificParams + e->cbTypeSpecificParams);
        LeaveCriticalSection(&g_cs);
    }

    void Deactivate()
    {
        EnterCriticalSection(&g_cs);
        m_playing = false;
        g_active.erase(std::remove(g_active.begin(), g_active.end(), this), g_active.end());
        LeaveCriticalSection(&g_cs);
    }

    bool Expired(ULONGLONG now) const
    {
        if (!m_playing) return true;
        if (m_duration == INFINITE || m_iter == INFINITE) return false;
        ULONGLONG total = (ULONGLONG)(m_duration / 1000) * m_iter;
        return now - m_start >= total;
    }

    float Level() const        // 0..1
    {
        float mag = 0.5f;
        if (IsEqualGUID(m_guid, GUID_ConstantForce) && m_params.size() >= sizeof(LONG)) {
            mag = fabsf((float)*(const LONG*)m_params.data()) / 10000.0f;
        } else if (IsEqualGUID(m_guid, GUID_RampForce) && m_params.size() >= 2 * sizeof(LONG)) {
            const LONG* p = (const LONG*)m_params.data();
            mag = fmaxf(fabsf((float)p[0]), fabsf((float)p[1])) / 10000.0f;
        } else if (m_params.size() >= sizeof(DWORD) && FindEffect(m_guid) && FindEffect(m_guid)->type == DIEFT_PERIODIC) {
            mag = (float)*(const DWORD*)m_params.data() / 10000.0f;
        }
        mag *= m_gain / 10000.0f;
        return fminf(mag, 1.0f);
    }

    // ---- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInputEffect)) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    // ---- IDirectInputEffect
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID g) override { if (!g) return E_POINTER; *g = m_guid; return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT e, DWORD flags) override
    {
        if (!e) return E_POINTER;
        if (flags & DIEP_DURATION) e->dwDuration = m_duration;
        if (flags & DIEP_GAIN)     e->dwGain = m_gain;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT e, DWORD flags) override
    {
        if (!e) return E_POINTER;
        Apply(e, flags);
        Log("Effect SetParameters flags=0x%lx duration=%lu gain=%lu", flags, m_duration, m_gain);
        if (flags & DIEP_START) Start(1, 0);
        else UpdateRumble();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Start(DWORD iterations, DWORD) override
    {
        EnterCriticalSection(&g_cs);
        m_iter = iterations ? iterations : 1;
        m_start = GetTickCount64();
        m_playing = true;
        if (std::find(g_active.begin(), g_active.end(), this) == g_active.end()) g_active.push_back(this);
        LeaveCriticalSection(&g_cs);
        Log("Effect Start iter=%lu level=%.2f", iterations, Level());
        UpdateRumble();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override { Deactivate(); UpdateRumble(); return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD s) override
    {
        if (!s) return E_POINTER;
        *s = (m_playing && !Expired(GetTickCount64())) ? DIEGES_PLAYING : 0;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Download() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Unload() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
};

static bool g_rumbling = false;

static void StopAllEffects()
{
    EnterCriticalSection(&g_cs);
    for (auto* e : g_active) e->m_playing = false;
    g_active.clear();
    LeaveCriticalSection(&g_cs);
    UpdateRumble();
}

static void UpdateRumble()
{
    float lvl = 0;
    EnterCriticalSection(&g_cs);
    ULONGLONG now = GetTickCount64();
    for (auto it = g_active.begin(); it != g_active.end();) {
        VirtualEffect* e = *it;
        if (e->Expired(now)) { e->m_playing = false; it = g_active.erase(it); }
        else { lvl = fmaxf(lvl, e->Level()); ++it; }
    }
    LeaveCriticalSection(&g_cs);

    if (!g_pad || !g_cfg.rumble) { g_rumbling = false; return; }

    if (lvl > 0) {
        auto motor = [&](float scale) {
            float v = lvl * g_cfg.rumbleStrength * scale;
            return (Uint16)(fminf(v, 1.0f) * 65535.0f);
        };
        SDL_RumbleGamepad(g_pad, motor(g_cfg.rumbleLow), motor(g_cfg.rumbleHigh), 250);
        g_rumbling = true;
    } else if (g_rumbling) {
        SDL_RumbleGamepad(g_pad, 0, 0, 0);
        g_rumbling = false;
    }
}

// =================================================================== virtual device
enum class Kind { None, Axis, Slider, Pov, Button };
struct Slot { DWORD ofs; Kind kind; int idx; };

struct ObjDesc { const GUID* guid; DWORD type; const char* name; DWORD ofs; };

static const ObjDesc kObjects[] = {
    { &GUID_XAxis,  DIDFT_AXIS | DIDFT_MAKEINSTANCE(0), "X Axis",     0  },
    { &GUID_YAxis,  DIDFT_AXIS | DIDFT_MAKEINSTANCE(1), "Y Axis",     4  },
    { &GUID_ZAxis,  DIDFT_AXIS | DIDFT_MAKEINSTANCE(2), "Z Axis",     8  },
    { &GUID_RxAxis, DIDFT_AXIS | DIDFT_MAKEINSTANCE(3), "X Rotation", 12 },
    { &GUID_RyAxis, DIDFT_AXIS | DIDFT_MAKEINSTANCE(4), "Y Rotation", 16 },
    { &GUID_RzAxis, DIDFT_AXIS | DIDFT_MAKEINSTANCE(5), "Z Rotation", 20 },
    { &GUID_POV,    DIDFT_POV  | DIDFT_MAKEINSTANCE(0), "Hat",        32 },
};

class VirtualPad : public IDirectInputDevice8A
{
    LONG  m_ref = 1;
    std::vector<Slot> m_slots;
    DWORD m_dataSize = 0;
    bool  m_haveFmt = false;
    LONG  m_min[6], m_max[6], m_dz[6], m_sat[6];

public:
    VirtualPad()
    {
        for (int i = 0; i < 6; ++i) { m_min[i] = 0; m_max[i] = 65535; m_dz[i] = 0; m_sat[i] = 10000; }
    }

    // ---- IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInputDevice8A) ||
            IsEqualIID(riid, IID_IDirectInputDeviceA)) { *pp = this; AddRef(); return S_OK; }
        *pp = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) delete this;
        return r;
    }

    // ---- helpers
    int AxisFromHeader(const DIPROPHEADER* h) const   // -1 = all, -2 = not found
    {
        if (h->dwHow == DIPH_DEVICE) return -1;
        if (h->dwHow == DIPH_BYOFFSET) {
            for (auto& s : m_slots) if (s.ofs == h->dwObj && s.kind == Kind::Axis) return s.idx;
            for (int i = 0; i < 6; ++i) if (kObjects[i].ofs == h->dwObj) return i;
            return -2;
        }
        if (h->dwHow == DIPH_BYID) {
            if (DIDFT_GETTYPE(h->dwObj) & DIDFT_AXIS) {
                int i = DIDFT_GETINSTANCE(h->dwObj);
                return i < 6 ? i : -2;
            }
        }
        return -2;
    }

    static void FillObjInstance(DIDEVICEOBJECTINSTANCEA& o, const GUID* g, DWORD type, const char* name, DWORD ofs)
    {
        memset(&o, 0, sizeof(o));
        o.dwSize = sizeof(o);
        if (g) o.guidType = *g;
        o.dwOfs = ofs;
        o.dwType = type;
        o.dwFlags = (type & DIDFT_AXIS) ? DIDOI_ASPECTPOSITION : 0;
        if (g_cfg.rumble && (type & DIDFT_AXIS) && DIDFT_GETINSTANCE(type) < 2) {
            o.dwFlags |= DIDOI_FFACTUATOR;
            o.dwFFMaxForce = 10000;
            o.dwFFForceResolution = 1;
        }
        strncpy(o.tszName, name, MAX_PATH - 1);
    }

    // ---- IDirectInputDevice8A
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS c) override
    {
        if (!c) return E_POINTER;
        DWORD sz = c->dwSize;
        memset(c, 0, sz); c->dwSize = sz;
        c->dwFlags   = DIDC_ATTACHED | DIDC_EMULATED | (g_cfg.rumble ? DIDC_FORCEFEEDBACK : 0);
        c->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
        c->dwAxes = 6; c->dwButtons = kNumButtons; c->dwPOVs = 1;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(LPDIENUMDEVICEOBJECTSCALLBACKA cb, LPVOID ref, DWORD flags) override
    {
        DIDEVICEOBJECTINSTANCEA o;
        for (auto& d : kObjects) {
            bool isAxis = d.type & DIDFT_AXIS;
            bool want = (flags == DIDFT_ALL) || (isAxis && (flags & DIDFT_AXIS)) || (!isAxis && (flags & DIDFT_POV));
            if (!want) continue;
            FillObjInstance(o, d.guid, d.type, d.name, d.ofs);
            if (cb(&o, ref) == DIENUM_STOP) return DI_OK;
        }
        if ((flags == DIDFT_ALL) || (flags & DIDFT_BUTTON)) {
            for (int i = 0; i < kNumButtons; ++i) {
                char n[32]; sprintf(n, "Button %d", i);
                FillObjInstance(o, &GUID_Button, DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i), n, 48 + i);
                if (cb(&o, ref) == DIENUM_STOP) return DI_OK;
            }
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID p, LPDIPROPHEADER h) override
    {
        if (&p == &DIPROP_RANGE) {
            int a = AxisFromHeader(h); if (a == -1) a = 0;
            if (a < 0) return DIERR_OBJECTNOTFOUND;
            auto* r = (LPDIPROPRANGE)h; r->lMin = m_min[a]; r->lMax = m_max[a]; return DI_OK;
        }
        if (&p == &DIPROP_DEADZONE || &p == &DIPROP_SATURATION) {
            int a = AxisFromHeader(h); if (a == -1) a = 0;
            if (a < 0) return DIERR_OBJECTNOTFOUND;
            ((LPDIPROPDWORD)h)->dwData = (&p == &DIPROP_DEADZONE) ? m_dz[a] : m_sat[a]; return DI_OK;
        }
        if (&p == &DIPROP_AXISMODE)   { ((LPDIPROPDWORD)h)->dwData = DIPROPAXISMODE_ABS; return DI_OK; }
        if (&p == &DIPROP_BUFFERSIZE) { ((LPDIPROPDWORD)h)->dwData = 0; return DI_OK; }
        if (&p == &DIPROP_JOYSTICKID) { ((LPDIPROPDWORD)h)->dwData = 0; return DI_OK; }
        if (&p == &DIPROP_FFGAIN)     { ((LPDIPROPDWORD)h)->dwData = 10000; return DI_OK; }
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID p, LPCDIPROPHEADER h) override
    {
        if (&p == &DIPROP_RANGE) {
            auto* r = (const DIPROPRANGE*)h;
            Log("SetProperty RANGE how=%lu obj=%lu [%ld..%ld]", h->dwHow, h->dwObj, r->lMin, r->lMax);
            int a = AxisFromHeader(h);
            if (a == -1) { for (int i = 0; i < 6; ++i) { m_min[i] = r->lMin; m_max[i] = r->lMax; } }
            else if (a >= 0) { m_min[a] = r->lMin; m_max[a] = r->lMax; }
            return DI_OK;
        }
        if (&p == &DIPROP_DEADZONE || &p == &DIPROP_SATURATION) {
            LONG v = ((const DIPROPDWORD*)h)->dwData;
            LONG* arr = (&p == &DIPROP_DEADZONE) ? m_dz : m_sat;
            Log("SetProperty %s how=%lu obj=%lu = %ld", (&p == &DIPROP_DEADZONE) ? "DEADZONE" : "SATURATION", h->dwHow, h->dwObj, v);
            int a = AxisFromHeader(h);
            if (a == -1) { for (int i = 0; i < 6; ++i) arr[i] = v; }
            else if (a >= 0) arr[a] = v;
            return DI_OK;
        }
        Log("SetProperty (ignored) ptr=%p", (void*)&p);
        return DI_OK;          // BUFFERSIZE, AXISMODE, AUTOCENTER, FFGAIN, etc.: accept silently
    }

    HRESULT STDMETHODCALLTYPE Acquire() override   { UpdatePad(); return DI_OK; }
    HRESULT STDMETHODCALLTYPE Unacquire() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Poll() override      { UpdatePad(); return DI_OK; }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override
    {
        if (!data) return E_POINTER;
        if (!m_haveFmt) return DIERR_INVALIDPARAM;
        UpdatePad();
        Snapshot s; ReadPad(s);
        memset(data, 0, cb);
        BYTE* base = (BYTE*)data;

        for (auto& sl : m_slots) {
            if (sl.kind == Kind::None) continue;
            DWORD need = (sl.kind == Kind::Button) ? 1 : 4;
            if (sl.ofs + need > cb) continue;

            switch (sl.kind) {
            case Kind::Axis: {
                int a = sl.idx;
                float f = s.axis[a];
                if (g_cfg.honorGameDZ) {
                    float dz = m_dz[a] / 10000.0f;
                    float sat = m_sat[a] / 10000.0f;
                    float m = fabsf(f);
                    if (m < dz) f = 0;
                    else { m = fminf((m - dz) / fmaxf(sat - dz, 0.01f), 1.0f); f = copysignf(m, f); }
                }
                LONG v = m_min[a] + (LONG)((f + 1.0f) * 0.5f * (float)(m_max[a] - m_min[a]));
                *(LONG*)(base + sl.ofs) = v;
                break; }
            case Kind::Slider:
                *(LONG*)(base + sl.ofs) = m_min[0] + (m_max[0] - m_min[0]) / 2;
                break;
            case Kind::Pov:
                *(DWORD*)(base + sl.ofs) = (sl.idx == 0) ? s.pov : 0xFFFFFFFF;
                break;
            case Kind::Button:
                base[sl.ofs] = (sl.idx < kNumButtons && s.btn[sl.idx]) ? 0x80 : 0;
                break;
            default: break;
            }
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD, LPDIDEVICEOBJECTDATA, LPDWORD n, DWORD) override
    {   // buffered mode not implemented; game is expected to use GetDeviceState
        if (n) *n = 0;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT df) override
    {
        if (!df) return E_POINTER;
        m_slots.clear();
        m_dataSize = df->dwDataSize;
        int axisSeq = 0, sliderSeq = 0, povSeq = 0, btnSeq = 0;
        Log("SetDataFormat: size=%lu flags=0x%lx objs=%lu", df->dwDataSize, df->dwFlags, df->dwNumObjs);

        for (DWORD i = 0; i < df->dwNumObjs; ++i) {
            const DIOBJECTDATAFORMAT& o = df->rgodf[i];
            Slot s{ o.dwOfs, Kind::None, 0 };
            DWORD inst = DIDFT_GETINSTANCE(o.dwType);
            bool any = (inst == 0xFFFF);

            if (o.pguid) {
                if      (IsEqualGUID(*o.pguid, GUID_XAxis))  { s.kind = Kind::Axis; s.idx = 0; }
                else if (IsEqualGUID(*o.pguid, GUID_YAxis))  { s.kind = Kind::Axis; s.idx = 1; }
                else if (IsEqualGUID(*o.pguid, GUID_ZAxis))  { s.kind = Kind::Axis; s.idx = 2; }
                else if (IsEqualGUID(*o.pguid, GUID_RxAxis)) { s.kind = Kind::Axis; s.idx = 3; }
                else if (IsEqualGUID(*o.pguid, GUID_RyAxis)) { s.kind = Kind::Axis; s.idx = 4; }
                else if (IsEqualGUID(*o.pguid, GUID_RzAxis)) { s.kind = Kind::Axis; s.idx = 5; }
                else if (IsEqualGUID(*o.pguid, GUID_Slider)) { s.kind = Kind::Slider; s.idx = sliderSeq++; }
                else if (IsEqualGUID(*o.pguid, GUID_POV))    { s.kind = Kind::Pov;    s.idx = povSeq++; }
                else if (IsEqualGUID(*o.pguid, GUID_Button)) { s.kind = Kind::Button; s.idx = any ? btnSeq++ : (int)inst; }
            }
            if (s.kind == Kind::None) {
                if      (o.dwType & DIDFT_BUTTON) { s.kind = Kind::Button; s.idx = any ? btnSeq++ : (int)inst; }
                else if (o.dwType & DIDFT_POV)    { s.kind = Kind::Pov;    s.idx = povSeq++; }
                else if (o.dwType & DIDFT_AXIS)   { s.kind = Kind::Axis;   s.idx = any ? axisSeq++ : (int)inst; if (s.idx > 5) s.kind = Kind::None; }
            }
            Log("  obj[%lu] ofs=%lu type=0x%lx -> kind=%d idx=%d", i, o.dwOfs, o.dwType, (int)s.kind, s.idx);
            m_slots.push_back(s);
        }
        m_haveFmt = true;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD) override { return DI_OK; }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(LPDIDEVICEOBJECTINSTANCEA o, DWORD obj, DWORD how) override
    {
        for (auto& d : kObjects) {
            if ((how == DIPH_BYOFFSET && d.ofs == obj) || (how == DIPH_BYID && d.type == (obj & ~DIDFT_OPTIONAL))) {
                FillObjInstance(*o, d.guid, d.type, d.name, d.ofs); return DI_OK;
            }
        }
        if (how == DIPH_BYOFFSET && obj >= 48 && obj < 48 + (DWORD)kNumButtons) {
            char n[32]; sprintf(n, "Button %lu", obj - 48);
            FillObjInstance(*o, &GUID_Button, DIDFT_BUTTON | DIDFT_MAKEINSTANCE(obj - 48), n, obj); return DI_OK;
        }
        return DIERR_OBJECTNOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(LPDIDEVICEINSTANCEA i) override
    {
        if (!i) return E_POINTER;
        DWORD sz = i->dwSize; memset(i, 0, sz); i->dwSize = sz;
        i->guidInstance = GUID_SDLPad;
        i->guidProduct  = { 0x028E045E, 0, 0, { 0, 0, 'P', 'I', 'D', 'V', 'I', 'D' } };
        i->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
        strcpy(i->tszInstanceName, "SDL3 Gamepad");
        strcpy(i->tszProductName,  "SDL3 Gamepad");
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }

    // ---- force feedback
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* pp, LPUNKNOWN) override
    {
        LogGuid("CreateEffect", g);
        if (!g_cfg.rumble) return DIERR_UNSUPPORTED;
        if (!pp) return E_POINTER;
        if (!FindEffect(g)) return DIERR_DEVICENOTREG;
        *pp = new VirtualEffect(g, e);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(LPDIENUMEFFECTSCALLBACKA cb, LPVOID ref, DWORD type) override
    {
        if (!g_cfg.rumble) return DI_OK;
        for (auto& e : kEffects) {
            if (type != DIEFT_ALL && !(e.type & type)) continue;
            DIEFFECTINFOA info; FillEffectInfo(info, e);
            if (cb(&info, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(LPDIEFFECTINFOA info, REFGUID g) override
    {
        const EffDesc* e = FindEffect(g);
        if (!e || !info) return DIERR_DEVICENOTREG;
        FillEffectInfo(*info, *e);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD s) override
    {
        if (!s) return E_POINTER;
        *s = DIGFFS_ACTUATORSON | DIGFFS_POWERON | (g_active.empty() ? DIGFFS_EMPTY : 0);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD cmd) override
    {
        Log("SendForceFeedbackCommand 0x%lx", cmd);
        if (cmd & (DISFFC_RESET | DISFFC_STOPALL)) StopAllEffects();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(LPCSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(LPCSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE BuildActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetActionMap(LPDIACTIONFORMATA, LPCSTR, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetImageInfo(LPDIDEVICEIMAGEINFOHEADERA) override { return DIERR_UNSUPPORTED; }
};

// =================================================================== IDirectInput8A wrapper
struct EnumCtx { LPDIENUMDEVICESCALLBACKA cb; LPVOID ref; };

static BOOL CALLBACK FilterCb(LPCDIDEVICEINSTANCEA inst, LPVOID ctx)
{
    auto* c = (EnumCtx*)ctx;
    BYTE type = GET_DIDEVICE_TYPE(inst->dwDevType);
    if (g_cfg.hideReal && type != DI8DEVTYPE_KEYBOARD && type != DI8DEVTYPE_MOUSE)
        return DIENUM_CONTINUE;
    return c->cb(inst, c->ref);
}

class DInputWrapper : public IDirectInput8A
{
    LONG m_ref = 1;
    IDirectInput8A* m_real;
public:
    explicit DInputWrapper(IDirectInput8A* r) : m_real(r) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override
    {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInput8A)) { *pp = this; AddRef(); return S_OK; }
        return m_real->QueryInterface(riid, pp);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG r = InterlockedDecrement(&m_ref);
        if (!r) { m_real->Release(); delete this; }
        return r;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID g, LPDIRECTINPUTDEVICE8A* pp, LPUNKNOWN outer) override
    {
        LogGuid("CreateDevice", g);
        if (IsEqualGUID(g, GUID_SDLPad)) {
            if (!pp) return E_POINTER;
            EnsureSDL();
            *pp = new VirtualPad();
            return DI_OK;
        }
        HRESULT hr = m_real->CreateDevice(g, pp, outer);
        if (SUCCEEDED(hr) && g_cfg.hideReal && pp && *pp) {
            DIDEVICEINSTANCEA di{}; di.dwSize = sizeof(di);
            if (SUCCEEDED((*pp)->GetDeviceInfo(&di))) {
                BYTE t = LOBYTE(di.dwDevType);
                if (t != DI8DEVTYPE_KEYBOARD && t != DI8DEVTYPE_MOUSE) {
                    Log("Blocked real device %s", di.tszProductName);
                    (*pp)->Release(); *pp = nullptr;
                    return DIERR_DEVICENOTREG;
                }
            }
        }
        return hr;
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD devType, LPDIENUMDEVICESCALLBACKA cb, LPVOID ref, DWORD flags) override
    {
        Log("EnumDevices type=0x%lx flags=0x%lx", devType, flags);
        BYTE cls = (BYTE)devType;
        bool gameClass = (devType == DI8DEVCLASS_GAMECTRL) || (devType == DI8DEVCLASS_ALL) ||
                         (cls != DI8DEVTYPE_KEYBOARD && cls != DI8DEVTYPE_MOUSE && devType > DI8DEVCLASS_GAMECTRL);
        bool wantsReal = (devType != DI8DEVCLASS_GAMECTRL);

        if (gameClass) {
            UpdatePad();
            if (g_cfg.alwaysPresent || g_pad) {
                DIDEVICEINSTANCEA inst; memset(&inst, 0, sizeof(inst)); inst.dwSize = sizeof(inst);
                inst.guidInstance = GUID_SDLPad;
                inst.guidProduct  = { 0x028E045E, 0, 0, { 0, 0, 'P', 'I', 'D', 'V', 'I', 'D' } };
                inst.dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
                strcpy(inst.tszInstanceName, "SDL3 Gamepad");
                strcpy(inst.tszProductName,  "SDL3 Gamepad");
                if (cb(&inst, ref) == DIENUM_STOP) return DI_OK;
            }
        }
        if (!wantsReal && g_cfg.hideReal) return DI_OK;
        if (g_cfg.hideReal) { EnumCtx c{ cb, ref }; return m_real->EnumDevices(devType, FilterCb, &c, flags); }
        return m_real->EnumDevices(devType, cb, ref, flags);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override
    { return IsEqualGUID(g, GUID_SDLPad) ? DI_OK : m_real->GetDeviceStatus(g); }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return m_real->RunControlPanel(h, f); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override { return m_real->Initialize(h, v); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID a, LPCSTR b, LPGUID c) override { return m_real->FindDevice(a, b, c); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(LPCSTR a, LPDIACTIONFORMATA b, LPDIENUMDEVICESBYSEMANTICSCBA c, LPVOID d, DWORD e) override
    {
        Log("EnumDevicesBySemantics (blocked)");
        return g_cfg.hideReal ? DI_OK : m_real->EnumDevicesBySemantics(a, b, c, d, e);
    }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK a, LPDICONFIGUREDEVICESPARAMSA b, DWORD c, LPVOID d) override
    { return m_real->ConfigureDevices(a, b, c, d); }
};

// =================================================================== exported entry point
typedef HRESULT(WINAPI* PFN_DirectInput8Create)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static PFN_DirectInput8Create g_realCreate = nullptr;

#pragma comment(linker, "/EXPORT:DirectInput8Create=_ProxyDirectInput8Create@20")
extern "C" HRESULT WINAPI ProxyDirectInput8Create(HINSTANCE hInst, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    EnsureConfig();
    if (!g_realCreate) {
        char path[MAX_PATH];
        GetSystemDirectoryA(path, MAX_PATH);            // SysWOW64 for a 32-bit process
        strcat(path, "\\dinput8.dll");
        HMODULE real = LoadLibraryA(path);
        if (!real) return E_FAIL;
        g_realCreate = (PFN_DirectInput8Create)GetProcAddress(real, "DirectInput8Create");
        if (!g_realCreate) return E_FAIL;
    }

    LogGuid("DirectInput8Create riid", riid);
    HRESULT hr = g_realCreate(hInst, ver, riid, out, outer);
    if (SUCCEEDED(hr) && out && *out && IsEqualIID(riid, IID_IDirectInput8A))
        *out = new DInputWrapper((IDirectInput8A*)*out);
    return hr;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = h;
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
    }
    return TRUE;
}