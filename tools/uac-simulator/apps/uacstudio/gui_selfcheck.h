// GUI 非视觉自检（win32-gui-tool-dev：--selftest）。
// 不需要管理员、不需要设备、不弹框、不截图。抓这几类回归：
//   - 控件没套上深色主题 / 什么都没画（离屏像素统计，WM_PRINTCLIENT 打进内存位图）
//   - 控件被别的控件盖住、跑出窗口、尺寸为 0、互相重叠（几何审计 + 命中测试，含最小窗口尺寸）
//   - 控制层自己的逻辑坏了（按钮表、请求体构建）
// 它【不判断好不好看】：位置是否美观、文字是否被截断、配色、DPI 观感都不在检查范围内。
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>

namespace uacstudio {

struct SelfCheckResult {
    // 0 通过；3 主题；4 控制层逻辑；5 几何。多类问题并存时按 3、4、5 的顺序取第一个
    int exitCode = 0;
    std::wstring report;
};

// main：已显示的主窗口。expectedIds：必须存在的控件 ID（缺了说明窗口没建全）。
SelfCheckResult RunGuiSelfCheck(HWND main, const std::vector<int>& expectedIds);

} // namespace uacstudio
