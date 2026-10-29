// 环境自检实现：系统版本 / uacsimd / usbip-win2 驱动 / usbip.exe / Hyper-V / OpenCppCoverage
#include "http_client.h"   // winsock2 必须先于 windows.h
#include "env_check.h"
#include <winsvc.h>
#include <shellapi.h>
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")

namespace uacstudio {

namespace {

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

std::vector<EnvItem> RunEnvChecks(std::uint16_t port) {
    std::vector<EnvItem> items;

    // 1) 系统版本（usbip-win2 下限 Win10 1903 / build 18362）
    {
        EnvItem it;
        it.name = L"Windows 版本（需 Win10 1903 / 内部版本 18362+）";
        const auto buildStr = RegReadString(HKEY_LOCAL_MACHINE,
                                            L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                                            L"CurrentBuildNumber");
        const DWORD ubr = RegReadDword(HKEY_LOCAL_MACHINE,
                                       L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", L"UBR", 0);
        const DWORD build = static_cast<DWORD>(wcstol(buildStr.c_str(), nullptr, 10));
        it.detail = L"内部版本 " + buildStr + L"." + std::to_wstring(ubr);
        if (build >= 18362) {
            it.state = 2;
        } else {
            it.state = 0;
            it.fixNote = L"系统过低：usbip-win2 驱动要求 Windows 10 1903 或更高，请先升级系统。";
        }
        items.push_back(std::move(it));
    }

    // 2) uacsimd 服务连接
    {
        EnvItem it;
        it.name = L"uacsimd 仿真器服务（127.0.0.1:3241）";
        const auto r = http_req(port, "GET", "/status", "");
        if (!r.empty()) {
            it.state = 2;
            it.detail = L"已连接，控制 API 正常";
        } else {
            it.state = 0;
            it.detail = L"未连接";
            it.fixNote =
                L"启动方式：仓库根目录运行 build.bat 生成后，双击/命令行启动 "
                L"tools\\uac-simulator\\build\\release\\uacsimd.exe（无需管理员）。";
            it.fixCmd = L"tools\\uac-simulator\\build\\release\\uacsimd.exe";
        }
        items.push_back(std::move(it));
    }

    // 3) usbip-win2 驱动（实机 attach 必需）
    {
        EnvItem it;
        it.name = L"usbip-win2 驱动（usbip2_ude 服务）";
        if (ServiceExists(L"usbip2_ude")) {
            it.state = 2;
            it.detail = L"已安装";
        } else {
            it.state = 0;
            it.detail = L"未安装（仅影响实机 attach，回环/单测不受影响）";
            it.fixNote =
                L"三步：\n"
                L"  1. 打开官网下载页取 v0.9.7.8 的 x64 安装包，放入 "
                L"tools\\uac-simulator\\third_party\\usbip-win2\\\n"
                L"  2. 管理员 PowerShell 运行 tools\\uac-simulator\\scripts\\elevated-install-driver.ps1"
                L"（自动创建系统还原点；安装会短暂重启所有 USB 集线器，系统无需重启）\n"
                L"  3. 安装后 usbip.exe 可用：usbip attach -r 127.0.0.1 -b 1-1";
            it.fixCmd = L"powershell -ExecutionPolicy Bypass -File tools\\uac-simulator\\scripts\\elevated-install-driver.ps1";
            it.url = L"https://github.com/vadimgrn/usbip-win2/releases";
        }
        items.push_back(std::move(it));
    }

    // 4) usbip.exe（命令行 attach 工具，随驱动安装）
    {
        EnvItem it;
        it.name = L"usbip.exe 命令行工具";
        wchar_t found[MAX_PATH]{};
        if (SearchPathW(nullptr, L"usbip.exe", nullptr, MAX_PATH, found, nullptr) ||
            FileExists(L"C:\\Program Files\\USBip\\usbip.exe")) {
            it.state = 2;
            it.detail = found[0] ? found : L"C:\\Program Files\\USBip\\usbip.exe";
        } else {
            it.state = 0;
            it.detail = L"未找到（随 usbip-win2 驱动安装）";
            it.fixNote = L"安装 usbip-win2 驱动后自动包含；无需单独下载。";
            it.url = L"https://github.com/vadimgrn/usbip-win2/releases";
        }
        items.push_back(std::move(it));
    }

    // 5) Hyper-V（可选：M0 Linux 探针 VM）
    {
        EnvItem it;
        it.name = L"Hyper-V（可选：M0 探针虚拟机）";
        if (ServiceExists(L"vmms")) {
            it.state = 2;
            it.detail = L"可用";
        } else {
            it.state = 1;
            it.detail = L"未启用（不装驱动实机测试可不启用）";
            it.fixNote =
                L"启用步骤（需要重启电脑）：\n"
                L"  1. 管理员 PowerShell：Enable-WindowsOptionalFeature -Online -FeatureName "
                L"Microsoft-Hyper-V -All\n"
                L"  2. 重启系统\n"
                L"  3. 运行 tools\\uac-simulator\\scripts\\m0\\elevated-m0-vm.ps1 创建探针 VM";
            it.fixCmd =
                L"Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V -All";
        }
        items.push_back(std::move(it));
    }

    // 6) OpenCppCoverage（可选：覆盖率门禁）
    {
        EnvItem it;
        it.name = L"OpenCppCoverage（可选：覆盖率统计）";
        if (FileExists(L"C:\\Program Files\\OpenCppCoverage\\OpenCppCoverage.exe")) {
            it.state = 2;
            it.detail = L"已安装";
        } else {
            it.state = 1;
            it.detail = L"未安装";
            it.fixNote = L"安装命令（无需重启）：winget install OpenCppCoverage.OpenCppCoverage";
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

// ---------- 检查窗口 ----------
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

struct CheckWnd {
    HWND wnd = nullptr;
    std::vector<CheckRow> rows;
    HWND hint = nullptr, recheck = nullptr, done = nullptr;
    HBRUSH brBg = nullptr, brPanel = nullptr;
    HFONT fUi = nullptr, fMono = nullptr, fTitle = nullptr;
    HWND title = nullptr;
    std::uint16_t port = 3241;
};

CheckWnd* g_ck = nullptr;

int S(HWND h, int logical) { return MulDiv(logical, static_cast<int>(GetDpiForWindow(h)), 96); }

void Rebuild(CheckWnd* ck) {
    // 清旧控件
    for (auto& r : ck->rows) {
        for (HWND c : {r.led, r.name, r.detail, r.openUrl, r.copyCmd})
            if (c) DestroyWindow(c);
    }
    ck->rows.clear();
    auto items = RunEnvChecks(ck->port);
    const DWORD vis = WS_CHILD | WS_VISIBLE;
    int i = 0;
    for (auto& it : items) {
        CheckRow r;
        r.item = it;
        r.led = CreateWindowExW(0, L"STATIC", L"", vis | SS_OWNERDRAW, 0, 0, 10, 10, ck->wnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(1000 + i * 10)),
                                nullptr, nullptr);
        r.name = CreateWindowExW(0, L"STATIC", it.name.c_str(), vis, 0, 0, 10, 10, ck->wnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001 + i * 10)),
                                 nullptr, nullptr);
        r.detail = CreateWindowExW(0, L"STATIC", it.detail.c_str(), vis, 0, 0, 10, 10, ck->wnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(1002 + i * 10)),
                                   nullptr, nullptr);
        if (it.url)
            r.openUrl = CreateWindowExW(
                0, L"BUTTON", L"打开官网", vis | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 10, 10, ck->wnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(1003 + i * 10)), nullptr, nullptr);
        if (!it.fixCmd.empty())
            r.copyCmd = CreateWindowExW(
                0, L"BUTTON", L"复制命令", vis | BS_OWNERDRAW | WS_TABSTOP, 0, 0, 10, 10, ck->wnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(1004 + i * 10)), nullptr, nullptr);
        for (HWND c : {r.led, r.name, r.detail})
            if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ck->fUi), TRUE);
        for (HWND c : {r.openUrl, r.copyCmd})
            if (c) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ck->fUi), TRUE);
        ck->rows.push_back(std::move(r));
        ++i;
    }
    // 汇总提示
    int missing = 0, optional = 0;
    for (auto& it : items) {
        if (it.state == 0) ++missing;
        if (it.state == 1) ++optional;
    }
    std::wstring summary = L"就绪环境 ";
    summary += std::to_wstring(items.size() - missing - optional);
    summary += L" 项 · 缺失 ";
    summary += std::to_wstring(missing);
    summary += L" 项 · 可选未装 ";
    summary += std::to_wstring(optional);
    summary += L" 项。缺失项按 fixNote 步骤处理后点“重新检测”。";
    SetWindowTextW(ck->hint, summary.c_str());
    // 布局
    RECT rc;
    GetClientRect(ck->wnd, &rc);
    const int W = rc.right, m = S(ck->wnd, 16);
    int y = S(ck->wnd, 56);
    MoveWindow(ck->title, m, S(ck->wnd, 16), W - 2 * m, S(ck->wnd, 26), TRUE);
    for (auto& r : ck->rows) {
        MoveWindow(r.led, m, y + S(ck->wnd, 3), S(ck->wnd, 12), S(ck->wnd, 12), TRUE);
        MoveWindow(r.name, m + S(ck->wnd, 22), y, W - 2 * m - S(ck->wnd, 200), S(ck->wnd, 20),
                   TRUE);
        MoveWindow(r.detail, m + S(ck->wnd, 22), y + S(ck->wnd, 20), W - 2 * m - S(ck->wnd, 200),
                   S(ck->wnd, 18), TRUE);
        int bx = W - m - S(ck->wnd, 96);
        if (r.copyCmd) {
            MoveWindow(r.copyCmd, bx, y, S(ck->wnd, 92), S(ck->wnd, 24), TRUE);
            bx -= S(ck->wnd, 98);
        }
        if (r.openUrl) MoveWindow(r.openUrl, bx, y, S(ck->wnd, 92), S(ck->wnd, 24), TRUE);
        y += S(ck->wnd, 44);
    }
    MoveWindow(ck->hint, m, y + S(ck->wnd, 4), W - 2 * m - S(ck->wnd, 200), S(ck->wnd, 36), TRUE);
    MoveWindow(ck->recheck, W - m - 2 * S(ck->wnd, 100), y + S(ck->wnd, 8), S(ck->wnd, 92),
               S(ck->wnd, 28), TRUE);
    MoveWindow(ck->done, W - m - S(ck->wnd, 100), y + S(ck->wnd, 8), S(ck->wnd, 92),
               S(ck->wnd, 28), TRUE);
    InvalidateRect(ck->wnd, nullptr, TRUE);
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
        // 键帽按钮（与主窗一致的最小实现）
        wchar_t text[64]{};
        GetWindowTextW(dis->hwndItem, text, 64);
        const bool down = (dis->itemState & ODS_SELECTED) != 0;
        HBRUSH b = CreateSolidBrush(down ? RGB(0x38, 0x40, 0x4F) : RGB(0x27, 0x2D, 0x39));
        FillRect(dis->hDC, &dis->rcItem, b);
        DeleteObject(b);
        HPEN pen = CreatePen(PS_SOLID, 1, C_ACCENT);
        auto oldPen = static_cast<HPEN>(SelectObject(dis->hDC, pen));
        auto oldBr = static_cast<HBRUSH>(SelectObject(dis->hDC, GetStockObject(NULL_BRUSH)));
        Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right,
                  dis->rcItem.bottom);
        SelectObject(dis->hDC, oldPen);
        SelectObject(dis->hDC, oldBr);
        DeleteObject(pen);
        SetBkMode(dis->hDC, TRANSPARENT);
        SetTextColor(dis->hDC, C_INK);
        auto oldF = static_cast<HFONT>(SelectObject(dis->hDC, ck->fUi));
        RECT tr = dis->rcItem;
        DrawTextW(dis->hDC, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dis->hDC, oldF);
        return TRUE;
    }
    case WM_COMMAND: {
        if (!ck) return 0; // null guard BEFORE any ck dereference
        const int id = LOWORD(wp);
        if (id == 1) { // 重新检测
            Rebuild(ck);
            return 0;
        }
        if (id == 2) { // 完成
            DestroyWindow(h);
            return 0;
        }
        if (id >= 1000) {
            const int row = (id - 1000) / 10, act = (id - 1000) % 10;
            if (row < static_cast<int>(ck->rows.size())) {
                auto& it = ck->rows[row].item;
                if (act == 3 && it.url) ShellExecuteW(h, L"open", it.url, nullptr, nullptr,
                                                      SW_SHOWNORMAL);
                if (act == 4 && !it.fixCmd.empty()) {
                    CopyToClipboard(h, it.fixCmd);
                    MessageBoxW(h, (L"已复制到剪贴板：\n\n" + it.fixCmd + L"\n\n步骤：\n" +
                                    it.fixNote)
                                     .c_str(),
                                L"安装引导", MB_OK | MB_ICONINFORMATION);
                }
            }
        }
        return 0;
    }
    case WM_ERASEBKGND: {
        auto dc = reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(h, &rc);
        if (ck) FillRect(dc, &rc, ck->brBg);
        return 1;
    }
    case WM_DESTROY:
        // Clear the stack object's window handle FIRST so the modal loop in
        // ShowEnvCheck() observes the exit and unwinds (previously it only
        // exited on WM_QUIT: MarkFirstRunDone never ran and each open nested
        // another permanent message loop).
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
    ck.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"UacEnvCheck", L"uacstudio · 环境自检",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT,
                             CW_USEDEFAULT, 760, 480, owner, nullptr, nullptr, nullptr);
    SetWindowLongPtrW(ck.wnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&ck));
    ck.title = CreateWindowExW(0, L"STATIC", L"首次运行环境自检", WS_CHILD | WS_VISIBLE, 0, 0, 10,
                               10, ck.wnd, nullptr, nullptr, nullptr);
    SendMessageW(ck.title, WM_SETFONT, reinterpret_cast<WPARAM>(ck.fTitle), TRUE);
    ck.hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, ck.wnd,
                              nullptr, nullptr, nullptr);
    SendMessageW(ck.hint, WM_SETFONT, reinterpret_cast<WPARAM>(ck.fUi), TRUE);
    ck.recheck = CreateWindowExW(0, L"BUTTON", L"重新检测", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                                 0, 0, 10, 10, ck.wnd, reinterpret_cast<HMENU>(1), nullptr,
                                 nullptr);
    ck.done = CreateWindowExW(0, L"BUTTON", L"完成", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0,
                              10, 10, ck.wnd, reinterpret_cast<HMENU>(2), nullptr, nullptr);
    for (HWND c : {ck.recheck, ck.done})
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(ck.fUi), TRUE);
    Rebuild(&ck);
    EnableWindow(owner, FALSE);
    ShowWindow(ck.wnd, SW_SHOW);
    UpdateWindow(ck.wnd);
    // 模态消息循环（窗口销毁即退出）
    MSG msg;
    while (ck.wnd && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_QUIT) {
            PostQuitMessage(0);
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    MarkFirstRunDone();
}

} // namespace uacstudio
