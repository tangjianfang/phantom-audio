// Environment check: Windows version / uacsimd / usbip-win2 driver / usbip.exe / Hyper-V / OpenCppCoverage.
//
// All text is English (the Windows Sandbox image has no CJK fonts, Chinese rendered as boxes).
//
// THREADING: RunEnvChecks() blocks (a connect to a closed loopback port, a SHA-256 over the driver payload, registry and
// service queries), so the window never calls it on the UI thread. A checker thread runs it and reports each step
// ("Checking: ...") and the final list through PostMessage; the driver install runs on its own thread the same way.
// Closing the window while a thread is running is deferred until it has reported back, so a thread never posts to a
// window that is gone (the CheckWnd object lives on ShowEnvCheck()'s stack and threads never touch it directly).
#include "http_client.h"   // winsock2 must come before windows.h
#include "env_check.h"
#include "driver_installer.h"
#include "host_ops.h"
#include "sim_process.h"
#include <winsvc.h>
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace uacstudio {

namespace {

std::string Utf8(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

// The folder of uacstudio.exe (uacsimd.exe, usbip-payload\ and profiles\ sit next to it in the zip)
std::string SelfDir() {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring w(p);
    return Utf8(w.substr(0, w.find_last_of(L"\\/")));
}

// Default location of the slim driver payload: usbip-payload\ next to uacstudio.exe (the layout inside the zip)
std::string DefaultPayloadDir() { return SelfDir() + "\\usbip-payload"; }

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool ServiceExists(const wchar_t* name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    if (svc) {
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return true;
    }
    CloseServiceHandle(scm);
    return false;
}

DWORD RegReadDword(HKEY root, const wchar_t* sub, const wchar_t* value, DWORD def) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return def;
    DWORD v = def, size = sizeof(v), type = 0;
    if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<LPBYTE>(&v), &size) !=
        ERROR_SUCCESS)
        v = def;
    RegCloseKey(k);
    return v;
}

std::wstring RegReadString(HKEY root, const wchar_t* sub, const wchar_t* value) {
    HKEY k;
    if (RegOpenKeyExW(root, sub, 0, KEY_READ, &k) != ERROR_SUCCESS) return L"";
    wchar_t buf[64]{};
    DWORD size = sizeof(buf), type = 0;
    if (RegQueryValueExW(k, value, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &size) !=
        ERROR_SUCCESS)
        buf[0] = 0;
    RegCloseKey(k);
    return buf;
}

