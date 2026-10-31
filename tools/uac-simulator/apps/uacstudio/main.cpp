// uacstudio - Win32 GUI for the UAC simulator test bench (C++20, standard controls + owner-draw key caps)
// Design system: dark instrument panel - bg #171A21 / panel #1F242E / ink #DDE3EA / muted #8B93A1 /
//   amber accent #E8A33D / signal green #4FB573 / fault red #E05252; Segoe UI + Cascadia Mono.
//
// All user-visible text is English: the Windows Sandbox image has no CJK fonts, so Chinese rendered as boxes.
//
// THREADING RULE - the UI thread never waits for anything:
//   A blocking connect() to a loopback port nobody listens on takes ~2.0 s on Windows, so the old code (timers that
//   issued HTTP requests on the UI thread) froze the window whenever uacsimd was not running.
//   - a poller thread fetches /status and /log and PostMessage()s the results;
//   - a request thread executes every button action from a queue (a click only enqueues);
//   - the auto test runs on its own thread; closing the window during a run requests cancellation and the window
//     closes when the run has cleaned up (no join on the UI thread);
//   - the bottom status bar always says what the program is doing and whether uacsimd is reachable.
//   scripts/smoke.ps1 launches the real GUI against a closed port and probes the UI thread with SendMessageTimeout.
//
// Command line:
//   uacstudio.exe [--port N] [--env-check] [--no-env-check] [--start-simulator]
//                                            open the window (--env-check: show the environment check first;
//                                            --no-env-check: never show it, also on a first run - automation
//                                            needs a plain window it can close; --start-simulator: start
//                                            uacsimd.exe next to it, stopped again on exit)
//   uacstudio.exe --selftest                 non-visual self check (per-control pixel statistics + geometry audit +
//                                            controller logic); report in %TEMP%\uacstudio-selftest.txt; exit 0/3/4/5
//   uacstudio.exe --autotest [--port N] [--out DIR] [--exe-dir DIR] [--allow-host] [--profile NAME]
//                                            headless one-key auto test (same case table as the GUI button);
//                                            exit 0 = no failures, 1 = failures
//   uacstudio.exe --install-driver [--payload DIR] [--install-dir DIR]
//   uacstudio.exe --uninstall-driver [--payload DIR] [--install-dir DIR]
//                                            install / remove the usbip-win2 slim payload (see driver_installer.h).
//                                            Needs administrator and a disposable machine (Sandbox/VM); the dev machine
//                                            is refused. Exit 0 ok, 1 failed, 2 refused by the guard, 3 ok but reboot needed
#include "http_client.h"
#include "env_check.h"
#include "auto_test.h"
#include "controller.h"
#include "gui_selfcheck.h"
#include "driver_installer.h"
#include "host_ops.h"
#include <uac/version.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr COLORREF C_BG = RGB(0x17, 0x1A, 0x21);
constexpr COLORREF C_PANEL = RGB(0x1F, 0x24, 0x2E);
constexpr COLORREF C_KEY = RGB(0x27, 0x2D, 0x39);
constexpr COLORREF C_KEY_HOVER = RGB(0x2F, 0x36, 0x44);
constexpr COLORREF C_KEY_DOWN = RGB(0x38, 0x40, 0x4F);
constexpr COLORREF C_KEY_BORDER = RGB(0x3C, 0x44, 0x52);
constexpr COLORREF C_INK = RGB(0xDD, 0xE3, 0xEA);
constexpr COLORREF C_MUTED = RGB(0x8B, 0x93, 0xA1);
constexpr COLORREF C_ACCENT = RGB(0xE8, 0xA3, 0x3D);
constexpr COLORREF C_OK = RGB(0x4F, 0xB5, 0x73);
constexpr COLORREF C_FAULT = RGB(0xE0, 0x52, 0x52);

constexpr uint16_t kDefaultPort = 3241;
uint16_t g_port = kDefaultPort;

HBRUSH g_brBg = nullptr, g_brPanel = nullptr;
HFONT g_fUi = nullptr, g_fTitle = nullptr, g_fMono = nullptr;

