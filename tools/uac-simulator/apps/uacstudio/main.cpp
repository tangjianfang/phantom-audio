// uacstudio —— UAC 仿真测试台 Win32 GUI（最小版：Device/Inject/Log 三面板）
// 设计：纯标准控件（BUTTON/COMBOBOX/EDIT），不自绘 —— 无 GDI+ 生命周期风险；
//       Per-Monitor-V2 DPI；逻辑坐标布局（96 基准，绘制时按窗口 DPI 换算）。
//       所有操作经 127.0.0.1 控制 API（与 CLI/CI 同源）。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "http_client.h"
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace {

constexpr uint16_t kDefaultPort = 3241;
uint16_t g_port = kDefaultPort;

struct Ctl {
    HWND wnd = nullptr;
    void create(const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle, HWND parent,
                int id, HINSTANCE hi) {
        wnd = CreateWindowExW(exStyle, cls, text, style, 0, 0, 10, 10, parent,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), hi, nullptr);
        SendMessageW(wnd, WM_SETFONT,
                     reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }
};

Ctl g_profileCombo, g_attachBtn, g_detachBtn, g_statusStatic;
Ctl g_btnVolUp, g_btnVolDown, g_btnMute, g_btnHidUp, g_btnHidDown, g_btnHidMute, g_btnHidRel;
Ctl g_btnStall, g_btnDelay, g_btnDisconnect, g_btnClear;
Ctl g_logEdit;
HWND g_main = nullptr;

const wchar_t* kProfiles[] = {L"uac2-48k", L"uac1-cmedia-sim", L"uac2-hid-headset", L"uac2-sim"};

int Scale(HWND hwnd, int logical) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return logical * static_cast<int>(dpi) / 96;
}

void DoLayout() {
    if (!g_main) return;
    RECT rc{};
    GetClientRect(g_main, &rc);
    const int W = rc.right, H = rc.bottom;
    const int m = Scale(g_main, 10);
    int y = m;

    // ---- Device 面板 ----
    MoveWindow(g_profileCombo.wnd, m, y, W - 2 * m - Scale(g_main, 180), Scale(g_main, 24), TRUE);
    MoveWindow(g_attachBtn.wnd, W - m - Scale(g_main, 175), y, Scale(g_main, 85),
               Scale(g_main, 24), TRUE);
    MoveWindow(g_detachBtn.wnd, W - m - Scale(g_main, 85), y, Scale(g_main, 85),
               Scale(g_main, 24), TRUE);
    y += Scale(g_main, 30);
    MoveWindow(g_statusStatic.wnd, m, y, W - 2 * m, Scale(g_main, 20), TRUE);
    y += Scale(g_main, 28);

    // ---- Inject 面板（两行按钮）----
    const int btnW = Scale(g_main, 92), btnH = Scale(g_main, 26), gap = Scale(g_main, 6);
    Ctl row1[] = {g_btnVolUp, g_btnVolDown, g_btnMute, g_btnHidUp, g_btnHidDown};
    for (int i = 0; i < 5; ++i)
        MoveWindow(row1[i].wnd, m + i * (btnW + gap), y, btnW, btnH, TRUE);
    y += btnH + gap;
    Ctl row2[] = {g_btnHidMute, g_btnHidRel, g_btnStall, g_btnDelay, g_btnDisconnect,
                  g_btnClear};
    for (int i = 0; i < 6; ++i)
        MoveWindow(row2[i].wnd, m + i * (btnW + gap), y, btnW, btnH, TRUE);
    y += btnH + Scale(g_main, 10);

    // ---- Log 面板 ----
    MoveWindow(g_logEdit.wnd, m, y, W - 2 * m, std::max(H - y - m, Scale(g_main, 60)), TRUE);
}

void AppendLog(const std::string& text) {
    int len = GetWindowTextLengthA(g_logEdit.wnd);
    SendMessageA(g_logEdit.wnd, EM_SETSEL, len, len);
    SendMessageA(g_logEdit.wnd, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>((std::string(text) + "\r\n").c_str()));
}

std::string CurrentProfile() {
    wchar_t buf[128]{};
    GetWindowTextW(g_profileCombo.wnd, buf, 128);
    std::string s;
    for (int i = 0; buf[i]; ++i) s.push_back(static_cast<char>(buf[i]));
    return s;
}

void RefreshStatus() {
    const auto r = uacstudio::http_req(g_port, "GET", "/status", "");
    if (r.empty()) {
        SetWindowTextA(g_statusStatic.wnd, "status: uacsimd 未连接 (127.0.0.1:3241)");
        return;
    }
    SetWindowTextA(g_statusStatic.wnd, ("status: " + r).c_str());
}

void RefreshLog() {
    const auto r = uacstudio::http_req(g_port, "GET", "/log?n=200", "");
    if (r.empty() || r.size() < 3) return;
    if (r.size() > 60000) return; // 防巨量重绘
    SetWindowTextA(g_logEdit.wnd, r.c_str());
    SendMessageA(g_logEdit.wnd, EM_SETSEL, r.size() - 1, r.size() - 1); // 滚到底
}

