# UAC 仿真测试台 M0–M4 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在单台 Windows 上交付纯软件 UAC1/UAC2 虚拟 USB 音频设备仿真测试台（uacsimd 核心 + CLI + Win32 GUI + host-probe + 自动化回归），走完 M0 链路探针到 M4 收尾。

**Architecture:** usbip-win2（已签名 UDE vhci，版本锁定）作为虚拟 USB 主控；自研 C++20 用户态进程实现 USB/IP v1.1.1 server 侧协议、UAC1/UAC2 描述符引擎（含 UDE 适配层）、AudioControl/AudioStreaming 处理（含虚拟微帧时钟 pacing）、事件与错误注入、HTTP 控制 API；三客户端（CLI/GUI/CI）+ host-probe 主机侧断言。

**Tech Stack:** C++20 / MSVC 19.3x（VS2022 Community）/ CMake≥3.24 + Ninja / Winsock2 / Win32 GDI / doctest（vendored）/ nlohmann-json（vendored）/ PowerShell 脚本 / Hyper-V + Ubuntu 24.04（仅 M0 探针）。

## Global Constraints

- 仅 x64 Windows；MSVC 工具链 `cmake --preset win-x64-release`（预设统一定义）。
- 第三方仅两个 vendored 单头：`third_party/doctest/doctest.h`（MIT）、`third_party/nlohmann/json.hpp`（MIT）；其余（HTTP/WAV/Goertzel/JSON 使用之外的一切）手写，无包管理器。
- 仿真器进程只监听 `127.0.0.1`；USB/IP 端口 3240，控制 API 端口 3241。
- usbip-win2 驱动版本锁定 **v0.9.7.8**（M0 Task 9 复核；升级需重跑回归）。
- 目标命名：核心库 `uac_core`，进程 `uacsimd.exe`（服务）、`uacsim.exe`（CLI）、`uacstudio.exe`（GUI）、`host_probe.exe`（主机断言）。
- 所有代码路径相对仓库根 `C:\tjf\github\phantom-audio`；源码根 `tools/uac-simulator/`。
- 提交信息格式：`<type>: <desc>` + 空行 + 详情，结尾 `Co-Authored-By: Claude Code <noreply@anthropic.com>`；type ∈ {feat,test,docs,build,chore,fix}。
- 测试策略：核心库 100% 分支/行覆盖（doctest 单测，OpenCppCoverage 度量）；驱动/系统相关为"门控集成测试"（脚本检测 vhci 存在才跑）；每任务 TDD（先测后码）。
- 需要用户提权执行的步骤统一收敛到 `scripts/elevated-*.ps1`，由用户以管理员 PowerShell 运行（`! powershell -File ...` 或右键管理员运行）。
- 字节序约定（依据内核权威文档 Documentation/usb/usbip_protocol.rst 已核实）：**全协议大端（network byte order）**，含 URB 头（48B：basic 20B + cmd/ret 28B）；`OP_REQ_DEVLIST=0x8005/OP_REP_DEVLIST=0x0005/OP_REQ_IMPORT=0x8003/OP_REP_IMPORT=0x0003`；非 iso 的 `number_of_packets=0xffffffff`；iso_packet_descriptor 16B×n。经 `include/uac/endian.h` 的 `rd_be/wr_be` 集中处理。

---

## Phase A — 骨架与 USB/IP 协议层（全自动）

### Task 1: 仓库骨架 + CMake 预设 + doctest 冒烟

**Files:**
- Create: `tools/uac-simulator/CMakeLists.txt`
- Create: `tools/uac-simulator/CMakePresets.json`
- Create: `tools/uac-simulator/third_party/doctest/doctest.h`（从官方单头复制，版本 2.4.12）
- Create: `tools/uac-simulator/third_party/nlohmann/json.hpp`（单头 3.11.x）
- Create: `tools/uac-simulator/tests/test_smoke.cpp`
- Create: `tools/uac-simulator/scripts/envcheck.ps1`

**Interfaces:**
- Produces: 构建系统（preset `win-x64-debug`/`win-x64-release`，测试目标 `uac_tests`）；后续所有任务向此 CMake 追加目标。

- [x] **Step 1: 写目录与 CMake**

`tools/uac-simulator/CMakeLists.txt`：

```cmake
cmake_minimum_required(VERSION 3.24)
project(uac_simulator LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:preprocessor /MP)
endif()

add_library(uac_core STATIC)            # 源码在 Task 2+ 填充
target_include_directories(uac_core PUBLIC include third_party)

enable_testing()
add_executable(uac_tests tests/test_smoke.cpp)
target_link_libraries(uac_tests PRIVATE uac_core)
add_test(NAME uac_tests COMMAND uac_tests)
```

`CMakePresets.json`（节选关键字段，完整文件含 configurePreset `win-x64-debug`/`win-x64-release`：generator Ninja、binaryDir `${sourceDir}/build/{preset}`、cacheVariable CMAKE_BUILD_TYPE）：

```json
{
  "version": 6,
  "configurePresets": [
    { "name": "win-x64-debug", "generator": "Ninja", "binaryDir": "${sourceDir}/build/debug",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" },
      "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" } },
    { "name": "win-x64-release", "generator": "Ninja", "binaryDir": "${sourceDir}/build/release",
      "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" } }
  ],
  "buildPresets": [
    { "name": "win-x64-debug", "configurePreset": "win-x64-debug" },
    { "name": "win-x64-release", "configurePreset": "win-x64-release" }
  ],
  "testPresets": [
    { "name": "win-x64-debug", "configurePreset": "win-x64-debug", "output": { "outputOnFailure": true } }
  ]
}
```

- [x] **Step 2: 写冒烟测试**

