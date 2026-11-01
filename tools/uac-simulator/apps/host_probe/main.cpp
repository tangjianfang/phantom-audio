// host_probe —— 主机侧断言 CLI（MMDevice 枚举 / 音量回调 / 设备通知 / WASAPI 采集 Goertzel）
// 命令：
//   enum [--all]                  枚举音频端点（JSON 行）；--all 含所有状态并多一个 "state" 字段（诊断用）
//   session                       当前会话信息（是否远程会话；Sandbox 里是远程会话）
//   volume-watch --seconds N [--endpoint <id>]   IAudioEndpointVolume 回调（JSON 行）
//   notify-watch --seconds N                     设备增删/默认变更（JSON 行）
//   capture --freq F --seconds N [--endpoint <id>] [--loopback] [--tolerance 0.05]
//                 采集并做 Goertzel 校验（默认渲染回环）；退出码 0=检出 1=未检出 2=错误
#include <uac/goertzel.h>
#include <windows.h>
#include <audioclient.h>
#include <combaseapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")

namespace {

struct CoInit {
    HRESULT hr;
    CoInit() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~CoInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

std::string wide_to_utf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

const char* state_name(DWORD s) {
    switch (s) {
    case DEVICE_STATE_ACTIVE: return "active";
    case DEVICE_STATE_DISABLED: return "disabled";
    case DEVICE_STATE_NOTPRESENT: return "notpresent";
    case DEVICE_STATE_UNPLUGGED: return "unplugged";
    default: return "unknown";
    }
}

// all=false: only active endpoints, no "state" field (the format the auto test parses).
// all=true : every endpoint in every state with a "state" field - diagnostics for "the device is bound but no endpoint shows up".
void cmd_enum(bool all) {
    CoInit co;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en)))) {
        printf("{\"error\":\"enumerator\"}\n");
        return;
    }
    for (int flow = 0; flow < 2; ++flow) {
        IMMDeviceCollection* col = nullptr;
        en->EnumAudioEndpoints(flow ? eCapture : eRender, all ? DEVICE_STATEMASK_ALL : DEVICE_STATE_ACTIVE, &col);
        UINT n = 0;
        if (col) col->GetCount(&n);
        for (UINT i = 0; i < n; ++i) {
            IMMDevice* d = nullptr;
            col->Item(i, &d);
            LPWSTR id = nullptr;
            d->GetId(&id);
            IPropertyStore* ps = nullptr;
            d->OpenPropertyStore(STGM_READ, &ps);
            PROPVARIANT pv;
            PropVariantInit(&pv);
            std::string name;
            if (ps && SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)))
                name = wide_to_utf8(pv.pwszVal);
            if (all) {
                DWORD st = 0;
                d->GetState(&st);
                printf("{\"flow\":\"%s\",\"id\":\"%s\",\"name\":\"%s\",\"state\":\"%s\"}\n",
                       flow ? "capture" : "render", wide_to_utf8(id).c_str(), name.c_str(), state_name(st));
            } else {
                printf("{\"flow\":\"%s\",\"id\":\"%s\",\"name\":\"%s\"}\n",
                       flow ? "capture" : "render", wide_to_utf8(id).c_str(), name.c_str());
            }
            if (ps) { PropVariantClear(&pv); ps->Release(); }
            CoTaskMemFree(id);
            d->Release();
        }
        if (col) col->Release();
    }
    en->Release();
}

// Which session are we in? In a remote (RDP-style) session Windows may expose only the redirected audio device and hide
// locally attached ones - Windows Sandbox is such a session. This is what tells "no endpoint because the driver failed"
// apart from "no endpoint because this session cannot see local audio hardware".
void cmd_session() {
    DWORD sid = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &sid);
    printf("{\"sessionId\":%lu,\"consoleSessionId\":%lu,\"remoteSession\":%s}\n", static_cast<unsigned long>(sid),
           static_cast<unsigned long>(WTSGetActiveConsoleSessionId()), GetSystemMetrics(SM_REMOTESESSION) ? "true" : "false");
}

// volume-watch 回调
struct VolNotify : IAudioEndpointVolumeCallback {
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA n) override {
        printf("{\"event\":\"volume\",\"master\":%.3f,\"mute\":%s}\n", n->fMasterVolume,
               n->bMuted ? "true" : "false");
        fflush(stdout);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IAudioEndpointVolumeCallback)) {
            *p = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
};

IMMDevice* pick_device(IMMDeviceEnumerator* en, const std::string& id, bool loopback) {
    if (!id.empty()) {
        wchar_t w[512]{};
        MultiByteToWideChar(CP_UTF8, 0, id.c_str(), -1, w, 512);
        IMMDevice* d = nullptr;
        return SUCCEEDED(en->GetDevice(w, &d)) ? d : nullptr;
    }
    IMMDevice* d = nullptr;
    en->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &d);
    return d;
}

