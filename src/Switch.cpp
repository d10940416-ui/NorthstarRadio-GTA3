// Must be defined before ANY plugin-sdk header is included
#define GTAIII

#include "plugin.h"
#include "CPad.h"
#include "cDMAudio.h"
#include "CRunningScript.h"
#include "CTheScripts.h"
#include "CGame.h"
#include "safetyhook.hpp"
#include <fstream>
#include <string>
#include <atomic>
#include <windows.h>
#include <Xinput.h>
#pragma comment(lib, "Xinput.lib")

using namespace plugin;

// ============================================================
// GTA III PORT NOTES — Switch.cpp
// ============================================================
// All hook addresses below are marked GTA3_TODO.
// They were hardcoded for VC's gta3.exe; you must find the
// equivalent addresses in GTA III's gta3.exe (v1.0).
//
// RECOMMENDED METHOD — x64dbg + plugin-sdk symbols:
//   1. Open gta3.exe in x64dbg, load the .pdb / map file from
//      the GTA III plugin-sdk (symbols folder).
//   2. Search for each function name listed in the TODO comment
//      above each address and paste the correct address here.
//
// ALTERNATIVE — re3 open-source project:
//   https://github.com/halpz/re3
//   Function addresses are often in the .cpp files as comments
//   or can be found via grep of the function name.
//
// SCM OPCODE CHANGES vs VC:
//   0x057D  PLAY_ANNOUNCEMENT  — NOT present in GTA III main.scm.
//           Disabled below; left as dead code for custom scripts.
//   0x041E  SET_RADIO_CHANNEL  — GTA3_TODO: verify the opcode
//           number in GTA III. It may be the same or different.
//   0x0394  PLAY_MISSION_PASSED_TUNE — GTA3_TODO: verify.
//   0x03D1  PLAY_MISSION_AUDIO — GTA3_TODO: verify.
// ============================================================

extern std::ofstream gLog;

bool gSwitchNext = false;
bool gSwitchPrev = false;

std::atomic<int> gPendingAnnouncement{ -1 };
std::atomic<int> gPendingScmStation{ -1 };
std::atomic<int> gPendingScmStationTime{ -1 };
std::atomic<DWORD> gMissionPassedDuckUntil{ 0 };
std::atomic<DWORD> gDialogueDuckUntil{ 0 };

// ============================================================
// GTA3_TODO — MOUSE WHEEL ADDRESSES
// ============================================================
// VC addresses: WheelUp = 0x94D78B, WheelDown = 0x94D78C
// These point to CPad::NewMouseControllerState.wheelUp/Down.
//
// To find GTA III equivalents:
//   1. In x64dbg, set a write breakpoint on the mouse wheel
//      handler while scrolling in-game.
//   2. Note the addresses written. They will be near each other
//      (typically 1 byte apart) inside the CPad struct.
//   3. Paste the two addresses below.
// ============================================================
static uint8_t* const pMouseWheelUp   = (uint8_t*)0x00000000; // GTA3_TODO
static uint8_t* const pMouseWheelDown = (uint8_t*)0x00000001; // GTA3_TODO

static int gRadioSwitchNextKey = 82;  // R key (unchanged from VC)
static int gRadioSwitchNextPad = 0;   // controller: 0 = disabled

static bool gScriptIntegrationEnabled = true;

static void LoadControlsFromINI()
{
    char path[MAX_PATH];
    HMODULE hModule = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&LoadControlsFromINI, &hModule);
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string scriptsFolder = std::string(path);
    scriptsFolder = scriptsFolder.substr(0, scriptsFolder.find_last_of("\\/") + 1);

    std::string iniPath = scriptsFolder + "NorthstarRadio.ini";
    std::ifstream ini(iniPath, std::ios::binary);
    if (!ini.is_open()) return;

    unsigned char bom[3] = {};
    ini.read((char*)bom, 3);
    if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
        ini.seekg(0);

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end   = s.find_last_not_of(" \t\r\n");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    };

    bool inControlsSection = false;
    std::string line;
    while (std::getline(ini, line)) {
        size_t comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        trim(line);
        if (line.empty()) continue;

        if (line[0] == '[') {
            std::string header = line;
            std::transform(header.begin(), header.end(), header.begin(), ::tolower);
            trim(header);
            inControlsSection = (header == "[controls]");
            continue;
        }
        if (!inControlsSection) continue;

        size_t sep = line.find('=');
        if (sep == std::string::npos) continue;
        std::string key = line.substr(0, sep);
        std::string val = line.substr(sep + 1);
        trim(key); trim(val);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);

        if (key == "radioswitchnext" && !val.empty())
            gRadioSwitchNextKey = atoi(val.c_str());
        else if (key == "radioswitchnextpad" && !val.empty())
            gRadioSwitchNextPad = atoi(val.c_str());
    }
}