`tests/test_smoke.cpp`：

```cpp
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <uac/version.h>

TEST_CASE("version") { CHECK(uac::kVersion[0] == 'v'); }
```

配套 `include/uac/version.h`：`#pragma once` + `namespace uac { inline constexpr char kVersion[] = "v0.1.0-m1"; }`

- [x] **Step 3: 构建+运行验证**

```bash
cd tools/uac-simulator && cmake --preset win-x64-debug && cmake --build --preset win-x64-debug && ctest --preset win-x64-debug
```
预期：构建成功，1 test PASS。

- [x] **Step 4: envcheck 脚本**

`scripts/envcheck.ps1`：输出 cmake/ninja/cl 版本、是否管理员、HypervisorPresent、C 盘剩余 GB、vhci 服务 `usbip2_ude` 是否存在、`Get-VMSwitch` 是否可用（try/catch）。纯只读，非提权可跑。

- [x] **Step 5: Commit**

```bash
git add tools/uac-simulator
git commit -m "build: uac-simulator 骨架（CMake 预置 + doctest 冒烟 + envcheck）

Co-Authored-By: Claude Code <noreply@anthropic.com>"
```

---

### Task 2: USB/IP 协议编解码层（含字节级黄金测试）

**Files:**
- Create: `include/uac/endian.h`、`include/uac/usbip_proto.h`
- Create: `src/usbip_proto.cpp`
- Create: `tests/test_usbip_proto.cpp`
- Modify: `CMakeLists.txt`（uac_core 加 `src/usbip_proto.cpp`）

**Interfaces:**
- Produces:
  - `namespace uac::usbip`：`struct BasicHeader{u32 command,seqnum;u64 devid;u32 direction,ep;}`、`struct CmdSubmit{u32 transfer_flags;i32 transfer_buffer_length,start_frame,number_of_packets,interval;u8 setup[8];}`、`struct RetSubmit{i32 status,actual_length,start_frame,number_of_packets,error_count;}`、`struct CmdUnlink{u32 unlink_seqnum;}`、`struct RetUnlink{i32 status;}`
  - `constexpr u32 OP_REQ_DEVLIST=0x8000, OP_REP_DEVLIST=0x0001, OP_REQ_IMPORT=0x8003, OP_REP_IMPORT=0x0003, OP_CMD_SUBMIT=0x0001, OP_CMD_UNLINK=0x0002, OP_RET_SUBMIT=0x0003, OP_RET_UNLINK=0x0004;`
  - `struct UsbIpDevice{std::string path,busid;u32 busnum,devnum,speed;u16 idVendor,idProduct,bcdDevice;u8 bDeviceClass,bDeviceSubClass,bDeviceProtocol,bConfigurationValue,bNumConfigurations,bNumInterfaces;}`
  - `std::vector<u8> encode_devlist_reply(const std::vector<UsbIpDevice>&)`、`std::optional<ImportRequest> decode_import_req(span<const u8>)`、`std::vector<u8> encode_import_reply(const UsbIpDevice&, u32 status)`、`usize decode_urb_header(span<const u8>, BasicHeader&, CmdSubmit&)`（返回消费字节数）
  - `endian.h`：`rd_le<T>(p)/wr_le<T>(p,v)` 显式小端助手

- [x] **Step 1: 写失败测试（往返 + 尺寸 + 字节级常量）**

`tests/test_usbip_proto.cpp` 核心断言：

```cpp
TEST_CASE("wire sizes") {
    CHECK(sizeof(BasicHeader) == 20); CHECK(sizeof(CmdSubmit) == 28);
    CHECK(sizeof(RetSubmit) == 28);   CHECK(usbip::kUrbHeaderSize == 48); // 文档 0x30
}
TEST_CASE("devlist reply golden first bytes") {
    auto v = encode_devlist_reply({make_cmedia()});
    CHECK(v.size() > 4);
    CHECK(v[0]==0); CHECK(v[1]==0); CHECK(v[2]==0); CHECK(v[3]==1); // be32 ndev=1（操作码层大端）
    // usbip_usb_device 段小端: busnum 等字段
}
TEST_CASE("urb header decode") {
    std::array<u8,52> h{}; // 构造 CMD_SUBMIT: command=1, seqnum=7, ep=0, setup=SET_CUR volume
    h[0]=1; ... // 写入字段后
    BasicHeader b; CmdSubmit c;
    CHECK(decode_urb_header(h, b, c) == 52);
    CHECK(b.command == OP_CMD_SUBMIT); CHECK(b.seqnum == 7);
    CHECK(c.transfer_buffer_length == 1920); CHECK(c.number_of_packets == 10);
}
```

（黄金字节集在测试文件内以 `const u8 GOLDEN_DEVLIST[] = {...}` 全量写出，构造函数 `make_cmedia()` 返回 issue #35 中 C-Media 0D8C:0103 的 UsbIpDevice。）

- [x] **Step 2: 跑测试确认失败**

`cmake --build --preset win-x64-debug && ctest --preset win-x64-debug` → 链接错误（实现不存在）。

- [x] **Step 3: 实现**

`endian.h`：模板 `rd_le/wr_le` 用 `memcpy` + 无符号移位（不依赖主机序）。`usbip_proto.h/.cpp`：所有编码走 `wr_le` 逐字段写入 `std::vector<u8>`；**操作码/版本号前 8 字节（版本 0x0111、操作码、状态）按大端，正文结构体按小端**（kernel usbip_common.h 惯例：`usbip_header` 以 `#pragma pack(1)` 直传，x86 LE；OP_* 控制包用 `be32`）。`kUrbHeaderSize=52`。

- [x] **Step 4: 跑测试确认通过**