bool FileExists(const wchar_t* path) {
    return GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

std::vector<EnvItem> RunEnvChecks(std::uint16_t port, const EnvProgress& progress) {
    auto say = [&](const std::wstring& what) {
        if (progress) progress(what);
    };
    std::vector<EnvItem> items;

    // 1) Windows version (usbip-win2 needs Win10 1903 / build 18362)
    {
        say(L"reading the Windows version ...");
        EnvItem it;
        it.name = L"Windows version (needs Win10 1903 / build 18362+)";
        const auto buildStr = RegReadString(HKEY_LOCAL_MACHINE,
                                            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                            L"CurrentBuildNumber");
        const DWORD ubr = RegReadDword(HKEY_LOCAL_MACHINE,
                                       L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", 0);
        const DWORD build = static_cast<DWORD>(wcstol(buildStr.c_str(), nullptr, 10));
        it.detail = L"Build " + buildStr + L"." + std::to_wstring(ubr);
        if (build >= 18362) {
            it.state = 2;
        } else {
            it.state = 0;
            it.fixNote = L"Windows is too old: the usbip-win2 driver needs Windows 10 1903 or newer.";
        }
        items.push_back(std::move(it));
    }

    // 2) uacsimd service (a network call: bounded by the 300 ms connect deadline in http_client.h)
    {
        say(L"connecting to uacsimd on 127.0.0.1:" + std::to_wstring(port) + L" ...");
        EnvItem it;
        it.name = L"uacsimd simulator service (127.0.0.1:" + std::to_wstring(port) + L")";
        const auto r = http_req(port, "GET", "/status", "");
        if (!r.empty()) {
            it.state = 2;
            it.detail = L"Connected, the control API is up";
        } else {
            it.state = 0;
            it.detail = L"Not connected";
            it.fixNote = L"Start uacsimd.exe (it sits next to uacstudio.exe; no administrator rights needed).";
            it.fixCmd = L"uacsimd.exe";
        }
        items.push_back(std::move(it));
    }

    // 3) usbip-win2 driver (needed to attach for real)
    {
        say(L"looking for the usbip-win2 driver service ...");
        EnvItem it;
        it.name = L"usbip-win2 driver (service usbip2_ude)";
        if (ServiceExists(L"usbip2_ude")) {
            it.state = 2;
            it.detail = L"Installed";
        } else {
            it.state = 0;
            it.detail = L"Not installed (only affects real attach; loopback tests and unit tests are not affected)";
            // The slim payload (usbip-payload\, about 3 MB, shipped in the zip) is in place -> one-click install,
            // but only on a disposable machine
            std::string why;
            const bool disposable = HostCasesAllowed(true, &why);
            say(L"verifying the driver payload (SHA-256 of every file) ...");
            const PayloadCheck pc = CheckPayload(DefaultPayloadDir());
            if (pc.ok && disposable) {
                it.canInstallDriver = true;
                it.detail = L"Not installed; the slim payload was found - use Install driver below (" + Widen(why) + L")";
                it.fixNote = L"Press Install driver below: it runs pnputil (filter driver) and devnode (UDE driver + virtual host "
                             L"controller). Installing restarts USB 3 hubs briefly; whether a reboot is needed is reported "
                             L"by the installer.";
            } else if (pc.ok) {
                it.detail = L"Not installed. This machine is not disposable, so no driver is installed here - "
                            L"install it in Windows Sandbox / a VM";
                it.fixNote = L"The GUI offers no driver install on a machine that is not Windows Sandbox / a VM. Copy the "
                             L"zip made by build.bat into the Sandbox, extract it and double-click run.cmd.";
            } else {
                it.fixNote =
                    L"No usable slim payload (" + Widen(pc.error) + L"). Two ways:\n"
                    L"  1. Use the zip made by build.bat (it contains usbip-payload\\) and extract it inside Sandbox / a VM\n"
                    L"  2. Take the x64 installer from the usbip-win2 releases page, install it on a disposable machine, then "
                    L"run collect-usbip-payload.bat to collect the slim payload\n"
                    L"Do not install drivers on a development machine. Once installed, usbip.exe works: "
                    L"usbip attach -r 127.0.0.1 -b 1-1";
                it.fixCmd = L"powershell -ExecutionPolicy Bypass -File tools\\uac-simulator\\sandbox\\launch-sandbox.ps1";
                it.url = L"https://github.com/vadimgrn/usbip-win2/releases";
            }
        }
        items.push_back(std::move(it));
    }

    // 3b) VHCI 虚拟主控设备节点（usbip attach 的挂载点）。服务在 ≠ 主控在：热装卸/半途卸载会把
    // ROOT\USBIP_WIN2\UDE 节点摘掉，此后 attach 全部失败而服务仍显示 Running——2026-11-01 真机
    // 踩过：GUI 只报"Attached"而设备永远不出现。这里主动探测并给出修复步骤。
    {
        say(L"probing the usbip virtual host controller (usbip port) ...");
        EnvItem it;
        it.name = L"usbip virtual host controller (VHCI root device)";
        const auto ds = DetectDriver();
        if (!ds.usbipPresent) {
            it.state = 1; // usbip.exe 没有（驱动未装）时不算这一项的问题
            it.detail = L"Skipped: usbip.exe not found (driver not installed)";
        } else {
            const auto r = RunTool(ds.usbipPath, "port", 15000);
            if (r.started && !r.timedOut && r.out.find("not found") == std::string::npos) {
                it.state = 2;
                it.detail = L"Present (usbip port answered)";
            } else {
                it.state = 0;
                it.detail = r.started && !r.timedOut
                                ? Widen(r.out.substr(0, 120))
                                : L"usbip port did not answer";
                it.fixNote =
                    L"The virtual host controller is gone although the service still runs - this "
                    L"happens when the driver is installed/uninstalled without a reboot in between.\n"
                    L"  1. Restart Windows\n"
                    L"  2. Run: uacstudio --install-driver   (recreates the root device node)\n"
                    L"Until then every Attach to Windows fails and no audio endpoint will appear, "
                    L"even though the Attach button in the main window reports success.";
                it.fixCmd = L"uacstudio --install-driver";
            }
        }
        items.push_back(std::move(it));
    }

    // 4) usbip.exe (command-line attach tool, installed with the driver)
    {
        say(L"looking for usbip.exe ...");
        EnvItem it;
        it.name = L"usbip.exe command-line tool";
        wchar_t found[MAX_PATH]{};
        if (SearchPathW(nullptr, L"usbip.exe", nullptr, MAX_PATH, found, nullptr) ||
            FileExists(L"C:\\Program Files\\USBip\\usbip.exe")) {
            it.state = 2;
            it.detail = found[0] ? found : L"C:\\Program Files\\USBip\\usbip.exe";
        } else {
            it.state = 0;
            it.detail = L"Not found (installed together with the usbip-win2 driver)";
            it.fixNote = L"Included with the usbip-win2 driver; there is no separate download.";
            it.url = L"https://github.com/vadimgrn/usbip-win2/releases";
        }
        items.push_back(std::move(it));
    }

    // 5) Hyper-V (optional: the M0 Linux probe VM)
    {
        say(L"checking for Hyper-V ...");
        EnvItem it;
        it.name = L"Hyper-V (optional: M0 probe VM)";
        if (ServiceExists(L"vmms")) {
            it.state = 2;
            it.detail = L"Available";
        } else {
            it.state = 1;
            it.detail = L"Not enabled (not needed unless you test with a real driver)";
            it.fixNote =
                L"To enable (needs a reboot):\n"
                L"  1. Administrator PowerShell: Enable-WindowsOptionalFeature -Online -FeatureName "
                L"Microsoft-Hyper-V -All\n"
                L"  2. Reboot\n"
                L"  3. Run tools\\uac-simulator\\scripts\\m0\\elevated-m0-vm.ps1 to create the probe VM";
            it.fixCmd =
                L"Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V -All";
        }
        items.push_back(std::move(it));
    }

    // 6) OpenCppCoverage (optional: coverage gate)
    {
        say(L"checking for OpenCppCoverage ...");
        EnvItem it;
        it.name = L"OpenCppCoverage (optional: coverage report)";
        if (FileExists(L"C:\\Program Files\\OpenCppCoverage\\OpenCppCoverage.exe")) {
            it.state = 2;
            it.detail = L"Installed";
        } else {
            it.state = 1;
            it.detail = L"Not installed";
            it.fixNote = L"Install (no reboot needed): winget install OpenCppCoverage.OpenCppCoverage";
            it.fixCmd = L"winget install OpenCppCoverage.OpenCppCoverage";
        }
        items.push_back(std::move(it));
    }

    return items;
}

bool IsFirstRun() {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PhantomAudio\\uacstudio", 0, KEY_READ, &k) !=
        ERROR_SUCCESS)
        return true;
    DWORD v = 0, size = sizeof(v);
    const BOOL ok =
        RegQueryValueExW(k, L"FirstRunDone", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&v), &size) == ERROR_SUCCESS &&
        v == 1;
    RegCloseKey(k);
    return !ok;
}

