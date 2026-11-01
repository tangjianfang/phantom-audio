# phantom-audio 架构设计

> 类型：arch ｜ 模式：Reverse ｜ 代码基线：`936376c` ｜ 最后核对：2026-11-01 ｜ 范围：`tools/uac-simulator/` ｜ lang: zh

## Overview

phantom-audio 是 Windows 纯软件虚拟 USB 音频设备（UAC1/UAC2）测试台：**4 个用户态进程 + 1 个第三方内核驱动**组成，被测软件经 WASAPI 看到"真 USB 声卡"（Windows 内置类驱动绑定）。全部设备模型在用户态；内核侧复用开源 usbip-win2（UDE 框架），零内核开发。

图例：蓝色 = 本仓库用户态；橙色 = 第三方内核（usbip-win2）；灰色 = Windows 系统组件；白底描边药丸 = 判定。

**TL;DR**

- 控制面（HTTP 127.0.0.1:3241）与数据面（USB/IP TCP 3240）分离，三个客户端共享同一 API
- 设备模型全部用户态：Profile → DeviceSnapshot → DeviceModel（AcProcessor + AsEngine）
- 已知边界：usbip-win2/UDE 等时数据载荷在本机（Win11 26200）双向不通，详见 Risks
- 单测 230 例 + GUI 非视觉自检 3 阶段 + 一键 autotest 20 用例三道防线

## Architecture Context

```mermaid
%%{init:{"theme":"base","themeVariables":{"fontSize":"14px","primaryColor":"#f3f4f6","primaryBorderColor":"#6b7280","primaryTextColor":"#111827","lineColor":"#4b5563","tertiaryColor":"#ffffff","edgeLabelBackground":"#ffffff","fontFamily":"Segoe UI, Microsoft YaHei, PingFang SC, sans-serif"},"flowchart":{"nodeSpacing":40,"wrappingWidth":360}}}%%
flowchart TB
    %% level: L0
    subgraph repo["本仓库（用户态）"]
        SIM[uacsimd 仿真器服务]
        GUI[uacstudio GUI 控制台]
        CLI[uacsim CLI]
        PROBE[host_probe 主机侧探针]
    end
    subgraph win["Windows"]
        WSAPISYS[usbaudio.sys / usbaudio2.sys<br/>内置音频类驱动]
        MIXER[音频引擎 / WASAPI]
    end
    KERNEL[("<b>usbip-win2 内核驱动</b><br/><span style='color:#6b7280;font-size:12px'>UDE 虚拟主控</span>")]
    DEV[("<b>被测音频软件</b>")]

    DEV -->|"WASAPI 播放/采集"| MIXER
    MIXER --- WSAPISYS
    WSAPISYS ---|"USB 请求（URB）"| KERNEL
    KERNEL -->|"USB/IP 协议 TCP 3240"| SIM
    GUI -->|"HTTP 3241"| SIM
    CLI -->|"HTTP 3241"| SIM
    PROBE -->|"WASAPI 枚举/采集"| MIXER
    classDef own fill:#dbeafe,stroke:#1d4ed8,color:#0f172a;
    classDef ext fill:#fed7aa,stroke:#c2410c,color:#0f172a;
    classDef sys fill:#e2e8f0,stroke:#475569,color:#0f172a;
    class SIM,GUI,CLI,PROBE own;
    class KERNEL ext;
    class WSAPISYS,MIXER,DEV sys;
```

被测软件与音频栈是 Windows 系统组件；唯一的外部内核依赖是 usbip-win2（BSD-2-Clause，已签名）。

## Logical Architecture