- [x] **Step 5: Commit** `feat: USB/IP v1.1.1 协议编解码层（小端助手+黄金字节测试）`

---

### Task 3: TCP 传输层 + 导入握手 + 回环集成测试

**Files:**
- Create: `include/uac/usbip_server.h`、`src/usbip_server.cpp`
- Create: `include/uac/urb_dispatcher.h`（纯虚接口）
- Create: `tests/test_usbip_server.cpp`（含 fake-vhci 回环客户端）

**Interfaces:**
- Produces:
  - `class UrbDispatcher { public: virtual ~UrbDispatcher()=default; virtual void on_import(u64 devid)=0; virtual void on_urb(const BasicHeader&, const CmdSubmit&, span<const u8> payload, CompletionFn complete)=0; virtual void on_unlink(u32 seqnum, UnlinkFn done)=0; virtual void on_detach()=0; };` 其中 `CompletionFn = std::function<void(const RetSubmit&, std::vector<u8> payload, std::vector<IsoPacket> iso)>`，`struct IsoPacket{u32 offset, length; i32 status, actual_length;}`
  - `class UsbIpServer { public: bool start(u16 port, UrbDispatcher&); void stop(); bool attached() const; }`（单设备单连接模型：一次 IMPORT 后只服务该连接直到 RET_UNLINK/断开）
- Consumes: Task 2 编解码。

- [x] **Step 1: 失败测试** —— fake vhci 客户端（测试内用 winsock 直连）：连接 → 发 OP_REQ_DEVLIST → 断言收到 1 设备 → 发 OP_REQ_IMPORT(busid) → 断言 status=0 → 发 CMD_SUBMIT(ep0 GET_DESCRIPTOR device, seqnum=1) → dispatcher 假实现立即 complete(RetSubmit{0,18,...}+18B 描述符) → 客户端断言收到 RET_SUBMIT 且 payload 为设备描述符 → 服务端 on_detach 被调。全部在 `127.0.0.1:3240`，端口可注入避免冲突。
- [x] **Step 2: 确认失败** → **Step 3: 实现**（每连接一条 `std::jthread`，收满 48B 头再按 `transfer_buffer_length+number_of_packets*4` 收 payload/iso 描述符；`on_urb` 后台化不阻塞读线程；UNLINK 支持取消挂起完成项）→ **Step 4: 通过** → **Step 5: Commit** `feat: USB/IP server 传输层（导入握手+URB 分发+回环测试）`

---

## Phase B — M1 设备模型（全自动）

### Task 4: USB 描述符构建/解析库（黄金 dump = issue #35 C-Media）

**Files:**
- Create: `include/uac/usb_desc.h`、`src/usb_desc.cpp`、`tests/test_usb_desc.cpp`

**Interfaces:**
- Produces: `namespace uac::desc`：`struct DeviceDesc{u16 bcdUSB=0x110;u16 vid,pid,bcdDevice;u8 maxPacket0=8;u8 iManufacturer,iProduct,iSerial;}`；`struct EndpointDesc{u8 address,attributes;u16 maxPacketSize;u8 interval;}`；`class Builder{ void bytes(span<const u8>); Builder& device(const DeviceDesc&); Builder& config(...); Builder& interface_(...); Builder& endpoint(...); Builder& cs_interface(span<const u8>); std::vector<u8> build(); }`；`std::optional<ConfigView> parse_config(span<const u8>)`（用于 lint 与热切换 diff）；字符串描述符 UTF-16LE 编码 `std::vector<u8> string_desc(std::string_view)`

- [x] **Step 1: 失败测试** —— 黄金配置描述符：把 issue #35 中 C-Media 116 字节 dump 写成 `constexpr u8 GOLDEN_CMEDIA_CFG[116]`（计划附录 A），`parse_config` 断言 2 接口/AC 43B/AS alt1 含 EP 0x06 iso-out wMaxPacketSize 192 bInterval 1；`Builder` 重新拼装出字节相等的数组。UTF-16LE 字符串 "A" → `03 01 41 00`。
- [x] **Step 2-5: 失败→实现→通过→Commit** `feat: USB 描述符构建/解析库（C-Media 黄金字节对齐）`

### Task 5: UAC1 模板 + UDE 适配层 + 约束 lint

**Files:**
- Create: `include/uac/desc_uac1.h`、`src/desc_uac1.cpp`、`include/uac/ude_adapter.h`、`src/ude_adapter.cpp`、`tests/test_desc_uac1.cpp`

**Interfaces:**
- Produces:
  - `struct Uac1Profile{ u16 vid=0x0D8C,pid=0x0103; std::string manufacturer="Phantom Audio",product="UAC1 Sim",serial; u8 channels=2; std::vector<u32> rates={48000}; struct {i16 minDb=-6144,maxDb=0,resDb=256;} vol; bool micIn=true, spkOut=true; }`
  - `std::vector<u8> build_uac1_config(const Uac1Profile&)`（IAD 不需要；AC: IT(USB streaming)→FU(Mute/Volume)→OT(speaker) + mic 路径 IT(mic)→FU→OT(USB streaming)；AS alt0 零端点 + alt1 双向 iso EP）
  - `ude_adapter.h`：`void adapt_for_ude(std::vector<u8>& cfg)` —— 遍历 EP 描述符：iso/int EP `interval = std::max<u8>(4, interval)`（HS 微帧语义下 ≥4 ⇒ ≥1ms）；`DeviceDesc adapt_device_for_ude(DeviceDesc)`（保持 bcdUSB 0x110，usbip 报告 speed=high）
  - `std::vector<LintError> lint_uac1(std::vector<u8> cfg)` —— 规则见测试

