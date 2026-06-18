#include "plugin.h"
#include "CPad.h"
#include "CPlayerPed.h"
#include "CVehicle.h"
#include "cDMAudio.h"
#include "CMenuManager.h"
#include "CHud.h"
#include "CFont.h"
#include "CControllerConfigManager.h"
#include "CCutsceneMgr.h"
#include "CCamera.h"
#include "bass.h"
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>
#include <windows.h>

using namespace plugin;

// ============================================================
// GTA III PORT NOTES
// ============================================================
// Lines marked GTA3_TODO need manual verification against
// gta3.exe v1.0. Use x64dbg + plugin-sdk symbols, or the
// re3 open-source project, to confirm addresses and offsets.
//
// Everything else has been adapted automatically:
//   - pMusicVolume  -> FrontEndMenuManager.m_nPrefsMusicVolume
//   - vehicle radio -> CVehicle::m_nRadioStation  (plugin-sdk)
//   - player state  -> CPed::m_nPedState          (plugin-sdk)
//   - radio-off val -> GTA3_RADIO_OFF (9 instead of VC's 10)
//   - widescreen    -> TheCamera.m_bWideScreen     (GTA3 field)
//   - VC comments   -> updated to GTA III
//   - SFX indices   -> see TODO block below
// ============================================================

// "Radio off" station index for GTA III. GTA III has 9 stations
// (indices 0-8), so the native "off" token is 9. This replaces
// VC's value of 10 (VC had 10 stations, 0-9, then 10 = off).
// GTA3_TODO: confirm against cDMAudio / CVehicle in gta3.exe.
#define GTA3_RADIO_OFF 9

// GTA3_TODO: Replace these two placeholder addresses with the
// real GTA III v1.0 screen-resolution variables.
// Quickest approach: #include "RW/RwCore.h" and use
//   RsGlobal.maximumWidth / RsGlobal.maximumHeight
// instead of raw pointers, then remove these two lines and
// update the HUD block at the bottom of this file.
static DWORD* const pResWidth  = (DWORD*)0xA0FD04; // GTA3_TODO
static DWORD* const pResHeight = (DWORD*)0xA0FD08; // GTA3_TODO

// Declared in Switch.cpp
extern bool gSwitchNext;
extern bool gSwitchPrev;

// Declared in RadioVehicles.cpp
int GetStationForVehicle(CVehicle* pVehicle);
void OnPlayerExitVehicle(CVehicle* pVehicle);
bool IsNoRadioVehicle(int modelId);

struct RadioStation {
    std::string name;
    std::string file;
};

std::vector<RadioStation> stations;
static std::vector<std::string> gMp3Files;
static int gMp3StationIndex = -1;

std::string gGameFolder;
std::string gScriptsFolder;
std::ofstream gLog;
bool gAmbientRadioEnabled = false;

static HSTREAM gStream = 0;
bool gWasInVehicle = false;
bool gPlayerInVehicle = false;
static CVehicle* gActiveVehicle = nullptr;
static bool gBassReady = false;
static bool gWasPaused = false;
static unsigned char gLastVolume = 0;

// Audio ducking
extern std::atomic<DWORD> gMissionPassedDuckUntil;
extern std::atomic<DWORD> gDialogueDuckUntil;
static float gDuckFactor = 1.0f;
static float gLastDuckApplied = 1.0f;
static const float RADIO_DUCK_LEVEL = 0.35f;

// GTA III music-volume preference.
// FrontEndMenuManager.m_nPrefsMusicVolume is the correct
// plugin-sdk accessor; the type is int in GTA III (vs unsigned
// char in VC). We cast to unsigned char* so all existing
// *pMusicVolume reads still work on little-endian x86.
// GTA3_TODO: confirm the volume range. VC is 0-127; GTA III
// might differ. Update MUSIC_VOLUME_MAX below if needed.
unsigned char* const pMusicVolume =
    (unsigned char*)&FrontEndMenuManager.m_nPrefsMusicVolume;

int gCurrentStation = -1;