int cmd_volume_watch(int seconds, const std::string& id) {
    CoInit co;
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
    if (!en) return 2;
    IMMDevice* d = pick_device(en, id, false);
    if (!d) { printf("{\"error\":\"no endpoint\"}\n"); return 2; }
    IAudioEndpointVolume* v = nullptr;
    if (FAILED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&v)))) return 2;
    VolNotify notify;
    v->RegisterControlChangeNotify(&notify);
    FLOAT cur = 0; BOOL mute = FALSE;
    v->GetMasterVolumeLevelScalar(&cur);
    v->GetMute(&mute);
    printf("{\"event\":\"initial\",\"master\":%.3f,\"mute\":%s}\n", cur, mute ? "true" : "false");
    fflush(stdout);
    Sleep(seconds * 1000);
    v->UnregisterControlChangeNotify(&notify);
    v->Release();
    d->Release();
    en->Release();
    return 0;
}

// notify-watch：IMMNotificationClient
struct DevNotify : IMMNotificationClient {
    LONG refs = 1;
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override {
        printf("{\"event\":\"added\",\"id\":\"%s\"}\n", wide_to_utf8(id).c_str());
        fflush(stdout);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        printf("{\"event\":\"removed\",\"id\":\"%s\"}\n", wide_to_utf8(id).c_str());
        fflush(stdout);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD st) override {
        printf("{\"event\":\"state\",\"id\":\"%s\",\"state\":%lu}\n", wide_to_utf8(id).c_str(),
               static_cast<unsigned long>(st));
        fflush(stdout);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override {
        if (r == __uuidof(IUnknown) || r == __uuidof(IMMNotificationClient)) {
            *p = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
};

int cmd_notify_watch(int seconds) {
    CoInit co;
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
    if (!en) return 2;
    DevNotify n;
    en->RegisterEndpointNotificationCallback(&n);
    Sleep(seconds * 1000);
    en->UnregisterEndpointNotificationCallback(&n);
    en->Release();
    return 0;
}

int cmd_capture(double freq, int seconds, const std::string& id, bool loopback, double tolerance) {
    CoInit co;
    IMMDeviceEnumerator* en = nullptr;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                     __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
    if (!en) return 2;
    IMMDevice* d = pick_device(en, id, loopback);
    if (!d) { printf("{\"error\":\"no endpoint\"}\n"); return 2; }
    IAudioClient* ac = nullptr;
    if (FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(&ac)))) {
        printf("{\"error\":\"activate\"}\n");
        return 2;
    }
    WAVEFORMATEX* fmt = nullptr;
    REFERENCE_TIME dur = 10000000; // 1s 缓冲
    if (FAILED(ac->GetMixFormat(&fmt))) {
        printf("{\"error\":\"mixformat\"}\n");
        return 2;
    }
    const bool sharedLoopback = loopback;
    if (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, sharedLoopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0,
                              dur, 0, fmt, nullptr))) {
        printf("{\"error\":\"initialize\"}\n");
        return 2;
    }
    IAudioCaptureClient* cap = nullptr;
    if (FAILED(ac->GetService(__uuidof(IAudioCaptureClient),
                              reinterpret_cast<void**>(&cap)))) return 2;
    ac->Start();

    const double rate = fmt->nSamplesPerSec;
    std::vector<float> pcm;
    const auto deadline = GetTickCount64() + static_cast<ULONGLONG>(seconds) * 1000;
    while (GetTickCount64() < deadline) {
        Sleep(100);
        UINT32 packets = 0;
        cap->GetNextPacketSize(&packets);
        while (packets) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                pcm.insert(pcm.end(), frames, 0.0f);
            } else if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                       (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                        reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat.Data1 == 3)) {
                const auto* f = reinterpret_cast<const float*>(data);
                const UINT32 total = frames * fmt->nChannels;
                for (UINT32 i = 0; i < total; i += fmt->nChannels) pcm.push_back(f[i]); // 首声道
            } else if (fmt->wBitsPerSample == 16) {
                const auto* s = reinterpret_cast<const short*>(data);
                const UINT32 total = frames * fmt->nChannels;
                for (UINT32 i = 0; i < total; i += fmt->nChannels)
                    pcm.push_back(s[i] / 32768.0f);
            }
            cap->ReleaseBuffer(frames);
            cap->GetNextPacketSize(&packets);
        }
    }
    ac->Stop();
    cap->Release();
    ac->Release();
    CoTaskMemFree(fmt);
    d->Release();
    en->Release();

    const double p = uac::goertzel_power(pcm, freq, rate);
    printf("{\"samples\":%zu,\"rate\":%.0f,\"freq\":%.1f,\"power\":%.6f}\n", pcm.size(), rate,
           freq, p);
    return p >= tolerance ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("host_probe <enum|volume-watch|notify-watch|capture> [options]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    int seconds = 10;
    double freq = 1000.0, tolerance = 0.05;
    bool loopback = false, all = false;
    std::string endpoint;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--all") all = true;
        else if (a == "--seconds") seconds = atoi(next().c_str());
        else if (a == "--freq") freq = atof(next().c_str());
        else if (a == "--endpoint") endpoint = next();
        else if (a == "--loopback") loopback = true;
        else if (a == "--tolerance") tolerance = atof(next().c_str());
    }
    if (cmd == "enum") { cmd_enum(all); return 0; }
    if (cmd == "session") { cmd_session(); return 0; }
    if (cmd == "volume-watch") return cmd_volume_watch(seconds, endpoint);
    if (cmd == "notify-watch") return cmd_notify_watch(seconds);
    if (cmd == "capture") return cmd_capture(freq, seconds, endpoint, loopback, tolerance);
    printf("unknown command: %s\n", cmd.c_str());
    return 2;
}
