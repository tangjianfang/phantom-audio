// GUI 非视觉自检实现，见 gui_selfcheck.h
#include "gui_selfcheck.h"
#include "controller.h"
#include <nlohmann/json.hpp>
#include <sstream>

namespace uacstudio {

namespace {

struct Child {
    HWND h = nullptr;
    RECT r{};          // 主窗口客户区坐标
    std::wstring cls, text;
    int id = 0;
    bool interactive = false; // 按钮/编辑框/下拉框（STATIC 标签与状态灯不参与重叠判定）
};

std::wstring ClassOf(HWND h) {
    wchar_t b[64]{};
    GetClassNameW(h, b, 64);
    return b;
}
std::wstring TextOf(HWND h) {
    wchar_t b[96]{};
    GetWindowTextW(h, b, 96);
    return b;
}

std::vector<Child> Collect(HWND main) {
    std::vector<Child> v;
    for (HWND h = GetWindow(main, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h)) continue;
        Child c;
        c.h = h;
        GetWindowRect(h, &c.r);
        MapWindowPoints(nullptr, main, reinterpret_cast<POINT*>(&c.r), 2);
        c.cls = ClassOf(h);
        c.text = TextOf(h);
        c.id = GetDlgCtrlID(h);
        c.interactive = c.cls == L"Button" || c.cls == L"Edit" || c.cls == L"ComboBox";
        v.push_back(std::move(c));
    }
    return v;
}

int W(const RECT& r) { return r.right - r.left; }
int H(const RECT& r) { return r.bottom - r.top; }

// ---- 离屏像素统计 ----
constexpr UINT32 kSentinel = 0x00646464; // RGB(100,100,100)：没画的像素保持这个值，不会被误判为"深色"

struct Pixels {
    long long total = 0, light = 0, dark = 0, unpainted = 0;
    bool ok = false;
};

Pixels Measure(HWND h) {
    Pixels p;
    RECT rc{};
    GetClientRect(h, &rc);
    const int w = W(rc), ht = H(rc);
    if (w <= 0 || ht <= 0) return p;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -ht; // 自上而下
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        auto* px = static_cast<UINT32*>(bits);
        const long long n = static_cast<long long>(w) * ht;
        for (long long i = 0; i < n; ++i) px[i] = kSentinel;
        HGDIOBJ old = SelectObject(mem, bmp);
        // 先显式擦背景再打印。实测（只读多行 EDIT）：WM_PRINTCLIENT 带 PRF_ERASEBKGND 时只画有文字的行，
        // 不擦整块客户区，导致 90% 以上被误判为"没画"（亮像素仍≈0，即主题其实是对的）。
        SendMessageW(h, WM_ERASEBKGND, reinterpret_cast<WPARAM>(mem), 0);
        SendMessageW(h, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(mem), PRF_CLIENT | PRF_ERASEBKGND);
        GdiFlush();
        for (long long i = 0; i < n; ++i) {
            const UINT32 v = px[i] & 0x00FFFFFF;
            if (v == kSentinel) { ++p.unpainted; continue; }
            const int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
            if (r >= 140 && g >= 140 && b >= 140) ++p.light;
            else if (r <= 60 && g <= 60 && b <= 60) ++p.dark;
        }
        p.total = n;
        p.ok = true;
        SelectObject(mem, old);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return p;
}

std::wstring Pct(long long part, long long whole) {
    std::wostringstream o;
    o.setf(std::ios::fixed);
    o.precision(1);
    o << (whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0) << L"%";
    return o.str();
}

// 一轮几何审计。返回问题文本（空 = 无问题）；notes 收集"不算失败但值得知道"的信息
std::wstring AuditGeometry(HWND main, const std::vector<Child>& kids, std::wstring* notes) {
    std::wostringstream bad;
    RECT cr{};
    GetClientRect(main, &cr);
    for (const auto& c : kids) {
        if (W(c.r) < 4 || H(c.r) < 4) {
            bad << L"  no size: [" << c.cls << L" '" << c.text << L"' id " << c.id << L"] " << W(c.r) << L"x"
                << H(c.r) << L"\n";
            continue;
        }
        if (c.r.left < cr.left || c.r.top < cr.top || c.r.right > cr.right || c.r.bottom > cr.bottom)
            bad << L"  outside client area: [" << c.cls << L" '" << c.text << L"' id " << c.id << L"]\n";
        if (!c.interactive) continue;
        // 命中测试：控件中心点上最顶层的子窗口应该是它自己
        const POINT mid{(c.r.left + c.r.right) / 2, (c.r.top + c.r.bottom) / 2};
        HWND hit = ChildWindowFromPointEx(main, mid, CWP_SKIPINVISIBLE);
        if (hit != c.h)
            bad << L"  covered: [" << c.cls << L" '" << c.text << L"' id " << c.id << L"] centre hits '"
                << (hit ? TextOf(hit) : L"?") << L"'\n";
    }
    for (std::size_t i = 0; i < kids.size(); ++i)
        for (std::size_t j = i + 1; j < kids.size(); ++j) {
            RECT x{};
            if (!IntersectRect(&x, &kids[i].r, &kids[j].r) || W(x) <= 0 || H(x) <= 0) continue;
            const std::wstring a = L"[" + kids[i].cls + L" '" + kids[i].text + L"' id " + std::to_wstring(kids[i].id) + L"]";
            const std::wstring b = L"[" + kids[j].cls + L" '" + kids[j].text + L"' id " + std::to_wstring(kids[j].id) + L"]";
            if (kids[i].interactive && kids[j].interactive)
                bad << L"  overlap: " << a << L" x " << b << L"\n";
            else if (notes)
                *notes += L"  note (label box overlaps a control; text may still fit): " + a + L" x " + b + L"\n";
        }
    return bad.str();
}

} // namespace

