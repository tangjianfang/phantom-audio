# M0 链路探针报告

> 执行日期：____ ｜ 执行人：____ ｜ usbip-win2 版本：v0.9.7.8（锁定）｜ VM：Ubuntu 24.04（内核 ____）

## 结论：Go / No-Go

- [ ] Windows 枚举到 f_uac2 声卡（Get-PnpDevice, Class=MEDIA）
- [ ] 绑定 usbaudio2.sys（DEVPKEY_Device_Service）
- [ ] 声音设置中手动播放/录制 10s 正常（无持续爆音/快进）
- [ ] 黄金抓包已存档（m0.pcap）

## 观察项

| 项 | 结果 |
|---|---|
| iso 传输经 UDE 是否正常（attach 后流是否建立） | |
| 中断端点是否被主机读取（VM dmesg int ep 活动） | |
| #200 QueryBusTime 纪元问题是否可见（长时间流是否漂移） | |
| attach/detach 循环稳定性（3 次） | |

## 协议层黄金校正结论

- [ ] m0.pcap 中 CMD_SUBMIT iso 描述符双方向形态与 tools/uac-simulator 实现一致（Task 2 假设验证）
- [ ] 非 ISO 的 number_of_packets 实际取值（0 / 0xffffffff）

## 回退触发

No-Go 时按 RFC v2：评估锁旧版重测 → B 档（Linux VM 长期运行，可保 R6）→ C 档（自研 UDE，重新评审）。