- [x] **Step 1: 失败测试** —— (a) adapt_for_ude(黄金 C-Media cfg) 后 EP interval==4 且其余字节不变；(b) lint 用例：alt0 有端点→错、FU Volume 未声明→错、正常模板→0 错；(c) build_uac1_config 产物 parse 后拓扑完整（1 AC + 2 AS，FU 双通道 bmaControls）。
- [x] **Step 2-5** → Commit `feat: UAC1 模板 + UDE 适配（bInterval>=4）+ lint`

### Task 6: AudioControl 处理器（Volume/Mute/日志/STALL 注入点）

**Files:**
- Create: `include/uac/ac_processor.h`、`src/ac_processor.cpp`、`include/uac/log_store.h`、`src/log_store.cpp`、`tests/test_ac_processor.cpp`

**Interfaces:**
- Produces:
  - `struct AcState{ i16 volCur[2]{0,0}; i16 volMin=-6144, volMax=0, volRes=256; bool mute[2]{false,false}; u32 sampleRate=48000; }`
  - `class AcProcessor { AcProcessor(AcState&, LogStore&); ControlResult on_setup_transfer(u8 bmReqType,u8 bRequest,u16 wValue,u16 wIndex,u16 wLength, span<const u8> in); }` `ControlResult{ enum{Ok,Stall,Delayed} kind; std::vector<u8> data; }`
  - 覆盖：标准 GET_DESCRIPTOR(device/config/string)、UAC1 FU SET_CUR/GET_CUR/GET_MIN/MAX/RES（Volume/Mute）、端点 SET_CUR/GET_CUR 采样率、无效请求→Stall；每条请求写入 `LogStore`（NDJSON 行：`{ts,dir,ep,bmRequestType,bRequest,wValue,wIndex,wLength,resp,data}`）
  - `class LogStore { void append(std::string_view ndjsonLine); std::vector<std::string> tail(size_t n); size_t count(); void clear(); }`（内存环形 + 可选文件 sink）

- [x] **Step 1: 失败测试** —— SET_CUR(vol=-30.00dB=-7680? 用 -2560=-10dB) → GET_CUR 相等；GET_MIN/MAX/RES 黄金字节（little-endian i16 对）；Mute SET_CUR(1)→GET_CUR=={1}；采样率 SET_CUR(0xBB80)→GET_CUR=={80 BB 00}；stall case: bRequest=0x77 → Stall。
- [x] **Step 2-5** → Commit `feat: AC 处理器（音量/静音/采样率 + NDJSON 日志）`

### Task 7: 虚拟微帧时钟 + AS 引擎（pacing/打包/解包/feedback）

**Files:**
- Create: `include/uac/microframe_clock.h`、`src/microframe_clock.cpp`、`include/uac/as_engine.h`、`src/as_engine.cpp`、`include/uac/wav.h`、`src/wav.cpp`、`tests/test_as_engine.cpp`、`tests/test_wav.cpp`

**Interfaces:**
- Produces:
  - `class MicroframeClock { MicroframeClock(u64 nowUs=0); void advance_to(u64 us); u64 nowUs() const; u64 deadline_for(u32 nPackets, EndpointKind k) const; }`（测试可注入；生产实现 `WallClockImpl`：`CreateWaitableTimerExW(HIGH_RESOLUTION)` 1ms tick 内插微帧计数）
  - `class AsEngine { AsEngine(MicroframeClock&, LogStore&); void on_alt_setting(u8 alt); void on_iso_in(const CmdSubmit&, CompletionFn); void on_iso_out(span<const u8> payload, span<const IsoPacket>); void set_source(Source s); /*Sine{hz,amp}|File{path}|Silence*/ void set_capture_sink(const char* wavPath); Stats stats() const; }`
  - IN 打包：每微帧标称 `slotsPerUframe = rate*ch*bytes/8000`（±1 slot 漂移可配，默认 0），按请求包数生成；
  - pacing：`on_iso_in` 计算到期 `nowUs + nPackets*125`，由内部 `std::jthread` 定时器到期后回调 `complete`；UNLINK 取消。
  - OUT：按 iso 包切分累计写入 WAV（16bit PCM header 修正式写法）；Stats `{u64 inBytes,outBytes,underruns,overruns}`。
  - feedback（M2 用，本任务实现数值层）：`u32 fb_value_12_13(u32 rateNominal, i32 ppmDrift)`。

- [x] **Step 1: 失败测试** —— (a) 打包器：48k/stereo/16bit、10 包请求 → 每包 24 slots=48B? no——48k*2ch*2B/8000µframes? 计算黄金：每微帧 12 samples*2ch*2B=48B，但 UAC1 HS bInterval=4 → 每 1ms 8 微帧只在第 1 微帧传 192B（单包/间隔）→ 断言 10 包 × 192B；(b) WAV：写 3 组 OUT 包后 dump 与 `tests/data/expected_sine48k.wav`（生成脚本在测试内自产自比对 RIFF 结构与 PCM 字节）一致；(c) pacing：注入时钟，`on_iso_in` 立即回调必须未发生，`advance_to(deadline)` 后恰好发生一次（同步条件变量等待）。
- [x] **Step 2-5** → Commit `feat: AS 引擎（微帧时钟 pacing + IN/OUT 流 + WAV）`

### Task 8: 设备状态机整合 + uacsimd v0 端到端（无驱动回环验收）

**Files:**
- Create: `include/uac/device_model.h`、`src/device_model.cpp`、`apps/uacsimd/main.cpp`、`tests/test_device_model_loopback.cpp`
- Modify: `CMakeLists.txt`（app 目标）