void Post(const std::string& path, const std::string& body, const char* what) {
    std::string out;
    if (uacstudio::http_post(g_port, path, body, &out))
        AppendLog(std::string("[") + what + "] " + out);
    else
        AppendLog(std::string("[") + what + "] 请求失败（uacsimd 未启动？）");
    RefreshStatus();
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        const DWORD vis = WS_CHILD | WS_VISIBLE;
        g_profileCombo.create(L"COMBOBOX", nullptr, vis | CBS_DROPDOWNLIST | WS_VSCROLL, 0, h, 100,
                              cs->hInstance);
        for (auto* p : kProfiles) SendMessageW(g_profileCombo.wnd, CB_ADDSTRING, 0,
                                               reinterpret_cast<LPARAM>(p));
        SendMessageW(g_profileCombo.wnd, CB_SETCURSEL, 0, 0);
        g_attachBtn.create(L"BUTTON", L"Attach", vis, 0, h, 101, cs->hInstance);
        g_detachBtn.create(L"BUTTON", L"Detach", vis, 0, h, 102, cs->hInstance);
        g_statusStatic.create(L"STATIC", L"status: -", vis, 0, h, 103, cs->hInstance);
        g_btnVolUp.create(L"BUTTON", L"Vol +", vis, 0, h, 200, cs->hInstance);
        g_btnVolDown.create(L"BUTTON", L"Vol -", vis, 0, h, 201, cs->hInstance);
        g_btnMute.create(L"BUTTON", L"Mute", vis, 0, h, 202, cs->hInstance);
        g_btnHidUp.create(L"BUTTON", L"HID Vol+", vis, 0, h, 203, cs->hInstance);
        g_btnHidDown.create(L"BUTTON", L"HID Vol-", vis, 0, h, 204, cs->hInstance);
        g_btnHidMute.create(L"BUTTON", L"HID Mute", vis, 0, h, 205, cs->hInstance);
        g_btnHidRel.create(L"BUTTON", L"HID 释放", vis, 0, h, 206, cs->hInstance);
        g_btnStall.create(L"BUTTON", L"STALL", vis, 0, h, 210, cs->hInstance);
        g_btnDelay.create(L"BUTTON", L"延迟注入", vis, 0, h, 211, cs->hInstance);
        g_btnDisconnect.create(L"BUTTON", L"拔线", vis, 0, h, 212, cs->hInstance);
        g_btnClear.create(L"BUTTON", L"清注入", vis, 0, h, 213, cs->hInstance);
        g_logEdit.create(L"EDIT", nullptr,
                         vis | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, WS_EX_CLIENTEDGE,
                         h, 300, cs->hInstance);
        SetTimer(h, 1, 1000, nullptr);  // 状态轮询
        SetTimer(h, 2, 500, nullptr);   // 日志轮询
        DoLayout();
        RefreshStatus();
        return 0;
    }
    case WM_SIZE: DoLayout(); return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize = {Scale(h, 560), Scale(h, 420)};
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) RefreshStatus();
        if (wp == 2) RefreshLog();
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        switch (id) {
        case 101: Post("/attach", "{\"profile\":\"" + CurrentProfile() + "\"}", "attach"); break;
        case 102: Post("/detach", "", "detach"); break;
        case 200: Post("/event", "{\"type\":\"volume_up\"}", "vol+"); break;
        case 201: Post("/event", "{\"type\":\"volume_down\"}", "vol-"); break;
        case 202: Post("/event", "{\"type\":\"mute\"}", "mute"); break;
        case 203: Post("/event", "{\"type\":\"hid_vol_up\"}", "hid+"); break;
        case 204: Post("/event", "{\"type\":\"hid_vol_down\"}", "hid-"); break;
        case 205: Post("/event", "{\"type\":\"hid_mute\"}", "hidMute"); break;
        case 206: Post("/event", "{\"type\":\"hid_release\"}", "hidRel"); break;
        case 210: Post("/inject", "{\"kind\":\"stall\",\"bRequest\":129,\"nth\":1}", "stall"); break;
        case 211: Post("/inject", "{\"kind\":\"delay_ms\",\"bRequest\":6,\"ms\":200}", "delay"); break;
        case 212: Post("/inject", "{\"kind\":\"disconnect\",\"afterMs\":0}", "拔线"); break;
        case 213: Post("/inject", "{\"kind\":\"clear\"}", "清注入"); break;
        default: break;
        }
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(h, msg, wp, lp);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int nCmdShow) {
    // DPI 感知必须在任何窗口/度量调用之前
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"UacStudio";
    RegisterClassW(&wc);

    g_main = CreateWindowExW(0, L"UacStudio", L"uacstudio — UAC 仿真控制台", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, 760, 560, nullptr, nullptr, hi, nullptr);
    ShowWindow(g_main, nCmdShow);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    WSACleanup();
    return static_cast<int>(msg.wParam);
}