const float MUSIC_VOLUME_MAX = 127.0f; // GTA3_TODO: verify range
float RadioVolume(unsigned int pref)
{
    float v = (float)pref / MUSIC_VOLUME_MAX;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

// Forced seek (SCM opcode 041E).
// GTA3_TODO: verify opcode 041E exists in GTA III main.scm;
// see Switch.cpp for details. If not present, this is dead code.
static int gForcedSeekStation = -1;
static double gForcedSeekMs = 0.0;

// Global radio clock
double gRadioTime = 0.0;
static DWORD gLastTick = 0;

static double gRadioStartOffset = 0.0;

std::map<int, double> gStationStartOffsetMs;
static bool gStartOffsetsActive = true;
static bool gOriginalsFromTop = true;
static std::map<int, double> gStationAnchorDelta;

static double ParseTimecodeMs(const std::string& s)
{
    size_t colon = s.find(':');
    if (colon != std::string::npos) {
        double mins = atof(s.substr(0, colon).c_str());
        double secs = atof(s.substr(colon + 1).c_str());
        return (mins * 60.0 + secs) * 1000.0;
    }
    return atof(s.c_str()) * 1000.0;
}

// Shared playback timeline.
// GTA III has 9 original stations (indices 0-8), same count as VC,
// so the constant 9 here is unchanged. The comment below has been
// updated from "Vice City" to "GTA III" to reflect the new game.
double StationTimelineMs(int index, bool applyStartOffset)
{
    // Indices 0..8 are the nine original GTA III stations listed in
    // the INI. Index 9 and up are added/custom stations.
    double base = (index >= 0 && index < 9 && gOriginalsFromTop)
        ? (gRadioTime - gRadioStartOffset)
        : gRadioTime;

    if (applyStartOffset && gStartOffsetsActive) {
        auto off = gStationStartOffsetMs.find(index);
        if (off != gStationStartOffsetMs.end()) {
            auto anc = gStationAnchorDelta.find(index);
            if (anc == gStationAnchorDelta.end())
                anc = gStationAnchorDelta.emplace(index, off->second - base).first;
            base += anc->second;
        }
    }
    return base;
}

// MP3 player state
static std::mutex gMp3Mutex;
static double gMp3SeekMs = 0.0;
static std::string gMp3FilePath = "";
static double gMp3RandomOffset = 0.0;
static double gMp3TotalDuration = 0.0;

// Background loading
static std::mutex gLoadMutex;
static std::vector<BYTE> gLoadedBuffer;
static std::atomic<bool> gBufferReady(false);
static std::atomic<bool> gLoadingInProgress(false);
static int gLoadingStation = -1;

// Station display + debounce
static bool gWaitingToPlay = false;
static int gLastNativeStation = -1;
static bool gPoliceRadioPlaying = false;
int gPendingStation = -1;
static DWORD gLastSwitchTick = 0;
static const DWORD SWITCH_DEBOUNCE_MS = 500;
static std::string gStationNameToShow = "";
static DWORD gStationNameTimer = 0;
static const DWORD STATION_NAME_DURATION = 3000;

// Announcements (SCM opcode 057D).
// GTA3_TODO: GTA III's main.scm does NOT have opcode 057D
// (PLAY_ANNOUNCEMENT). This infrastructure is kept in place for
// completeness and for custom scripts; it will simply never fire
// with the stock GTA III main.scm. See Switch.cpp for details.
extern std::atomic<int> gPendingAnnouncement;
static bool gAnnouncementPlaying = false;
static int gQueuedAnnouncement = -1;

// Mission-scripted station change (SCM opcode 041E).
// GTA3_TODO: see Switch.cpp — opcode number may differ in GTA III.
extern std::atomic<int> gPendingScmStation;
extern std::atomic<int> gPendingScmStationTime;

static bool gNoRadioVehicle = false;

// SFX (tuning click + static sweep)
static HSTREAM gSfxTuningStream = 0;
static HSTREAM gSfxStaticStream = 0;
static bool gSfxLoaded = false;

// SDT entry — same 20-byte layout in both GTA III and VC.
struct SdtEntry {
    DWORD offset;
    DWORD size;
    DWORD sampleRate;
    DWORD loopStart;
    DWORD loopEnd;
};

// ============================================================
// GTA3_TODO — SFX SOUND INDICES
// ============================================================
// In VC: tuning click = 343, static sounds = 344-355 (12 total).
// GTA III uses a different SFX bank so these indices are WRONG.
//
// How to find the correct indices for GTA III:
//   1. Open audio/SFX.SDT in a hex editor. Each entry is 20 bytes.
//      Entry N starts at offset N*20. Count from 0.
//   2. Load audio/SFX.RAW with a raw PCM viewer (16-bit signed,
//      mono, at the sampleRate in the SDT entry) to hear each sound.
//   3. Listen for the "click" (tuning) and the static/white-noise
//      bursts. Note their indices.
//   4. Update GTA3_SFX_TUNING and GTA3_SFX_STATIC_FIRST below.
//
// If you cannot find matching sounds, set GTA3_SFX_SKIP = 1 to
// disable SFX loading entirely. The radio will work silently
// (no click/static) instead of playing wrong audio.
// ============================================================
#define GTA3_SFX_SKIP         0   // set to 1 to skip SFX loading
#define GTA3_SFX_TUNING       343 // GTA3_TODO: find real index
#define GTA3_SFX_STATIC_FIRST 344 // GTA3_TODO: find real index
#define GTA3_SFX_STATIC_COUNT 12  // GTA3_TODO: may differ in GTA III

static int gStaticSoundIndex = 0;
static std::vector<BYTE> gWavTuning;
static std::vector<BYTE> gWavStatic[12]; // sized to match GTA3_SFX_STATIC_COUNT

static void BuildWav(const std::vector<BYTE>& pcm, DWORD sampleRate, std::vector<BYTE>& out)
{
    DWORD dataSize = (DWORD)pcm.size();
    out.resize(44 + dataSize);
    BYTE* p = out.data();

    auto write4 = [&](DWORD v) { memcpy(p, &v, 4); p += 4; };
    auto write2 = [&](WORD v)  { memcpy(p, &v, 2); p += 2; };

    memcpy(p, "RIFF", 4); p += 4;
    write4(36 + dataSize);
    memcpy(p, "WAVE", 4); p += 4;
    memcpy(p, "fmt ", 4); p += 4;
    write4(16);
    write2(1);
    write2(1);
    write4(sampleRate);
    write4(sampleRate * 2);
    write2(2);
    write2(16);
    memcpy(p, "data", 4); p += 4;
    write4(dataSize);
    memcpy(p, pcm.data(), dataSize);
}

static HSTREAM CreateRawStream(const std::vector<BYTE>& wavBuf, bool loop)
{
    if (wavBuf.empty()) return 0;
    HSTREAM s = BASS_StreamCreateFile(TRUE, wavBuf.data(), 0, (QWORD)wavBuf.size(),
        loop ? BASS_SAMPLE_LOOP : 0);
    if (!s) {
        gLog << "SFX: stream error " << BASS_ErrorGetCode() << std::endl;
        gLog.flush();
    }
    return s;
}

static void LoadAllSfx()
{
#if GTA3_SFX_SKIP
    gLog << "SFX: skipped (GTA3_SFX_SKIP=1)" << std::endl;
    gLog.flush();
    return;
#endif

    std::string sdtPath = gGameFolder + "audio\\SFX.SDT";
    std::string rawPath = gGameFolder + "audio\\SFX.RAW";

    std::ifstream sdt(sdtPath, std::ios::binary);
    if (!sdt.is_open()) {
        gLog << "SFX: cannot open SFX.SDT" << std::endl;
        gLog.flush();
        return;
    }
    std::ifstream raw(rawPath, std::ios::binary);
    if (!raw.is_open()) {
        gLog << "SFX: cannot open SFX.RAW" << std::endl;
        gLog.flush();
        return;
    }

    auto loadOne = [&](int index, std::vector<BYTE>& wavOut) -> bool {
        SdtEntry entry;
        sdt.seekg(index * sizeof(SdtEntry));
        sdt.read((char*)&entry, sizeof(SdtEntry));
        if (entry.size == 0) {
            gLog << "SFX: entry " << index << " has zero size" << std::endl;
            return false;
        }
        std::vector<BYTE> pcm(entry.size);
        raw.seekg(entry.offset);
        raw.read((char*)pcm.data(), entry.size);
        BuildWav(pcm, entry.sampleRate, wavOut);
        gLog << "SFX: sound " << index << " loaded — "
             << entry.size << " bytes @ " << entry.sampleRate << " Hz" << std::endl;
        return true;
    };

    bool ok = loadOne(GTA3_SFX_TUNING, gWavTuning);
    for (int i = 0; i < GTA3_SFX_STATIC_COUNT; i++)
        loadOne(GTA3_SFX_STATIC_FIRST + i, gWavStatic[i]);

    sdt.close();
    raw.close();

    gSfxLoaded = ok;
    gLog << "SFX loaded: " << (ok ? "yes" : "partial/failed") << std::endl;
    gLog.flush();
}

static void PlayTuningSound()
{
    if (!gSfxLoaded || gWavTuning.empty()) return;
    if (gSfxTuningStream) {
        BASS_ChannelStop(gSfxTuningStream);
        BASS_StreamFree(gSfxTuningStream);
        gSfxTuningStream = 0;
    }
    gSfxTuningStream = CreateRawStream(gWavTuning, false);
    if (gSfxTuningStream) {
        BASS_ChannelSetAttribute(gSfxTuningStream, BASS_ATTRIB_VOL, RadioVolume(*pMusicVolume));
        BASS_ChannelPlay(gSfxTuningStream, FALSE);
    }
}

static void StartStaticSound()
{
    if (!gSfxLoaded) return;
    if (gSfxStaticStream) {
        BASS_ChannelStop(gSfxStaticStream);
        BASS_StreamFree(gSfxStaticStream);
        gSfxStaticStream = 0;
    }
    int pick = gStaticSoundIndex % GTA3_SFX_STATIC_COUNT;
    gStaticSoundIndex = (gStaticSoundIndex + 1) % GTA3_SFX_STATIC_COUNT;
    if (gWavStatic[pick].empty()) return;
    gSfxStaticStream = CreateRawStream(gWavStatic[pick], true);
    if (gSfxStaticStream) {
        BASS_ChannelSetAttribute(gSfxStaticStream, BASS_ATTRIB_VOL, RadioVolume(*pMusicVolume));
        BASS_ChannelPlay(gSfxStaticStream, FALSE);
    }
}

static void StopStaticSound()
{
    if (gSfxStaticStream) {
        BASS_ChannelStop(gSfxStaticStream);
        BASS_StreamFree(gSfxStaticStream);
        gSfxStaticStream = 0;
    }
}

static void TurnRadioOff()
{
    if (gStream) {
        BASS_ChannelStop(gStream);
        BASS_StreamFree(gStream);
        gStream = 0;
    }
    StopStaticSound();
    { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer.clear(); }
    gBufferReady = false;
    gCurrentStation = -1;
    gWaitingToPlay = false;
    gStationNameToShow = "Radio Off";
    gStationNameTimer = GetTickCount();
    gLog << "Radio off" << std::endl;
    gLog.flush();
}

static void StopRadio()
{
    if (gStream) {
        BASS_ChannelStop(gStream);
        BASS_StreamFree(gStream);
        gStream = 0;
    }
}

static void StopAndResetRadio()
{
    StopRadio();
    StopStaticSound();
    { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer.clear(); }
    gBufferReady = false;
    gWaitingToPlay = false;
    gWasInVehicle = false;
    gCurrentStation = -1;
    gPendingStation = -1;
    gLastSwitchTick = 0;
    gAnnouncementPlaying = false;
    gQueuedAnnouncement = -1;
    gPoliceRadioPlaying = false;
    gNoRadioVehicle = false;
    gForcedSeekStation = -1;
    gPendingScmStation.exchange(-1);
    gPendingScmStationTime.exchange(-1);
    gStationNameToShow = "";
}

static bool IsADF(const std::string& path)
{
    std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".adf";
}

static void UpdateVolume()
{
    unsigned char vol = *pMusicVolume;
    float bassVol = RadioVolume(vol) * gDuckFactor;
    bool prefChanged = (vol != gLastVolume);
    bool duckChanged = (fabsf(gDuckFactor - gLastDuckApplied) > 0.001f);
    if (!prefChanged && !duckChanged) return;
    gLastVolume = vol;
    gLastDuckApplied = gDuckFactor;
    if (gStream)         BASS_ChannelSetAttribute(gStream,          BASS_ATTRIB_VOL, bassVol);
    if (gSfxStaticStream)BASS_ChannelSetAttribute(gSfxStaticStream, BASS_ATTRIB_VOL, bassVol);
    if (gSfxTuningStream)BASS_ChannelSetAttribute(gSfxTuningStream, BASS_ATTRIB_VOL, bassVol);
}

static std::vector<std::string> ScanMp3Folder(const std::string& folder)
{
    std::vector<std::string> files;
    std::string searchPath = folder + "mp3\\*.mp3";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return files;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            files.push_back(folder + "mp3\\" + fd.cFileName);
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    std::sort(files.begin(), files.end());
    return files;
}

static void EnsureMp3Durations()
{
    static bool loaded = false;
    if (loaded) return;
    double total = 0.0;
    for (const auto& f : gMp3Files) {
        HSTREAM s = BASS_StreamCreateFile(FALSE, f.c_str(), 0, 0, 0);
        if (s) {
            QWORD bytes = BASS_ChannelGetLength(s, BASS_POS_BYTE);
            double d = BASS_ChannelBytes2Seconds(s, bytes) * 1000.0;
            total += d > 0 ? d : 1.0;
            BASS_StreamFree(s);
        } else { total += 1.0; }
    }
    loaded = true;
    {
        std::lock_guard<std::mutex> lock(gMp3Mutex);
        gMp3TotalDuration = total;
        if (total > 0.0)
            gMp3RandomOffset = fmod((double)(rand() % 1000000), total);
    }
    gLog << "MP3 durations loaded, total: " << (DWORD)(total / 1000.0) << "s" << std::endl;
    gLog.flush();
}

static void GetMp3PlayerPosition(double radioTime, int& fileIndex, double& seekMs)
{
    if (gMp3Files.empty()) { fileIndex = 0; seekMs = 0; return; }
    EnsureMp3Durations();
    double total, offset;
    {
        std::lock_guard<std::mutex> lock(gMp3Mutex);
        total = gMp3TotalDuration;
        offset = gMp3RandomOffset;
    }
    if (total <= 0.0) { fileIndex = 0; seekMs = 0; return; }
    double pos = fmod(radioTime + offset, total);
    for (int i = 0; i < (int)gMp3Files.size(); i++) {
        HSTREAM s = BASS_StreamCreateFile(FALSE, gMp3Files[i].c_str(), 0, 0, 0);
        double dur = 1.0;
        if (s) {
            QWORD bytes = BASS_ChannelGetLength(s, BASS_POS_BYTE);
            double d = BASS_ChannelBytes2Seconds(s, bytes) * 1000.0;
            if (d > 0) dur = d;
            BASS_StreamFree(s);
        }
        if (pos < dur) { fileIndex = i; seekMs = pos; return; }
        pos -= dur;
    }
    fileIndex = 0; seekMs = 0;
}

static void LoadStationThread(int index)
{
    if (index == gMp3StationIndex) {
        int fileIndex; double seekMs;
        GetMp3PlayerPosition(gRadioTime, fileIndex, seekMs);
        {
            std::lock_guard<std::mutex> lock(gMp3Mutex);
            gMp3FilePath = gMp3Files[fileIndex];
            gMp3SeekMs = seekMs;
        }
        gLog << "Thread: MP3 PLAYER -> " << gMp3Files[fileIndex]
             << " at " << (DWORD)seekMs << " ms" << std::endl;
        gLog.flush();
        { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer.clear(); }
        gBufferReady = true;
        gLoadingInProgress = false;
        return;
    }

    std::string fullPath = gGameFolder + stations[index].file;
    gLog << "Thread: loading " << fullPath << std::endl;
    gLog.flush();

    std::vector<BYTE> buf;
    if (IsADF(fullPath)) {
        std::ifstream f(fullPath, std::ios::binary);
        if (!f.is_open()) {
            gLog << "Thread: cannot open file!" << std::endl;
            gLog.flush();
            gLoadingInProgress = false;
            return;
        }
        buf = std::vector<BYTE>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
        for (auto& b : buf) b ^= 0x22; // ADF decryption (same key as VC)
    }

    gLog << "Thread: loaded " << buf.size() << " bytes" << std::endl;
    gLog.flush();
    { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer = std::move(buf); }
    gBufferReady = true;
    gLoadingInProgress = false;
}

static void StartLoadingStation(int index)
{
    if (gLoadingInProgress) return;
    gBufferReady = false;
    gLoadingInProgress = true;
    gLoadingStation = index;
    gWaitingToPlay = true;
    std::thread(LoadStationThread, index).detach();
}

static void PlayLoadedStation(int index)
{
    StopRadio();
    StopStaticSound();
    gWaitingToPlay = false;

    if (index == gMp3StationIndex) {
        std::string filePath; double seekMs;
        { std::lock_guard<std::mutex> lock(gMp3Mutex); filePath = gMp3FilePath; seekMs = gMp3SeekMs; }
        gStream = BASS_StreamCreateFile(FALSE, filePath.c_str(), 0, 0, BASS_SAMPLE_LOOP);
        if (!gStream) {
            gLog << "BASS MP3 error: " << BASS_ErrorGetCode() << std::endl;
            gLog.flush();
            return;
        }
        QWORD seekBytes = BASS_ChannelSeconds2Bytes(gStream, seekMs / 1000.0);
        BASS_ChannelSetPosition(gStream, seekBytes, BASS_POS_BYTE);
        gLog << "MP3 PLAYER: " << filePath << " at " << (DWORD)seekMs << " ms" << std::endl;
        gLog.flush();
    } else {
        std::vector<BYTE> localBuffer;
        {
            std::lock_guard<std::mutex> lock(gLoadMutex);
            localBuffer = std::move(gLoadedBuffer);
            gLoadedBuffer.clear();
        }
        if (!localBuffer.empty()) {
            gStream = BASS_StreamCreateFile(TRUE, localBuffer.data(), 0, localBuffer.size(), BASS_SAMPLE_LOOP);
            if (!gStream) {
                gLog << "BASS error: " << BASS_ErrorGetCode() << std::endl;
                gLog.flush();
                return;
            }
            { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer = std::move(localBuffer); }
        } else {
            std::string fullPath = gGameFolder + stations[index].file;
            gStream = BASS_StreamCreateFile(FALSE, fullPath.c_str(), 0, 0, BASS_SAMPLE_LOOP);
            if (!gStream) {
                gLog << "BASS error: " << BASS_ErrorGetCode() << std::endl;
                gLog.flush();
                return;
            }
        }

        QWORD totalBytes = BASS_ChannelGetLength(gStream, BASS_POS_BYTE);
        double totalMs = BASS_ChannelBytes2Seconds(gStream, totalBytes) * 1000.0;
        if (totalMs > 0.0) {
            double seekMs;
            if (index == gForcedSeekStation && gForcedSeekMs >= 0.0)
                seekMs = fmod(gForcedSeekMs, totalMs);
            else
                seekMs = fmod(StationTimelineMs(index, true), totalMs);
            gForcedSeekStation = -1;
            QWORD seekBytes = BASS_ChannelSeconds2Bytes(gStream, seekMs / 1000.0);
            BASS_ChannelSetPosition(gStream, seekBytes, BASS_POS_BYTE);
            gLog << "[" << stations[index].name << "] seek: "
                 << (DWORD)seekMs << " / " << (DWORD)totalMs << " ms" << std::endl;
            gLog.flush();
        }
    }

    unsigned char vol = *pMusicVolume;
    gLastVolume = vol;
    BASS_ChannelSetAttribute(gStream, BASS_ATTRIB_VOL, RadioVolume(vol));
    BASS_ChannelPlay(gStream, FALSE);
    gCurrentStation = index;
    if (index >= 0 && index < (int)stations.size()) {
        gStationNameToShow = stations[index].name;
        gStationNameTimer = GetTickCount();
    }
}

static void LoadINI()
{
    char path[MAX_PATH];
    HMODULE hModule = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)&LoadINI, &hModule);
    GetModuleFileNameA(hModule, path, MAX_PATH);
    gScriptsFolder = std::string(path);
    gScriptsFolder = gScriptsFolder.substr(0, gScriptsFolder.find_last_of("\\/") + 1);
    std::string noSlash = gScriptsFolder.substr(0, gScriptsFolder.size() - 1);
    gGameFolder = noSlash.substr(0, noSlash.find_last_of("\\/") + 1);

    std::string iniPath = gScriptsFolder + "NorthstarRadio.ini";
    std::string logPath = gScriptsFolder + "NorthstarRadio.log";

    gLog.open(logPath);
    gLog << "Game folder: " << gGameFolder << std::endl;
    gLog << "Scripts folder: " << gScriptsFolder << std::endl;

    std::ifstream iniBin(iniPath, std::ios::binary);
    if (!iniBin.is_open()) {
        gLog << "ERROR: NorthstarRadio.ini not found" << std::endl;
        return;
    }
    std::vector<BYTE> rawBytes((std::istreambuf_iterator<char>(iniBin)),
                                std::istreambuf_iterator<char>());
    iniBin.close();

    std::string iniContent;
    if (rawBytes.size() >= 2 && rawBytes[0] == 0xFF && rawBytes[1] == 0xFE) {
        const wchar_t* wstr = (const wchar_t*)(rawBytes.data() + 2);
        int wlen = (int)((rawBytes.size() - 2) / 2);
        int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, nullptr, 0, nullptr, nullptr);
        iniContent.resize(needed);
        WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, &iniContent[0], needed, nullptr, nullptr);
        gLog << "INI: detected UTF-16 LE, converted automatically" << std::endl;
    } else if (rawBytes.size() >= 2 && rawBytes[0] == 0xFE && rawBytes[1] == 0xFF) {
        std::vector<BYTE> swapped(rawBytes.begin() + 2, rawBytes.end());
        for (size_t i = 0; i + 1 < swapped.size(); i += 2) std::swap(swapped[i], swapped[i + 1]);
        const wchar_t* wstr = (const wchar_t*)swapped.data();
        int wlen = (int)(swapped.size() / 2);
        int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, nullptr, 0, nullptr, nullptr);
        iniContent.resize(needed);
        WideCharToMultiByte(CP_UTF8, 0, wstr, wlen, &iniContent[0], needed, nullptr, nullptr);
        gLog << "INI: detected UTF-16 BE, converted automatically" << std::endl;
    } else if (rawBytes.size() >= 3 && rawBytes[0] == 0xEF && rawBytes[1] == 0xBB && rawBytes[2] == 0xBF) {
        iniContent.assign(rawBytes.begin() + 3, rawBytes.end());
        gLog << "INI: detected UTF-8 BOM, stripped automatically" << std::endl;
    } else if (rawBytes.size() >= 2 && rawBytes[0] == '{' && rawBytes[1] == '\\') {
        gLog << "ERROR: NorthstarRadio.ini is saved as RTF format." << std::endl;
        gLog << "Please open it in Notepad (not WordPad) and save as plain text." << std::endl;
        gLog.flush();
        return;
    } else {
        iniContent.assign(rawBytes.begin(), rawBytes.end());
    }

    std::istringstream ini(iniContent);
    auto trim = [](std::string& s) {
        size_t start = s.find_first_not_of(" \t\r\n");
        size_t end   = s.find_last_not_of(" \t\r\n");
        s = (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    };

    bool inStationsSection = false;
    bool inSettingsSection = false;
    bool inOffsetSection   = false;
    std::map<std::string, double> pendingOffsets;
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
            inStationsSection = (header == "[stations]");
            inSettingsSection = (header == "[settings]");
            inOffsetSection   = (header == "[startoffset]");
            continue;
        }

        if (inOffsetSection) {
            size_t sep = line.find('|');
            if (sep == std::string::npos) continue;
            std::string name = line.substr(0, sep);
            std::string tc   = line.substr(sep + 1);
            trim(name); trim(tc);
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (!name.empty() && !tc.empty())
                pendingOffsets[name] = ParseTimecodeMs(tc);
            continue;
        }

        if (inSettingsSection) {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                trim(key); trim(val);
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                if (key == "ambientradio")
                    gAmbientRadioEnabled = (val == "1");
            }
            continue;
        }

        if (!inStationsSection) continue;

        size_t sep = line.find('|');
        if (sep == std::string::npos) sep = line.find('\t');
        if (sep == std::string::npos) continue;

        RadioStation station;
        station.name = line.substr(0, sep);
        station.file = line.substr(sep + 1);
        trim(station.name); trim(station.file);
        if (station.name.empty() || station.file.empty()) continue;
        if (station.name.size() >= 2 && station.name.front() == '"' && station.name.back() == '"')
            station.name = station.name.substr(1, station.name.size() - 2);

        stations.push_back(station);
        gLog << "Station " << stations.size() << ": [" << station.name
             << "] -> [" << station.file << "]" << std::endl;
    }

    gLog << "Total stations loaded: " << stations.size() << std::endl;

    gMp3Files = ScanMp3Folder(gGameFolder);
    if (!gMp3Files.empty()) {
        gMp3StationIndex = (int)stations.size();
        RadioStation mp3Station;
        mp3Station.name = "MP3 PLAYER";
        mp3Station.file = "";
        stations.push_back(mp3Station);
        gLog << "MP3 PLAYER station added with " << gMp3Files.size() << " files" << std::endl;
    }

    if (!pendingOffsets.empty()) {
        for (int i = 0; i < (int)stations.size(); i++) {
            std::string lname = stations[i].name;
            std::transform(lname.begin(), lname.end(), lname.begin(), ::tolower);
            auto it = pendingOffsets.find(lname);
            if (it != pendingOffsets.end()) {
                gStationStartOffsetMs[i] = it->second;
                gLog << "Start offset: [" << stations[i].name << "] -> " << (DWORD)it->second << " ms" << std::endl;
            }
        }
    }

    gLog.flush();
}