**Interfaces:**
- Produces:
  - `class DeviceModel : public UrbDispatcher { DeviceModel(const DeviceSnapshot&); }`，`DeviceSnapshot{ DeviceDesc dev; std::vector<u8> config; std::map<u8,std::vector<u8>> strings; AcState ac; }`
  - 状态机：ep0 → AcProcessor；interface alt SET_INTERFACE / GET_INTERFACE；iso EP → AsEngine；中断 EP →（M2 EventInjector 预留槽位）；RET_UNLINK/断开 → 状态复位
  - `uacsimd main`：`--profile <json|preset>`（本任务仅内置 preset `uac1-cmedia-sim`）、`--port 3240`、`--log-file <path>`、`--console`（阻塞日志打印）
- [x] **Step 1: 失败测试（端到端回环剧本）** —— fake-vhci 客户端执行完整剧本：DEVLIST→IMPORT→GET_DESCRIPTOR(device)→GET_DESCRIPTOR(config)→SET_CONFIGURATION(1)→SET_INTERFACE(1,1)→10×CMD_SUBMIT(iso OUT 192B×10)→GET_CUR(vol)→SET_CUR(vol)→GET_CUR→断言日志 count==预期、RET_SUBMIT 顺序/seqnum 严格匹配、WAV 落盘 10ms 音频。
- [x] **Step 2-5** → Commit `feat: 设备状态机整合 + uacsimd v0 回环验收（M1 代码侧完成）`

---

## Phase C — M0 链路探针（用户提权配合，Go/No-Go）

### Task 9: M0 探针全流程（驱动安装 + VM + f_uac2 + 实测结论）

**Files:**
- Create: `scripts/elevated-install-driver.ps1`、`scripts/elevated-m0-vm.ps1`、`scripts/m0-probe.ps1`、`scripts/m0-vm-cloud-init.yaml`、`docs/m0-report.md`（结论落盘）

**Interfaces:**
- Consumes: usbip-win2 v0.9.7.8 安装包（`third_party/usbip-win2/` 存放 MSI/EXE，不入 git——.gitignore 加 `third_party/usbip-win2/*.exe`）；Ubuntu 24.04 cloud VHDX（用户下载或脚本 `Invoke-WebRequest`）。
- Produces: `docs/m0-report.md`（Go/No-Go + iso 时序观察 + 线上格式黄金抓包校正结论 + 版本锁定声明）。

- [ ] **Step 1: 还原点 + 驱动安装（用户管理员执行）** `elevated-install-driver.ps1`：`Checkpoint-Computer -Description "before-usbip-win2"` → 静默安装 → `pnputil /enum-drivers | findstr usbip` 验证 → 输出 usbip.exe 版本。提示用户：安装会重启所有 USB3 hub。
- [ ] **Step 2: VM 创建（用户管理员执行）** `elevated-m0-vm.ps1`：`New-VMSwitch -SwitchType Internal`（避免占用物理网卡）→ `New-VM -Name uac-m0 -MemoryStartupBytes 2GB -VHDPath uac-m0.vhdx -Generation 2`（动态盘最大 10GB）→ cloud-init ISO（`m0-vm-cloud-init.yaml`：装 `linux-modules-extra-$(uname -r)`、`modprobe usbip-core usbip-vudc libcomposite`、configfs 挂载脚本 `scripts/m0-gadget-setup.sh` 内嵌创建 f_uac2 UAC2 gadget：VID 0x1D6B PID 0x0104 g_audio 风格、48k/2ch、启用 `c_mute/c_volume` 与通知端点、`usbipd -D --device /sys/class/udc/...`）→ 启动 VM。
- [ ] **Step 3: 探针执行（普通权限）** `m0-probe.ps1`：等 VM IP → `usbip list -r <ip>` → `usbip attach -r <ip> -b <busid>` → 轮询 `Get-PnpDevice -Class MEDIA` 30s → 出现 "Linux Gadget" 声卡 → 用 host-probe（Phase E 前用临时 PowerShell WASAPI 脚本 `scripts/m0-play-test.ps1` 播放 10s 正弦 + 录制 5s）→ 抓包侧：`netsh trace` 或在 VM 内 `tcpdump -i any port 3240 -w /tmp/m0.pcap` 存黄金 USB/IP 流（供 Task 2 字节序/格式校正复核）。
- [ ] **Step 4: 结论落盘** `docs/m0-report.md`：播放/录制是否正常、中断端点是否被 Windows 读取（VM dmesg `g_audio: int ep` + USBView）、#200 纪元问题观察（WPP 日志摘录）、Go/No-Go。No-Go → 触发 RFC 回退路径（B 档），停止 M1 实机集成，仅保留回环。
- [ ] **Step 5: Commit** `docs: M0 探针报告（Go/No-Go + 黄金抓包）`

---

## Phase D — M2（UAC2 + 事件，代码全自动 + 一次实机验证）

### Task 10: UAC2 模板（Clock Source/IAD/feedback EP）+ lint 扩展

**Files:** `include/uac/desc_uac2.h`、`src/desc_uac2.cpp`、`tests/test_desc_uac2.cpp`

**Interfaces:**
- `struct Uac2Profile{ u16 vid=0x1234,pid=0x5678; std::string manufacturer,product,serial; std::vector<u32> rates={44100,48000,96000}; u8 ch=2; struct{u32 subslot=4, bits=32;} pcm; VolRange vol; bool feedbackEp=true; }`
- `std::vector<u8> build_uac2_config(const Uac2Profile&)`：bcdUSB 0x0200、IAD、AC（Header/CD1.x/ClockSource(bmControls freq RW)/IT/OT/FU 双通道）、每采样率一个 AS alt（alt0 零端点）+ 数据 iso EP + feedback iso IN EP(wMaxPacketSize=4,bInterval=4, FDF 12.13)
- `lint_uac2`：单 ClockSource、alt0 零端点、升序、async OUT 必须有 feedback EP、bmFormats 单位、每 alt bTerminalLink 一致、bInterval≥4（UDE）