struct Ctl {
    HWND wnd = nullptr;
    void create(HWND parent, int id, const wchar_t* cls, const wchar_t* text, DWORD style,
                DWORD exStyle, HINSTANCE hi, HFONT f) {
        wnd = CreateWindowExW(exStyle, cls, text, style, 0, 0, 10, 10, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
        SendMessageW(wnd, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
    }
};

// Control IDs (the ids of the fixed-body one-key buttons live in the Actions() table of controller.cpp)
enum : int {
    ID_PROFILE = 100, ID_ATTACH = 101, ID_DETACH = 102,
    ID_LOG = 300,
    ID_HZ = 400, ID_SINE = 401, ID_SILENCE = 402, ID_CAPEDIT = 403, ID_CAPSTART = 404, ID_CAPSTOP = 405,
    ID_AUTOTEST = 500, ID_STATUS = 906, ID_ENV = 910,
};

Ctl g_led, g_title, g_sub;
Ctl g_profileCombo, g_attachBtn, g_detachBtn, g_envBtn;
Ctl g_lblInject;
std::vector<Ctl> g_actionBtns; // one per uacstudio::Actions() entry
Ctl g_lblStream, g_hzEdit, g_btnSine, g_btnSilence, g_capEdit, g_btnCapStart, g_btnCapStop;
Ctl g_lblLog, g_logEdit, g_btnAutoTest, g_status;

HWND g_main = nullptr;
int g_ledState = 0; // 0 offline, 1 listening, 2 imported
bool g_selfTest = false;
int g_selfTestExit = 0;

// ---- messages posted by background threads (payloads are heap objects; the receiver owns and frees them) ----
constexpr UINT WM_APP_TESTLINE = WM_APP + 1; // drain g_lines into the log; wParam 1 = the auto test finished
constexpr UINT WM_APP_STATUS = WM_APP + 2;   // wParam 1 = uacsimd reachable; lParam = std::string* /status body
constexpr UINT WM_APP_LOGTEXT = WM_APP + 3;  // lParam = std::string* /log body
constexpr UINT WM_APP_ACTIVITY = WM_APP + 4; // something changed that the status bar shows
constexpr UINT WM_APP_EXITCLEAN = WM_APP + 5; // the exit clean-up finished; wParam 1 = ok
constexpr UINT WM_APP_SIMSTARTED = WM_APP + 6; // --start-simulator finished; wParam 1 = ok
constexpr UINT_PTR kTimerSelfTest = 99;

// ---- auto test ----
std::atomic<bool> g_testRunning{false};
std::atomic<bool> g_testCancel{false};
bool g_closing = false;           // UI thread only: close requested while the auto test is running
bool g_exitCleaning = false;      // UI thread only: the simulator this window started is being stopped before the window closes
std::thread g_exitThread{};
std::thread g_simStartThread{};
std::atomic<bool> g_simStarting{false}; // --start-simulator: uacsimd.exe is being started
bool g_exitAfterStart = false;          // UI thread only: close requested while it is being started
std::thread g_testThread{};
std::string g_testProgress;       // UI thread only: "[7/19] api.capture"
std::mutex g_lineMtx;
std::vector<std::string> g_lines;

// ---- network: poller + request queue ----
enum class Conn { Connecting, Online, Offline };
Conn g_conn = Conn::Connecting;   // UI thread only
std::atomic<bool> g_stop{false};
HANDLE g_pollWake = nullptr;      // auto-reset event: "refresh now"
std::thread g_poller;

struct Req {
    std::string path, body, what;
};
std::mutex g_reqMtx;
std::condition_variable g_reqCv;
std::deque<Req> g_reqs;           // guarded by g_reqMtx
std::string g_activeWhat;         // guarded by g_reqMtx: the request being sent right now
std::thread g_reqThread;

// ---------------------------------------------------------------- command line

std::vector<std::wstring> g_args;

void ParseArgs() {
    int n = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; argv && i < n; ++i) g_args.emplace_back(argv[i]);
    if (argv) LocalFree(argv);
}
bool HasArg(const wchar_t* name) {
    return std::find(g_args.begin(), g_args.end(), name) != g_args.end();
}
std::wstring ArgValue(const wchar_t* name) {
    for (std::size_t i = 0; i + 1 < g_args.size(); ++i)
        if (g_args[i] == name) return g_args[i + 1];
    return {};
}

std::string Narrow(const std::wstring& w) { // UTF-16 -> UTF-8
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Widen(const std::string& s) { // UTF-8 -> UTF-16
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// ---------------------------------------------------------------- drawing / layout

int Scale(HWND hwnd, int logical) {
    return MulDiv(logical, static_cast<int>(GetDpiForWindow(hwnd)), 96);
}

void DrawKey(LPDRAWITEMSTRUCT dis, const wchar_t* text, uacstudio::Tone tone) {
    using uacstudio::Tone;
    const RECT& r = dis->rcItem;
    const bool hover = (dis->itemState & ODS_HOTLIGHT) != 0;
    const bool down = (dis->itemState & ODS_SELECTED) != 0;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    COLORREF face = down ? C_KEY_DOWN : hover ? C_KEY_HOVER : C_KEY;
    COLORREF border = C_KEY_BORDER;
    COLORREF ink = C_INK;
    switch (tone) {
    case Tone::Accent: border = C_ACCENT; break;
    case Tone::Fault: border = C_FAULT; break;
    case Tone::Quiet: ink = C_MUTED; break;
    default: break;
    }
    if (disabled) { ink = C_MUTED; border = C_KEY_BORDER; }
    HBRUSH b = CreateSolidBrush(face);
    FillRect(dis->hDC, &r, b);
    DeleteObject(b);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HPEN oldPen = static_cast<HPEN>(SelectObject(dis->hDC, pen));
    HBRUSH oldBr = static_cast<HBRUSH>(SelectObject(dis->hDC, GetStockObject(NULL_BRUSH)));
    Rectangle(dis->hDC, r.left, r.top, r.right, r.bottom);
    SelectObject(dis->hDC, oldPen);
    SelectObject(dis->hDC, oldBr);
    DeleteObject(pen);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, ink);
    HFONT oldF = static_cast<HFONT>(SelectObject(dis->hDC, g_fUi));
    RECT tr = r;
    DrawTextW(dis->hDC, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis->hDC, oldF);
}

int LayoutSections(int W) {
    const int m = Scale(g_main, 12);
    int y = m;
    const int barH = Scale(g_main, 44);
    MoveWindow(g_led.wnd, m + Scale(g_main, 10), y + Scale(g_main, 12), Scale(g_main, 18),
               Scale(g_main, 18), TRUE);
    // The title / subtitle labels must not reach past the combo box: a STATIC fills its background with an opaque
    // brush, so a fixed-width label covered the left half of the combo below ~852 px window width.
    const int comboLeft = W - m - Scale(g_main, 452);
    const int labelX = m + Scale(g_main, 36);
    const int labelRoom = std::max(comboLeft - Scale(g_main, 8) - labelX, Scale(g_main, 40));
    MoveWindow(g_title.wnd, labelX, y + Scale(g_main, 5), std::min(Scale(g_main, 340), labelRoom),
               Scale(g_main, 22), TRUE);
    MoveWindow(g_sub.wnd, labelX, y + Scale(g_main, 27), std::min(Scale(g_main, 400), labelRoom),
               Scale(g_main, 16), TRUE);
    MoveWindow(g_profileCombo.wnd, comboLeft, y + Scale(g_main, 9),
               Scale(g_main, 158), Scale(g_main, 200), TRUE);
    MoveWindow(g_envBtn.wnd, W - m - Scale(g_main, 284), y + Scale(g_main, 8),
               Scale(g_main, 96), Scale(g_main, 28), TRUE);
    MoveWindow(g_attachBtn.wnd, W - m - Scale(g_main, 182), y + Scale(g_main, 8),
               Scale(g_main, 86), Scale(g_main, 28), TRUE);
    MoveWindow(g_detachBtn.wnd, W - m - Scale(g_main, 90), y + Scale(g_main, 8),
               Scale(g_main, 86), Scale(g_main, 28), TRUE);
    y += barH + Scale(g_main, 16);
    MoveWindow(g_lblInject.wnd, m, y, Scale(g_main, 420), Scale(g_main, 18), TRUE);
    y += Scale(g_main, 24);
    const int keyW = Scale(g_main, 88), keyH = Scale(g_main, 28), gap = Scale(g_main, 6);
    // The button grid is driven by the Actions() table: the first kActionsInFirstRow in row one, the rest in row two
    for (std::size_t i = 0; i < g_actionBtns.size(); ++i) {
        const bool first = i < static_cast<std::size_t>(uacstudio::kActionsInFirstRow);
        const int col = static_cast<int>(first ? i : i - uacstudio::kActionsInFirstRow);
        MoveWindow(g_actionBtns[i].wnd, m + col * (keyW + gap), y + (first ? 0 : keyH + gap), keyW,
                   keyH, TRUE);
    }
    y += 2 * keyH + gap + Scale(g_main, 16);
    MoveWindow(g_lblStream.wnd, m, y, Scale(g_main, 420), Scale(g_main, 18), TRUE);
    y += Scale(g_main, 24);
    MoveWindow(g_hzEdit.wnd, m, y, Scale(g_main, 64), Scale(g_main, 26), TRUE);
    MoveWindow(g_btnSine.wnd, m + Scale(g_main, 68), y, keyW, keyH, TRUE);
    MoveWindow(g_btnSilence.wnd, m + Scale(g_main, 68) + keyW + gap, y, keyW, keyH, TRUE);
    MoveWindow(g_btnCapStart.wnd, W - m - 2 * (keyW + gap), y, keyW, keyH, TRUE);
    MoveWindow(g_btnCapStop.wnd, W - m - (keyW + gap), y, keyW, keyH, TRUE);
    const int capX = m + Scale(g_main, 68) + 2 * (keyW + gap);
    MoveWindow(g_capEdit.wnd, capX, y,
               std::max(W - m - capX - 2 * (keyW + gap), Scale(g_main, 80)), Scale(g_main, 26),
               TRUE);
    y += keyH + Scale(g_main, 16);
    MoveWindow(g_lblLog.wnd, m, y, Scale(g_main, 420), Scale(g_main, 18), TRUE);
    y += Scale(g_main, 24);
    return y;
}

void DoLayout() {
    if (!g_main) return;
    RECT rc{};
    GetClientRect(g_main, &rc);
    const int y = LayoutSections(rc.right);
    const int m = Scale(g_main, 12);
    const int statusH = Scale(g_main, 20);
    const int logBottom = rc.bottom - m - statusH - Scale(g_main, 6);
    MoveWindow(g_logEdit.wnd, m, y, rc.right - 2 * m, std::max(logBottom - y, Scale(g_main, 60)), TRUE);
    MoveWindow(g_status.wnd, m, rc.bottom - m - statusH, rc.right - 2 * m, statusH, TRUE);
    InvalidateRect(g_main, nullptr, TRUE);
}

// ---------------------------------------------------------------- log / status (UI thread only)

void AppendLog(const std::string& utf8) {
    const std::wstring w = Widen(utf8) + L"\r\n";
    const int len = GetWindowTextLengthW(g_logEdit.wnd);
    SendMessageW(g_logEdit.wnd, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
    SendMessageW(g_logEdit.wnd, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(w.c_str()));
}

std::string CurrentProfile() {
    wchar_t buf[128]{};
    GetWindowTextW(g_profileCombo.wnd, buf, 128);
    return Narrow(buf);
}

std::string EditText(const Ctl& c) {
    wchar_t buf[MAX_PATH]{};
    GetWindowTextW(c.wnd, buf, MAX_PATH);
    return Narrow(buf);
}

void SetWindowTextIfChanged(HWND w, const std::wstring& text) {
    wchar_t cur[512]{};
    GetWindowTextW(w, cur, 512);
    if (text != cur) SetWindowTextW(w, text.c_str());
}

void SetLed(int state, const std::wstring& sub) {
    if (g_ledState != state) {
        g_ledState = state;
        InvalidateRect(g_led.wnd, nullptr, TRUE);
    }
    SetWindowTextIfChanged(g_sub.wnd, sub);
}

// The bottom status bar: what the program is doing right now + whether uacsimd is reachable.
void UpdateStatusBar() {
    if (!g_status.wnd) return;
    std::wstring activity;
    if (g_simStarting.load()) {
        activity = L"Starting the simulator (uacsimd.exe) ...";
    } else if (g_exitCleaning) {
        activity = L"Exiting: detaching from Windows and stopping the simulator this window started - the window closes when done";
    } else if (g_closing) {
        activity = L"Stopping the auto test (finishing the current case and cleaning up) - the window closes when done";
    } else if (g_testRunning.load()) {
        activity = g_testProgress.empty() ? L"Auto test starting ..." : L"Auto test running " + Widen(g_testProgress);
    } else {
        std::string active;
        std::size_t queued = 0;
        {
            std::lock_guard lk(g_reqMtx);
            active = g_activeWhat;
            queued = g_reqs.size();
        }
        if (!active.empty()) {
            activity = L"Sending: " + Widen(active) + L" ...";
            if (queued) activity += L" (+" + std::to_wstring(queued) + L" queued)";
        } else if (queued) {
            activity = L"Sending " + std::to_wstring(queued) + L" queued request(s) ...";
        } else {
            activity = L"Ready";
        }
    }
    const std::wstring where = L"127.0.0.1:" + std::to_wstring(g_port);
    std::wstring net;
    switch (g_conn) {
    case Conn::Connecting: net = L"uacsimd: connecting to " + where + L" ..."; break;
    case Conn::Online: net = L"uacsimd: connected (" + where + L")"; break;
    case Conn::Offline: net = L"uacsimd: NOT reachable on " + where + L" - use Environment > Start simulator (retrying)"; break;
    }
    SetWindowTextIfChanged(g_status.wnd, activity + L"    |    " + net);
    InvalidateRect(g_status.wnd, nullptr, TRUE); // the colour depends on g_conn
}

// Close the main window. A simulator that THIS window started must not outlive it (it would keep the ports and the virtual
// device): it is stopped first - after detaching its devices from Windows when this machine allows that - on a worker thread,
// and the window closes when that has reported back. A simulator somebody else started is left alone.
// (uacsimd also watches this process via --parent-pid, so even a crash cannot leave it behind.)
void BeginExit(HWND h) {
    if (g_exitCleaning) return; // already on its way
    if (g_simStarting.load()) { // the simulator is still being started: wait for that, then clean it up like any other
        g_exitAfterStart = true;
        UpdateStatusBar();
        return;
    }
    if (uacstudio::TheSimulator().Owns()) {
        g_exitCleaning = true;
        UpdateStatusBar();
        const std::uint16_t port = g_port;
        g_exitThread = std::thread([h, port] {
            const uacstudio::InstallOptions opt;
            const auto r = uacstudio::StopSimulatorOp(port, 3240, opt);
            PostMessageW(h, WM_APP_EXITCLEAN, r.ok ? 1 : 0, 0);
        });
        return;
    }
    DestroyWindow(h);
}

void ApplyStatus(bool reachable, const std::string& body) {
    g_conn = reachable ? Conn::Online : Conn::Offline;
    if (!reachable) {
        SetLed(0, L"uacsimd not connected - 127.0.0.1:" + std::to_wstring(g_port));
    } else {
        const auto view = uacstudio::DescribeStatus(body); // names the profile; says so when several instances are attached
        SetLed(view.led, view.text);
    }
    UpdateStatusBar();
}

std::string g_lastLog; // UI thread only
void ApplyLogText(const std::string& body) {
    if (g_testRunning.load()) return; // the auto test output owns the log panel
    if (body.size() < 3 || body.size() > 60000 || body == g_lastLog) return;
    g_lastLog = body;
    SetWindowTextW(g_logEdit.wnd, Widen(body).c_str());
    SendMessageW(g_logEdit.wnd, EM_SETSEL, body.size() - 1, body.size() - 1);
}

void SetEnabled(bool on) { EnableWindow(g_main, on ? TRUE : FALSE); }

// ---------------------------------------------------------------- background threads

// Post a heap string to the window; if the window is gone nobody would free it, so free it here.
void PostString(HWND h, UINT msg, WPARAM wp, std::string s) {
    auto* p = new std::string(std::move(s));
    if (!PostMessageW(h, msg, wp, reinterpret_cast<LPARAM>(p))) delete p;
}

void PushLine(HWND h, const std::string& line) {
    {
        std::lock_guard lk(g_lineMtx);
        g_lines.push_back(line);
    }
    PostMessageW(h, WM_APP_TESTLINE, 0, 0);
}

void PokePoller() {
    if (g_pollWake) SetEvent(g_pollWake);
}

// Polls /status and /log. With uacsimd down every round costs at most the connect deadline (300 ms) and then waits
// 500 ms; the UI thread is never involved.
void PollerMain(HWND h) {
    while (!g_stop.load()) {
        const std::string st = uacstudio::http_req(g_port, "GET", "/status", "");
        PostString(h, WM_APP_STATUS, st.empty() ? 0 : 1, st);
        if (!st.empty() && !g_testRunning.load()) {
            const std::string log = uacstudio::http_req(g_port, "GET", "/log?n=200", "");
            if (!log.empty()) PostString(h, WM_APP_LOGTEXT, 0, log);
        }
        WaitForSingleObject(g_pollWake, 500); // woken early by PokePoller()
    }
}

// Executes the button actions one after the other, off the UI thread.
void RequestMain(HWND h) {
    for (;;) {
        Req r;
        {
            std::unique_lock lk(g_reqMtx);
            g_reqCv.wait(lk, [] { return g_stop.load() || !g_reqs.empty(); });
            if (g_stop.load()) return;
            r = std::move(g_reqs.front());
            g_reqs.pop_front();
            g_activeWhat = r.what;
        }
        PostMessageW(h, WM_APP_ACTIVITY, 0, 0);
        std::string out;
        const bool ok = uacstudio::http_post(g_port, r.path, r.body, &out);
        {
            std::lock_guard lk(g_reqMtx);
            g_activeWhat.clear();
        }
        PushLine(h, "[" + r.what + "] " + (ok ? out : "request failed - is uacsimd running?"));
        PostMessageW(h, WM_APP_ACTIVITY, 0, 0);
        PokePoller(); // the device state probably changed: refresh the LED now instead of in half a second
    }
}

// A click only enqueues; the result comes back as a log line.
void Post(const std::string& path, const std::string& body, const std::string& what) {
    {
        std::lock_guard lk(g_reqMtx);
        g_reqs.push_back({path, body, what});
    }
    g_reqCv.notify_one();
    UpdateStatusBar();
}

void StartThreads(HWND h) {
    g_pollWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_poller = std::thread([h] { PollerMain(h); });
    g_reqThread = std::thread([h] { RequestMain(h); });
}

void StopThreads() {
    g_stop.store(true);
    {
        std::lock_guard lk(g_reqMtx);
        g_reqs.clear(); // do not send queued actions while shutting down
    }
    g_reqCv.notify_all();
    PokePoller();
    // Bounded: the poller is at most inside one http_req (300 ms connect + 2 s I/O deadline); the window is already gone
    if (g_poller.joinable()) g_poller.join();
    if (g_reqThread.joinable()) g_reqThread.join();
    if (g_pollWake) {
        CloseHandle(g_pollWake);
        g_pollWake = nullptr;
    }
}

std::wstring WindowTitle() {
    std::wstring t = L"uacstudio ";
    for (const char* p = uac::kVersion; *p; ++p) t.push_back(static_cast<wchar_t>(*p));
    // The build time is in the title so logs / result files people send back can be matched to a build
    t += L" (build ";
    for (const char* p = __DATE__ " " __TIME__; *p; ++p) t.push_back(static_cast<wchar_t>(*p));
    t += L") - UAC Simulator Test Bench";
    return t;
}

// ---------------------------------------------------------------- non-visual self check

void FinishSelfTest(HWND h) {
    std::vector<int> ids = {ID_PROFILE, ID_ATTACH, ID_DETACH, ID_LOG, ID_HZ, ID_SINE, ID_SILENCE,
                            ID_CAPEDIT, ID_CAPSTART, ID_CAPSTOP, ID_AUTOTEST, ID_STATUS, ID_ENV};
    const auto res = uacstudio::RunGuiSelfCheck(h, ids);
    wchar_t tmp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"uacstudio-selftest.txt";
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f << Narrow(res.report);
    }
    g_selfTestExit = res.exitCode;
    DestroyWindow(h); // WM_DESTROY ends the message loop with g_selfTestExit
}

// ---------------------------------------------------------------- window procedure

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        const DWORD vis = WS_CHILD | WS_VISIBLE;
        const DWORD key = vis | BS_OWNERDRAW | WS_TABSTOP;
        g_brBg = CreateSolidBrush(C_BG);
        g_brPanel = CreateSolidBrush(C_PANEL);
        g_fUi = CreateFontW(-Scale(h, 14), 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fTitle = CreateFontW(-Scale(h, 22), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        g_fMono = CreateFontW(-Scale(h, 13), 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Cascadia Mono");
        g_led.create(h, 900, L"STATIC", nullptr, vis | SS_OWNERDRAW, 0, cs->hInstance, g_fUi);
        // SS_ENDELLIPSIS: when the window is narrow and the label is shortened, end with "..." instead of cutting
        g_title.create(h, 901, L"STATIC", L"uacstudio", vis | SS_ENDELLIPSIS, 0, cs->hInstance, g_fTitle);
        g_sub.create(h, 902, L"STATIC", L"Connecting to uacsimd ...", vis | SS_ENDELLIPSIS, 0, cs->hInstance, g_fUi);
        g_profileCombo.create(h, ID_PROFILE, L"COMBOBOX", nullptr, vis | CBS_DROPDOWNLIST | WS_VSCROLL,
                              0, cs->hInstance, g_fUi);
        for (const auto& p : uacstudio::ProfileNames())
            SendMessageW(g_profileCombo.wnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(p.c_str()));
        SendMessageW(g_profileCombo.wnd, CB_SETCURSEL, 0, 0);
        g_attachBtn.create(h, ID_ATTACH, L"BUTTON", L"Attach", key, 0, cs->hInstance, g_fUi);
        g_detachBtn.create(h, ID_DETACH, L"BUTTON", L"Detach", key, 0, cs->hInstance, g_fUi);
        g_envBtn.create(h, ID_ENV, L"BUTTON", L"Environment", key, 0, cs->hInstance, g_fUi);
        g_lblInject.create(h, 903, L"STATIC", L"Device events and fault injection", vis, 0, cs->hInstance,
                           g_fUi);
        for (const auto& a : uacstudio::Actions()) {
            Ctl c;
            c.create(h, a.id, L"BUTTON", a.label, key, 0, cs->hInstance, g_fUi);
            g_actionBtns.push_back(c);
        }
        g_btnAutoTest.create(h, ID_AUTOTEST, L"BUTTON", L"Auto test", key, 0, cs->hInstance, g_fUi);
        g_lblStream.create(h, 904, L"STATIC", L"Audio stream (microphone source / speaker capture)", vis, 0,
                           cs->hInstance, g_fUi);
        g_hzEdit.create(h, ID_HZ, L"EDIT", L"1000", vis | ES_AUTOHSCROLL | WS_BORDER,
                        WS_EX_CLIENTEDGE, cs->hInstance, g_fMono);
        g_btnSine.create(h, ID_SINE, L"BUTTON", L"Sine", key, 0, cs->hInstance, g_fUi);
        g_btnSilence.create(h, ID_SILENCE, L"BUTTON", L"Silence", key, 0, cs->hInstance, g_fUi);
        g_capEdit.create(h, ID_CAPEDIT, L"EDIT", L"capture.wav", vis | ES_AUTOHSCROLL | WS_BORDER,
                         WS_EX_CLIENTEDGE, cs->hInstance, g_fMono);
        g_btnCapStart.create(h, ID_CAPSTART, L"BUTTON", L"Record", key, 0, cs->hInstance, g_fUi);
        g_btnCapStop.create(h, ID_CAPSTOP, L"BUTTON", L"Stop rec", key, 0, cs->hInstance, g_fUi);
        g_lblLog.create(h, 905, L"STATIC", L"Request log (NDJSON, refreshed every 0.5 s)", vis, 0,
                        cs->hInstance, g_fUi);
        g_logEdit.create(h, ID_LOG, L"EDIT", nullptr,
                         // 不换行：EDIT 的自动换行让每次 SetWindowTextW 都做全文 ScriptItemize
                         // 布局，风暴期间 0.5s 一整块 200 行长 JSON 直接把 UI 线程烧满（真机
                         // 2026-11-01 cdb 栈：SetWindowTextW→ECSetText→MLBuildchLines→ECWord）。
                         // NDJSON 行本来就按行读，横向滚动即可。
                         vis | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                             ES_AUTOHSCROLL | ES_READONLY,
                         WS_EX_CLIENTEDGE, cs->hInstance, g_fMono);
        g_status.create(h, ID_STATUS, L"STATIC", L"", vis | SS_ENDELLIPSIS | SS_LEFTNOWORDWRAP, 0,
                        cs->hInstance, g_fUi);
        if (g_selfTest) SetTimer(h, kTimerSelfTest, 1200, nullptr); // wait for the first layout and paint
        DoLayout();
        UpdateStatusBar();
        StartThreads(h); // no network on the UI thread: the first status arrives as WM_APP_STATUS
        return 0;
    }
    case WM_SIZE:
        DoLayout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {Scale(h, 720), Scale(h, 480)};
        return 0;
    }
    case WM_TIMER:
        if (wp == kTimerSelfTest) {
            KillTimer(h, kTimerSelfTest);
            FinishSelfTest(h);
        }
        return 0;
    case WM_APP_STATUS: {
        std::unique_ptr<std::string> body(reinterpret_cast<std::string*>(lp));
        ApplyStatus(wp == 1, body ? *body : std::string());
        return 0;
    }
    case WM_APP_LOGTEXT: {
        std::unique_ptr<std::string> body(reinterpret_cast<std::string*>(lp));
        if (body) ApplyLogText(*body);
        return 0;
    }
    case WM_APP_SIMSTARTED:
        if (g_simStartThread.joinable()) g_simStartThread.join(); // it has just posted this message: returns at once
        g_simStarting.store(false);
        PokePoller(); // look for the new simulator now instead of at the next poll
        UpdateStatusBar();
        if (g_exitAfterStart) {
            g_exitAfterStart = false;
            BeginExit(h);
        }
        return 0;
    case WM_APP_EXITCLEAN:
        if (g_exitThread.joinable()) g_exitThread.join(); // it has just posted this message: returns at once
        g_exitCleaning = false;
        DestroyWindow(h);
        return 0;
    case WM_APP_ACTIVITY:
        UpdateStatusBar();
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: { // the drop-down list goes through LISTBOX, the edit part through EDIT
        auto dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, C_PANEL);
        SetTextColor(dc, C_INK);
        return reinterpret_cast<LRESULT>(g_brPanel);
    }
    case WM_CTLCOLORSTATIC: {
        auto dc = reinterpret_cast<HDC>(wp);
        auto ctrl = reinterpret_cast<HWND>(lp);
        SetBkColor(dc, C_BG);
        COLORREF ink = C_INK;
        if (ctrl == g_sub.wnd || ctrl == g_lblInject.wnd || ctrl == g_lblStream.wnd || ctrl == g_lblLog.wnd)
            ink = C_MUTED;
        if (ctrl == g_status.wnd) // amber while connecting, red when uacsimd cannot be reached
            ink = g_conn == Conn::Online ? C_MUTED : g_conn == Conn::Connecting ? C_ACCENT : C_FAULT;
        SetTextColor(dc, ink);
        return reinterpret_cast<LRESULT>(g_brBg);
    }
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lp);
        if (dis->CtlID == 900) { // status LED: grey = offline, amber = listening, green = imported
            const COLORREF c = g_ledState == 2 ? C_OK : g_ledState == 1 ? C_ACCENT : C_KEY_BORDER;
            HBRUSH b = CreateSolidBrush(c);
            const int cx = (dis->rcItem.left + dis->rcItem.right) / 2;
            const int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            const int r = Scale(h, 5);
            auto old = static_cast<HBRUSH>(SelectObject(dis->hDC, b));
            HPEN pn = static_cast<HPEN>(SelectObject(dis->hDC, GetStockObject(NULL_PEN)));
            Ellipse(dis->hDC, cx - r, cy - r, cx + r, cy + r);
            SelectObject(dis->hDC, pn);
            SelectObject(dis->hDC, old);
            DeleteObject(b);
            return TRUE;
        }
        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, 64);
        uacstudio::Tone tone = uacstudio::Tone::Neutral;
        if (const auto* a = uacstudio::FindAction(static_cast<int>(dis->CtlID))) {
            tone = a->tone;
        } else {
            switch (dis->CtlID) {
            case ID_ATTACH:
            case ID_AUTOTEST: tone = uacstudio::Tone::Accent; break;
            case ID_SILENCE:
            case ID_CAPSTOP: tone = uacstudio::Tone::Quiet; break;
            default: break;
            }
        }
        DrawKey(dis, text, tone);
        return TRUE;
    }
    case WM_APP_TESTLINE: {
        std::vector<std::string> drain;
        {
            std::lock_guard lk(g_lineMtx);
            drain.swap(g_lines);
        }
        for (auto& l : drain) {
            AppendLog(l);
            // The case runner logs "== [i/n] id - title" before each case: show it in the status bar
            if (l.rfind("== [", 0) == 0) {
                const auto dash = l.find(" - ");
                g_testProgress = l.substr(3, dash == std::string::npos ? std::string::npos : dash - 3);
            }
        }
        if (wp == 1) { // the case table is done (the worker thread has posted its last message)
            if (g_testThread.joinable()) g_testThread.join(); // returns at once: the thread is finishing
            g_testRunning.store(false);
            g_testCancel.store(false);
            g_testProgress.clear();
            EnableWindow(g_btnAutoTest.wnd, TRUE);
            PokePoller();
            if (g_closing) { // the user closed the window while the test was running: finish closing now
                BeginExit(h);
                return 0;
            }
        }
        UpdateStatusBar();
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (const auto* a = uacstudio::FindAction(id)) { // table-driven one-key buttons
            Post(a->path, a->body, a->what);
            return 0;
        }
        switch (id) {
        case ID_ATTACH: // switches to the selected profile: the request queue runs the steps in order (detach, attach)
            for (const auto& step : uacstudio::BuildAttachPlan(CurrentProfile())) Post(step.path, step.body, step.what);
            break;
        case ID_DETACH: Post("/detach", "", "Detach"); break;
        case ID_AUTOTEST: {
            if (g_testRunning.exchange(true)) {
                AppendLog("The previous auto test is still running");
                break;
            }
            g_testCancel.store(false);
            g_testProgress.clear();
            EnableWindow(g_btnAutoTest.wnd, FALSE);
            AppendLog("==== Auto test started (worker thread; the result file path is printed at the end) ====");
            UpdateStatusBar();
            g_testThread = std::thread([h] {
                uacstudio::AutoTestOptions o;
                o.port = g_port;
                // host.* cases attach a virtual USB sound card to this Windows. The one-click button must run
                // them whenever the machine allows it (elevated + disposable/UACSIM_FORCE_HOST_TESTS=1) -
                // IsDisposableMachine() alone ignored the env override, so the dev machine never ran host.*
                // from the GUI even when everything was in place. HostCasesAllowed also checks elevation
                // (usbip attach needs admin); when it declines, the cases SKIP with the reason.
                std::string hostWhy;
                o.allowHost = uacstudio::HostCasesAllowed(true, &hostWhy);
                PushLine(h, std::string("host cases: ") + (o.allowHost ? "enabled" : "skipped - " + hostWhy));
                o.cancel = &g_testCancel; // checked between cases; the cleanup case still runs
                o.log = [h](const std::string& line) { PushLine(h, line); };
                const auto out = uacstudio::RunAutoTest(o);
                PushLine(h, "AUTO TEST RESULT: " + std::to_string(out.passed) + " passed, " +
                                std::to_string(out.failed) + " failed, " +
                                std::to_string(out.skipped) + " skipped");
                PushLine(h, "Result file: " + out.reportPath);
                PostMessageW(h, WM_APP_TESTLINE, 1, 0); // 1 = finished
            });
            break;
        }
        case ID_ENV:
            SetEnabled(false);
            uacstudio::ShowEnvCheck(h, g_port); // modal, but it pumps messages and never blocks on the network
            SetEnabled(true);
            PokePoller();
            break;
        case ID_SINE: {
            const std::string body = uacstudio::BuildSineBody(EditText(g_hzEdit));
            if (body.empty()) AppendLog("[Sine] invalid frequency (a number between 0 and 24000 is expected)");
            else Post("/source", body, "Sine");
            break;
        }
        case ID_SILENCE: Post("/source", R"({"kind":"silence"})", "Silence source"); break;
        case ID_CAPSTART: {
            const std::string path = EditText(g_capEdit);
            if (path.empty()) AppendLog("[Record] enter a .wav path first");
            else Post("/capture", uacstudio::BuildCaptureBody(path), "Record");
            break;
        }
        case ID_CAPSTOP: Post("/capture", uacstudio::BuildCaptureBody(""), "Stop recording"); break;
        default: break;
        }
        return 0;
    }
    case WM_ERASEBKGND: {
        auto dc = reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, g_brBg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        const int m = Scale(h, 12);
        RECT bar{m, m, rc.right - m, m + Scale(h, 44)};
        FillRect(dc, &bar, g_brPanel); // the device status bar background (the only panel-style element)
        EndPaint(h, &ps);
        return 0;
    }
    case WM_CLOSE:
        // Never wait for the auto test on the UI thread (it can run for minutes): ask it to stop - it finishes the
        // current case and runs its cleanup - and close the window when it reports back (see WM_APP_TESTLINE).
        if (g_testRunning.load()) {
            g_closing = true;
            g_testCancel.store(true);
            EnableWindow(g_btnAutoTest.wnd, FALSE);
            UpdateStatusBar();
            return 0;
        }
        BeginExit(h);
        return 0;
    case WM_DESTROY:
        g_testCancel.store(true);
        StopThreads();
        if (g_exitThread.joinable()) g_exitThread.join();
        if (g_simStartThread.joinable()) g_simStartThread.join(); // BeginExit waits for the start to finish, so this is quick
        if (g_testThread.joinable()) g_testThread.join(); // only reachable here if the test has already finished
        PostQuitMessage(g_selfTest ? g_selfTestExit : 0);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