static bool ReadScriptIntegrationFlag()
{
    char path[MAX_PATH];
    HMODULE hModule = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&ReadScriptIntegrationFlag, &hModule);
    GetModuleFileNameA(hModule, path, MAX_PATH);
    std::string scriptsFolder = std::string(path);
    scriptsFolder = scriptsFolder.substr(0, scriptsFolder.find_last_of("\\/") + 1);

    std::ifstream ini(scriptsFolder + "NorthstarRadio.ini", std::ios::binary);
    if (!ini.is_open()) return true;

    unsigned char bom[3] = {};
    ini.read((char*)bom, 3);
    if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
        ini.seekg(0);

    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end   = s.find_last_not_of(" \t\r\n");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    };

    bool inSettings = false;
    std::string line;
    while (std::getline(ini, line)) {
        size_t comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        trim(line);
        if (line.empty()) continue;
        if (line[0] == '[') {
            std::string header = line;
            std::transform(header.begin(), header.end(), header.begin(), ::tolower);
            inSettings = (header == "[settings]");
            continue;
        }
        if (!inSettings) continue;
        size_t sep = line.find('=');
        if (sep == std::string::npos) continue;
        std::string key = line.substr(0, sep);
        std::string val = line.substr(sep + 1);
        trim(key); trim(val);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        if (key == "scriptintegration" && !val.empty())
            return atoi(val.c_str()) != 0;
    }
    return true;
}

// ============================================================
// HOOK: Block native radio station switching
// ============================================================
// GTA3_TODO: Replace 0x00000000 with the GTA III v1.0 address
// of CPad::ChangeStationJustDown (or whatever the equivalent
// function is called in the GTA III plugin-sdk / re3).
//
// In VC it was at 0x4AA590. To find it in GTA III:
//   - Search re3 for "ChangeStation" or "GetCarRadioOn"
//   - Or find the function that returns true for one frame when
//     the radio-change button is pressed while in a car.
// ============================================================
#define GTA3_ADDR_ChangeStationJustDown   ((void*)0x00000000) // GTA3_TODO

static bool __fastcall Hook_ChangeStationJustDown(CPad* self, void* edx)
{
    return false;
}

// ============================================================
// HOOKS: Suppress native SetRadioInCar + vehicle radio process
// ============================================================
// GTA3_TODO: Replace addresses below with GTA III v1.0 values.
//
// GTA3_ADDR_SetRadioInCar:
//   VC had this at 0x5F9730 (cDMAudio::SetRadioInCar).
//   In GTA III plugin-sdk search for "SetRadioInCar" or look in
//   cDMAudio.h for the function address annotation.
//
// GTA3_ADDR_VehicleRadioProcess:
//   VC had the vehicle radio audio-process function at 0x5FB600.
//   In GTA III this is the function that reads wheel input and
//   drives the HUD radio-name banner. Find it by tracing the
//   call chain from CGame::Process -> DMAudio -> vehicle radio.
// ============================================================
#define GTA3_ADDR_SetRadioInCar         ((void*)0x00000000) // GTA3_TODO
#define GTA3_ADDR_VehicleRadioProcess   ((void*)0x00000000) // GTA3_TODO

static SafetyHookInline gSetRadioInCarHook;
static SafetyHookInline gRadioProcessHook;

extern bool gPlayerInVehicle;

static bool LetNativeAudioRun()
{
    // Allow native radio/ambient audio only when the player is
    // on foot inside an interior. Same logic as VC.
    return CGame::currArea != 0 && !gPlayerInVehicle;
}

static void __fastcall Hook_SetRadioInCar(cDMAudio* self, void* edx, unsigned int radio)
{
    if (LetNativeAudioRun())
        gSetRadioInCarHook.thiscall<void>(self, radio);
}

static void __fastcall Hook_VehicleRadioProcess(void* self, void* edx)
{
    if (LetNativeAudioRun())
        gRadioProcessHook.thiscall<void>(self);
}