```mermaid
%%{init:{"theme":"base","themeVariables":{"fontSize":"14px","primaryColor":"#f3f4f6","primaryBorderColor":"#6b7280","primaryTextColor":"#111827","lineColor":"#4b5563","tertiaryColor":"#ffffff","edgeLabelBackground":"#ffffff","fontFamily":"Segoe UI, Microsoft YaHei, PingFang SC, sans-serif"},"flowchart":{"nodeSpacing":40,"wrappingWidth":360}}}%%
flowchart TB
    %% level: L1
    subgraph core["uac_core（静态库）"]
        DM[DeviceModel<br/>设备快照 + URB 路由]
        ACP[AcProcessor<br/>控制传输 / 描述符]
        ASE[AsEngine<br/>等时流 + 记账限速]
        UIS[UsbIpServer<br/>USB/IP 协议端点]
        CAP[ControlApi<br/>回环 HTTP + 策略]
        DES[desc_uac1 / desc_uac2<br/>描述符生成]
        INJ[Event / FaultInjector<br/>事件与故障注入]
    end
    subgraph hostlib["uac_host"]
        SH["<b>SimHost</b><br/><span style='color:#6b7280;font-size:12px'>编排: attach/状态/监听</span>"]
        SPM[SpeakerMonitor<br/>虚拟扬声器→真实声卡]
    end
    subgraph studiolib["uac_studio_core"]
        AT[auto_test<br/>20 用例运行器]
        HO[host_ops / driver_installer<br/>usbip 与驱动操作]
        EC[env_check<br/>环境自检 7 项]
        SP[sim_process<br/>模拟器进程生命周期]
        CTL[controller<br/>按钮表 / 请求体]
    end
    SIM2[uacsimd main<br/>每秒 URB 统计倾倒]
    GUI2[uacstudio main<br/>主窗口 / Environment 窗口]
    CLI2[uacsim main]

    SIM2 --- SH
    SH --- DM
    DM --- ACP
    DM --- ASE
    SH --- UIS
    SH --- CAP
    SH --- SPM
    DM --- DES
    DM --- INJ
    GUI2 --- CTL
    GUI2 --- AT
    GUI2 --- EC
    AT --- HO
    EC --- SP
    EC --- HO
    CLI2 -->|"http_client"| CAP
    classDef own fill:#dbeafe,stroke:#1d4ed8,color:#0f172a;
    class DM,ACP,ASE,UIS,CAP,DES,INJ,SH,SPM,AT,HO,EC,SP,CTL,SIM2,GUI2,CLI2 own;
```

三个静态库按"能否单测"分层：`uac_core` 无窗口依赖，`uac_host` 编排层，`uac_studio_core` 含非窗口 GUI 逻辑（`CMakeLists.txt:56-74`）。

## Component Responsibilities

| Component | Owns | Does not own |
|---|---|---|
| SimHost | profile 解析与校验、多实例、监听/导入状态、扬声器监听接线 | USB/IP 协议编解码、描述符内容 |
| DeviceModel | 设备快照持有、URB 按端点路由、诊断计数 | 速率控制算法、控制请求语义 |
| AcProcessor | 控制 transfer 语义（时钟/FU/描述符/字符串）、NDJSON 日志 | 等时数据 |
| AsEngine | 等时 IN 打包（内容记账限速）、OUT 解码、监听回调 | USB/IP 帧格式 |
| UsbIpServer | TCP 通道、CMD/RET 编解码、完成回调封装 | 设备语义 |
| ControlApi | 端点分发、回环/Origin/体积/时限安全策略 | 业务语义 |
| SpeakerMonitor | winmm 双缓冲播放到真实输出设备 | 音频处理（直通 16 位 PCM） |
| auto_test | 20 用例表 + 运行器 + 报告文件 | 单个用例的实现 |
| host_ops | Attach/Detach to Windows、驱动装卸编排 | 守卫判定（调 HostCasesAllowed） |
| env_check | 7 项环境检测 + 动作按钮窗口 | 驱动安装本身 |
| sim_process | 起/停 uacsimd、--parent-pid 兜底、只动自己启动的 | HTTP 通信 |

## Dependency Architecture