// The GUI subsystem has no console: when started from one, attach to the parent console to print progress
void AttachParentConsole() {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
    }
}

std::string SelfExeDir() {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring w(p);
    return Narrow(w.substr(0, w.find_last_of(L"\\/")));
}

// Install / remove the usbip-win2 slim payload. Exit: 0 ok; 1 failed; 2 refused by the guard (nothing touched);
// 3 ok but a reboot is required. Disposable machines only (guard = HostCasesAllowed); needs administrator.
int RunCliDriver(bool install) {
    AttachParentConsole();
    uacstudio::InstallOptions o;
    o.payloadRoot = Narrow(ArgValue(L"--payload"));
    if (o.payloadRoot.empty()) o.payloadRoot = SelfExeDir() + "\\usbip-payload";
    o.installDir = Narrow(ArgValue(L"--install-dir"));
    uacstudio::InstallDeps deps;
    deps.log = [](const std::string& line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    const auto r = install ? uacstudio::InstallDriver(o, deps) : uacstudio::UninstallDriver(o, deps);
    std::printf("\n%s", r.Summary().c_str());
    return r.refused ? 2 : !r.ok ? 1 : r.rebootRequired ? 3 : 0;
}

// Headless one-key auto test.
int RunCliAutoTest() {
    AttachParentConsole();
    uacstudio::AutoTestOptions o;
    if (const auto p = ArgValue(L"--port"); !p.empty()) o.port = static_cast<uint16_t>(_wtoi(p.c_str()));
    else o.port = g_port;
    o.reportDir = Narrow(ArgValue(L"--out"));
    o.exeDir = Narrow(ArgValue(L"--exe-dir"));
    o.allowHost = HasArg(L"--allow-host");
    if (const auto prof = ArgValue(L"--profile"); !prof.empty()) o.hostProfile = Narrow(prof);
    o.log = [](const std::string& line) {
        std::printf("%s\n", line.c_str());
        std::fflush(stdout);
    };
    const auto out = uacstudio::RunAutoTest(o);
    std::printf("RESULT: %d passed, %d failed, %d skipped\nreport: %s\n", out.passed, out.failed,
                out.skipped, out.reportPath.c_str());
    return out.failed == 0 ? 0 : 1;
}

} // namespace

