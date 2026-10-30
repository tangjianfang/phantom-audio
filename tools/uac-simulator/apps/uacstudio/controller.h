// uacstudio 控制层：按钮表、请求体构建、自检校验——全部是数据进数据出，不碰窗口。
// GUI（main.cpp）只负责"读控件 → 调这里 → 显示结果"；单测与 --selftest 共用同一份逻辑
// （win32-gui-tool-dev 原则 1/2）。一键操作做成数据表驱动：加一个按钮只改 Actions()。
#pragma once
#include <string>
#include <vector>

namespace uacstudio {

enum class Tone { Neutral, Accent, Fault, Quiet };

// 一个固定请求体的"一键"按钮
struct Action {
    int id;              // 控件 ID（WM_COMMAND）
    const wchar_t* label; // 按钮文字
    const char* path;    // 控制 API 路径
    const char* body;    // 固定 JSON 请求体
    const char* what;    // 日志里的名字（UTF-8）
    Tone tone;
};

// 第一排按钮数（其余排第二排）；布局只依赖这个数，不依赖具体按钮
constexpr int kActionsInFirstRow = 5;

const std::vector<Action>& Actions();
const Action* FindAction(int id);

// Profile 下拉框的内容。每一项必须能被仿真器 attach（见单测）。
const std::vector<std::wstring>& ProfileNames();

// 动态请求体：用 JSON 库序列化，路径里的反斜杠/引号才不会产生非法 JSON
std::string BuildAttachBody(const std::string& profile);

// GUI 的 Attach 按钮发的请求序列。模拟器的 /attach 是"再追加一个实例"（最多 4 个，每个占一个 vhci 端口），
// 而用户点 Attach 想要的是"切换到这个设备"，所以先 /detach 再 /attach；多实例仍可用命令行 `uacsim attach`。
struct Step {
    std::string path, body, what;
};
std::vector<Step> BuildAttachPlan(const std::string& profile);

// 轮询到的 /status 应答 → 顶部状态灯 + 文字。文字带当前 profile 名；实例数大于 1 时注明，
// 免得用户连点几次 Attach 叠出一堆实例却看不出来。解析失败当作"没有信息"（灯灭）。
struct StatusView {
    int led = 0; // 0 闲置  1 监听中（等 usbip attach）  2 已被 Windows 导入
    std::wstring text;
};
StatusView DescribeStatus(const std::string& statusBody);
std::string BuildCaptureBody(const std::string& wavPath); // 空串 = 停止录音
// 频率输入框 → 请求体。非数字、非有限、不在 (0, 24000] Hz 内返回空串（调用方提示"频率无效"）。
std::string BuildSineBody(const std::string& hzText);

// 自检：按钮表本身是否自洽（ID 唯一、路径是已知路由、请求体是合法 JSON 对象）。
// 返回空串 = 通过，否则是带名字的错误文本。ValidateActionTable 接受任意表，供单测喂坏表验证检查有牙齿。
std::string ValidateActionTable(const std::vector<Action>& table);
std::string ValidateActions();

} // namespace uacstudio
