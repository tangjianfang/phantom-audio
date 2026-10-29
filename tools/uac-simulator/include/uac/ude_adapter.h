#pragma once
// UDE 适配层 —— usbip-win2 issue #35 "三位一体" 法则的描述符侧改造：
//   1) 设备呈现 High-Speed（UsbIpDevice.speed = 3，由服务层设置；bcdUSB 可保留 0x0110）
//   2) ISOCH/中断端点 bInterval >= 4（UDE/USBHUB3 按 125µs 微帧解释；HS 下 4 = 1ms 轮询）
#include <uac/usb_desc.h>
#include <cstdint>
#include <string>
#include <vector>

namespace uac::ude {

// 就地修改配置描述符：iso/int 端点 interval 提升到 >=4。返回修改的端点数。
int adapt_config_for_ude(std::vector<std::uint8_t>& cfg);

// 描述符模板 lint（UAC1/UAC2 通用规则在此，协议专属规则在各自模板文件）
struct LintError {
    std::string code; // 如 "as-alt0-has-endpoint"
    std::string message;
    size_t offset = 0;
};
std::vector<LintError> lint_common(const desc::ConfigView& cfg);

} // namespace uac::ude