```mermaid
%%{init:{"theme":"base","themeVariables":{"fontSize":"14px","primaryColor":"#f3f4f6","primaryBorderColor":"#6b7280","primaryTextColor":"#111827","lineColor":"#4b5563","tertiaryColor":"#ffffff","edgeLabelBackground":"#ffffff","fontFamily":"Segoe UI, Microsoft YaHei, PingFang SC, sans-serif"},"flowchart":{"nodeSpacing":40,"wrappingWidth":360}}}%%
flowchart TB
    %% level: L1
    subgraph third["第三方"]
        NLOH[nlohmann/json]
        DOCT[doctest]
        USBIP2[("<b>usbip-win2 驱动</b><br/><span style='color:#6b7280;font-size:12px'>运行时依赖</span>")]
    end
    subgraph winlibs["Windows SDK"]
        WS2[ws2_32]
        WINMM[winmm]
        COMCTL[comctl32 / uxtheme]
    end
    CORE[uac_core]
    HOST[uac_host]
    STUDIO[uac_studio_core]
    APPS[四个可执行文件]

    CORE --> NLOH
    CORE --> WS2
    HOST --> CORE
    HOST --> WINMM
    STUDIO --> NLOH
    APPS --> CORE
    APPS --> HOST
    APPS --> STUDIO
    APPS -.->|"运行时：usbip attach"| USBIP2
    APPS --> COMCTL
    TESTS[uac_tests] --> CORE
    TESTS --> HOST
    TESTS --> STUDIO
    TESTS --> DOCT
    classDef own fill:#dbeafe,stroke:#1d4ed8,color:#0f172a;
    classDef ext fill:#fed7aa,stroke:#c2410c,color:#0f172a;
    classDef sys fill:#e2e8f0,stroke:#475569,color:#0f172a;
    class CORE,HOST,STUDIO,APPS,TESTS own;
    class NLOH,DOCT,USBIP2 ext;
    class WS2,WINMM,COMCTL sys;
    %% fix-style: generated below, do not edit
    classDef faded opacity:0.6,stroke-dasharray:4 3
    class USBIP2 faded
    linkStyle 8 stroke:#9ca3af,stroke-opacity:0.7,color:#6b7280
```

内核驱动仅在宿主侧操作（attach/驱动装卸）时需要；仿真与全部单测无需驱动。

## Runtime / Deployment Architecture

```mermaid
%%{init:{"theme":"base","themeVariables":{"fontSize":"14px","primaryColor":"#f3f4f6","primaryBorderColor":"#6b7280","primaryTextColor":"#111827","lineColor":"#4b5563","tertiaryColor":"#ffffff","edgeLabelBackground":"#ffffff","fontFamily":"Segoe UI, Microsoft YaHei, PingFang SC, sans-serif"},"flowchart":{"nodeSpacing":40,"wrappingWidth":360}}}%%
flowchart LR
    %% level: L0
    %% size: large
    subgraph dev["开发机 / CI"]
        U1[uac_tests<br/>230 用例]
        U2[uacstudio --selftest]
        U3[uacsimd + GUI<br/>无驱动模式]
    end
    subgraph target["目标机（Sandbox/VM/开发机显式开启）"]
        U4[uacsimd]
        U5[uacstudio 提权]
        K[("<b>usbip2_ude + filter</b>")]
        W[Windows 音频栈]
    end
    U4 -->|"HTTP 3241"| U5
    U5 -->|"usbip attach -b 1-1"| K
    K --- W
    classDef own fill:#dbeafe,stroke:#1d4ed8,color:#0f172a;
    classDef ext fill:#fed7aa,stroke:#c2410c,color:#0f172a;
    classDef sys fill:#e2e8f0,stroke:#475569,color:#0f172a;
    class U1,U2,U3,U4,U5 own;
    class K ext;
    class W sys;
```

同一套产物两种部署：开发机跑 ①②层验证（无驱动），zip（`build.bat` 产 `dist/uacsim-sandbox-*.zip`）拷进 Sandbox 跑 ③层；开发机经 `UACSIM_FORCE_HOST_TESTS=1` 显式开启后同样可跑（CLAUDE.md 硬规则 1）。

