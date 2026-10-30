#pragma once
// 远程输入（HTTP 控制 API）的名字/路径校验：纯函数，无 I/O，便于单测。
// 原则：用白名单描述"允许什么"，而不是黑名单列"禁止什么"——此前 profile 名的黑名单
// 把 '\' 写成了 "\."（C++ 转义成 '.'），反斜杠根本没进黑名单，UNC 路径可以绕过。
#include <string>

namespace uac::policy {

// profile 名：1..64 个 [A-Za-z0-9_-]，可带 ".json" 后缀。
// 不含任何路径分隔符、冒号、点路径，所以无法表达绝对/相对/UNC 路径。
bool valid_profile_name(const std::string& name);

// 媒体文件路径（/capture 写入、/source 读取）。要求：
//  - 以 ".wav" 结尾（大小写不敏感），因此即使被滥用也不能覆盖 .bashrc / .dll 之类的文件；
//  - 不是 UNC / 设备命名空间路径（以 \\ 或 // 开头，含 \\?\ 与 \\.\）——避免触发 SMB 外联；
//  - 除盘符冒号（第 2 个字符，且其后必须是分隔符）外不含 ':'——拒绝 NTFS 备用数据流和 "C:foo"；
//  - 无 ".." 路径分量；无控制字符与 Windows 非法字符 "<>|*?；
//  - 任一分量的主干名不是保留设备名（CON/PRN/AUX/NUL/COM1-9/LPT1-9，"con.wav" 同样是设备）；
//  - 总长不超过 259。
// 失败时 *why（可为空）给出原因。
bool valid_media_path(const std::string& path, std::string* why = nullptr);

} // namespace uac::policy