void MarkFirstRunDone() {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PhantomAudio\\uacstudio", 0, nullptr, 0,
                        KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS)
        return;
    DWORD one = 1;
    RegSetValueExW(k, L"FirstRunDone", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&one), sizeof(one));
    RegCloseKey(k);
}

// ---------- the check window ----------
namespace {
constexpr COLORREF C_BG = RGB(0x17, 0x1A, 0x21);
constexpr COLORREF C_PANEL = RGB(0x1F, 0x24, 0x2E);
constexpr COLORREF C_INK = RGB(0xDD, 0xE3, 0xEA);
constexpr COLORREF C_MUTED = RGB(0x8B, 0x93, 0xA1);
constexpr COLORREF C_ACCENT = RGB(0xE8, 0xA3, 0x3D);
constexpr COLORREF C_OK = RGB(0x4F, 0xB5, 0x73);
constexpr COLORREF C_WARN = RGB(0xE8, 0xA3, 0x3D);
constexpr COLORREF C_FAULT = RGB(0xE0, 0x52, 0x52);

struct CheckRow {
    HWND led = nullptr, name = nullptr, detail = nullptr;
    HWND openUrl = nullptr, copyCmd = nullptr;
    EnvItem item;
};

// Messages from the background threads. lParam is a heap object that the receiver owns and frees.
constexpr UINT kMsgOpDone = WM_APP + 20;         // lParam OpDone*  (an install / uninstall / attach / detach / clean-up finished)
constexpr UINT kMsgCheckProgress = WM_APP + 21;  // lParam std::wstring* ("what is being checked")
constexpr UINT kMsgCheckDone = WM_APP + 22;      // lParam CheckDone*

struct OpDone {
    std::string title;
    bool ok = false, reboot = false, refused = false;
    std::string text; // multi-line step list
};
struct CheckDone {
    std::vector<EnvItem> items;
    HostState host; // what the action buttons may do
};

// The action buttons under the check list. The enabled state comes from PlanHostActions() (host_ops.h), which is unit tested.
struct ActionDef {
    HostAction action;
    int id; // control id; below 1000 so it never collides with the row controls
    const wchar_t* label;
    int row; // 0 = simulator and Windows, 1 = driver and clean-up
};
constexpr ActionDef kActionDefs[] = {
    {HostAction::StartSimulator, 20, L"Start simulator", 0},
    {HostAction::StopSimulator, 21, L"Stop simulator", 0},
    {HostAction::AttachToWindows, 22, L"Attach to Windows", 0},
    {HostAction::DetachFromWindows, 23, L"Detach from Windows", 0},
    {HostAction::InstallDriver, 24, L"Install driver", 1},
    {HostAction::UninstallDriver, 25, L"Uninstall driver", 1},
    {HostAction::CleanUpAll, 26, L"Clean up all", 1},
};
constexpr int kActionCount = 7;
constexpr unsigned kUsbipPort = 3240; // the simulator's USB/IP port (uacsimd default)

struct CheckWnd {
    HWND wnd = nullptr;
    std::vector<CheckRow> rows;
    HWND hint = nullptr, recheck = nullptr, done = nullptr;
    HWND actionBtn[kActionCount]{};
    HBRUSH brBg = nullptr, brPanel = nullptr;
    HFONT fUi = nullptr, fMono = nullptr, fTitle = nullptr;
    HWND title = nullptr;
    std::uint16_t port = 3241;
    HostState host;                  // UI thread only; filled by the last finished check
    std::vector<ActionView> plan;    // PlanHostActions(host)
    // Background work. Threads only hold the window handle and report through PostMessage - CheckWnd is a stack object
    // and threads must not touch it. While one runs, closing is deferred (closePending) until it has reported back.
    std::atomic<bool> checking{false};
    std::atomic<bool> operating{false}; // an install / uninstall / attach / detach / clean-up is running
    bool closePending = false; // UI thread only
    std::thread checker;
    std::thread worker;
};

CheckWnd* g_ck = nullptr;

int S(HWND h, int logical) { return MulDiv(logical, static_cast<int>(GetDpiForWindow(h)), 96); }

template <class T>
void PostOwned(HWND wnd, UINT msg, WPARAM wp, T* payload) {
    if (!PostMessageW(wnd, msg, wp, reinterpret_cast<LPARAM>(payload))) delete payload; // window gone: nobody would free it
}

void SetHint(CheckWnd* ck, const std::wstring& text) {
    if (ck && ck->hint) SetWindowTextW(ck->hint, text.c_str());
}

void LayoutAll(CheckWnd* ck) {
    RECT rc;
    GetClientRect(ck->wnd, &rc);
    const int W = rc.right, m = S(ck->wnd, 16);
    int y = S(ck->wnd, 56);
    MoveWindow(ck->title, m, S(ck->wnd, 16), W - 2 * m, S(ck->wnd, 26), TRUE);
    // English captions are longer than the old ones ("Install driver", "Copy command"): wide buttons, and the name /
    // detail text ends where the leftmost button of the row begins (a STATIC under a button would be hidden by it).
    const int bw = S(ck->wnd, 112), bgap = S(ck->wnd, 6);
    for (auto& r : ck->rows) {
        const int nBtn = (r.copyCmd ? 1 : 0) + (r.openUrl ? 1 : 0);
        const int textX = m + S(ck->wnd, 22);
        const int textW = std::max(W - m - nBtn * (bw + bgap) - S(ck->wnd, 4) - textX, S(ck->wnd, 120));
        MoveWindow(r.led, m, y + S(ck->wnd, 3), S(ck->wnd, 12), S(ck->wnd, 12), TRUE);
        MoveWindow(r.name, textX, y, textW, S(ck->wnd, 20), TRUE);
        MoveWindow(r.detail, textX, y + S(ck->wnd, 20), textW, S(ck->wnd, 18), TRUE);
        int bx = W - m - bw;
        for (HWND b : {r.copyCmd, r.openUrl}) {
            if (!b) continue;
            MoveWindow(b, bx, y, bw, S(ck->wnd, 24), TRUE);
            bx -= bw + bgap;
        }
        y += S(ck->wnd, 44);
    }
    // Action rows: the buttons of a row share the width equally
    y += S(ck->wnd, 4);
    const int agap = S(ck->wnd, 6);
    for (int row = 0; row < 2; ++row) {
        int n = 0;
        for (const auto& def : kActionDefs) n += def.row == row ? 1 : 0;
        const int aw = (W - 2 * m - (n - 1) * agap) / n;
        int col = 0;
        for (int i = 0; i < kActionCount; ++i)
            if (kActionDefs[i].row == row) MoveWindow(ck->actionBtn[i], m + col++ * (aw + agap), y, aw, S(ck->wnd, 30), TRUE);
        y += S(ck->wnd, 30) + S(ck->wnd, 6);
    }
    y += S(ck->wnd, 2);
    // Status text (up to three lines) with Recheck / Done to its right
    MoveWindow(ck->hint, m, y, W - 2 * m - S(ck->wnd, 200), S(ck->wnd, 54), TRUE);
    MoveWindow(ck->recheck, W - m - 2 * S(ck->wnd, 100), y + S(ck->wnd, 4), S(ck->wnd, 92), S(ck->wnd, 28), TRUE);
    MoveWindow(ck->done, W - m - S(ck->wnd, 100), y + S(ck->wnd, 4), S(ck->wnd, 92), S(ck->wnd, 28), TRUE);
    InvalidateRect(ck->wnd, nullptr, TRUE);
}

bool Busy(const CheckWnd* ck) { return ck->checking.load() || ck->operating.load(); }

// Enable each action button from the plan, unless something is running (one background operation at a time).
void RefreshButtons(CheckWnd* ck) {
    for (int i = 0; i < kActionCount; ++i) {
        bool on = false;
        for (const auto& v : ck->plan)
            if (v.action == kActionDefs[i].action) on = v.enabled;
        EnableWindow(ck->actionBtn[i], (on && !Busy(ck)) ? TRUE : FALSE);
    }
    EnableWindow(ck->recheck, Busy(ck) ? FALSE : TRUE);
}

// Build the rows from finished check results (UI thread; no blocking work here)
void BuildRows(CheckWnd* ck, std::vector<EnvItem> items) {
    for (auto& r : ck->rows) {
        for (HWND c : {r.led, r.name, r.detail, r.openUrl, r.copyCmd})
            if (c) DestroyWindow(c);
    }
    ck->rows.clear();
    const DWORD vis = WS_CHILD | WS_VISIBLE;
    int i = 0;
    for (auto& it : items) {
        CheckRow r;
        r.item = it;
        r.led = CreateWindowExW(0, L"STATIC", L"", vis | SS_OWNERDRAW, 0, 0, 10, 10, ck->wnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(1000 + i * 10)),
                                nullptr, nullptr);
        r.name = CreateWindowExW(0, L"STATIC", it.name.c_str(), vis | SS_ENDELLIPSIS, 0, 0, 10, 10, ck->wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001 + i * 10)),
                                 nullptr, nullptr);
        r.detail = CreateWindowExW(0, L"STATIC", it.detail.c_str(), vis | SS_ENDELLIPSIS, 0, 0, 10, 10, ck->wnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(1002 + i * 10)),
                                   nullptr, nullptr);
        if (it.url)
            r.openUrl = CreateWindowExW(
                0, L"BUTTON", L"Open website", vis | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 10, 10, ck->wnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(1003 + i * 10)), nullptr, nullptr);
        if (!it.fixCmd.empty())
            r.copyCmd = CreateWindowExW(
                0, L"BUTTON", L"Copy command", vis | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 10, 10, ck->wnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(1004 + i * 10)), nullptr, nullptr);
        for (HWND c : {r.led, r.name, r.detail, r.openUrl, r.copyCmd})
            if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ck->fUi), TRUE);
        ck->rows.push_back(std::move(r));
        ++i;
    }
    int missing = 0, optional = 0;
    for (auto& it : items) {
        if (it.state == 0) ++missing;
        if (it.state == 1) ++optional;
    }
    std::wstring summary = L"Ready: ";
    summary += std::to_wstring(items.size() - missing - optional);
    summary += L"  |  missing: ";
    summary += std::to_wstring(missing);
    summary += L"  |  optional, not installed: ";
    summary += std::to_wstring(optional);
    summary += missing > 0 ? L".  Fix the missing items following their notes, then click Recheck." : L".";
    // When the guard says no, say so once, next to the buttons that are greyed out because of it
    if (!ck->host.allowed)
        summary += L"\nThe driver and Windows buttons are off: this machine is not a Windows Sandbox / VM. To allow them on a test machine "
                   L"you can throw away, start uacstudio with UACSIM_FORCE_HOST_TESTS=1 in the environment.";
    SetHint(ck, summary);
    LayoutAll(ck);
}