## Key Data Flow / Sequence

挂载链路（用户点 Attach 到设备出现在 Windows）：

```mermaid
%%{init:{"theme":"base","themeVariables":{"fontSize":"14px","primaryColor":"#f3f4f6","primaryBorderColor":"#d1d5db","primaryTextColor":"#111827","lineColor":"#4b5563","actorBkg":"#ffffff","actorBorder":"#6b7280","actorLineColor":"#9ca3af","noteBkgColor":"#ffffff","noteBorderColor":"#9ca3af","noteTextColor":"#374151","signalColor":"#4b5563","sequenceNumberColor":"#ffffff","fontFamily":"Segoe UI, Microsoft YaHei, PingFang SC, sans-serif"},"sequence":{"width":120,"height":50,"actorMargin":40,"mirrorActors":false,"messageFontSize":14}}}%%
sequenceDiagram
    %% level: L1
    %% size: large
    autonumber
    participant U as 用户
    participant G as uacstudio
    participant S as SimHost
    participant M as DeviceModel
    participant K as usbip-win2
    participant W as Windows 音频栈

    U->>G: 选 profile，点 Attach
    G->>S: POST /attach {"profile"}
    S->>S: 解析+校验（≥2 速率等，sim_host.cpp:103）
    S->>M: build_snapshot → 新 DeviceModel
    S-->>G: {"attached","busid":"1-1"}
    G-->>U: NOTE: 仅模拟器侧；附 Windows 导入状态
    U->>G: Environment → Attach to Windows
    G->>K: usbip attach -r 127.0.0.1 -b 1-1
    K->>S: IMPORT + 控制传输枚举
    S->>M: URB 路由（设备日志 NDJSON）
    K->>W: 设备节点 → usbaudio2.sys 绑定
    W-->>U: 声音设置出现扬声器/麦克风端点
```

①-⑤ 为软件侧挂载（任何机器可用）；⑥-⑩ 需要驱动与守卫通过。步骤 ⑨ 之后：控制传输工作正常；等时数据载荷受已知平台缺陷影响（见 Risks）。

## Interface / Contract

控制 API（回环 HTTP，`control_api.cpp:240-266` 分发；策略：拒绝 Origin、Host 须回环、体 ≤1 MiB、总时限）：

| 接口 | 方向 | 载荷 | 语义 |
|---|---|---|---|
| `GET /status` | 客户端→仿真器 | — | 运行/profile/实例/导入/音量/字节计数 |
| `POST /attach` | 同上 | `{"profile"}` | 挂载（校验 ≥2 不同速率） |
| `POST /detach` | 同上 | — | 全部卸载 |
| `POST /event` | 同上 | `{"type":"volume_up"...}` | 设备侧按键事件 |
| `POST /inject` | 同上 | `{"kind":"stall"/"delay_ms"/"disconnect"/"clear"}` | 故障注入 |
| `POST /source` | 同上 | `{"kind":"sine","hz"}` / silence / file | 麦克风源 |
| `POST /capture` | 同上 | `{"path":".wav"}` | 扬声器侧录音落盘 |
| `POST /monitor` | 同上 | `{"on":bool}` | 虚拟扬声器→真实声卡播放开关 |
| `GET /log?n=` / `POST /shutdown` | 同上 | — | NDJSON 尾巴 / 优雅停止 |

完整字段与错误码见 `docs/detailed-design.md` 的 ControlApi 节。

## State / Lifecycle