class NoRadioPlugin
{
public:
    NoRadioPlugin()
    {
        LoadINI();

        srand((unsigned int)time(NULL));
        double randomMs = (double)((((DWORD)rand() << 15) | (DWORD)rand()) % 7200000U);
        gRadioTime = randomMs;
        gRadioStartOffset = randomMs;
        gLastTick = GetTickCount();
        gLog << "Radio time start: " << (DWORD)gRadioTime << " ms (offset "
             << (DWORD)gRadioStartOffset << "; original stations start at 0)" << std::endl;
        gLog.flush();

        Events::initGameEvent.Add([]()
        {
            if (FrontEndMenuManager.m_bWantToLoad) {
                gStartOffsetsActive = false;
                gOriginalsFromTop = false;
            }

            if (!gBassReady) {
                if (BASS_Init(-1, 44100, 0, 0, NULL))
                    gBassReady = true;
                else if (BASS_ErrorGetCode() == 8)
                    gBassReady = true;
                gLog << "BASS ready: " << gBassReady << std::endl;
                gLog.flush();
                LoadAllSfx();
                if (gBassReady && !stations.empty())
                    StartLoadingStation(0);
            }
        });

        Events::gameProcessEvent.Add([]()
        {
            DWORD now = GetTickCount();
            gRadioTime += (double)(now - gLastTick);
            gLastTick = now;

            // GTA III: m_bWantToRestart does not exist; check only m_bWantToLoad
            if (FrontEndMenuManager.m_bWantToLoad) {
                gStartOffsetsActive = false;
                gOriginalsFromTop = false;
                StopAndResetRadio();
                gWasPaused = false;
                return;
            }

            CPlayerPed* pPlayer = FindPlayerPed();
            if (!pPlayer) {
                StopAndResetRadio();
                gWasPaused = false;
                return;
            }

            {
                static CPlayerPed* sLastPed = nullptr;
                if (sLastPed && sLastPed != pPlayer && !gStationAnchorDelta.empty()) {
                    gStartOffsetsActive = false;
                    gOriginalsFromTop = false;
                    gStationAnchorDelta.clear();
                }
                sLastPed = pPlayer;
            }

            // GTA III: field is m_bWideScreenOn (same as re3 source)
            bool inCutscene = CCutsceneMgr::ms_running || TheCamera.m_bWideScreenOn;
            bool isPaused = FrontEndMenuManager.m_bMenuActive || inCutscene;

            if (isPaused && !gWasPaused) {
                if (gStream) BASS_ChannelPause(gStream);
                if (gSfxStaticStream) BASS_ChannelPause(gSfxStaticStream);
                gWasPaused = true;
            } else if (!isPaused && gWasPaused) {
                UpdateVolume();
                if (gStream) BASS_ChannelPlay(gStream, FALSE);
                if (gSfxStaticStream) BASS_ChannelPlay(gSfxStaticStream, FALSE);
                gWasPaused = false;
            }
            if (isPaused) return;

            CVehicle* pVehicle = pPlayer->m_pVehicle;
            // GTA III: derive inVehicle from pointer — avoids m_bInVehicle naming issues
            bool inVehicle = (pVehicle != nullptr);
            gPlayerInVehicle = inVehicle;

            if (inVehicle && pVehicle)
                gActiveVehicle = pVehicle;

            // ===== No-radio vehicles ([NORADIO] section) =====
            if (inVehicle && pVehicle && IsNoRadioVehicle(pVehicle->m_nModelIndex)) {
                if (!gNoRadioVehicle) {
                    StopRadio(); StopStaticSound();
                    gNoRadioVehicle = true;
                    gWasInVehicle = true;
                    gPoliceRadioPlaying = false;
                    gCurrentStation = -1;
                    gPendingStation = -1;
                    gLastSwitchTick = 0;
                    gWaitingToPlay = false;
                    gBufferReady = false;
                    gAnnouncementPlaying = false;
                    gStationNameToShow = "";
                    gLog << "No-radio vehicle (model " << pVehicle->m_nModelIndex << "): radio disabled" << std::endl;
                    gLog.flush();
                }
                gSwitchNext = false;
                gSwitchPrev = false;
                gPendingAnnouncement.exchange(-1);
                gQueuedAnnouncement = -1;
                gPendingScmStation.exchange(-1);
                gPendingScmStationTime.exchange(-1);
                UpdateVolume();
                return;
            }

            // ===== Police radio =====
            if (inVehicle && pVehicle && pVehicle->IsLawEnforcementVehicle()) {
                if (!gPoliceRadioPlaying) {
                    StopRadio(); StopStaticSound();
                    std::string policePath = gGameFolder + "audio\\police.mp3";
                    gStream = BASS_StreamCreateFile(FALSE, policePath.c_str(), 0, 0, BASS_SAMPLE_LOOP);
                    if (gStream) {
                        unsigned char vol = *pMusicVolume;
                        gLastVolume = vol;
                        BASS_ChannelSetAttribute(gStream, BASS_ATTRIB_VOL, RadioVolume(vol));
                        BASS_ChannelPlay(gStream, FALSE);
                        gLog << "Police radio started" << std::endl;
                        gLog.flush();
                    }
                    gPoliceRadioPlaying = true;
                }
                UpdateVolume();
                return;
            }

            if (gPoliceRadioPlaying) {
                StopRadio();
                gPoliceRadioPlaying = false;
                gWasInVehicle = false;
                gCurrentStation = -1;
                gWaitingToPlay = false;
            }

            // Suppress native radio — use GTA3_RADIO_OFF (9) instead of VC's 10
            if (inVehicle)
                DMAudio.SetRadioInCar(GTA3_RADIO_OFF);

            // Detect SCM-forced station changes via the vehicle's native radio byte.
            // GTA3_TODO: CVehicle::m_nRadioStation must exist in the GTA III
            // plugin-sdk for this to compile. If it does not, replace with a raw
            // offset read: *(BYTE*)((BYTE*)pVehicle + 0x????) — find the offset
            // by searching for CVehicle::m_nRadioStation in the GTA III source or
            // using x64dbg to watch the byte that changes when you scroll the radio.
            if (inVehicle && pVehicle) {
                int nativeStation = pVehicle->m_nRadioStation;
                if (nativeStation != GTA3_RADIO_OFF &&
                    nativeStation != gLastNativeStation && gWasInVehicle)
                {
                    if (nativeStation >= 0 && nativeStation < (int)stations.size()) {
                        gLastNativeStation = nativeStation;
                        StopRadio();
                        { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer.clear(); }
                        gBufferReady = false;
                        gPendingStation = -1;
                        gLastSwitchTick = 0;
                        gCurrentStation = nativeStation;
                        StartLoadingStation(nativeStation);
                        gLog << "SCM: station set to [" << stations[nativeStation].name << "]" << std::endl;
                        gLog.flush();
                    }
                }
                // Force native station back to "off" so the game's audio system
                // never acts on it. Value 9 = GTA3_RADIO_OFF.
                pVehicle->m_nRadioStation = GTA3_RADIO_OFF;
            } else {
                gLastNativeStation = -1;
            }

            // ===== Seated detection =====
            // GTA3_TODO: Confirm that m_nPedState == 50 (0x32) means "seated /
            // driving" in GTA III. In VC the raw offset 0x244 was used; here we
            // use the plugin-sdk member directly. Value 50 = PED_DRIVING in the
            // shared ePedState enum (same across GTA III, VC, SA).
            bool isSeated = (pPlayer->m_nPedState == 50); // 50 = PED_DRIVING

            if (isSeated && !gWasInVehicle && pVehicle) {
                gWasInVehicle = true;
                int station = GetStationForVehicle(pVehicle);
                StopRadio();
                { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer.clear(); }
                gBufferReady = false;
                gCurrentStation = station;
                StartLoadingStation(station);
            }

            if (gWasInVehicle && gWaitingToPlay && gBufferReady)
                PlayLoadedStation(gLoadingStation);

            // ===== Announcements =====
            // (opcode 057D; not in stock GTA III — see Switch.cpp for details)
            {
                int req = gPendingAnnouncement.exchange(-1);
                if (req == 0 || req == 1) gQueuedAnnouncement = req;
            }

            if (gAnnouncementPlaying) {
                if (!inVehicle) {
                    StopRadio();
                    gAnnouncementPlaying = false;
                    gQueuedAnnouncement = -1;
                } else if (!gStream || BASS_ChannelIsActive(gStream) == BASS_ACTIVE_STOPPED) {
                    gAnnouncementPlaying = false;
                    gSwitchNext = false;
                    gSwitchPrev = false;
                    if (gCurrentStation >= 0) {
                        StopRadio();
                        { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer.clear(); }
                        gBufferReady = false;
                        StartLoadingStation(gCurrentStation);
                        gLog << "Announcement finished, resuming station" << std::endl;
                        gLog.flush();
                    }
                    UpdateVolume();
                    return;
                } else {
                    gSwitchNext = false;
                    gSwitchPrev = false;
                    UpdateVolume();
                    return;
                }
            } else if (gQueuedAnnouncement != -1 && gWasInVehicle && inVehicle) {
                int ann = gQueuedAnnouncement;
                gQueuedAnnouncement = -1;
                std::string annFile = gGameFolder + "audio\\" + (ann == 0 ? "BCLOSED.mp3" : "BOPEN.mp3");
                StopRadio(); StopStaticSound();
                { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer.clear(); }
                gBufferReady = false;
                gWaitingToPlay = false;
                gPendingStation = -1;
                gLastSwitchTick = 0;
                gSwitchNext = false;
                gSwitchPrev = false;
                gStream = BASS_StreamCreateFile(FALSE, annFile.c_str(), 0, 0, 0);
                if (gStream) {
                    unsigned char vol = *pMusicVolume;
                    gLastVolume = vol;
                    BASS_ChannelSetAttribute(gStream, BASS_ATTRIB_VOL, RadioVolume(vol));
                    BASS_ChannelPlay(gStream, FALSE);
                    gAnnouncementPlaying = true;
                    gStationNameToShow = "";
                    gLog << "Announcement playing: " << annFile << std::endl;
                    gLog.flush();
                    UpdateVolume();
                    return;
                } else {
                    gLog << "Announcement: BASS error " << BASS_ErrorGetCode()
                         << " (" << annFile << ")" << std::endl;
                    gLog.flush();
                }
            }

            // ===== Mission-scripted radio change (SCM opcode 041E) =====
            {
                int scmStation = gPendingScmStation.load();
                if (scmStation >= 0 && inVehicle && gWasInVehicle && !gLoadingInProgress) {
                    int scmTime = gPendingScmStationTime.load();
                    gPendingScmStation = -1;
                    gPendingScmStationTime = -1;
                    // Stations 0..8 are the nine original GTA III stations.
                    if (scmStation <= 8 && scmStation < (int)stations.size()
                        && scmStation != gCurrentStation)
                    {
                        StopRadio();
                        { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer.clear(); }
                        gBufferReady = false;
                        gPendingStation = -1;
                        gLastSwitchTick = 0;
                        gCurrentStation = scmStation;
                        if (scmTime >= 0) {
                            gForcedSeekStation = scmStation;
                            gForcedSeekMs = (double)scmTime;
                            gLog << "SCM 041E: radio -> [" << stations[scmStation].name
                                 << "] @ " << scmTime << " ms (scripted timecode)" << std::endl;
                        } else {
                            gForcedSeekStation = -1;
                            gLog << "SCM 041E: radio -> [" << stations[scmStation].name
                                 << "] (synced clock)" << std::endl;
                        }
                        StartLoadingStation(scmStation);
                        gLog.flush();
                    }
                }
            }

            // ===== MP3 PLAYER: advance to next file when current ends =====
            if (gCurrentStation == gMp3StationIndex && gStream) {
                if (BASS_ChannelIsActive(gStream) == BASS_ACTIVE_STOPPED) {
                    StopRadio();
                    { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer.clear(); }
                    gBufferReady = false;
                    StartLoadingStation(gMp3StationIndex);
                }
            }

            // ===== Station switching (wheel / key / controller) =====
            if (gSwitchNext || gSwitchPrev) {
                bool goNext = gSwitchNext;
                gSwitchNext = false;
                gSwitchPrev = false;

                if (gWasInVehicle) {
                    int next;
                    if (gCurrentStation == -1 && gPendingStation == -1)
                        next = goNext ? 0 : (int)stations.size() - 1;
                    else {
                        int base = gPendingStation != -1 ? gPendingStation : gCurrentStation;
                        next = base + (goNext ? 1 : -1);
                    }

                    PlayTuningSound();

                    if (next < 0 || next >= (int)stations.size()) {
                        gPendingStation = -1;
                        gLastSwitchTick = 0;
                        StopStaticSound();
                        TurnRadioOff();
                    } else {
                        if (!gSfxStaticStream) StartStaticSound();
                        StopRadio();
                        gPendingStation = next;
                        gLastSwitchTick = GetTickCount();
                        gStationNameToShow = stations[next].name;
                        gStationNameTimer = GetTickCount();
                        gLog << "Pending: [" << stations[next].name << "]" << std::endl;
                        gLog.flush();
                    }
                }
            }

            if (gPendingStation != -1 && gLastSwitchTick != 0) {
                if (GetTickCount() - gLastSwitchTick >= SWITCH_DEBOUNCE_MS) {
                    int next = gPendingStation;
                    gPendingStation = -1;
                    gLastSwitchTick = 0;
                    { std::lock_guard<std::mutex> lock(gLoadMutex); gLoadedBuffer.clear(); }
                    gBufferReady = false;
                    gCurrentStation = next;
                    StartLoadingStation(next);
                    gLog << "Loading: [" << stations[next].name << "]" << std::endl;
                    gLog.flush();
                }
            }

            // ===== Vehicle exit =====
            if (!inVehicle && gWasInVehicle) {
                OnPlayerExitVehicle(gActiveVehicle ? gActiveVehicle : pVehicle);
                gActiveVehicle = nullptr;
                StopRadio(); StopStaticSound();
                gStationNameToShow = "";
                gWasInVehicle = false;
                gNoRadioVehicle = false;
                gCurrentStation = -1;
                gPendingStation = -1;
                gLastSwitchTick = 0;
                gWaitingToPlay = false;
                gPendingScmStation.exchange(-1);
                gPendingScmStationTime.exchange(-1);
                gForcedSeekStation = -1;
            }

            // ===== Audio ducking =====
            {
                DWORD nowTick = GetTickCount();
                bool duck = gStream && (gDialogueDuckUntil.load() > nowTick
                                        || gMissionPassedDuckUntil.load() > nowTick);
                float target = duck ? RADIO_DUCK_LEVEL : 1.0f;
                gDuckFactor += (target - gDuckFactor) * 0.18f;
                if (gDuckFactor > 0.999f) gDuckFactor = 1.0f;
                if (gDuckFactor < RADIO_DUCK_LEVEL) gDuckFactor = RADIO_DUCK_LEVEL;
            }

            UpdateVolume();
        });

        Events::drawHudEvent.Add([]()
        {
            if (gStationNameToShow.empty()) return;
            if (GetTickCount() - gStationNameTimer > STATION_NAME_DURATION) {
                gStationNameToShow = "";
                return;
            }

            // GTA3_TODO: If pResWidth/pResHeight addresses are wrong above,
            // replace these two lines with:
            //   float resW = (float)RsGlobal.maximumWidth;
            //   float resH = (float)RsGlobal.maximumHeight;
            // (requires #include "RW/RwCore.h" at the top of this file)
            float resW = (float)*pResWidth;
            float resH = (float)*pResHeight;

            float scale = (resW / 1920.0f) * 1.10f;

            // GTA III: CFont::PrintString takes char*, not wchar_t*
            // AsciiToUnicode / wchar_t are VC-only — removed
            const char* name = gStationNameToShow.c_str();

            CFont::SetCentreOff();
            CFont::SetRightJustifyOff();
            CFont::SetJustifyOff();
            // SetBackgroundOff, SetBackGroundOnlyTextOff, SetProportional
            // do not exist in GTA III's CFont — removed

            CFont::SetFontStyle(FONT_HEADING);
            CFont::SetScale(scale, scale * 1.5f);

            float textWidth = CFont::GetStringWidth(name, true);
            float centerX = (resW * 0.5f) - (textWidth * 0.5f);
            float posY = resH * 0.05f;

            CFont::SetColor(CRGBA(0, 0, 0, 200));
            CFont::PrintString(centerX + 2.0f * scale, posY + 2.0f * scale, name);
            CFont::SetColor(CRGBA(255, 255, 255, 255));
            CFont::PrintString(centerX, posY, name);
        });
    }
} noRadioPlugin;
