#include "audio.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <vector>

// Releases the COM pointer when it goes out of scope.
template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T* operator->() const { return p; }
    T** operator&() { return &p; }
};

static DWORD g_cachedPid, g_lastUse;
static float g_level;  // tracked here during a burst so each step doesn't have to ask Windows
static std::wstring g_cachedName;
static std::vector<ISimpleAudioVolume*> g_cached;

static std::wstring exeName(DWORD pid) {
    std::wstring name;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return name;
    wchar_t buf[MAX_PATH];
    DWORD len = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, buf, &len)) {
        name = buf;
        name = name.substr(name.find_last_of(L'\\') + 1);
        for (auto& c : name) c = towlower(c);
    }
    CloseHandle(h);
    return name;
}

// Volume controls for every audio session of the given app, across all output devices.
// Matches by exe name too, since browsers and Electron apps play audio from child processes.
static std::vector<ISimpleAudioVolume*> sessionsFor(DWORD pid, const std::wstring& exe) {
    std::vector<ISimpleAudioVolume*> out;
    Com<IMMDeviceEnumerator> enumerator;
    Com<IMMDeviceCollection> devices;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
        return out;

    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);
    for (UINT d = 0; d < deviceCount; d++) {
        Com<IMMDevice> device;
        Com<IAudioSessionManager2> manager;
        Com<IAudioSessionEnumerator> sessions;
        if (FAILED(devices->Item(d, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, (void**)&manager)) ||
            FAILED(manager->GetSessionEnumerator(&sessions)))
            continue;

        int count = 0;
        sessions->GetCount(&count);
        for (int i = 0; i < count; i++) {
            Com<IAudioSessionControl> control;
            Com<IAudioSessionControl2> control2;
            if (FAILED(sessions->GetSession(i, &control)) ||
                FAILED(control->QueryInterface(IID_PPV_ARGS(&control2))))
                continue;

            DWORD sessionPid = 0;
            control2->GetProcessId(&sessionPid);
            if (sessionPid != pid && exeName(sessionPid) != exe) continue;

            ISimpleAudioVolume* volume = nullptr;
            if (SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&volume)))) out.push_back(volume);
        }
    }
    return out;
}

void releaseAudio() {
    for (auto* v : g_cached) v->Release();
    g_cached.clear();
    g_cachedPid = 0;
}

bool planVolumeChange(float delta, std::wstring& appName, float& level) {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);

    // Sessions are looked up once per burst of presses, which keeps holding a key cheap.
    // A new burst looks again, picking up anything that started playing since.
    if (pid != g_cachedPid || g_cached.empty() || GetTickCount() - g_lastUse > 1500) {
        releaseAudio();
        std::wstring exe = exeName(pid);
        if (exe.empty()) return false;
        g_cachedPid = pid;
        g_cachedName = exe.substr(0, exe.find_last_of(L'.'));
        g_cached = sessionsFor(pid, exe);
        // The first session is the reference so all of the app's sessions end up the same.
        if (!g_cached.empty()) g_cached[0]->GetMasterVolume(&g_level);
    }
    g_lastUse = GetTickCount();
    appName = g_cachedName;
    level = -1;
    if (g_cached.empty()) return true;

    g_level = std::clamp(std::round((g_level + delta) * 100) / 100, 0.0f, 1.0f);
    level = g_level;
    return true;
}

void applyVolumeChange() {
    for (auto* v : g_cached) {
        v->SetMasterVolume(g_level, nullptr);
        v->SetMute(FALSE, nullptr);
    }
}