// Run the checks on a background thread; progress and the result come back as messages.
void StartCheck(CheckWnd* ck) {
    if (ck->checking.load()) return;
    if (ck->checker.joinable()) ck->checker.join(); // the previous run has already reported back
    ck->checking = true;
    RefreshButtons(ck);
    SetHint(ck, L"Checking the environment ...");
    const HWND wnd = ck->wnd;
    const std::uint16_t port = ck->port;
    ck->checker = std::thread([wnd, port] {
        auto progress = [wnd](const std::wstring& step) { PostOwned(wnd, kMsgCheckProgress, 0, new std::wstring(step)); };
        auto* d = new CheckDone;
        d->items = RunEnvChecks(port, progress);
        progress(L"what the install / attach / clean-up buttons may do (registry, driver payload hash, usbip ports) ...");
        InstallOptions o;
        o.payloadRoot = DefaultPayloadDir();
        d->host = QueryHostState(port, o, kUsbipPort);
        PostOwned(wnd, kMsgCheckDone, 0, d);
    });
}

// Start one install / uninstall / attach / detach / clean-up on the worker thread. `busyText` is what the window says while
// it runs; the result comes back as kMsgOpDone. Only one at a time (Busy), and the window cannot close underneath it.
void StartOp(CheckWnd* ck, const std::wstring& busyText, std::function<OpDone()> job) {
    if (Busy(ck)) return;
    if (ck->worker.joinable()) ck->worker.join(); // the previous operation reported back long ago
    ck->operating = true;
    RefreshButtons(ck);
    SetHint(ck, busyText);
    const HWND wnd = ck->wnd;
    ck->worker = std::thread([wnd, job = std::move(job)] { PostOwned(wnd, kMsgOpDone, 0, new OpDone(job())); });
}

