// 一键自动测试（win32-gui-tool-dev / case-runner）：用例表 + 自判对错 + 单用例超时 + 结果文件。
//
// 三个入口共用同一张用例表（不要为新功能另起按钮或脚本）：
//   1. GUI "自动测试" 按钮        2. 命令行 uacstudio.exe --autotest
//   3. Sandbox 脚本 sandbox/run-in-sandbox.ps1（调用 2）
//
// 用例分两类：
//   api.*   只依赖 uacsimd 的 HTTP 控制 API，任何机器都能跑（单测里对着进程内 SimHost 跑）；
//   host.*  需要 usbip-win2 驱动 + 真正的 Windows 音频栈（attach、WASAPI 枚举、音量回调、采集），
//           只在一次性机器（Windows Sandbox / VM）上、且显式加 --allow-host 时运行，否则 SKIP。
#pragma once
#include "http_client.h"
#include <nlohmann/json.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace uacstudio {

enum class CheckStatus { Pass, Fail, Skip };
const char* StatusName(CheckStatus s);

struct CheckInfo {
    std::string id;      // "area.name"
    std::string feature; // 所属功能，用于分组
    std::string title;   // 验证什么（一句话，写给人看）
};

struct CheckResult {
    std::string id;
    CheckStatus status = CheckStatus::Skip;
    std::string detail;
    long long ms = 0;
};

struct AutoTestOptions {
    std::uint16_t port = 3241;
    std::string exeDir;             // host_probe.exe 所在目录；空 = 本进程 exe 所在目录
    std::string reportDir;          // 结果文件目录；空 = 桌面，失败回退 %TEMP%
    bool allowHost = false;         // 请求运行 host.* 用例（仍需通过一次性机器守卫）
    // host.* 用例使用的 Profile。同一张用例表换 uac1-cmedia-sim 再跑一遍，
    // host.volume_event 的结果就回答 R-006：usbaudio.sys 是否响应 UAC1 的 Status Word 中断
    std::string hostProfile = "uac2-48k";
    int caseTimeoutSec = 60;        // 单用例默认超时
    std::function<void(const std::string&)> log; // 进度行；可能在工作线程调用
    std::atomic<bool>* cancel = nullptr;         // 在两个用例之间检查
};

struct AutoTestOutcome {
    std::vector<CheckResult> results;
    std::string reportPath;
    int passed = 0, failed = 0, skipped = 0;
};

// 用例之间共享的前提，只算一次、谁需要谁请求（权限、驱动、端点基线……）
struct Fixture {
    bool apiUp = false;
    bool hostEnabled = false;
    std::string hostWhy; // hostEnabled=false 时的原因（SKIP 文案）
    bool driver = false;
    std::string usbipExe, probeExe;
    std::set<std::string> baselineIds; // attach 之前已有的音频端点
    std::string newRenderId, newCaptureId;
    std::set<std::string> usbipPortsBefore, usbipPortsOurs;
    bool hostTouched = false; // 已对系统做过 attach（cleanup 才需要还原）
    bool attached = false;    // usbip attach 成功（退出码 0）。与"音频端点出现"无关：驱动认领检查只需要前者
};

struct CheckFailed { std::string why; };
struct CheckSkipped { std::string why; };

struct Shared; // 一次运行的共享状态（日志、选项、Fixture）；工作线程超时被放弃后仍需存活，故 shared_ptr

class Ctx {
public:
    explicit Ctx(std::shared_ptr<Shared> s) : s_(std::move(s)) {}

    [[noreturn]] void Fail(const std::string& why) { throw CheckFailed{why}; }
    [[noreturn]] void Skip(const std::string& why) { throw CheckSkipped{why}; } // 做不到，以及为什么；绝不悄悄通过
    void Expect(bool ok, const std::string& why) { if (!ok) Fail(why); }
    void Note(const std::string& detail) { note_ = detail; } // 通过时显示的一句话
    const std::string& note() const { return note_; }
    void Log(const std::string& line);

    const AutoTestOptions& opts() const;
    Fixture& fx();

    std::string Get(const std::string& path);
    std::string Post(const std::string& path, const std::string& body = "",
                     const std::string& extraHeaders = "");
    // 解析失败/无应答 → Fail（带上原始文本，便于从结果文件直接看出原因）
    nlohmann::json GetJson(const std::string& path);
    nlohmann::json PostJson(const std::string& path, const std::string& body = "");

private:
    std::shared_ptr<Shared> s_;
    std::string note_;
};

using CaseFn = void (*)(Ctx&);
struct CaseDef {
    CheckInfo info;
    CaseFn fn = nullptr;
    int timeoutSec = 0; // 0 = opts.caseTimeoutSec
};

// 完整用例表（按运行顺序）。首项 env.api，末项 cleanup.devices。
const std::vector<CaseDef>& Cases();
std::vector<CheckInfo> ListChecks();

// 运行一个外部程序并收集合并后的 stdout/stderr；超时则终止进程。host.* 用例靠它调用
// usbip.exe / host_probe.exe，导出来是为了能在开发机上单测它（不必等到 Sandbox 里第一次运行）。
struct ToolResult {
    unsigned long code = 0; // 退出码（超时被终止时为 1）
    bool started = false, timedOut = false;
    std::string out;
};
ToolResult RunTool(const std::string& exe, const std::string& args, int timeoutMs);

// 一次性机器：Windows Sandbox（用户 WDAGUtilityAccount）、Hyper-V 来宾、常见虚拟机 BIOS 标识
bool IsDisposableMachine(std::string* why = nullptr);
// host.* 是否允许运行：flag && (一次性机器 || 环境变量 UACSIM_FORCE_HOST_TESTS=1)
bool HostCasesAllowed(bool flag, std::string* why);

// 运行用例表（或自定义用例，供单测检验运行器本身）。不抛异常。
AutoTestOutcome RunCases(const AutoTestOptions& opts, const std::vector<CaseDef>& cases);
inline AutoTestOutcome RunAutoTest(const AutoTestOptions& opts) { return RunCases(opts, Cases()); }

// 自动测试覆盖不到、必须人工确认的事项（写进报告，不悄悄省略）
const std::vector<std::string>& ManualChecklist();

// 报告文本：RESULT 行在最前，其后每用例一行，然后人工确认清单，最后是完整日志
std::string FormatReport(const std::vector<CheckResult>& results, const std::string& fullLog);

} // namespace uacstudio
