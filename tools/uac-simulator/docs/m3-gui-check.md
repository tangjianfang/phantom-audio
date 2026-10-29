# uacstudio 手验清单（M3 GUI 验收）

> 前置：`uacsimd.exe --http-port 3241` 已运行；GUI 默认连 3241。

- [ ] 窗口出现三面板：Device（Profile 下拉 + Attach/Detach + 状态行）、Inject（10 按钮）、Log（只读滚动区）
- [ ] Attach 后 1s 内状态行出现 `"listening":true` 与 profile 名
- [ ] 点击 Vol+/Vol-/Mute，日志区出现 `"event":"inject_volume"` / `inject_mute`
- [ ] 点击 HID Vol+，日志区出现 `"event":"inject_hid","keys":1`
- [ ] 点击"拔线"，1s 内状态行 `"imported"` 变 false（若 vhci 已导入）
- [ ] 125% / 150% DPI（设置→显示→缩放，或拖到副屏）控件无重叠、无模糊（Per-Monitor-V2）
- [ ] 缩放窗口至最小尺寸（560×420 逻辑）无裁切
- [ ] 截图三张（默认/DPI 变更/注入后）存 `docs/screenshots/`
