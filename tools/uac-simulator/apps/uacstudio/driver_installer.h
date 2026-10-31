// usbip-win2 精简载荷的校验与安装（取代 25 MB 的官方安装包）。
//
// 载荷目录布局（由 sandbox\collect-usbip-payload.bat 在装过官方包的 Sandbox 里收集）：
//   bin\      devnode.exe usbip.exe libusbip.dll resources.dll
//   driver\   usbip2_ude.inf_amd64_*\{.inf .sys .cat}   usbip2_filter.inf_amd64_*\{.inf .sys .cat}   （微软签名，不可改）
//   runtime\  vcruntime140.dll vcruntime140_1.dll msvcp140.dll msvcp140_atomic_wait.dll   （官方二进制要求 14.51+）
//   MANIFEST.txt   每个文件的 SHA256
//
// 安装步骤复刻官方 Inno Setup 脚本（setup.iss）里真正起作用的三条命令，不含 VC redist、GUI、SDK、PDB：
//   pnputil /add-driver usbip2_filter.inf /install
//   devnode install usbip2_ude.inf ROOT\USBIP_WIN2\UDE
// 重启：官方包写死 AlwaysRestart；devnode 只在自己判定需要时才打印 "Reboot is required"，我们按这句话如实报告。
//
// 会往系统里装内核驱动，因此只在一次性机器（Sandbox/VM）上运行：默认守卫 = HostCasesAllowed(true)。
// 所有会碰系统的动作都做成 InstallDeps 里的缝，单测用假的执行器验证整个流程；真正的安装只发生在 Sandbox 里。
#pragma once
#include "auto_test.h"
#include <functional>
#include <string>
#include <vector>

namespace uacstudio {

struct DriverPayload {
    std::string root;
    std::string udeInf, filterInf;       // driver\ 下对应包里 .inf 的完整路径
    std::vector<std::string> binFiles;   // 完整路径
    std::vector<std::string> runtimeFiles;
};

struct PayloadCheck {
    bool ok = false;
    std::string error;                   // 第一个问题；ok 时为空
    std::vector<std::string> notes;      // 非致命信息（"MANIFEST 已核对 14 个文件"、"没有 runtime\"……）
    DriverPayload payload;
};

// 定位并校验载荷目录。有 MANIFEST.txt 时逐文件核对 SHA256，缺失/不一致一律算错。
PayloadCheck CheckPayload(const std::string& root);

// 小写十六进制 SHA256（Windows CNG）；读不了文件返回空串。
std::string Sha256File(const std::string& path);

struct DriverStatus {
    bool serviceInstalled = false;       // 服务 usbip2_ude
    std::string serviceState;
    bool usbipPresent = false;           // usbip.exe 在 installDir 或 PATH 里
    std::string usbipPath;
};
DriverStatus DetectDriver(const std::string& installDir = "");

struct InstallStep {
    std::string name;
    bool ok = false;
    std::string detail;
};

struct InstallResult {
    bool ok = false;
    bool refused = false;                // 守卫拒绝：什么都没动
    bool rebootRequired = false;
    std::string installDir;
    std::vector<InstallStep> steps;
    std::string Summary() const;         // 多行、给人看
};

// 可替换的系统交互。默认值（留空）指向真实系统。
struct InstallDeps {
    std::function<ToolResult(const std::string& exe, const std::string& args, int timeoutMs)> run; // 默认 RunTool
    std::function<bool(std::string* why)> allowed;       // 默认 HostCasesAllowed(true, why)
    std::function<bool()> isAdmin;                       // 默认：令牌里 Administrators 组已启用
    std::function<bool(std::string* state)> serviceInstalled; // 默认：服务 usbip2_ude
    std::string infDir;                                  // 默认 %SystemRoot%\INF（卸载时扫描 oem*.inf）
    std::function<void(const std::string&)> log;
};

struct InstallOptions {
    std::string payloadRoot;
    std::string installDir;              // 默认 %ProgramFiles%\USBip（与官方包一致，autotest 的 FindUsbip 能找到）
};

std::string DefaultInstallDir();

// 本进程令牌是否带启用的 Administrators 组（UAC 未提权 = false；UAC 关闭/已提权 = true）。
// host.* 用例与 Attach to Windows 都需要它：usbip attach 只认管理员。
bool IsProcessAdmin();

// Fills every empty seam of `deps` with the real implementation (so other modules - host_ops - share the same defaults).
InstallDeps ResolveDeps(InstallDeps deps);

// 步骤：守卫 -> 管理员 -> 校验载荷 -> 拷贝文件 -> pnputil(filter) -> devnode install(ude) -> 校验服务。第一个失败即停。
InstallResult InstallDriver(const InstallOptions& opt, const InstallDeps& deps = {});

// 步骤：守卫 -> 管理员 -> devnode remove -> pnputil /delete-driver（扫描 oem*.inf 里属于 usbip2_* 的）-> 只删我们自己拷的文件。
InstallResult UninstallDriver(const InstallOptions& opt, const InstallDeps& deps = {});

} // namespace uacstudio