// ============================================================
// SCM OPCODE HOOK
// ============================================================
// GTA3_TODO: Replace 0x00000000 with the address of
// CRunningScript::ProcessOneCommand in GTA III v1.0.
// VC had it at 0x44FBE0. In re3 search for "ProcessOneCommand"
// or look at CRunningScript.cpp.
//
// OPCODE NUMBERS — GTA III vs VC:
//
//   0x057D  PLAY_ANNOUNCEMENT  (VC only — not in GTA III)
//           Disabled: GTA III main.scm has no announcements.
//           If you write a custom script that uses this opcode,
//           re-enable by un-commenting the block below.
//
//   0x041E  SET_RADIO_CHANNEL
//           GTA3_TODO: Verify this opcode exists in GTA III
//           with the same number. Open main.scm in Sanny Builder
//           and search for "set_radio_channel" or opcode 041E.
//           If the number differs, update it here.
//
//   0x0394  PLAY_MISSION_PASSED_TUNE
//           GTA3_TODO: Verify opcode number in GTA III.
//
//   0x03D1  PLAY_MISSION_AUDIO
//           GTA3_TODO: Verify opcode number in GTA III.
// ============================================================
#define GTA3_ADDR_ProcessOneCommand     ((void*)0x00000000) // GTA3_TODO

// GTA3_TODO: Confirm these opcode numbers against GTA III main.scm.
// Open main.scm in Sanny Builder 3 (GTA III mode) and search for
// each opcode. If numbers differ, change the #defines here.
#define GTA3_OP_SET_RADIO_CHANNEL       0x041E // GTA3_TODO: verify
#define GTA3_OP_MISSION_PASSED_TUNE     0x0394 // GTA3_TODO: verify
#define GTA3_OP_MISSION_AUDIO           0x03D1 // GTA3_TODO: verify
// #define GTA3_OP_ANNOUNCEMENT         0x057D // NOT in GTA III — disabled

static SafetyHookMid gScmHook;

// GTA III SCM parameter type bytes (same values as VC).
// Using raw bytes instead of SCRIPTPARAM_* enum names to avoid
// any naming differences between plugin-sdk versions.
// GTA III / VC SCM spec:
//   0x01 = INT32    (4 bytes)
//   0x02 = GLOBAL   (2-byte index into ScriptSpace)
//   0x03 = LOCAL    (2-byte index into local var array)
//   0x04 = INT8     (1 byte, signed)
//   0x05 = INT16    (2 bytes, signed)
//   0x06 = FLOAT32  (4 bytes)
static bool ReadScmInt(CRunningScript* self,
                       const unsigned char* ss, int* off,
                       int* out, unsigned char* typeOut)
{
    unsigned char type = ss[*off];
    *off += 1;
    if (typeOut) *typeOut = type;
    switch (type) {
    case 0x01: // INT32
        *out = *(const int*)(ss + *off);        *off += 4; return true;
    case 0x05: // INT16
        *out = *(const short*)(ss + *off);      *off += 2; return true;
    case 0x04: // INT8
        *out = *(const signed char*)(ss + *off);*off += 1; return true;
    case 0x06: // FLOAT32
        *out = (int)*(const float*)(ss + *off); *off += 4; return true;
    case 0x02: { // GLOBAL VAR
        unsigned short goff = *(const unsigned short*)(ss + *off); *off += 2;
        *out = *(const int*)(ss + goff);
        return true;
    }
    case 0x03: { // LOCAL VAR
        unsigned short idx = *(const unsigned short*)(ss + *off); *off += 2;
        if (self && idx < 16) { *out = self->m_aLocalVars[idx].iParam; return true; }
        return false;
    }
    default: return false;
    }
}

static void OnProcessOneCommand(SafetyHookContext& ctx)
{
    CRunningScript* self = (CRunningScript*)ctx.ecx;
    if (!self) return;

    int ip = self->m_nIp;
    if (ip < 0) return;

    const unsigned char* ss = CTheScripts::ScriptSpace;
    unsigned short op = (*(const unsigned short*)(ss + ip)) & 0x7FFF;

    // GTA3_TODO: if any opcode numbers above are wrong for GTA III,
    // this early-exit will simply skip the handler harmlessly.
    if (op != GTA3_OP_SET_RADIO_CHANNEL &&
        op != GTA3_OP_MISSION_PASSED_TUNE &&
        op != GTA3_OP_MISSION_AUDIO)
        return;

    // PLAY_MISSION_PASSED_TUNE: duck radio for ~5 seconds
    if (op == GTA3_OP_MISSION_PASSED_TUNE) {
        gMissionPassedDuckUntil = GetTickCount() + 5000;
        return;
    }

    int off = ip + 2;
    int first;
    if (!ReadScmInt(self, ss, &off, &first, nullptr))
        return;

    // PLAY_MISSION_AUDIO: refresh dialogue duck deadline
    if (op == GTA3_OP_MISSION_AUDIO) {
        gDialogueDuckUntil = GetTickCount() + 5000;
        return;
    }

    // --- PLAY_ANNOUNCEMENT (0x057D) ---
    // Disabled for GTA III: the stock main.scm does not use this
    // opcode. To re-enable for custom scripts, uncomment below
    // and add GTA3_OP_ANNOUNCEMENT to the check above.
    //
    // if (op == GTA3_OP_ANNOUNCEMENT) {
    //     gPendingAnnouncement = first;
    //     return;
    // }

    // SET_RADIO_CHANNEL: param1 = station index, param2 = timecode ms
    int timecode = -1;
    ReadScmInt(self, ss, &off, &timecode, nullptr);
    gPendingScmStationTime = timecode;
    gPendingScmStation     = first;
}