OpDone FromOp(const HostOpResult& r) {
    OpDone d;
    d.title = r.title;
    d.ok = r.ok;
    d.reboot = r.rebootRequired;
    d.refused = r.refused;
    d.text = r.Summary();
    return d;
}

// What each button does. Destructive ones ask first (default answer: No).
void RunHostAction(CheckWnd* ck, HostAction action) {
    if (Busy(ck)) return;
    const HWND h = ck->wnd;
    const std::uint16_t port = ck->port;
    InstallOptions o;
    o.payloadRoot = DefaultPayloadDir();
    auto ask = [&](const wchar_t* title, const wchar_t* text) {
        return MessageBoxW(h, text, title, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    };
    switch (action) {
    case HostAction::StartSimulator:
        StartOp(ck, L"Starting the simulator (uacsimd.exe) ...", [port] {
            SimStartOptions so;
            so.exeDir = SelfDir();
            so.httpPort = port;
            so.usbipPort = kUsbipPort;
            std::error_code ec;
            const std::string profiles = so.exeDir + "\\profiles";
            if (std::filesystem::is_directory(std::filesystem::path(profiles), ec)) so.profilesDir = profiles;
            return FromOp(FromSim("Start simulator", "start uacsimd.exe", TheSimulator().Start(so)));
        });
        break;
    case HostAction::StopSimulator:
        StartOp(ck, L"Stopping the simulator (detaching from Windows first when this machine allows it) ...",
                [o, port] { return FromOp(StopSimulatorOp(port, kUsbipPort, o)); });
        break;
    case HostAction::InstallDriver:
        if (!ask(L"Install driver",
                 L"This will install the usbip-win2 kernel driver on this machine (pnputil + devnode) and create a "
                 L"virtual USB host controller.\n\n"
                 L"Installing restarts USB 3 hubs briefly; USB devices that are plugged in drop off for a moment. "
                 L"No restore point is created.\n"
                 L"Only do this on a disposable machine (Windows Sandbox / a VM) or a test machine you can throw away.\n\nContinue?"))
            return;
        StartOp(ck, L"Installing the driver ... (pnputil / devnode; this can take a minute or two - please do not close the window)",
                [o] {
                    const InstallResult r = InstallDriver(o);
                    return FromOp(FromInstall("Install driver", r));
                });
        break;
    case HostAction::UninstallDriver:
        if (!ask(L"Uninstall driver",
                 L"This removes the usbip-win2 virtual host controller and the usbip2_* driver packages from this machine, and "
                 L"the files this program copied.\n\n"
                 L"Anything still attached through usbip loses its device. Use 'Clean up all' to detach first.\n\nContinue?"))
            return;
        StartOp(ck, L"Uninstalling the driver ... (devnode / pnputil; please do not close the window)",
                [o] { return FromOp(FromInstall("Uninstall driver", UninstallDriver(o))); });
        break;
    case HostAction::AttachToWindows:
        StartOp(ck, L"Attaching the simulated device to Windows (usbip attach -r 127.0.0.1 -b 1-1) ...",
                [o, port] { return FromOp(AttachToWindows(port, "1-1", o)); });
        break;
    case HostAction::DetachFromWindows:
        StartOp(ck, L"Detaching this simulator's devices from Windows (usbip detach) ...",
                [o] { return FromOp(DetachFromWindows(kUsbipPort, o)); });
        break;
    case HostAction::CleanUpAll:
        if (!ask(L"Clean up all",
                 L"This detaches the simulated devices from Windows, detaches them in the simulator, STOPS the simulator "
                 L"(uacsimd.exe) and then UNINSTALLS the usbip-win2 driver.\n\n"
                 L"usbip ports that do not belong to this simulator are left alone.\n\nContinue?"))
            return;
        StartOp(ck, L"Cleaning up: Windows ports, simulator, driver ... (please do not close the window)",
                [o, port] { return FromOp(CleanUpAll(port, kUsbipPort, o)); });
        break;
    }
}

void CopyToClipboard(HWND owner, const std::wstring& text) {
    if (OpenClipboard(owner)) {
        EmptyClipboard();
        const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (auto* p = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            auto* dst = static_cast<wchar_t*>(GlobalLock(p));
            wcscpy_s(dst, text.size() + 1, text.c_str());
            GlobalUnlock(p);
            SetClipboardData(CF_UNICODETEXT, p);
        }
        CloseClipboard();
    }
}

// ShellExecute can stall the calling thread for a moment (shell / default-browser start-up): do it off the UI thread
void OpenUrlAsync(const wchar_t* url) {
    std::thread([u = std::wstring(url)] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", u.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        CoUninitialize();
    }).detach();
}

