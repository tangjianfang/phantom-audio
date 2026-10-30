# uacstudio 验证清单

> 前置：`uacsimd.exe --http-port 3241` 已运行；GUI 默认连 3241。

## 已自动化（不需要人看）

| 检查 | 入口 | 说明 |
|---|---|---|
| 每个控件的深色主题（离屏像素统计）、没被盖住、没跑出窗口、不重叠（含最小窗口尺寸）、控制层逻辑 | `scripts/smoke.ps1`（即 `uacstudio.exe --selftest`） | 不需要管理员/设备；退出码 0/3/4/5；报告 `%TEMP%\uacstudio-selftest.txt` |
| 每个一键按钮发出的请求都被仿真器接受 | `ctest`（`test_controller.cpp`） | 按钮表与服务端 `on_event/on_inject` 不会悄悄漂移 |
| Attach / 音量 / 静音 / HID / 录音 / 注入 / 多实例等 API 链路 | GUI **Auto test** 按钮 / `uacstudio --autotest` | 结果文件在桌面（或 `--out`），第一行 `RESULT: ...` |

## 仍需人工确认（自检**不判断外观**）

- [ ] 窗口外观：布局、对齐、文字是否被截断、配色是否符合设计（深色仪表台面）
- [ ] 125% / 150% DPI（设置→显示→缩放，或拖到副屏）下控件无重叠、无模糊（Per-Monitor-V2）
- [ ] 缩放窗口至最小尺寸（720×480 逻辑）：标题/副标题以"…"收尾而不是压住 Profile 下拉框（几何审计只保证不重叠，不保证观感）
- [ ] Attach 后 1 s 内状态行出现"监听中"与 profile 名；点击"拔线"后 1 s 内状态行回到非"已导入"（若 vhci 已导入）
- [ ] 点击 Vol+/Vol-/Mute/HID，日志区出现对应的 `inject_*` 事件（逻辑已有单测与自动测试覆盖，此处只看显示）
- [ ] 日志区中文显示正确（日志控件已改为 Unicode 收发；在非 UTF-8 代码页系统上看一眼）
- [ ] 若要留档：截图存 `docs/screenshots/`

## 新增控件时

按钮加进 `apps/uacstudio/controller.cpp` 的 `Actions()` 表（布局、重绘色调、`WM_COMMAND` 分发、自检、单测都从这张表走）；
然后 `scripts/smoke.ps1` 通过即可，并把"新增控件的位置/文字没人看过"写进交付说明。