```mermaid
%%{init:{"theme":"base","themeVariables":{"fontSize":"14px","primaryColor":"#f3f4f6","primaryBorderColor":"#6b7280","primaryTextColor":"#111827","lineColor":"#4b5563","fontFamily":"Segoe UI, Microsoft YaHei, PingFang SC, sans-serif"}}}%%
stateDiagram-v2
    %% level: L1
    [*] --> Idle: uacsimd 启动
    Idle --> Listening: /attach 成功<br/>（校验通过，导出 busid）
    Listening --> Listening: 换 profile（重建模型）
    Listening --> Imported: usbip attach（vhci IMPORT）
    Imported --> Listening: usbip detach
    Listening --> Idle: /detach
    Imported --> Idle: /detach（含清理）
    Idle --> [*]: /shutdown 或父进程退出
```

导入态（Imported）由 `server_.attached()` 报告；多实例（≤4）共享一条总线，每实例独立 busid/devnum（`sim_host.cpp` 导出清单重建）。

## Non-Functional Requirements

| 类别 | 要求 |
|---|---|
| 安全 | 回环 HTTP：无认证但拒绝浏览器跨域（Origin/Host/白名单/体积/时限六道策略） |
| 延迟 | UI 线程任何探测 ≤400 ms（smoke 第二阶段强制） |
| 并发 | ≤4 设备实例（vhci 端口数）；控制 API 并发请求安全 |
| 兼容 | Windows 10/11；Sandbox 无 CJK 字体 → 界面文字全 ASCII |
| 可靠性 | 模拟器 `--parent-pid` 兜底退出；GUI 只停自己启动的进程 |
| 资源 | iso 日志环形 20000 行封顶；监听队列 ≤1 s 丢弃 |

## Key Decisions

| ID | 决策 | 状态 | 备选 | 原因 | 影响 |
|---|---|---|---|---|---|
| ADR-001 | 复用 usbip-win2（UDE）而非自研内核驱动 | Accepted | 自研 UDE 驱动；WDM vhci | 零内核开发、已签名 | 等时数据受上游缺陷制约（见 R-001） |
| ADR-002 | 控制面 HTTP / 数据面 USB/IP 分离 | Accepted | 单通道 | 三客户端共享；CI 可驱动 | 两套端口（3241/3240） |
| ADR-003 | UAC2 profile 强制 ≥2 不同采样率 | Accepted | 允许单速率 | usbaudio2 单速率不启动音频功能（真机实证） | profile 设计约束 |
| ADR-004 | iso IN 立即完成 + 墙钟内容记账限速 | Accepted | 延迟完成 | 客户端对延迟 RET 弃管（真机实证） | 暖机首包为合法零长帧 |
| ADR-005 | 设备模型全用户态 | Accepted | 内核态模型 | 可测性（230 单测直驱设备模型） | URB 往返多一层 TCP |
| ADR-006 | 界面文字全 ASCII | Accepted | 本地化 | Sandbox 无 CJK 字体 | `test_env_check` 扫描强制 |
| ADR-007 | 驱动装卸节奏"卸载→重启→安装" | Accepted | 热装卸 | 热装卸楔死内核栈（两次实证，runbook） | 换版本成本高 |

## Risks / Open Issues

| ID | 问题/风险 | 影响 | 状态 | 责任 |
|---|---|---|---|---|
| R-001 | usbip-win2/UDE 等时数据载荷双向不通（本机 Win11 26200；0.9.8.0/0.9.8.1、UAC1/UAC2 均复现） | 高：麦克风流无声、混音器不跟设备音量 | Open · 证据链见 runbook "iso IN" | 上游 usbip-win2 |
| R-002 | R-001 的出路三选一：上游修复 / WDM vhci 对照（料已备） / 自研 UDE（uacvirt） | 中：决定 ADR-001 是否维持 | Open | 维护者 |
| R-003 | WDM vhci 为测试签名，启用需 testsigning（水印+内核安全面） | 低：仅实验路径 | Open | 维护者 |

## 待确认项

- 覆盖率门禁当前值（M4 时为 95%；今日改动后未重测，`coverage.ps1` 可复测）
- `docs/superpowers/` 历史 spec 与本文档的对应关系未逐条核对（按 CLAUDE.md 约定以本文档为准）