// Close the window now, or - while a background thread is still running - as soon as it has reported back.
void RequestClose(CheckWnd* ck, HWND h) {
    if (ck && Busy(ck)) {
        ck->closePending = true;
        SetHint(ck, ck->operating.load() ? L"An operation is still running - the window closes when it is done ..."
                                         : L"Finishing the check - the window closes in a moment ...");
        return;
    }
    DestroyWindow(h);
}

LRESULT CALLBACK CheckProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* ck = reinterpret_cast<CheckWnd*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE:
        return 0;
    case WM_CTLCOLORSTATIC: {
        auto dc = reinterpret_cast<HDC>(wp);
        SetBkColor(dc, C_BG);
        SetTextColor(dc, ck && reinterpret_cast<HWND>(lp) == ck->hint ? C_MUTED : C_INK);
        return reinterpret_cast<LRESULT>(ck ? ck->brBg : nullptr);
    }
    case WM_DRAWITEM: {
        if (!ck) return DefWindowProcW(h, msg, wp, lp);
        auto* dis = reinterpret_cast<LPDRAWITEMSTRUCT>(lp);
        int idx = static_cast<int>(dis->CtlID);
        if (idx >= 1000 && (idx % 10) == 0) { // LED
            const int row = (idx - 1000) / 10;
            if (row >= static_cast<int>(ck->rows.size())) return TRUE;
            const int state = ck->rows[row].item.state;
            const COLORREF c = state == 2 ? C_OK : state == 1 ? C_WARN : C_FAULT;
            HBRUSH b = CreateSolidBrush(c);
            const int cx = (dis->rcItem.left + dis->rcItem.right) / 2;
            const int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
            const int r = S(h, 4);
            auto old = static_cast<HBRUSH>(SelectObject(dis->hDC, b));
            HPEN pn = static_cast<HPEN>(SelectObject(dis->hDC, GetStockObject(NULL_PEN)));
            Ellipse(dis->hDC, cx - r, cy - r, cx + r, cy + r);
            SelectObject(dis->hDC, pn);
            SelectObject(dis->hDC, old);
            DeleteObject(b);
            return TRUE;
        }
        // Key-cap button (minimal version of the main window's)
        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, 64);
        const bool down = (dis->itemState & ODS_SELECTED) != 0;
        const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
        HBRUSH b = CreateSolidBrush(down ? RGB(0x38, 0x40, 0x4F) : RGB(0x27, 0x2D, 0x39));
        FillRect(dis->hDC, &dis->rcItem, b);
        DeleteObject(b);
        HPEN pen = CreatePen(PS_SOLID, 1, disabled ? RGB(0x3C, 0x44, 0x52) : C_ACCENT);
        auto oldPen = static_cast<HPEN>(SelectObject(dis->hDC, pen));
        auto oldBr = static_cast<HBRUSH>(SelectObject(dis->hDC, GetStockObject(NULL_BRUSH)));
        Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
        SelectObject(dis->hDC, oldPen);
        SelectObject(dis->hDC, oldBr);
        DeleteObject(pen);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, disabled ? C_MUTED : C_INK);
        auto oldF = static_cast<HFONT>(SelectObject(dis->hDC, ck->fUi));
        RECT tr = dis->rcItem;
        DrawTextW(dis->hDC, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dis->hDC, oldF);
        return TRUE;
    }
    case kMsgCheckProgress: {
        std::unique_ptr<std::wstring> step(reinterpret_cast<std::wstring*>(lp));
        if (ck && ck->checking.load() && step) SetHint(ck, L"Checking: " + *step);
        return 0;
    }
    case kMsgCheckDone: {
        std::unique_ptr<CheckDone> done(reinterpret_cast<CheckDone*>(lp)); // the receiver frees it
        if (!ck) return 0;
        ck->checking = false;
        if (ck->closePending) {
            DestroyWindow(h);
            return 0;
        }
        if (done) {
            ck->host = std::move(done->host);
            ck->plan = PlanHostActions(ck->host);
            BuildRows(ck, std::move(done->items)); // also writes the status text, which mentions the guard
        }
        RefreshButtons(ck);
        return 0;
    }
    case kMsgOpDone: {
        std::unique_ptr<OpDone> done(reinterpret_cast<OpDone*>(lp)); // the receiver frees it
        if (ck) ck->operating = false;
        if (done) {
            const std::wstring title = Widen(done->title) + (done->refused ? L" - refused"
                                                             : !done->ok   ? L" - failed"
                                                             : done->reboot ? L" - done (reboot required)"
                                                                            : L" - done");
            MessageBoxW(h, Widen(done->text).c_str(), title.c_str(),
                        MB_OK | (done->ok ? MB_ICONINFORMATION : done->refused ? MB_ICONWARNING : MB_ICONERROR));
        }
        if (ck) {
            if (ck->closePending) DestroyWindow(h);
            else StartCheck(ck); // look again: the rows and the buttons follow what the operation changed
        }
        return 0;
    }
    case WM_COMMAND: {
        if (!ck) return 0; // null guard BEFORE any ck dereference
        const int id = LOWORD(wp);
        if (id == 1) { // Recheck
            if (!Busy(ck)) StartCheck(ck);
            return 0;
        }
        if (id == 2) { // Done
            RequestClose(ck, h);
            return 0;
        }
        for (int i = 0; i < kActionCount; ++i)
            if (id == kActionDefs[i].id) {
                // A disabled button cannot be clicked, but the plan is checked again here: it is the rule, the button is a view
                for (const auto& v : ck->plan)
                    if (v.action == kActionDefs[i].action && v.enabled) RunHostAction(ck, v.action);
                return 0;
            }
        if (id >= 1000) {
            const int row = (id - 1000) / 10, act = (id - 1000) % 10;
            if (row < static_cast<int>(ck->rows.size())) {
                auto& it = ck->rows[row].item;
                if (act == 3 && it.url) OpenUrlAsync(it.url);
                if (act == 4 && !it.fixCmd.empty()) {
                    CopyToClipboard(h, it.fixCmd);
                    MessageBoxW(h, (L"Copied to the clipboard:\n\n" + it.fixCmd + L"\n\nSteps:\n" + it.fixNote).c_str(),
                                L"Setup guide", MB_OK | MB_ICONINFORMATION);
                }
            }
        }
        return 0;
    }
    case WM_CLOSE:
        RequestClose(ck, h);
        return 0;
    case WM_ERASEBKGND: {
        auto dc = reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(h, &rc);
        if (ck) FillRect(dc, &rc, ck->brBg);
        return 1;
    }
    case WM_DESTROY:
        // Clear the stack object's window handle FIRST so the modal loop in ShowEnvCheck() observes the exit and unwinds
        // (previously it only exited on WM_QUIT: MarkFirstRunDone never ran and each open nested another permanent
        // message loop).
        if (ck) ck->wnd = nullptr;
        EnableWindow(GetParent(h), TRUE);
        SetForegroundWindow(GetParent(h));
        SetFocus(GetParent(h));
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

} // namespace