SelfCheckResult RunGuiSelfCheck(HWND main, const std::vector<int>& expectedIds) {
    SelfCheckResult res;
    std::wostringstream rep;
    bool themeBad = false, logicBad = false, geomBad = false;

    RECT cr{};
    GetClientRect(main, &cr);
    rep << L"uacstudio --selftest (non-visual)\n";
    rep << L"client area " << W(cr) << L"x" << H(cr) << L" px, DPI " << GetDpiForWindow(main) << L"\n\n";

    // ---- 1. 离屏像素：每个控件深色主题是否生效 ----
    rep << L"[theme] offscreen pixel statistics per control (light > 25% or mostly unpainted = problem)\n";
    const auto kids = Collect(main);
    int measured = 0;
    for (const auto& c : kids) {
        if (static_cast<long long>(W(c.r)) * H(c.r) <= 400) continue; // 状态灯等小控件
        const Pixels p = Measure(c.h);
        if (!p.ok) continue;
        ++measured;
        const bool light = p.light * 100 > p.total * 25;
        const bool unpainted = p.unpainted * 100 > p.total * 60;
        if (light || unpainted) {
            themeBad = true;
            rep << L"  PROBLEM [" << c.cls << L" '" << c.text << L"' id " << c.id << L"] " << W(c.r) << L"x"
                << H(c.r) << L": light " << Pct(p.light, p.total) << L", dark " << Pct(p.dark, p.total)
                << L", unpainted " << Pct(p.unpainted, p.total) << L"\n";
        }
    }
    rep << L"  measured " << measured << L" controls: " << (themeBad ? L"PROBLEMS above" : L"ok") << L"\n\n";

    // ---- 1b. 边框环：带 CLIENTEDGE 的控件，其 2px 非客户区边框必须是深色 ----
    // 起因：2026-11-01 用户截屏发现三个 EDIT 一圈白框——客户区统计永远测不到非客户区，
    // 主题检查于是漏放。这里对主窗口做一次带子窗口的全量渲染，量每个带边控件的边框环带。
    rep << L"[borders] non-client frame ring of EDIT/ComboBox must be dark\n";
    {
        const int bw = GetSystemMetrics(SM_CXEDGE) + 1, bh = GetSystemMetrics(SM_CYEDGE) + 1;
        const int w = W(cr), ht = H(cr);
        bool borderBad = false;
        if (w > 0 && ht > 0) {
            HDC screen = GetDC(nullptr);
            HDC mem = CreateCompatibleDC(screen);
            BITMAPINFO bi{};
            bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -ht, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
            void* bits = nullptr;
            HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bmp && bits) {
                auto* px = static_cast<UINT32*>(bits);
                HGDIOBJ old = SelectObject(mem, bmp);
                SendMessageW(main, WM_PRINT, reinterpret_cast<WPARAM>(mem),
                             PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN | PRF_NONCLIENT);
                GdiFlush();
                int checked = 0;
                for (const auto& c : kids) {
                    if (c.cls != L"Edit" && c.cls != L"ComboBox") continue;
                    ++checked;
                    long long ring = 0, light = 0;
                    long long eT = 0, eB = 0, eL = 0, eR = 0; // 按边分布（诊断）
                    // 组合框窗口高度含下拉列表区（本项目里 200 单位），可见的只是顶部"闭合框"
                    //（itemHeight + 上下边框）。环带必须只绕闭合框——否则采样到相邻控件的文字。
                    int visBottom = c.r.bottom;
                    if (c.cls == L"ComboBox") {
                        const LRESULT ih = SendMessageW(c.h, CB_GETITEMHEIGHT, 0, 0);
                        if (ih > 0) visBottom = c.r.top + static_cast<int>(ih) + 2 * bh;
                    }
                    for (int y = c.r.top; y < visBottom; ++y) {
                        const bool vband = y < c.r.top + bh || y >= visBottom - bh;
                        // 组合框跳过顶带：它 200 单位高的窗体与副标题标签区域重叠，离屏渲染时
                        // 兄弟控件（更低的 Z 序后画）会把文字印进这个带——屏幕实况并非如此。
                        if (c.cls == L"ComboBox" && y < c.r.top + bh) continue;
                        for (int x = c.r.left; x < c.r.right; ++x) {
                            const bool hband = x < c.r.left + bw || x >= c.r.right - bw;
                            if (!vband && !hband) continue; // 只取边框环带
                            if (x < 0 || y < 0 || x >= w || y >= ht) continue;
                            const UINT32 v = px[y * w + x] & 0x00FFFFFF;
                            const int r = (v >> 16) & 0xFF, g = (v >> 8) & 0xFF, b = v & 0xFF;
                            ++ring;
                            if (r >= 140 && g >= 140 && b >= 140) {
                                ++light;
                                if (y < c.r.top + bh) ++eT;
                                else if (y >= visBottom - bh) ++eB;
                                else if (x < c.r.left + bw) ++eL;
                                else ++eR;
                            }
                        }
                    }
                    if (ring > 0 && light * 100 > ring * 40) { // 环带超 40% 亮 = 白框
                        borderBad = true;
                        themeBad = true;
                        { // 诊断：左上 8x8 像素颜色（定位白色到底是谁画的）
                            rep << L"  DBG px@top-left:";
                            for (int y = c.r.top; y < c.r.top + 8 && y < ht; ++y) {
                                rep << L" [";
                                for (int x = c.r.left; x < c.r.left + 8 && x < w; ++x) {
                                    const UINT32 v = px[y * w + x] & 0x00FFFFFF;
                                    wchar_t buf[16];
                                    swprintf(buf, 16, L"%06X,", v);
                                    rep << buf;
                                }
                                rep << L"]";
                            }
                            rep << L"\n";
                        }
                        rep << L"  PROBLEM [" << c.cls << L" id " << c.id << L"] frame ring light "
                            << Pct(light, ring) << L" (T" << eT << L"/B" << eB << L"/L" << eL << L"/R"
                            << eR << L") - white border on a dark theme\n";
                    }
                }
                rep << L"  checked " << checked << L" framed controls: "
                    << (borderBad ? L"PROBLEMS above" : L"ok") << L"\n";
                SelectObject(mem, old);
            }
            if (bmp) DeleteObject(bmp);
            DeleteDC(mem);
            ReleaseDC(nullptr, screen);
        }
    }
    rep << L"\n";

    // ---- 2. 控制层逻辑 ----
    rep << L"[logic] controller self-test\n";
    auto logic = [&](const std::wstring& what, bool ok) {
        if (!ok) { logicBad = true; rep << L"  FAIL: " << what << L"\n"; }
    };
    const std::string actionErrors = ValidateActions();
    if (!actionErrors.empty()) {
        logicBad = true;
        rep << L"  FAIL: action table: " << std::wstring(actionErrors.begin(), actionErrors.end()) << L"\n";
    }
    for (int id : expectedIds) logic(L"control id " + std::to_wstring(id) + L" does not exist", GetDlgItem(main, id) != nullptr);
    for (const auto& a : Actions()) logic(L"action button id " + std::to_wstring(a.id) + L" was not created", GetDlgItem(main, a.id) != nullptr);
    logic(L"profile list is empty", !ProfileNames().empty());
    HWND combo = GetDlgItem(main, 100);
    logic(L"profile combo item count != profile list", combo && static_cast<std::size_t>(SendMessageW(combo, CB_GETCOUNT, 0, 0)) == ProfileNames().size());
    logic(L"BuildSineBody(\"1000\") is empty", !BuildSineBody("1000").empty());
    logic(L"BuildSineBody accepted garbage", BuildSineBody("abc").empty() && BuildSineBody("440abc").empty() && BuildSineBody("-5").empty());
    {
        const char bs = 0x5C;
        const std::string path = std::string("C:") + bs + "tmp" + bs + "a\"b.wav";
        const auto j = nlohmann::json::parse(BuildCaptureBody(path), nullptr, false);
        logic(L"BuildCaptureBody does not round-trip a path with backslash/quote", j.is_object() && j.value("path", "") == path);
    }
    rep << L"  " << (logicBad ? L"PROBLEMS above" : L"ok") << L"\n\n";

    // ---- 3. 几何：当前尺寸与最小窗口尺寸 ----
    rep << L"[geometry] hit-test, bounds and overlap at the current and at the minimum window size\n";
    std::wstring notes;
    std::wstring issues = AuditGeometry(main, kids, &notes);
    if (!issues.empty()) { geomBad = true; rep << L"  at current size:\n" << issues; }

    MINMAXINFO mm{};
    SendMessageW(main, WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&mm));
    RECT wr{};
    GetWindowRect(main, &wr);
    if (mm.ptMinTrackSize.x > 0 && (W(wr) != mm.ptMinTrackSize.x || H(wr) != mm.ptMinTrackSize.y)) {
        SetWindowPos(main, nullptr, 0, 0, mm.ptMinTrackSize.x, mm.ptMinTrackSize.y, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        const auto atMin = Collect(main); // 注意：不能叫 small——rpcndr.h 里 #define small char
        issues = AuditGeometry(main, atMin, nullptr);
        if (!issues.empty()) { geomBad = true; rep << L"  at minimum size " << mm.ptMinTrackSize.x << L"x" << mm.ptMinTrackSize.y << L":\n" << issues; }
        SetWindowPos(main, nullptr, 0, 0, W(wr), H(wr), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    rep << L"  " << (geomBad ? L"PROBLEMS above" : L"ok") << L"\n";
    if (!notes.empty()) rep << notes;

    res.exitCode = themeBad ? 3 : logicBad ? 4 : geomBad ? 5 : 0;
    rep << L"\nexit code " << res.exitCode << L"\n";
    rep << L"NOTE: this proves the controls are dark-themed, not covered, inside the window and that the controller\n"
           L"      logic works. It does NOT judge how the window looks: positions, text fit/truncation, colours,\n"
           L"      DPI scaling and the interaction feel have not been looked at by anyone.\n";
    res.report = rep.str();
    return res;
}

} // namespace uacstudio