// --start-simulator: start uacsimd.exe (next to this exe) when the window opens, unless one already answers. The simulator it
// starts is this window's: it is stopped again when the window closes (see BeginExit).
void StartSimulatorAtLaunch(HWND h) {
    g_simStarting.store(true);
    UpdateStatusBar();
    const std::uint16_t port = g_port;
    g_simStartThread = std::thread([h, port] {
        uacstudio::SimStartOptions so;
        so.exeDir = SelfExeDir();
        so.httpPort = port;
        so.usbipPort = 3240;
        std::error_code ec;
        const std::string profiles = so.exeDir + "\\profiles";
        if (std::filesystem::is_directory(std::filesystem::path(profiles), ec)) so.profilesDir = profiles;
        const auto r = uacstudio::TheSimulator().Start(so);
        PushLine(h, std::string("[Start simulator] ") + (r.ok ? "" : "FAILED: ") + r.detail);
        PostMessageW(h, WM_APP_SIMSTARTED, r.ok ? 1 : 0, 0);
    });
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    ParseArgs();
    if (HasArg(L"--install-driver") || HasArg(L"--uninstall-driver")) {
        const int code = RunCliDriver(HasArg(L"--install-driver"));
        WSACleanup();
        return code;
    }
    if (HasArg(L"--autotest")) {
        const int code = RunCliAutoTest();
        WSACleanup();
        return code;
    }
    g_selfTest = HasArg(L"--selftest");
    // Which uacsimd port the GUI talks to. The smoke script points it at a port nobody listens on to check that the
    // window stays responsive without uacsimd.
    if (const auto port = ArgValue(L"--port"); !port.empty()) g_port = static_cast<uint16_t>(_wtoi(port.c_str()));
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hIcon = static_cast<HICON>(LoadImageW(
        hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_DEFAULTCOLOR));
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr; // painted by WM_ERASEBKGND
    wc.lpszClassName = L"UacStudio";
    RegisterClassW(&wc);
    const std::wstring title = WindowTitle();
    g_main = CreateWindowExW(0, L"UacStudio", title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, 880, 640, nullptr, nullptr, hi, nullptr);
    ShowWindow(g_main, g_selfTest ? SW_SHOWNOACTIVATE : nCmdShow);
    UpdateWindow(g_main);
    if (!g_selfTest && HasArg(L"--start-simulator")) StartSimulatorAtLaunch(g_main);
    // No modal dialog in self-check mode: nobody is there to click it
    // --no-env-check lets automation (smoke.ps1) suppress the first-run modal: with the modal up, a WM_CLOSE sent to
    // the disabled owner window cannot destroy it and the process would hang around after CloseMainWindow().
    if (!g_selfTest &&
        (HasArg(L"--env-check") || (uacstudio::IsFirstRun() && !HasArg(L"--no-env-check")))) {
        uacstudio::ShowEnvCheck(g_main, g_port);
        PokePoller();
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    WSACleanup();
    return static_cast<int>(msg.wParam);
}