void ShowEnvCheck(HWND owner, std::uint16_t port) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = CheckProc;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"UacEnvCheck";
        RegisterClassW(&wc);
        registered = true;
    }
    CheckWnd ck;
    ck.port = port;
    ck.brBg = CreateSolidBrush(C_BG);
    ck.brPanel = CreateSolidBrush(C_PANEL);
    ck.fUi = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    ck.fMono = ck.fUi;
    ck.fTitle = CreateFontW(-22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, 0, 0, 0, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    // Window size in logical pixels scaled by the owner's DPI (it used to be raw pixels and clipped at 150 %)
    const int dpi = owner ? static_cast<int>(GetDpiForWindow(owner)) : 96;
    ck.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"UacEnvCheck", L"uacstudio - Environment and driver",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                             CW_USEDEFAULT, MulDiv(880, dpi, 96), MulDiv(590, dpi, 96), owner, nullptr, nullptr, nullptr);
    SetWindowLongPtrW(ck.wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ck));
    ck.title = CreateWindowExW(0, L"STATIC", L"Environment and driver", WS_CHILD | WS_VISIBLE, 0, 0, 10,
                               10, ck.wnd, nullptr, nullptr, nullptr);
    SendMessageW(ck.title, WM_SETFONT, reinterpret_cast<WPARAM>(ck.fTitle), TRUE);
    ck.hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, ck.wnd,
                              nullptr, nullptr, nullptr);
    SendMessageW(ck.hint, WM_SETFONT, reinterpret_cast<WPARAM>(ck.fUi), TRUE);
    ck.recheck = CreateWindowExW(0, L"BUTTON", L"Recheck", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                 0, 0, 10, 10, ck.wnd, reinterpret_cast<HMENU>(1), nullptr,
                                 nullptr);
    ck.done = CreateWindowExW(0, L"BUTTON", L"Done", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0,
                              10, 10, ck.wnd, reinterpret_cast<HMENU>(2), nullptr, nullptr);
    for (HWND c : {ck.recheck, ck.done})
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ck.fUi), TRUE);
    // The action buttons start disabled: what they may do is only known when the first check has reported back
    for (int i = 0; i < kActionCount; ++i) {
        ck.actionBtn[i] = CreateWindowExW(0, L"BUTTON", kActionDefs[i].label, WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP | WS_DISABLED,
                                          0, 0, 10, 10, ck.wnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kActionDefs[i].id)),
                                          nullptr, nullptr);
        SendMessageW(ck.actionBtn[i], WM_SETFONT, reinterpret_cast<WPARAM>(ck.fUi), TRUE);
    }
    LayoutAll(&ck);
    EnableWindow(owner, FALSE);
    ShowWindow(ck.wnd, SW_SHOW);
    UpdateWindow(ck.wnd);
    StartCheck(&ck); // the window is already on screen and responsive; the list fills in when the check reports back
    // Modal message loop (ends when the window is destroyed)
    MSG msg;
    while (ck.wnd && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(0);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    // Closing is deferred while a thread runs, so by now both have reported back; join is instant
    if (ck.checker.joinable()) ck.checker.join();
    if (ck.worker.joinable()) ck.worker.join();
    MarkFirstRunDone();
}

} // namespace uacstudio