class SwitchDetectorPlugin
{
public:
    SwitchDetectorPlugin()
    {
        // GTA3_TODO: Both addresses below are 0 until you fill them in.
        // Passing 0 to MakeJMP or create_inline will crash on startup.
        // Fill in GTA3_ADDR_* defines before building.
        if (GTA3_ADDR_ChangeStationJustDown != (void*)0x00000000)
            injector::MakeJMP(GTA3_ADDR_ChangeStationJustDown,
                              (void*)Hook_ChangeStationJustDown, true);

        if (GTA3_ADDR_SetRadioInCar != (void*)0x00000000)
            gSetRadioInCarHook = safetyhook::create_inline(
                GTA3_ADDR_SetRadioInCar, (void*)Hook_SetRadioInCar);

        if (GTA3_ADDR_VehicleRadioProcess != (void*)0x00000000)
            gRadioProcessHook  = safetyhook::create_inline(
                GTA3_ADDR_VehicleRadioProcess, (void*)Hook_VehicleRadioProcess);

        gScriptIntegrationEnabled = ReadScriptIntegrationFlag();
        if (gScriptIntegrationEnabled &&
            GTA3_ADDR_ProcessOneCommand != (void*)0x00000000)
        {
            gScmHook = safetyhook::create_mid(
                GTA3_ADDR_ProcessOneCommand, OnProcessOneCommand);
        }

        Events::initGameEvent.Add([]()
        {
            LoadControlsFromINI();
            if (gLog.is_open()) {
                gLog << "GTA III port — addresses still needed:" << std::endl;
                if (GTA3_ADDR_ChangeStationJustDown == (void*)0x00000000)
                    gLog << "  [!] GTA3_ADDR_ChangeStationJustDown not set" << std::endl;
                if (GTA3_ADDR_SetRadioInCar == (void*)0x00000000)
                    gLog << "  [!] GTA3_ADDR_SetRadioInCar not set" << std::endl;
                if (GTA3_ADDR_VehicleRadioProcess == (void*)0x00000000)
                    gLog << "  [!] GTA3_ADDR_VehicleRadioProcess not set" << std::endl;
                if (GTA3_ADDR_ProcessOneCommand == (void*)0x00000000)
                    gLog << "  [!] GTA3_ADDR_ProcessOneCommand not set" << std::endl;
                if (pMouseWheelUp == (uint8_t*)0x00000000)
                    gLog << "  [!] pMouseWheelUp / pMouseWheelDown not set" << std::endl;
                gLog << "ScriptIntegration: "
                     << (gScriptIntegrationEnabled ? "ENABLED" : "DISABLED")
                     << std::endl;
                gLog.flush();
            }
        });

        Events::gameProcessEvent.Add([]()
        {
            // Consume wheel bytes so the native radio code never sees them.
            // Guarded by null-check because addresses start at 0 until filled.
            if (pMouseWheelUp  && pMouseWheelUp  != (uint8_t*)0x00000000 && *pMouseWheelUp) {
                gSwitchNext = true;
                *pMouseWheelUp = 0;
            }
            if (pMouseWheelDown && pMouseWheelDown != (uint8_t*)0x00000001 && *pMouseWheelDown) {
                gSwitchPrev = true;
                *pMouseWheelDown = 0;
            }

            // Keyboard key — fires once per press
            static bool gKeyWasDown = false;
            bool keyDown = (GetAsyncKeyState(gRadioSwitchNextKey) & 0x8000) != 0;
            if (keyDown && !gKeyWasDown)
                gSwitchNext = true;
            gKeyWasDown = keyDown;

            // Controller button via XInput — checks all 4 ports
            if (gRadioSwitchNextPad != 0) {
                static bool gPadWasDown = false;
                bool padDown = false;
                for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
                    XINPUT_STATE state = {};
                    if (XInputGetState(i, &state) == ERROR_SUCCESS) {
                        if (state.Gamepad.wButtons & gRadioSwitchNextPad) {
                            padDown = true;
                            break;
                        }
                    }
                }
                if (padDown && !gPadWasDown)
                    gSwitchNext = true;
                gPadWasDown = padDown;
            }
        });
    }
} switchDetectorPlugin;