- [x] **Step 1: 失败测试** —— parse 黄金：7 个 AS alt（1+3×2 in/out? 以 profile=3 rates, mic+spk → AC + 2 AS 组）拓扑断言；lint 负例（删 feedback EP → 报错；双 ClockSource → 报错）。**Step 2-5** → Commit `feat: UAC2 模板（ClockSource/IAD/feedback EP）+ lint`

### Task 11: 采样率协商（双协议）+ AcProcessor 扩展

**Files:** Modify `ac_processor.*`、`tests/test_ac_processor.cpp`（追加用例）

- UAC1：端点 SET_CUR/GET_CUR(wValue=0) 采样率（Le24）；UAC2：ClockSource CUR_FREQ34 SET/GET + RANGE（RAF 子范围：3×u32×n + numSubRanges Le16）。GET RANGE 黄金：44.1k/48k/96k 三离散档。RateGuard：SET 与 AS alt 不匹配→记日志不阻流。**Step 1-5** → Commit `feat: 双协议采样率协商`

### Task 12: AC 中断端点 Status Word 事件注入器

**Files:** `include/uac/event_injector.h`、`src/event_injector.cpp`、`tests/test_event_injector.cpp`、Modify `device_model.*`（中断 EP 接线）

**Interfaces:**
- `class EventInjector { void push(EntityRef, NotifyKind); /*VolumeChanged{newVal}/MuteChanged{newState}/ConnectorChanged*/ std::optional<std::array<u8,6>> pending_status_word(); }` —— UAC2 Status Word 格式（ADC-2 6.1.2）：`Le16 bmMeta+SrcInfo | u8 bEntityID | u8 bNotify | Le16 wValue`（共 6B）；主机经中断 IN CMD_SUBMIT 拉取时返回并弹出队列。
- 场景 API（控制台用）：`simulate_volume_up(step=256)`/`down`/`toggle_mute`：先改 AcState 再入队对应 Status Word（音量变化 bNotify=0x01 CUR changed）。
- **Step 1: 失败测试** —— 注入 vol+ 后：(a) 队列 Status Word 字节黄金（bEntityID=FU id、bNotify=1、wValue=0x0001）；(b) fake-vhci 发中断 IN submit → 收 6B + RET_SUBMIT；(c) 队列空时中断 IN 挂起（不立即完成， pacing 到期也保持挂起直到注入）。
- **Step 2-5** → Commit `feat: Status Word 事件注入器（R6 主通道）`

### Task 13: HID Consumer Volume 复合接口（UAC1 回退通道）

**Files:** `include/uac/desc_hid.h`、`src/desc_hid.cpp`、`tests/test_desc_hid.cpp`、Modify `device_model.*`

- 复合设备：UAC1 config 追加 HID 接口（class 0x03）+ 中断 IN EP(8B, bInterval=4)；报告描述符：Consumer Control 页(0x0C) Usage Volume Up/Down/Mute（报告 ID 1，3 bitfield）；`class HidInjector{ void press(HidKey k); void release(); }` 经中断 IN 返回 1 字节位域。
- **Step 1: 失败测试** —— 报告描述符黄金字节（手写 ~40B Consumer Control minimal）；press(VolUp)→submit 得 `{0x01,0x01,...}`；release→`{0x01,0}`。**Step 2-5** → Commit `feat: HID 音量键复合接口（R6 回退通道）`

### Task 14: M2 实机验证（UAC2 绑定/采样率/事件 + R-006 定级）

**Files:** `scripts/m2-verify.ps1`、`docs/m2-report.md`

- 前置：Task 9 驱动已装。脚本：加载 `uac2-48k` profile → attach → 断言设备管理器驱动 = `usbaudio2.sys`（`Get-PnpDeviceProperty DEVPKEY_Device_DriverVer`/Service）→ host-probe（Task 18 后用之；此前临时 PS）切换 44.1/48/96k 断言 GET_CUR 记录 → 注入 vol+/mute → 观察 `IAudioEndpointVolume` 回调（临时 PS 脚本注册 RegisterControlChangeNotify）→ UAC1 profile 同流程验证 usbaudio.sys 是否响应 Status Word（**R-006 结论**：若否，启用 HID 通道验证）。
- **Step 1-4 + Commit** `docs: M2 实机验证报告（R-006 定级）`

---

## Phase E — M3（控制台/Profile/注入/host-probe/GUI/回归）

### Task 15: Profile JSON（解析/校验/raw override/热切换）

**Files:** `include/uac/profile.h`、`src/profile.cpp`、`profiles/uac1-cmedia-sim.json`、`profiles/uac2-48k.json`、`profiles/uac2-hid-headset.json`、`tests/test_profile.cpp`

- schema（json.hpp 解析）：`{ "protocol":"uac1|uac2", "usb":{vid,pid,bcdDevice,strings:[{lang,text}]}, "audio":{channels,rates,subslot,bits,volume:{minDb,maxDb,resDb}, micIn,spkOut}, "hid":{consumerVolume:true|false}, "endpoints":{override:[{type,address,maxPacketSize,interval}]}, "rawConfigOverride": "<hex>" }`；`Profile load_profile(path)`（校验+默认值）；`bool DeviceSnapshot::diff_requires_reattach(const DeviceSnapshot&)`；热切换 = detach→重建→attach（模拟换设备，非在线改描述符）。
- **Step 1: 失败测试** —— 三个内置 profile 黄金字段断言；坏 JSON（缺 protocol）→ 抛 `ProfileError`；raw override 优先于模板。**Step 2-5** → Commit `feat: Profile JSON + 热切换`

### Task 16: HTTP 控制 API + uacsim CLI

**Files:** `include/uac/control_api.h`、`src/control_api.cpp`、`apps/uacsim/main.cpp`、`tests/test_control_api.cpp`

- 手写 mini HTTP/1.1（仅 127.0.0.1:3241，`Content-Length` 必选，keep-alive 可选）端点：
  - `GET /status` `{state, profile, ac, stats, version}`
  - `GET /log?n=100`、`DELETE /log`
  - `POST /attach {profile}` / `POST /detach`
  - `POST /event {type:"volume_up"|"volume_down"|"mute"|"hid_vol_up"|...}`
  - `POST /inject {kind:"stall"|"delay_ms"|"corrupt_desc_at"|"half_enumerate"|"disconnect"|"iso_error", ...params}`
  - `POST /source {kind:"sine"|"file"|"silence", hz, file}` / `POST /capture {path}`
- `uacsim` 子命令：`status|attach|detach|event|inject|source|capture|log -f`（HTTP 客户端用 winsock 手写）。
- **Step 1: 失败测试** —— 起 API（随机端口）→ 全端点往返断言（含 404/400 错误路径）。**Step 2-5** → Commit `feat: 控制 API + CLI`

### Task 17: 故障注入目录 + 剧本引擎

**Files:** `include/uac/fault_injector.h`、`src/fault_injector.cpp`、`tests/test_fault_injector.cpp`、`scripts/scenarios/*.json`

- `class FaultInjector { void arm(Rule r); void clear(); }`，Rule 变体：`StallNth{nth,bmRequestType,bRequest}`、`DelayMs{nth,ms}`、`CorruptDescByteAt{reqIndex,byteOffset,xor}`、`HalfEnumerate{stopAtStep}`、`Disconnect{afterMs}`（连接层直接 close+detach）、`IsoError{everyN,errorCount}`（RET_SUBMIT error_count 字段）；AcProcessor/AsEngine/DeviceModel 在关键路径询问 `fault.check(...)`。
- 剧本 = JSON 序列（arm→动作→断言），由 Task 20 回归器执行。
- **Step 1: 失败测试** —— 每 Rule 一条：arm 后对应请求/包行为改变、日志记录注入事件。**Step 2-5** → Commit `feat: 故障注入目录 + 剧本引擎`

### Task 18: host-probe（Goertzel + WASAPI + MMDevice）

**Files:** `apps/host_probe/main.cpp`、`include/uac/goertzel.h`、`src/goertzel.cpp`、`include/uac/wasapi_util.h`、`src/wasapi_util.cpp`、`tests/test_goertzel.cpp`、`tests/data/*.wav`

- 子命令：`enum`（MMDevice 端点列表+友好名）、`volume-watch <endpointId> --seconds N`（IAudioEndpointVolume::RegisterControlChangeNotify → JSON 行输出）、`notify-watch`（IMMNotificationClient 设备增删/状态→JSON 行）、`capture --endpoint <id> --freq F --seconds N --tolerance-db 6`（默认渲染回环 WASAPI_CAPTURE 或麦克风采正弦→Goertzel 幅值>阈值→exit 0/1）。
- Goertzel：`double goertzel_magnitude(const float* x, size_t n, double f, double fs)`；自测：合成 1kHz@48k 2048 点幅度 0.5 → 误差 <10%；2kHz 信号在 1kHz 探测 → <1%。
- **Step 1-5**（Goertzel/WAV 数学单测 TDD；WASAPI 部分手验+集成门控测试）→ Commit `feat: host-probe 主机侧断言工具`

### Task 19: uacstudio Win32 GUI 最小版（Device/Inject/Log）

**Files:** `apps/uacstudio/main.cpp`、`apps/uacstudio/ui.cpp`、`apps/uacstudio/ui.h`、`apps/uacstudio/http_client.h`

- 遵循仓库 `win32-gdi-dev` 技能规范：Per-Monitor-V2 DPI、双缓冲 WM_PAINT、GDI+ 一次 Startup/Shutdown、无控件库（纯 GDI 自绘 + 3 按钮行布局）。
- 三面板 Tab：**Device**（Profile 下拉（枚举 profiles/）、Attach/Detach 按钮、状态区（GET /status 1s 轮询））；**Inject**（Vol+/Vol-/Mute/HID 三键、STALL/Delay/Disconnect/半枚举 四剧本按钮）；**Log**（GET /log?n=200 500ms 轮询、虚拟滚动、过滤框）。
- 精度约束：所有 UI 逻辑仅依赖 Task 16 HTTP API（不链接 uac_core），保证 GUI 可独立演进。手验清单（DPI 125%/150% 截图、attach 状态刷新、注入后 log 变化）写入 `docs/m3-gui-check.md`。
- **Step 1-3 + Commit** `feat: uacstudio Win32 GUI 最小版（三面板）`（GUI 无单测，手验文档 + Task 20 集成断言 API 可达性）

### Task 20: 自动化回归套件 + 覆盖率 + CI

**Files:** `scripts/regression.ps1`、`scripts/coverage.ps1`、`scripts/scenarios/regression.json`、`.github/workflows/ci.yml`

- `regression.ps1`：三层——L1 单测（`ctest`）；L2 回环集成（`uac_tests --loopback-scenarios scripts/scenarios/regression.json`：fake-vhci 跑 12 剧本：枚举/音量/静音/采样率/事件/HID/STALL×2/延迟/损坏/半枚举/拔线）；L3 实机门控（检测 vhci 服务存在 + `-WithDriver` 开关：attach→host-probe capture 1kHz→volume-watch 事件→detach，产出 `build/regression-report.html`）。默认 L1+L2 全绿即过；L3 需驱动。
- `coverage.ps1`：OpenCppCoverage（`choco install opencppcoverage` 或提示安装）跑 `uac_tests --test-case-exclude=*loopback*`，输出 Cobertura XML + 行覆盖率摘要；**核心模块（usbip_proto/usb_desc/desc_uac1/desc_uac2/ude_adapter/ac_processor/as_engine/microframe_clock/event_injector/fault_injector/profile/log_store/wav/goertzel）行覆盖目标 100%**，未达标则列出缺口行号并退出非零。
- `.github/workflows/ci.yml`：windows-latest，VS2022，步骤 = cmake preset → build → ctest（L1+L2）→ 上传覆盖率 XML artifact。（L3 实机不在 CI，跑在本地/自托管。）
- **Step 1-4 + Commit** `test: 三层回归套件 + 100% 核心覆盖率门禁 + CI`

---

## Phase F — M4（多实例/GUI 打磨/MS OS 2.0/收尾）

### Task 21: 多实例（vhci 端口数 + 并发 attach）

**Files:** Modify `usbip_server.*`/`device_model.*`/`control_api.*`、`profiles/uac1-headset2.json`、`tests/test_multi_instance.cpp`

- UsbIpServer 支持多设备注册（DEVLIST 多条、IMPORT 按 busid 路由到对应 DeviceModel；连接数=同时 attach 数）；控制 API `POST /attach` 返回 `deviceId`，`/status` 数组化。实机侧：`scripts/elevated-set-vhci-ports.ps1`（注册表 `HKLM\SYSTEM\CurrentControlSet\Services\usbip2_ude\Parameters` 端口数=4，需重载驱动）。
- **Step 1-5**（回环：2 设备并发枚举互不串扰；实机手验 2 声卡并存）→ Commit `feat: 多实例并发仿真`

### Task 22: GUI Stream 面板 + 打磨

**Files:** Modify `apps/uacstudio/*`

- Stream 面板：源选择（Sine hz 输入/File/Silence）、Capture 保存路径+开始/停止、RMS 电平条（GET /status stats.outBytes 差分→dBfs 映射，双缓冲自绘）；深色主题；窗口位置持久化（注册表 `Software\PhantomAudio\uacstudio`）。
- **Step 1-3 + Commit** `feat: uacstudio Stream 面板与打磨`

### Task 23: MS OS 2.0 描述符 + 多语言字符串

**Files:** `include/uac/msos20.h`、`src/msos20.cpp`、`tests/test_msos20.cpp`、Modify `ac_processor.*`（BOS/MS OS 请求处理）、`profile.*`

- 实现 GET_DESCRIPTOR(BOS) + MS OS 2.0 Descriptor Set（Registry Property 注入 `T1_J01_ChannelMapping` 等示例 + 可选 ContainerID）；字符串多语言（LANGID 表 + per-lang 索引）。黄金测试对照 MS 文档示例字节（spec 文档中的 0x76 字节示例改写为可参数化生成）。
- **Step 1-5** → Commit `feat: MS OS 2.0 描述符 + 多语言字符串`

### Task 24: 全量验收 + 文档收尾

**Files:** `tools/uac-simulator/README.md`、`docs/runbook.md`、`docs/acceptance-m4.md`

- README：架构图、快速开始（装驱动→attach→host-probe）、profile 编写指南；runbook：故障排查（WPP 日志采集、BSOD dump 分析入口、#200 现象识别）、驱动升级流程（重跑 L3 回归）。
- 验收：按 RFC v2 里程碑表逐项打勾（M1 十分钟播放→L3 scenario `long_play`；M2 R-006 结论引用；M3 GUI 手验清单；M4 多实例截图），产出 `docs/acceptance-m4.md`。
- **Step 1-3 + Commit** `docs: M4 全量验收与运行手册`

---

## 附录 A：黄金字节（Task 4/2 引用）

C-Media 0D8C:0103 配置描述符 116B（来自 usbip-win2 issue #35，USBView dump）：

```
09 02 74 00 02 01 00 80 FA 09 04 00 00 00 01 01
00 00 09 24 01 00 01 2B 00 01 01 0C 24 02 01 01
01 00 02 03 00 00 00 0D 24 06 0D 01 02 01 02 02
00 02 00 00 09 24 03 03 01 03 00 0D 00 09 04 01
00 00 01 02 00 00 09 04 01 01 01 01 02 00 00 07
24 01 01 01 01 00 0E 24 02 01 02 02 10 02 44 AC
00 80 BB 00 09 05 06 09 C0 00 01 00 00 07 25 01
01 00 00 00
```

设备描述符 18B：`12 01 10 01 00 00 00 08 8C 0D 03 01 10 00 01 02 00 01`

## 附录 B：验收对照（RFC v2 → 任务）

| RFC 项 | 任务 |
|---|---|
| R1-R4 | Task 4-8（代码）+ Task 9（实机 Go）+ Task 14 |
| R5 | Task 6/11 |
| R6 主/回退 | Task 12/13/14 |
| R7 | Task 6(日志)/17(注入)/18(host-probe)/20(回归) |
| R8 | Task 16(CLI/API)/19(GUI)/22 |
| 虚拟微帧 pacing | Task 7 |
| UDE 适配 | Task 5 |
| 多实例 | Task 21 |
| MS OS 2.0/多语言 | Task 23 |
| 100% 核心覆盖率 | Task 20（coverage.ps1 门禁） |
| 自动化 | Task 20（L1/L2 全自动，L3 门控） |
