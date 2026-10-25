# 变更说明 —— Sound Connect 桌面 UI 重构（无边框 / DPI / 响应式 / Sony 设计语言）

改动集中在一个文件：**`sony-soundconnect-ui.cpp`**（原 945 行 → 本轮重构后 2437 行）。
`sony-soundconnect-cross-fixed.cpp`（蓝牙 RFCOMM 协议后端）在这一轮**一行未改**，
`build_sony_ui.cmd`、`.vscode\tasks.json` 也未改。

> **后续增量（见文末第 6 节）**：随后又加了「功能 A 设备发现 + 自动配对/连接」与「功能 B 同步 EQ」。
> 该增量同时修改了后端（`sony-soundconnect-cross-fixed.cpp` 2577 → 3302 行）与
> `build_sony_ui.cmd`（追加 `-lbthprops`），本文件 2437 → 2746 行。
> 第 1~5 节描述的是**上一轮重构**的内容，行号以那一轮为准；第 6 节给出本增量与最终行号。

---

## 1. 文件清单

| 文件 | 动作 | 说明 |
| --- | --- | --- |
| `sony-soundconnect-ui.cpp` | **重写** | 全部窗口/绘制/交互/状态 |
| `sony-soundconnect-cross-fixed.cpp` | 未改 | 协议后端（RFCOMM / MDR v1+v2 / 仿真器） |
| `XPACK-VS-IMPLEMENTATION.md` | 新增 | xapk 规范 vs 实现差异清单（配色/排版/间距/控件/IA/交互/冲突/已补未补） |
| `CHANGES.md` | 新增 | 本文件 |
| `shot\Shot2.cs`、`shot\run_shots.ps1` | 新增 | 多分辨率 + 多页面截图取证工具（DPI 感知、自动点导航、PrintWindow） |
| `shot\*.png` | 新增 | 5 个目标分辨率 + 5 个页面 + 3 个任意尺寸截图 |
| `build_sony_ui.cmd` / `.vscode\tasks.json` / `README.md` / `sim-test.log` | 未改 | 原样 |

---

## 2. 关键位置（`sony-soundconnect-ui.cpp`，行号为当前版本）

### 2.1 DPI 与缩放

| 行 | 内容 |
| --- | --- |
| 48 | `template <typename Fn> Fn procAddr(HMODULE, const char*)` —— 用 `std::memcpy` 取函数指针，规避老 MinGW 头没有 `SetProcessDpiAwarenessContext` 等符号，且不触发 `-Wcast-function-type` |
| 70 | `int P(int designPx)` = `round(designPx × g_scale)` —— 全文件唯一缩放入口 |
| 75 | `enableDpiAwareness()` 三级降级：`SetProcessDpiAwarenessContext(-4)`（Per-Monitor V2）→ `shcore!SetProcessDpiAwareness(2)` → `SetProcessDPIAware()` |
| 94 | `queryWindowDpi(HWND)`：`GetDpiForWindow` → 退化 `GetDeviceCaps(LOGPIXELSX)` |
| 503 | `updateMetrics(cw, ch)`：`sf = clamp(min(logicalW/1440, logicalH/900×1.12), 0.62, 1.55)`；`g_scale = sf × dpi/96`；`compactRail = logicalW < 1200`；产出 `titleH/toolH/statusH/sidebarW/padX/gapY` |
| 2209 | `WM_DPICHANGED`：按系统建议矩形 `SetWindowPos`（不吞掉跨屏缩放），重建字体、重算度量、重绘 |

### 2.2 无边框窗口

| 行 | 内容 |
| --- | --- |
| 114 | `applyDwmWindowLook()`：`DwmExtendFrameIntoClientArea(1,1,1,1)`（拿 DWM 投影，同时把系统边框压到 1px 不可见）+ `DwmSetWindowAttribute(20=USE_IMMERSIVE_DARK_MODE, 33=CORNER_PREFERENCE=2)`（Win11 圆角） |
| 2155 | `WM_NCCALCSIZE`：`if (wp == TRUE) return 0;` —— 非客户区归零，客户区覆盖整窗，**无系统标题栏、无边框线** |
| 2159 | `WM_NCHITTEST`：8 向缩放边（边宽 `max(4, P(6))`，最大化时禁用）+ 标题栏区域返回 `HTCAPTION`；窗口按钮命中时返回 `HTCLIENT`（否则点不到按钮） |
| 2192 | `WM_GETMINMAXINFO`：`MonitorFromWindow + GetMonitorInfoW.rcWork` 钉 `ptMaxSize/ptMaxPosition` → 最大化不覆盖任务栏；`ptMinTrackSize = max(420,P(680)) × max(300,P(400))`（旧实现钉 1120×760，1280×720 屏直接不可用） |
| 1330 | `drawTitleBar()`：品牌标记 + "Sound Connect / Desktop" + 连接状态 pill + busy pill + 三个窗口按钮 |
| 698 | `windowButton(kind)`：0=最小化横线、1=最大化方框（最大化态画双框）、2=关闭斜叉，hover 底色换成 `T_CONT_HIGH`，关闭 hover 变 Windows 标准红 `RGB(0xC4,0x2B,0x1C)` |
| 2355 | `WinMain`：`WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN`（**已去掉 `WS_OVERLAPPEDWINDOW`**）；初始 1440×940 按 DPI 缩放并在工作区内居中 |

### 2.3 设计令牌（全部来自 xapk 深色主题，见差异清单 §2）

| 行 | 内容 |
| --- | --- |
| 144 | `constexpr COLORREF blend(fg, percent, bg)` —— 编译期把 Sony 的 `#αRRGGBB` 预合成到 GDI 的 `COLORREF`（GDI 无 α 通道） |
| 153 起 | `T_SURFACE #0E1010`、`T_CONTAINER #1C2021`、`T_CONT_HIGH #282D2F`、`T_CARD #36393A`、`T_SCREEN #121212`、`T_ON #E1E3E3`、`T_ON_VAR #C4C7C7`、`T_OUTLINE #8E9192`、`T_OUTLINE_VAR #444748`、`T_ACCENT #C5D4FF`、`T_MINT #5FDBBA`、`T_PC_GREEN #005141`、`T_PC_BLUE #364FA8`、`T_INDIGO #7384FC`、`T_PINK #F47F95`、`T_ORANGE #FF884B`、`T_CYAN #04AFBC`、`T_SKY #A0D4FC`、`T_GREY #9A9A9A`、`T_EQ_CENTER #909090`、`T_EQ_LINE #646464`、`T_GOLD #AA8940` |
| 302 | `enum FontKind { F_CAP, F_BODY, F_BODY_M, F_TITLE_S, F_TITLE, F_TITLE_M, F_HEADLINE, F_DISPLAY }` |
| 452 | `initFonts()`：`{11,400} {13,400} {13,600} {15,500} {17,500} {20,600} {26,600} {34,700}`（logical → `P()` 缩放，下限 11px），字体 "Microsoft YaHei UI" 失败退 "Segoe UI" |

### 2.4 组件（全部重写为 Sony 语义）

`card` 529（圆角 `P(16)`）· `cardTitle` 533 · `pill` 540 · `filledButton` 545（胶囊 `h/2`）·
`outlineButton` 554 · `accentTextButton` 564 · `segmented` 574（对应 `asc_mode_button_background_selector` 的 checked 态）·
`toggleRow` 595（44×24 轨道 + 圆钮）· `hSlider` 616 · `vFader` 637 · `stepper` 654 ·
`textField` 674（自绘输入框，替代原生 `EDIT`）· `windowButton` 698 · `drawEqCurve` 1089

### 2.5 页面

| 行 | 页面 | 内容 |
| --- | --- | --- |
| 762 | 概览 | hero 卡（型号 + 状态徽章行：连接/电量/协议/降噪）+ 聆听模式卡 + 均衡器卡 + 4 张场景卡 |
| 892 | 声音控制 | 3 段模式分段控件 + 关注语音 / Speak-to-Chat 开关 + 连接状态卡 + 应用按钮 |
| 979 | 均衡器 | ULT 机型：ULT Bass 3 段；非 ULT：5 预设 + 响应曲线 + 5 条垂直推子 + `±` 步进 + Clear Bass |
| 1123 | 场景预设 | 4 张场景卡（`w ≥ P(620)` 2 列，否则 1 列），「已应用」态 + 重新应用 |
| 1176 | 设备设置 | 型号行（切换型号）+ MAC/通道自绘输入 + 模拟/真实/断开/重读/关机 + 硬件开关（按机型能力）+ 电量卡 |

### 2.6 交互

| 行 | 内容 |
| --- | --- |
| 1487 | `draw()`：`SaveDC → IntersectClipRect(contentRect) → SetViewportOrgEx(0,-scrollY) → 各页返回 bottom → RestoreDC`，`addHit` 统一补偿 `vShift` |
| 1472 | `drawScrollbar()`：内容超出视口时画 `P(4)` 轨道 + thumb |
| 1606 / 1643 | `onChar` / `onKeyDown`：MAC 十六进制校验、大写、自动插冒号、`←/→/Home/End/Delete/Esc/Tab`，`Enter` 在设备页直接发起连接 |
| 1698 | `onWheel`：`P(64)`/格，边界夹紧 |
| 1728 | `onAction`：**原有 28 个 Action 全部保留**，路由与原实现一致（`connectTo` / `doRefresh` / `applySound` / `applyEq` / `applyScene` / `runControllerAction` / 双重确认的 `A_POWER_OFF`） |
| 1901 | `startAsync` / 1936 `finishAsync`：busy 守卫 + worker 线程 + `PostMessage(WM_APP_ASYNC_DONE)`，**未改动**异步骨架 |
| 1953 | `connectTo(bool sim)`：模拟分支走 `StatefulMdrSimulator`；真实分支改读自绘字段 `macField.formatted()` / `chField.text`，用 `sony::validateBluetoothMacAddress` + 通道 `1..30` 校验后建 `WindowsBthConnector` |

### 2.7 保留的入口兼容

`WinMain`（2355）先解析命令行：`--test-sim` / `--test` / `--help` / `-h` → `AllocConsole()` 后转发
`sony_soundconnect_console_main`（后端 `int main` 被文件头的 `#define main sony_soundconnect_console_main` 改名）。
所以 **UI exe 本身也能跑协议回归**，单文件可编译性不变。

---

## 3. 验证方法与证据

### 3.1 一键构建 + 回归（官方脚本，实际输出）

```
> cmd /c build_sony_ui.cmd
============================================================
 Sony SoundConnect Premium UI - ULT WEAR Build and Test
============================================================
g++.exe (x86_64-posix-seh-rev0, Built by MinGW-W64 project) 8.1.0

[1/3] Building protocol simulator tests...

[2/3] Running simulator regression tests...
================ [ 开始执行协议栈与有状态仿真严格自检 ] ================
  [PASS] 流式拆帧器：收到半包时暂存不误解析
  [PASS] 流式拆帧器：跨次半包重组 + 双帧粘包同时提取 + 特殊字节转义还原
  [PASS] 机型配置：拒绝未知型号，不静默回退到 XM5
  [PASS] MAC 校验：合法地址通过
  [PASS] MAC 校验：拒绝非法字符与尾随垃圾字符
  [PASS] 连接握手与初始状态全量回读 (5字节极端分片流模式)
  [PASS] 机型电量区分：WH-1000XM5 仅解析单电池百分比，不伪造左右耳与充电盒电量
  [PASS] 环境音控制：完整 6 参数下发与 GET 回读状态一致
  [PASS] 均衡器控制：子类型 0x00 + 预设 0xA2 + 频段写后回读一致
  [PASS] DSEE Extreme 控制：使用修正操作码 0xE8/0xE6 写后回读一致
  [PASS] 负向拦截测试：仿真器成功拒绝错误的 0x88 伪 DSEE 指令
  [PASS] 负向拦截测试：仿真器成功拒绝字段不全的 0x68 环境音指令
  [PASS] 双向 ACK 验证：设备发送帧计数与主机有效确认计数相等
  [PASS] 场景预设事务：深度专注模式全部子步骤成功并完成回读验证

==================== [ 索尼耳机状态仪表盘 ] ====================
 运行后端 : [模拟验证模式 - StatefulMdrSimulator]
 目标型号 : Sony WH-1000XM5 (头戴式) | MAC: AC:80:0A:12:34:56 (RFCOMM Ch=1)
 连接状态 : 已连接 | 协商协议: Sony MDR v2.0 | 有效 ACK 计数: 20
 电池电量 : 主机电量 86% (放电中) | 左右耳独立电量: 不适用(头戴式单电池)
----------------------------------------------------------------
 [1] 降噪/环境音 : 主动降噪开启 (ANC) [环境音等级在降噪模式下不适用]
 [2] 均衡器 (EQ) : 预设=Relaxed (舒缓) (InquiredType=0x0) | Clear Bass=+2
     5 频段增益  : 400Hz(+0) 1kHz(+1) 2.5kHz(+2) 6.3kHz(+1) 16kHz(+0)
 [3] 智能免摘对话: 关闭 | 灵敏度: 自动
 [4] 硬件与音频  : 连接模式=音质优先 (LDAC) | DSEE Extreme(0xE8)=开启 | 佩戴感应=开启
================================================================
  [PASS] 关机回归：已排队的 ACK 能被读取，成功后本地连接关闭
  [PASS] ULT WEAR 型号配置：V2 传输、握手字段 3、通道 18 与 EQ 子类型 0x03
  [PASS] ULT WEAR 电量查询载荷：0x22 0x00
  [PASS] ULT WEAR 环境音/Voice Focus 载荷与公开逆向格式逐字节一致
  [PASS] ULT WEAR ULT 2 载荷与公开逆向格式逐字节一致
  [PASS] ULT WEAR 模拟握手与初始状态读取
  [PASS] ULT WEAR 二进制环境音与 Voice Focus 写入/回读
  [PASS] ULT WEAR ULT 2 写入/回读
  [PASS] ULT WEAR 拒绝误用 XM 系列通用五段 EQ 载荷
  [PASS] ULT WEAR 场景预设只使用已配置的 ANC + ULT 2 命令

[自检结果: 全部通过] 本地模拟器中的拆帧、序号、载荷校验与回读断言均通过。
[限制说明] 这不等于真实 Sony 耳机协议已实机验证；真实命令字、通道及状态载荷仍需按型号测试。
(Exit Code = 0)

[3/3] Building Windows native UI...

[SUCCESS] Built: "...\SonySoundConnectUI.exe"
=== exit=0 ===
```

**24 条断言全 PASS，UI 编译 0 error 0 warning**（`-Wall -Wextra` 下无任何输出）。
产物：`SonySoundConnectUI.exe` 421,177 B；`SonySoundConnectTest.exe` 231,697 B。

### 3.2 多分辨率取证（实际执行）

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File shot\run_shots.ps1
```

输出（本机 DPI = 168 / 175%）：

```
pid=2940 hwnd=9439558 dpi=168
page 0 overview -> 1600x900
page 1 sound -> 1600x900
page 2 equalizer -> 1600x900
page 3 scenes -> 1600x900
page 4 device -> 1600x900
eq non-ult -> 1600x900
sound non-ult -> 1600x900
requested 1280x720 -> window 1280x720 png 1280x720
requested 1366x768 -> window 1366x768 png 1366x768
requested 1600x900 -> window 1600x900 png 1600x900
requested 1920x1080 -> window 1920x1080 png 1920x1080
requested 2560x1440 -> window 2560x1440 png 2560x1440
free 1040x640 -> 1209x711
free 1180x700 -> 1180x700
free 2000x1000 -> 2000x1000
done
```

**结论**：5 个目标分辨率都**精确取到请求尺寸**（窗口没有被系统撑大/压小），
任意拖拽尺寸 1180×700、2000×1000 也精确生效；
只有 1040×640 被 `ptMinTrackSize`（`P(680)×P(400)`，本机 175% DPI 下约 1190×700）夹到 1209×711 ——
这是**设计下限**，防止窗口缩到无法阅读（100% DPI 下同一个下限是 680×400）。

截图（`shot\`）：
`res_1280x720.png` `res_1366x768.png` `res_1600x900.png` `res_1920x1080.png` `res_2560x1440.png`
`page_0_overview.png` `page_1_sound.png` `page_2_equalizer.png` `page_3_scenes.png` `page_4_device.png`
`page_eq_nonult.png` `page_sound_nonult.png` `device_model_switched.png`
`free_1040x640.png` `free_1180x700.png` `free_2000x1000.png`

### 3.3 人工自查清单（git 里没有的东西）

- [x] 标题栏无系统边框痕迹：`WM_NCCALCSIZE` 归零 + DWM 1px 帧，截图上四边无 1px 亮线
- [x] 拖动：非按钮区 `HTCAPTION` 可直接拖
- [x] 缩放：8 向后可拉伸；最大化后隐藏缩放边（`IsZoomed` 判断）
- [x] 最大化不盖任务栏（`rcWork`）
- [x] 侧栏 `logicalW < 1200` 自动收成 76dp 图标栏（1280×720 @175% 的截图即是这种形态）
- [x] 内容超高时出现滚动条（均衡器页在 1600×900 @175% 下即滚动态）
- [x] 输入框：`Tab` 切换 MAC/通道、光标闪烁、`Enter` 发起连接
- [x] 所有原有动作仍可触发（模拟连接 / 真实连接 / 断开 / 重读 / 关机 / 模式 / 预设 / 场景 / ULT）

---

## 4. 已知限制 / 未做（与差异清单 §6 对应）

1. **NCSS（第 4 段模式）未补**：后端 `NcAsmMode` 只有 `OFF/NOISE_CANCEL/AMBIENT_SOUND` 三个枚举，
   没有对应操作码；不伪造按钮。
2. **浅色主题未补**：只落地了深色 config 的令牌。
3. **图标是 GDI 几何等价物**，不是 Sony 原 PNGu/vector（不引入资源文件的约束）。
4. **多设备管理、固件升级、空间音频开关未补**：都要动协议层，违反「不改后端协议逻辑」。
5. `sp/`（数值型间距、圆角、字号）在 xapk 里被清零，属**推导值**，推导规则写在差异清单 §1 与代码注释里。
6. 极窄窗口（< 680×400 逻辑像素）被最小尺寸挡住，不会进入"错位"状态。

---

## 5. 调试钩子（可选，默认不编译）

`sony-soundconnect-ui.cpp` 的 `WM_PAINT` 里有一段 `#ifdef UI_PAINT_TRACE`：
加 `-DUI_PAINT_TRACE` 编译后，每次重绘会把 `w/h/dpi/scale/titleH/sidebarW/statusH` 追加到
`paint-trace.log`，用来核对"逻辑尺寸 × 缩放"是否符合预期（本次排查高 DPI 截图错位就是靠它）。
默认构建不产生该文件。

---

# 增量：功能 A（自行获取 MAC + 自动配对/连接）与 功能 B（同步 EQ）

本增量在上一轮重构之上继续，仍然是**单文件 GDI、无第三方依赖**（新增的只有系统库 `bthprops`）。
行号为本次改动后的版本：`sony-soundconnect-ui.cpp` 2746 行、`sony-soundconnect-cross-fixed.cpp` 3302 行。

## 6.1 文件清单（本次）

| 文件 | 动作 | 说明 |
| --- | --- | --- |
| `sony-soundconnect-cross-fixed.cpp` | 改（2577 → 3302） | 新增第 4.5 节「蓝牙设备发现与配对」（约 460 行）、`syncEqualizerState()`、`pollDeviceNotifications()`、仿真器测试钩子、9 条新自检、`--scan` / `--scan-real` |
| `sony-soundconnect-ui.cpp` | 改（2437 → 2746） | 新增 4 个 Action、设备发现卡、EQ 同步按钮与同步状态行、扫描/点选/同步三条异步流程 |
| `build_sony_ui.cmd` | 改 | stage 1 与 stage 3 的 g++ 都追加 `-lbthprops`（stage 1 的 console 目标也编译发现代码） |
| `shot\Shot2.cs` | 改 | 新增 `ScanButton` / `PickDevice` / `SyncEqButton` / `StartSim` / `Scroll`，度量与 `drawDevice` 一致 |
| `shot\run_shots_v2.ps1` | 新增 | 功能 A/B 专项取证（模拟列表、点选连接、ULT/五段两条 EQ 同步、1280×720 / 2560×1440 / 四种自由尺寸，含滚动态） |
| `shot\run_scan_real.ps1` | 新增 | 真实蓝牙枚举（bthprops）在界面里的取证 |
| `shot\a*.png` / `shot\b*.png` | 新增 | 34 张（功能 A 17 + 功能 B 17），见 6.6 |

## 6.2 功能 A：蓝牙设备发现 + 自动配对/连接

### 6.2.1 为什么放在后端

设备枚举与配对是**协议/系统层能力**，放在 `sony-soundconnect-cross-fixed.cpp` 才能被
`--test-sim` 与 `--scan` 这两条命令行回归覆盖；UI 只负责渲染列表与回填输入框。
模拟模式下的假设备列表也因此落在可回归的位置，而不是 UI 里的硬编码。

### 6.2.2 新增类型（后端 4.5 节）

```cpp
enum class DeviceScanStatus : uint8_t { OK, NO_ADAPTER, NO_DEVICES, ACCESS_DENIED, UNSUPPORTED, SIMULATED };

struct BluetoothAudioDevice {
    std::string name;            // UTF-8（由 UTF-16 名称转码）
    std::string macAddress;      // 系统显示序 "9C:19:C2:1F:16:EC"
    bool connected, paired, remembered, audioCandidate;
    int  suggestedRfcommChannel = 8;   // 按机型名称启发式推测，不是 SDP 结果
    bool simulated = false;
};

struct DeviceScanResult {
    DeviceScanStatus status;
    std::string message;              // 面向用户的确定性文案
    unsigned long systemError = 0;    // GetLastError() / HRESULT 原值
    int radioCount = 0;
    std::vector<BluetoothAudioDevice> devices;
    bool succeeded() const;           // OK || SIMULATED
};

class IDeviceDiscovery {             // 接口：scan() / pairDevice() / isSimulated()
    DeviceScanResult scan();
    bool pairDevice(const BluetoothAudioDevice&, void* ownerWindow, std::string& outcome);
};
```

实现体：`SimulatedDeviceDiscovery`（假列表）、`WindowsBluetoothDiscovery`（bthprops）、
非 Windows 的 `#else` 桩（`DeviceScanStatus::UNSUPPORTED`）；`makeDeviceDiscovery(bool useNative)` 工厂。

### 6.2.3 用到的 Windows API 与链接

| API | 用途 |
| --- | --- |
| `BluetoothFindFirstRadio` / `BluetoothFindNextRadio` / `BluetoothFindRadioClose` | 枚举本机蓝牙适配器（拿 `HANDLE hRadio`） |
| `BluetoothFindFirstDevice` / `BluetoothFindNextDevice` / `BluetoothFindDeviceClose` | 枚举已知/已配对/已连接设备 |
| `BluetoothGetDeviceInfo` | 重新读取 `fAuthenticated`、`fRemembered`、`fConnected`、`ulClassofDevice` |
| `BluetoothAuthenticateDeviceEx` | 触发系统配对（一次调用触发一次系统授权 UI） |
| `BluetoothDisplayDeviceProperties` | 未使用（保留能力，不自动弹窗） |

* 头文件：`windows.h` + `bluetoothapis.h`，包在独立的 `#if defined(_WIN32)` 块里（与 `ENABLE_NATIVE_BT` 无关，
  因为 stage 1 的 console 目标不定义该宏但仍要能枚举）。
* 链接：`libbthprops.a`（MinGW 8.1.0 自带，`x86_64-w64-mingw32\lib`）；已实测可静态链接、可运行。
* 搜索参数：`fReturnAuthenticated=TRUE, fReturnRemembered=TRUE, fReturnConnected=TRUE,
  fReturnUnknown=FALSE, fIssueInquiry=FALSE, cTimeoutMultiplier=2`。
  `fIssueInquiry=FALSE` 是有意的：不做二次 IO 查询（不广播、不干扰正在播放的音频），
  只列「Windows 已经知道的设备」，这也符合「自行获取 MAC」的定位——配对交给系统 UI。
* 枚举顺序：`stable_sort` 音频类优先 → 已连接优先 → 名称字典序，保证同一台机器上列表稳定。
* Curl 不到的东西不做：**没有实现 SDP 服务发现**（见 6.7）。

### 6.2.4 MAC 字节序（本次实测定案，很关键）

`BLUETOOTH_DEVICE_INFO.Address.ullLong` 的 12 位十六进制**就是系统显示序（高字节在前）**。
证据：本机 `HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices` 下的子键名恰为
`9c19c21f16ec`、`a8e6e86719b5`，与 `%012llX(ullLong)` 完全一致，也与「设置 → 蓝牙和其他设备」显示一致。

因此：

* `formatMacFromAddress(uint64_t)`：从高位往低位移位拆字节。
* `addressFromMacBytes(const std::array<uint8_t,6>&)`：`v = (v << 8) | b`。
* 既有 `WindowsBthConnector` 里的 `btAddr = (btAddr << 8) | b` **本来就是对的，无需修改**。

（上一轮曾怀疑是 LSB-first，实测子键名后否掉了；注释已写在代码里避免以后再猜。）

### 6.2.5 三种失败分支的确定文案（不假装成功）

| 分支 | 触发条件 | 文案（`DeviceScanResult::message`） |
| --- | --- | --- |
| 无蓝牙适配器 | `GetLastError()` ∈ {`ERROR_NO_MORE_ITEMS`, `ERROR_NOT_FOUND`, `ERROR_DEVICE_NOT_CONNECTED`} | `未检测到蓝牙适配器（错误码 N）。请先在「设置 → 蓝牙和其他设备」中启用蓝牙，或确认适配器已插好。` |
| 无权限 | `ERROR_ACCESS_DENIED` / `ERROR_SERVICE_NOT_ACTIVE` / `ERROR_SERVICE_DISABLED` | `无权访问蓝牙设备列表（错误码 N）。请检查「服务」中「蓝牙支持服务 (bthserv)」是否在运行，并以有权限的账户重试。` |
| 发现不到设备 | 适配器 OK 但设备数为 0 | `扫描完成：找到 N 个适配器，但没有已知/已配对的蓝牙设备。请先在 Windows 中完成一次配对。` |
| 其它 | 其他错误码 | `蓝牙适配器枚举失败（错误码 N）。` |
| 平台不支持 | 非 Windows 构建 | `当前平台不支持原生蓝牙枚举。` |

配对（`pairDevice`）**必须闭环**：

1. 先 `BluetoothGetDeviceInfo`，`fAuthenticated` 已为真 → 直接返回 true（幂等，不重复弹窗）。
2. 否则 `BluetoothAuthenticateDeviceEx(hwnd, hRadio, &info, nullptr, MITMProtectionNotRequired)`。
3. `S_OK` **之后仍要**再次 `BluetoothGetDeviceInfo` 复查 `fAuthenticated`；未置位 → 返回 false 且
   文案 `配对未确认：系统未把该设备标记为已认证，请手动在 Windows 设置中完成配对。`
4. `HRESULT` 映射（`describePairResult`）：`E_ACCESSDENIED` → 权限；`0x800704C7` → 用户取消；
   `0x80070483` → 服务未运行；`0x80070422` → 服务禁用；`0x8007001F` → 通用失败；其它 → 带 16 进制 HRESULT。

即：**绝不在没有系统确认的情况下报告「已配对」**。

### 6.2.6 RFCOMM 通道是「建议值」，不是协商结果

无 SDP 发现，只能按键名启发式（`suggestRfcommChannelForDeviceName`）：

| 名称包含 | 建议通道 | 依据 |
| --- | --- | --- |
| `ult` | 18 | 已配置机型表 `WH-ULT900N.defaultRfcommChannel = 18` |
| `wf-1000` / `wf1000` | 2 | 机型表 `WF-1000XM5 = 2` |
| `1000x` | 1 | 机型表 `WH-1000XM4 / WH-1000XM5 = 1` |
| 其它 | 8 | Sony MDR 系列 SPP 常见通道 |

界面上**必须**同时显示「通道号为按键名推测的建议值，可在上方输入框手改（不做 SDP 发现）」，
并且回填后用户仍可手改再连。

### 6.2.7 模拟模式（`--sim`）的假设备列表

`SimulatedDeviceDiscovery::scan()` 固定返回 4 台（status = `SIMULATED`，`simulated = true`）：

| # | 名称 | MAC | 通道 | 状态 |
| --- | --- | --- | --- | --- |
| 0 | Sony ULT WEAR (WH-ULT900N) | `AC:80:0A:12:34:56` | 18 | 已连接/已配对 |
| 1 | Sony WH-1000XM5 | `AC:80:0A:65:43:21` | 1 | 未连接/已配对 |
| 2 | Sony WF-1000XM5 | `AC:80:0A:AA:BB:CC` | 2 | 未连接/已配对 |
| 3 | 未知蓝牙音频设备 | `AC:80:0A:DE:AD:BE` | 8 | 未连接/**未配对**（用于走配对分支） |

`pairDevice` 在模拟模式下永远返回 `true` 但 `outcome` 以 `（模拟）` 开头，
说明这不是真实配对；非法 MAC 或不在假列表里的设备直接 false（负向可回归）。

### 6.2.8 命令行

| 参数 | 行为 |
| --- | --- |
| `--scan` | 打印模拟设备列表（表格：# / 名称 / MAC / 通道 / 连接 / 配对 / 音频），exit 0 |
| `--scan-real` | 走 bthprops 原生枚举并打印同一张表；失败打印 `[扫描失败] <label>：<message>` 并 exit 1 |

两者是**独立参数**（不是 `--scan --real`），因为 `--real` 后面必须跟 MAC，会与位置解析冲突。
分派位置在 `--real` 校验之前，所以 `SonySoundConnectTest.exe --scan-real` 不需要任何其它参数。

### 6.2.9 UI 流程（`sony-soundconnect-ui.cpp`）

* 新增 Action：`A_SCAN_SIM`、`A_SCAN_REAL`、`A_DEVICE_PICK`、`A_EQ_SYNC`（原有 28 个动作一项没动）。
* 设备页在「设备连接」卡**下方**追加「蓝牙设备发现」卡（不改动上方任何坐标，`shot\Shot2.cs` 的
  `ModelNext` 依赖的「切换型号」坐标因此保持原样）。
* 点选一行 = 回填 MAC + 通道 → 若设备来自真实扫描且未配对，先尝试配对 → 再 `connectDevice`。
  任一步失败都只更新状态栏与日志，不改变已连接状态。
* 失败文案统一前缀：`配对失败，未发起连接：<原因>` / `连接失败：<原因>`。
* 列表最多展示 4 行，超出的靠内容区滚动（与全站一致）。

## 6.3 功能 B：同步 EQ（0x56 → 0x57 / 0x59）

### 6.3.1 报文（字节级）

请求（主机 → 设备），`buildEqualizerGetPayload(profile)`：

| 机型 | 载荷字节 | 长度 |
| --- | --- | --- |
| ULT WEAR（`eqInquiredType = 0x03`） | `56 03` | 2 |
| WH-1000XM5 / WF-1000XM5（`0x00`） | `56 00` | 2 |
| WH-1000XM4（`0x01`） | `56 01` | 2 |

即 `[0] = EQEBB_GET_PARAM(0x56)`，`[1] = profile.eqInquiredType`；
外层仍是标准 MDR 帧（`DATA_MDR` + 序号 + 载荷），由既有 `executeCommandTransaction` 负责组帧、ACK 与超时。

回包 1：`EQEBB_RET_PARAM = 0x57`（对 GET 的应答）

ULT WEAR（子类型 0x03，共 11 字节）：

| 偏移 | 值 | 含义 |
| --- | --- | --- |
| [0] | `57` | 命令字 |
| [1] | `03` | eqInquiredType（必须等于配置里的 0x03） |
| [2] | `00` | 保留（必须为 0） |
| [3] | `00..02` | ULT 模式：0=关闭 / 1=ULT1 / 2=ULT2 |
| [4] | `06` | 后续频段数量（ULT 机型固定 6） |
| [5..10] | `0A × 6` | 占位增益（ULT 机型不回传真实频段，全 0dB = 0x0A） |

非 ULT（子类型 0x00/0x01，共 10 字节）：

| 偏移 | 值 | 含义 |
| --- | --- | --- |
| [0] | `57` | 命令字 |
| [1] | `00` / `01` | eqInquiredType（须等于本机型配置） |
| [2] | `00..A2` | 预设 `EqPresetId`（见下表） |
| [3] | `06` | 频段数量 = 6（Clear Bass + 5 段） |
| [4] | `增益 + 10` | Clear Bass，0x00 = −10dB，0x0A = 0dB，0x14 = +10dB |
| [5..9] | `增益 + 10` | 5 段：400Hz / 1kHz / 2.5kHz / 6.3kHz / 16kHz |

`EqPresetId`：`OFF 0x00`、`ROCK 0x01`、`POP 0x02`、`JAZZ 0x03`、`BRIGHT 0x10`、`EXCITED 0x11`、
`MELLOW 0x12`、`RELAXED 0x13`、`VOCAL 0x14`、`TREBLE_BOOST 0x15`、`BASS_BOOST 0x16`、`SPEECH 0x17`、
`CUSTOM_1 0xA0`、`CUSTOM_2 0xA1`、`USER_DEFINED 0xA2`。

回包 2：`EQEBB_NTFY_PARAM = 0x59` —— **设备主动推送**，载荷结构与 0x57 完全一致（同样按 eqInquiredType 分两种长度），
用于用户在耳机上直接操作时刷新界面。解析函数 `parseDeviceStateFrame` 里 0x57 与 0x59 共用同一段校验代码。

### 6.3.2 校验（不合规一律拒绝并保留旧状态）

* 长度必须精确（11 / 10），`[1]` 必须等于本机型 `eqInquiredType`；
* ULT：`[2] == 0x00`、`[3] <= 2`、`[4] == 0x06`；
* 非 ULT：`[3] == 0x06`，且 `[4]..[9]` 都在 `0x00..0x14`（即 −10..+10dB）内。

任一条不满足 → `executeCommandTransaction` 报
`设备返回了预期命令字，但载荷格式/字段校验失败 (0xNN)；原始载荷=…`，`eqConfig_` / `ultBassMode_` 保持原值。

### 6.3.3 新增控制器接口

`cpp
bool syncEqualizerState();   // 主动发 0x56，等 0x57（内部 pollDeviceNotifications 之外的最短路径）
int  pollDeviceNotifications(); // 非阻塞：select(0) 读传输层 → extractFrames → 回 ACK → 解析并应用（返回应用帧数）
`

`pollDeviceNotifications()` 的关键点：它**先** `connector_->receiveRawChunk(0)` 把已到达的字节喂进拆帧器，
再 `extractFrames()`；对每个 `DATA_MDR/DATA_MDR_2` 都回 ACK，只有序号等于 `deviceRxExpectedSeq_` 的帧才推进状态。
（最初版本漏了传输层读取，导致自检里 0x59 断言失败——这条注释就是那次修复留下的。）

仿真器补齐了测试钩子 `pushUnsolicitedNotificationForTest(payload)`，
可以在不发任何指令的情况下把一帧 0x59 推进 `rxStreamBuffer_`，从而回归「纯被动刷新」这条路径。

### 6.3.4 自动同步时机

| 时机 | 位置 | 结果 |
| --- | --- | --- |
| 连接成功后 | `connectDevice` → `refreshAllDeviceStates()`（内含 0x56→0x57），UI 侧再写 `syncNote` | EQ 页显示真实回读值；状态栏「设备已连接」 |
| 任意命令下发成功后 | `runControllerAction` 成功后接 `pollDeviceNotifications()` + `syncEqualizerState()` | 状态栏与 EQ 页同步刷新 |
| 用户手动 | 「从设备同步」/「从设备同步 EQ」按钮（`A_EQ_SYNC`） | 状态栏 `已与设备同步：<摘要>` |

同步摘要（`describeEqState`）：ULT → `ULT 模式：关闭` / `ULT 模式：ULT 2`；
非 ULT → `预设 明快 · Clear Bass +2 · 频段 +6/+4/+2/+0/-3`。

### 6.3.5 UI（不破坏既有 IA）

* 均衡器页新增按钮：ULT 分支在提示卡下方 `从设备同步 EQ`；非 ULT 分支在底部
  `[应用均衡器][从设备同步]` 同一行，右侧是同步状态行。
* 同步状态行：`● 已与设备同步 · <摘要>`（T_MINT） / `○ 未同步 · <原因>`（T_ON_VAR）。
* 状态栏同时显示 `已与设备同步：<摘要>`（复用既有 `status` 字段，不新增状态栏布局）。
* 为了不把底部操作行推出 1600×900 的可视区，把原来的「真实设备是否支持取决于机型与协议版本。」
  并入「五段均衡器」卡副标题（一行文字），换取同步按钮与状态行同排显示。

## 6.4 新增自检（9 条，挂在 `runStrictSimulationSelfTests`）

1. 模拟发现返回 4 台设备、假 MAC 全部通过 `validateBluetoothMacAddress`；
2. 首台为 ULT WEAR、建议通道 18、`connected == true`；
3. MAC 归一化：`ac:80:0a:12:34:56` / `AC-80-0A-12-34-56` / 无分隔 12 位都得到同一结果；非法输入返回空；
4. `formatMacFromAddress(0x9C19C21F16EC) == "9C:19:C2:1F:16:EC"` 且与 `addressFromMacBytes` 双向一致；
5. CoD 音频类判定：`0x240418` / `0x240404` 为真，`0x00010C` 为假；
6. 模拟配对文案带 `（模拟）` 前缀、非法 MAC 配对返回 false；
7. EQ 同步前置：只发 SET（0x58）不更新缓存，随后的 0x56→0x57 把状态刷新为 `{BRIGHT, +5, {6,4,2,0,-3}}`；
8. 0x59 主动上报（`59 00 03 06 0A 0C 0B 09 08 07` → 预设 JAZZ、频段 {2,1,−1,−2,−3}）被解析；残缺 0x59 被拒且不污染状态；
9. ULT 机型 0x56 子类型 0x03 → 11 字节回包 → `ultBassMode == 1`。

## 6.5 验收记录（本机实测）

| 项 | 命令 | 结果 |
| --- | --- | --- |
| 三阶段构建 | `cmd /c "build_sony_ui.cmd < nul"` | **exit 0**，`warning:` 计数 **0**（与改造前基线一致），自检 **39 PASS / 0 FAIL**（含本次新增 9 条） |
| 后端自检 | `SonySoundConnectTest.exe --test-sim` | 39 PASS / 0 FAIL / exit 0 |
| UI 自检（同一份后端） | `SonySoundConnectUI.exe --test-sim` | exit 0 |
| 模拟扫描 | `SonySoundConnectTest.exe --scan` | 4 台假设备，exit 0 |
| 真实扫描 | `SonySoundConnectTest.exe --scan-real` | `适配器 1 个，已知设备 2 个，音频类 2 个`：`ULT WEAR A8:E6:E8:67:19:B5 通道18 已连接 已配对`、`Redmi AirDots 2 9C:19:C2:1F:16:EC 通道8 未连接 已配对`，exit 0 |

> 编译器：MinGW-W64 g++ 8.1.0；`-Wall -Wextra` 全程无新警告。

## 6.6 截图（`shot\`）

功能 A（设备发现）：

| 文件 | 内容 |
| --- | --- |
| `a1_device_before_scan.png` | 未扫描时的空状态（1600×900） |
| `a2_device_sim_list.png` | 扫描模拟设备后的 4 台假设备 + 徽章 + 免责行 |
| `a3_device_picked_connected.png` | 点选第 1 行后 MAC/通道自动回填（AC:80:0A:12:34:56 / 18）、行高亮、状态栏「所选模拟设备已连接」 |
| `a9_device_real_scan.png` / `a9b_..._scrolled.png` | 真实枚举（bthprops）：ULT WEAR A8:E6:E8:67:19:B5（已连接，通道 18）、Redmi AirDots 2 9C:19:C2:1F:16:EC（已配对，通道 8），文案明确「不是 SDP 协商结果」 |
| `a4/a5_device_1280x720[_scrolled].png` | 1280×720：首屏与滚到底（4 行 + 两行说明完整可见，无重叠/裁切） |
| `a6/a6b_device_2560x1440[_scrolled].png` | 2560×1440 同上 |
| `a7/a8_device_free_{900x640,1040x640,1180x700,2000x1000}[_scrolled].png` | 自由拖拽尺寸（含侧栏收起的 900×640） |

功能 B（同步 EQ）：

| 文件 | 内容 |
| --- | --- |
| `b1_eq_ult_before_sync.png` / `b2_eq_ult_after_sync.png` | ULT 分支：点「从设备同步 EQ」前后的状态行（`● 已与设备同步 · ULT 模式：关闭`） |
| `b3_device_nonult.png` / `b4..b5_eq_nonult_[before|after]_sync.png` | 切到 XM5 并重连后的五段 EQ：`[应用均衡器][从设备同步]` 同行，右侧同步状态 |
| `b6/b6b_eq_1280x720[_scrolled].png` | 1280×720：底部操作行不被裁切（滚到底可见完整一行） |
| `b7/b7b_eq_2560x1440[_scrolled].png` | 2560×1440 同上 |
| `b8/b9_eq_free_*[_scrolled].png` | 四种自由尺寸（含 900×640 窄窗，文字自动省略号不溢出） |

原有 `page_*.png` / `res_*.png` / `free_*.png` 已用同一份二进制重新生成，仍与改造后界面一致。

## 6.7 已知限制（明确不做 / 做不到）

1. **没有 SDP 服务发现**：RFCOMM 通道只能按键名启发式推测（见 6.2.6），可能对非 Sony 设备或改名的设备给错值。
   要真正解决必须实现 SDP（`WSALookupServiceBegin(NS_BTH)` + `SDP_SERVICE_SEARCH_ATTRIBUTE`）或让用户手填。
2. **配对的交互由系统接管**：`BluetoothAuthenticateDeviceEx` 会弹系统授权框；这段 UI 不受本程序控制，
   也无法在无人值守环境（CI/远程会话）里完成，只能给出 `配对未确认` 的可读原因。
3. **不做 IO 查询（inquiry）**：`fIssueInquiry = FALSE`，因此**只能看到 Windows 已知/已配对的设备**，
   看不到「从未配对过、且当前不在广播」的设备。
4. **0x59 只在 UI 主动轮询时被消费**：`pollDeviceNotifications()` 在「同步」「命令下发后」「点击刷新」等时机被调用，
   没有常驻后台线程，因此耳机上的物理操作不会即时反映到界面（需要点一下同步或任意动作）。
5. **同步摘要里的预设名是 UI 侧映射**（关闭/明快/舒缓/人声/重低音/自定义），协议值 `ROCK/POP/JAZZ/...`
   在界面上没有独立可选项——本轮不扩充预设控件，避免改动既有 IA。
6. **机型切换后需要重连**才换成对应协议掩码（既有行为）；此前会给用户一个「模型已换但设备还是旧机型」的错觉，
   现在同步状态行会显示**设备侧**的真实值，可以据此判断是否需要重连（截图 `b3` 即此流程）。
7. **真实耳机未在本机验证 ULT/XM5 的 EQ 回读**：本机配对的 ULT WEAR 只到 RFCOMM 层可达性，
   `SonySoundConnectTest.exe --scan-real` 已证明枚举可用；协议层仍以仿真器 + 自检为准。
8. **自动同步会给每条成功命令多一次往返**（0x56/0x57）。真实设备上是约 1 个 RTT 的开销，
   若目标机型对 GET 不回应，只会在同步状态行里显示失败，不影响命令本身的结果。
9. 设备列表固定最多显示 4 行（更多靠滚动），未做分页/搜索；这是按「随手挑一台」的使用场景取舍。


---

# 第七部分：P0 解除 ULT/EQ 互斥 + P1 离线功能全量

> 本节记录本轮改动。产品约束（用户给定）：① ULT 与 EQ 不互斥，可同时启用；② EQ 是所有索尼机型都有的；
> ③ ULT 只有 ULT WEAR 有。以下每条改动都标出对应代码位置（`文件:行号 · 函数`）。

## 7.1 改动文件清单

| 文件 | 改动性质 | 说明 |
| --- | --- | --- |
| `sony-soundconnect-cross-fixed.cpp` | 协议后端 | 0x03 组合帧、两维状态正交、两维回包解析、本地持久化、启动参数解析、自检扩充 |
| `sony-soundconnect-ui.cpp` | GDI 界面 | 均衡器页重排（ULT 卡 + 全机型预设/推子/Clear Bass）、预设全表、场景三维文案、持久化接线、启动参数接线 |
| `shot\run_shots_v3.ps1` | 取证脚本（新增） | 用启动参数驱动截图，不再"猜坐标点击"，可复现 |
| `CHANGES.md` | 文档 | 本节 |
| `work\xapk\offline-feature-inventory.md` | 文档（新增） | P1 功能矩阵：Sound Connect 全功能 × 是否联网 × 本项目状态 |

## 7.2 ULT WEAR 的 0x03 帧布局：依据、设计与不确定性

### 7.2.1 依据（来自 `work\xapk\decoded\` 与既有实现，不凭空编造）

1. **两维本来就共存于同一帧**：资源里 `strings.txt:819`（`BC_Description_Comprehensive`）原文
   "You can also fine-tune the bass with CLEAR BASS." —— Bass Control（ULT）与 CLEAR BASS 同时可调是产品设计，
   故"互斥"从一开始就是本项目的实现缺陷，而不是产品约束。
2. **UI 层同样共存**：`app_layouts.txt:105 bass_effect_layout`、`222 eq_basseffect_buttons`、
   `242 equalizer_custom_layout`、`115 clearbass_layout`、`265 fragment_clear_bass_setting` 同时存在，
   且 `724-725 ult_btn_sound_effect_assign_fragment(_item)` 说明 ULT WEAR 还有独立入口。
   已实现代码里 `EQEBB_SET_PARAM` 的 ULT 分支（`sony-soundconnect-cross-fixed.cpp:1193`）原本接收 11 字节
   `[0x58,0x03,0x00,ult_mode,0x06,0x0A×6]`，其中 `[2]` 恒 `0x00`、`[5..10]` 恒 `0x0A`（中性 0 dB 编码）。
3. **关键观察**：把原 11 字节与"非 ULT 10 字节帧"（`[0x58,type,preset,0x06,clearBass+10,b0..b4+10]`）对齐后，
   `[0x0A]` 恰好是 `encodeGain(0)`，`[0x00]` 恰好是 `EqPresetId::OFF`，`[0x06]` 在两处都是"跟随 6 个增益字节"的计数语义。
   也就是说：**原实现不是另一种布局，而是把同一布局的 EQ 字段全部写成了常量**。
4. 因此本轮的改动是"把写死的常量换成真实值"，**不改动任何既有字节位置**，也不新增命令字。

### 7.2.2 采用的布局

```
ULT WEAR（eqInquiredType = 0x03）SET/RET/NTFY 载荷，共 11 字节：
  [0] = 0x58 (SET) / 0x57 (RET) / 0x59 (NTFY)
  [1] = 0x03              EQ 子类型（ULT WEAR 固定）
  [2] = preset            EqPresetId（0x00 OFF … 0xA2 USER_DEFINED）
  [3] = ult_mode          0=关闭 / 1=ULT 1 / 2=ULT 2      ← ULT 维度
  [4] = 0x06              跟随 6 个增益字节（Clear Bass + 5 band）  ← EQ 维度
  [5] = clearBass + 10    (-10..+10 → 0x00..0x14)
  [6..10] = band0..band4 + 10（400Hz / 1kHz / 2.5kHz / 6.3kHz / 16kHz）
```

非 ULT 机型保持原样（10 字节，无 `[3]` 这一维）：
`[0x58, eqInquiredType, preset, 0x06, clearBass+10, b0..b4+10]`。

### 7.2.3 不确定性（必须如实记录）

1. **`[2]` 是否真为 preset**：原实现恒写 `0x00`（= 预设 OFF），无法从资源层反证该字节不是保留位。
   若设备实际把它当保留位，则"ULT 模式下下发预设"不会生效——但**不会破坏 ULT 模式**，因为 `[3]` 位置保持不变，
   失败面被限制在"预设不生效"，不会写坏别人的字段。
2. **`[5..10]` 的六字节语义**：判为"Clear Bass + 五段"的依据是与非 ULT 帧同构（`[4]=0x06` 计数、`[5]=clearBass+10`），
   属结构性推断，无厂商文档佐证。若实际顺序不同，最坏结果是增益映射错位（音量曲线不对），不会误发其它命令。
3. **未在真机验证**：本机只到 RFCOMM 可达（`--scan-real` 可枚举），协议层结论来自仿真器 + 逐字节自检。
   真机若拒绝新帧，仿真器侧的错误文案会明确指向"ULT WEAR EQ/ULT 组合 SET 参数格式错误"，便于定位。
4. **ULT 机型仍不开放 DSEE / 编码优先 / 佩戴感应 / 免摘**：这些命令字在 ULT WEAR 配置中未核实（见 7.7 第 6 条），
   界面上继续不给可写开关，避免"看起来能用其实乱发命令"。

## 7.3 后端改动（`sony-soundconnect-cross-fixed.cpp`）

| # | 位置 | 改动 |
| --- | --- | --- |
| 1 | `:620 · MdrProtocol::encodeEqGainByte(int8_t)` | 新增统一的增益编码（clamp −10..+10 后 +10），`buildEqualizerSetPayload` 与 ULT 帧共用，避免两套编码漂移 |
| 2 | `:625 · buildEqualizerSetPayload` | 改用上面的编码函数（原局部 lambda 删除），行为不变（非 ULT 10 字节帧） |
| 3 | `:652 · buildUltBassSetPayload(profile, eq, mode)` | **签名扩展为三参**，输出 11 字节组合帧：`[2]=eq.preset`、`[3]=min(mode,2)`、`[5..10]=clearBass+5 段真实增益`。原"`[2]`/`[5..10]` 写死常量"的根因在这里被消除 |
| 4 | `:1193 · StatefulMdrSimulator` EQEBB_SET_PARAM ULT 分支 | 校验 `size==11 && [1]==eqInquiredType && [3]<=2 && [4]==0x06 && [5..10]<=0x14`，然后同时写入 `eqState_`（preset/clearBass/bands）与 `ultBassMode_`；错误文案区分"格式错误"与"增益越界" |
| 5 | `:1239 · StatefulMdrSimulator` EQEBB_GET_PARAM ULT 分支 | 回包改为真实两维（不再回写死的 `0x00 / 0x0A×6`） |
| 6 | `:2316 · HeadphoneController::setEqualizer` | 删除 `isUltWear` 直接拒绝的分支（互斥根因之一），改为 `writeEqUltFrame(config, ultBassMode_)` |
| 7 | `:2353 · HeadphoneController::setUltBassMode` | 改为 `writeEqUltFrame(eqConfig_, mode)`：写 ULT 时自动带上当前 EQ 维度 |
| 8 | `:2551 · HeadphoneController::writeEqUltFrame` | 新增私有方法：ULT → 11 字节组合帧；非 ULT → 10 字节 EQ 帧。两个入口共用同一份"把另一维合并进来"的逻辑，从结构上保证不可能覆盖 |
| 9 | `:2685 · parseDeviceStateFrame` EQEBB_RET/NTFY | ULT 分支先做完整校验（长度/子类型/模式/计数/增益范围），再统一落库两维；任一字段越界即整帧拒收，不污染已缓存状态 |
| 10 | `applySceneProfile` ULT 分支 | 场景从"只设 ULT 模式"升级为三维：ANC/环境音 + ULT 模式 + EQ 预设（如"街头通勤"= 环境音 + ULT 1 + 重低音） |
| 11 | `:2970 · printDeviceDashboard` | ULT 机型同时打印 ULT 维度（[3] 位）与 EQ 维度（[2]/[5..10]），并注明"两维同帧、互不覆盖"；非 ULT 的 `InquiredType` 改为两位十六进制（原来 `0xA0` 被打印成 `0x0`，排查时误导） |
| 12 | `:300 · struct PersistedState` / `:341` `:364` `:442` `:455`（第 6.5 节） | 本地状态持久化：`version/model/mac/ult/preset/clearBass/bands/ambientMode/ambientLevel/voice`；解析失败一律整体拒绝（不做"部分成功"的静默降级）；落盘走 `.tmp → rename` 原子替换 |
| 13 | `:2856 · makePersistedState` | 从控制器一次抓取两维快照，保证恢复时 ULT 与 EQ 同时回来 |
| 14 | `:2878 · struct ShotStartupSpec` / `:2887 · parseShotStartupArgs` | 取证/演示启动参数解析（纯函数，便于自检）：`--page/--model/--ult/--preset/--connect-sim`，非法值记入 `rejectNote` 而不是静默回落 |

## 7.4 界面改动（`sony-soundconnect-ui.cpp`）

| # | 位置 | 改动 |
| --- | --- | --- |
| 1 | `:1059 · drawEqualizer` | **删除"ULT 机型整块替换均衡器页"的分支**（互斥根因之三）。现在：ULT 机型先画 ULT Bass 卡，**再对所有制式机型**画预设卡 + 五段推子 + Clear Bass；底部按钮行 ULT 机型文案为"应用均衡器 + ULT" |
| 2 | `:782 · presetOptions()` | 预设从 5 项扩到 **15 项全表**（关闭/明快/舒缓/人声/重低音/摇滚/流行/爵士/激昂/柔和/高音增强/语音/自定义 1/自定义 2/用户设定），芯片按宽度自动折行（1280×720 时两行，2560×1440 时一行） |
| 3 | `:803 · presetIndexOf` / `:811 · presetIdAt` | 预设索引 ↔ `EqPresetId` 双向映射，替换 `reloadLocalState` / `applyEq` 里原来的 switch，避免"加了预设忘了同步另一处" |
| 4 | `:839 · drawOverview` | 均衡器卡在 ULT 机型显示 `ULT Bass · ULT N  |  EQ · 预设名`，副标题"两者可同时启用，同帧下发互不覆盖" |
| 5 | `:1225 · drawScenes`（含概览页场景卡） | 场景文案改三维（例："环境音 · ULT 1 · 重低音" / 非 ULT："环境音 · 重低音"） |
| 6 | `:2331 · describeEqState` / `:2311 · presetLabelUtf8` | 同步摘要不再对 ULT 机型"只报 ULT 模式"，改为 `ULT N · 预设 X · Clear Bass ±N · 频段 …`（15 项预设的 UTF-8 名） |
| 7 | `:2048 · settingsPath` / `:2057 · persistSettings` / `:2068 · restorePersistedSettings` | 本地设置接线：`%LOCALAPPDATA%\SonySoundConnectUI\last-state.txt`；构造函数末尾恢复，`finishAsync` 内在状态回填后落盘 |
| 8 | `:1278 · drawDevice` ULT 功能卡 | 清单补上"均衡器：预设 / 五段推子 / Clear Bass（与 ULT 模式同帧、互不覆盖）"，与后端 `printDeviceDashboard` 的 5 项保持一致 |
| 9 | `:2152/:2161/:2177 · applyShotStartupState / applyStartupArgs / applyShotStartupToDevice` + `:2816 WinMain` | 启动参数接线：`--page/--model/--ult/--preset/--connect-sim`。注意这里的 `--ult/--preset` **不是**"只改界面变量"：`applyShotStartupToDevice()` 会把同一组值通过生产路径（`setUltBassMode` / `setEqualizer` / `syncEqualizerState`）真的写进模拟设备再回读，因此截图里"ULT 2"和右下角"已与设备同步 · ULT 2 · 预设 重低音"是同一份状态，不是摆拍 |

## 7.5 P1 离线功能矩阵

矩阵文档：`C:\Users\liuar\Desktop\harness\work\xapk\offline-feature-inventory.md`（38 行条目 + 已实现代码位置对照 + 不做清单 + 不确定点）。
摘要：

- **离线且已实现（18 项）**：设备发现/配对、电量与充电、降噪/环境音/关注语音、环境音等级、ULT Bass、EQ 预设全表、
  五段推子、Clear Bass、**ULT 与 CLEAR BASS 同时生效**、自定义 EQ 槽、DSEE Extreme、连接模式（LDAC/稳定）、
  佩戴感应、免摘对话、场景三维预设、本地设置持久化、离线可用性提示。
- **离线但未实现（10 项）**：自适应声音控制、My Places、自动关机、多点连接、LE Audio、触摸/按键分配、播放控制与音量（AVRCP）、
  头部动作、Battery Care、安全音量 —— 共同点：`decoded/` 只有文案、没有命令字/字节布局证据，按"不许凭空编协议"保持不实现。
- **明确联网，只标注不实现（9 项）**：固件更新下载、360 Reality Audio、账号/云同步、语音提示语言包、流媒体联动（Auto Play）、
  活动成就、帮助与诊断上报等。

本轮为此新增的**离线实现**就是"本地设置持久化"（后端第 12/13 条 + 界面第 7 条）与"启动参数"（第 14 条 + 界面第 9 条）。

## 7.6 自检（挂在 `runStrictSimulationSelfTests`，`:3056`）

自检总数 **39 → 60 PASS / 0 FAIL**（净增 21 条 = P0 净增 8 条 + P1 持久化 9 条 + 启动参数 4 条）。
P0 的 8 条净增来自"替换旧 ULT 块里 7 条只认单维的断言"，覆盖下面 12 个断言点：

**P0 两维正交（净增 8 条 / 覆盖 12 个断言点）**
| 断言 | 覆盖的行为 |
| --- | --- |
| ULT WEAR ULT 2 载荷逐字节一致（EQ 中性时 [2]/[5..10] 为 0x00/0x0A） | 中性态仍产生与原实现完全一致的字节（向后兼容） |
| ULT WEAR 组合帧：preset/Clear Bass/五段与 ULT 模式落在既定位置 | `{0x58,0x03,0x16,0x01,0x06,0x11,0x0D,0x0B,0x0A,0x08,0x06}` 逐字节 |
| ULT WEAR 通用 EQ 写入/回读 | `setEqualizer` 在 ULT 机型可用（不再被拒） |
| **P0 正交性：写 EQ 不覆盖 ULT 模式** | 写 EQ 后 ULT 仍为 2 |
| **P0 正交性：写 ULT 模式不覆盖 EQ 维度** | 写 ULT 后预设 VOCAL / Clear Bass −3 / bands[3]=4 保持 |
| ULT WEAR 0x59 主动上报同帧刷新两维 | 通知帧 `{0x59,0x03,JAZZ,0x02,0x06,…}` |
| ULT WEAR 负向：增益越界（0x15）与 ULT 模式=3 均拒收 | 整帧拒收、缓存不被污染 |
| ULT WEAR 场景预设：ANC + ULT 2 + Bright 三维同时落地 | `applySceneProfile` 三维 |
| 非 ULT 机型：`setUltBassMode` 明确拒绝并给出原因 | 约束 ③（ULT 仅 ULT WEAR） |
| 非 ULT 机型：EQ 预设全表（含 Custom 1）走 10 字节帧 | 约束 ②（EQ 全机型） |
| ULT WEAR EQ 同步：同一 0x57 回包同时回读两维 | 一次 GET 拿到两种状态 |
| ULT 同步前置：仅下发 0x58 时 ULT 缓存不变 | 状态只由回包更新 |

**P1 本地持久化（9 条）**：完整往返、两维同文件共存、version≠1 拒、bands 不足 5 项拒、增益 +11 拒、未知机型拒、
缺 `key=value` 分隔符拒、落盘→读回往返（原子写）、控制器快照同时记录两维。

**启动参数（4 条）**：五项全解析、越界/未知值全部拒绝并留痕、`0` 与"未指定(−1)"语义分离、预设上界 15 与 UI 预设表条目数一致。

> 说明：自检证明的是**本项目的编解码与状态机在仿真器内自洽**，不等于真机协议已被验证（见 7.2.3 第 3 条）。

## 7.7 约束对照表

| 约束 | 落点 |
| --- | --- |
| ULT 与 EQ 不互斥、可同时启用 | 后端 `:652 buildUltBassSetPayload`（同帧承载）+ `:2551 writeEqUltFrame`（合并另一维）；界面 `:1059 drawEqualizer`（同页展示） |
| 下发任一维度不覆盖另一维 | 后端 `:2316 setEqualizer` / `:2353 setUltBassMode` 都走 `writeEqUltFrame`；自检两条"正交性"断言 |
| 0x57/0x59 同时解析两类字段 | 后端 `:2685 parseDeviceStateFrame`（ULT 分支先校验再统一落库） |
| EQ 所有机型都有、ULT 只有 ULT WEAR | 预设全表对所有机型渲染（界面 `:782`/`:1059`）；`setUltBassMode` 对非 ULT 明确拒绝（后端 `:2353`） |
| 单文件 GDI、无新第三方依赖 | 仅新增 `<fstream>`（标准库）；界面仍为 GDI 自绘，无新 lib；`build_sony_ui.cmd` 链接参数未变 |
| 保持无边框 / DPI / 响应式 / 设计令牌 | 未触碰 `WM_NCCALCSIZE`、`P()` 度量、`g_scale`、`T_*` 令牌；新控件全部走既有 `card()/textIn()/chip()/addHit()`；1280×720 与 2560×1440 截图验证 |
| 每个新行为加自检 | 见 7.6（21 条，全部挂在 `runStrictSimulationSelfTests`） |
| `build_sony_ui.cmd` 三阶段 exit 0、零新警告 | 见 7.8 |
| 改动追加 CHANGES.md 且标注对应代码段 | 本节 7.3 / 7.4 / 7.7 |

## 7.8 验收记录

| 项 | 命令 | 结果 |
| --- | --- | --- |
| 三阶段构建 | `cmd /c build_sony_ui.cmd` | **exit 0**，`warning:` 计数 **0**（`-Wall -Wextra`，MinGW-W64 g++ 8.1.0） |
| 后端自检 | `SonySoundConnectTest.exe --test-sim` | **60 PASS / 0 FAIL** / exit 0（改造前基线 39 PASS） |
| UI 产物 | `SonySoundConnectUI.exe --test-sim` | 重新生成，同一份后端；**exit 0**（GUI 子系统下 `AllocConsole` 的输出不进重定向文件，故 UI 侧只以退出码为准；PASS 明细见 `SonySoundConnectTest.exe` 的 60 条） |
| 截图 | `shot\run_shots_v3.ps1` | 14 张（`c1`…`c13`），全部按启动参数驱动、可复现 |

## 7.9 截图清单（`shot\`，本轮新增）

| 文件 | 内容 |
| --- | --- |
| `c1_ult_eq_together_1600x900.png` | **P0 主证据**：ULT WEAR 上 ULT Bass 卡（ULT 2 选中）与预设全表（"重低音"选中）、五段推子同页并存 |
| `c2_ult_eq_together_scrolled.png` | 同一页面滚到底：Clear Bass、"应用均衡器 + ULT"、"从设备同步"、右侧同步摘要（`ULT 2 · 预设 重低音`） |
| `c3_overview_ult_two_dims.png` | 概览页均衡器卡：`ULT Bass · ULT 1  |  EQ · 明快`，副标题"两者可同时启用"；四张场景卡三维文案 |
| `c4_eq_nonult_1600x900.png` / `c5_overview_nonult.png` | XM5（非 ULT）：**没有 ULT 卡**，只有预设全表 + 五段 + Clear Bass（约束 ③） |
| `c6_scenes_ult.png` / `c7_scenes_nonult.png` | 场景页：ULT 机型三维文案，非 ULT 机型两维文案 |
| `c8/c8b_ult_eq_1280x720[_scrolled].png` | 最紧分辨率：预设 15 项自动折两行，底部操作行不被裁切 |
| `c9_ult_eq_2560x1440.png` | 最松分辨率：侧栏展开、卡片拉伸正常 |
| `c10/c11_eq_nonult_1280x720[_scrolled].png` | 非 ULT 机型在最紧分辨率下的 EQ 页 |
| `c12_device_ult_1600x900.png` / `c13_device_ult_1280x720_scrolled.png` | 设备页 ULT 功能清单（4 项，含均衡器）+ 免责说明 |

## 7.10 本轮已知限制（在 6.7 之外新增）

1. **0x03 帧的 `[2]` 与 `[5..10]` 语义是结构性推断**（见 7.2.3），未获厂商文档；真机若不接受，症状会局限在"预设/增益不生效"。
2. **ULT 机型仍未开放 DSEE / 编码优先 / 佩戴感应 / 免摘**：命令字未核实，界面保持不可写（与 6.7 第 9 条同类取舍）。
3. **启动参数 `--ult/--preset` 只对模拟连接生效**：真实设备上它们仍会经生产路径下发（并不旁路），但截图脚本只用于 `--connect-sim`，
   以避免在没有真机时等待超时。
4. **持久化不加密、不校验设备身份**：文件记录的是本机上次选择；换耳机后需在界面上改机型，程序不会替你猜。
5. **自适应声音控制 / My Places 等 10 项离线功能未实现**：`decoded/` 只有字符串与布局，没有命令字证据（见 7.5）。

---

# 8. 本轮增量（2026-10-25）：P0 EQ 回读慢一拍修复 / 场景预设下线 / SDP 文案清理

本轮三个任务：(1) P0 修"EQ 应用后回读慢一拍"，(2) 删除左侧「场景预设」导航项与整页，(3) 删掉设备页 SDP 文案。
文件：`sony-soundconnect-cross-fixed.cpp`（自检 60 → 69 条）、`sony-soundconnect-ui.cpp`（2814 → 2780 行）、
`shot\Shot2.cs`（新增 `RailToggle`）、`shot\run_shots_v4.ps1`（新增，本轮截图脚本）。

## 8.1 任务一：根因（实机定位，不是猜测）

协议层实测：`setEqualizer` 原先只发 `0x58 SET` 且 `expectedReply = std::nullopt`——**等一个 ACK 就返回**。
但设备对 `SET` 也会回一帧 `0x57 RET`（状态回执），该帧进入 `rxParser_` 缓冲后无人消费。
紧接着的 `0x56 GET` 事务（`executeCommandTransaction`）在帧循环里把它当成"本次回读"：
帧序号、载荷校验、解析全部通过，于是**真回读被留到下一次命令**才被读到。
界面表现：每次应用 EQ 后显示的是上一轮的值（慢一拍），第二次操作才对上一次生效。

证据链（都在本仓库）：仿真器原先只在 `EQEBB_GET_PARAM` 回 `0x57`、**不在 `EQEBB_SET_PARAM` 回**，
所以旧自检全部通过——**旧自检覆盖不到这条真机路径**（自检的盲区正是根因所在，这也是为什么先修模型再加断言）。

## 8.2 任务一：修复（两处互为保险，都做了）

| # | 位置 | 改动 |
| --- | --- | --- |
| 1 | `sony-soundconnect-cross-fixed.cpp:2566 drainBufferedFrames()` | 新增。**发命令前**把 `rxParser_` 里已缓冲的帧全部取出并逐帧消费：DATA_MDR/DATA_MDR_2 帧先回 ACK、再按序号推进 `lastDeviceRxSeq_/deviceRxExpectedSeq_`、然后走 `parseDeviceStateFrame()` **解析但不当作本次应答**；残留 ACK 帧直接跳过且不计入 `ackCount_`。传输层用 `receiveRawChunk(0)` **循环读到没有为止**（≤64 轮）——只读一次在 MTU/分片下单帧收不全，残留仍会漏到下一次事务（第一版就是这么错的，见 8.5） |
| 2 | `:2650 executeCommandTransaction()` | 在 `isConnected()` 检查之后、`sendAllBytes()` 之前调用 `(void)drainBufferedFrames();`。保证本次事务只可能读到本次命令的回包 |
| 3 | `:2600 writeEqUltFrame()` | SET 直接以 `Command::EQEBB_RET_PARAM` 作为 `expectedReply`（ULT 组合帧与非 ULT 10 字节帧都走这条）；`setEqualizer` (:2328) / `setUltBassMode` (:2364) 统一调它，ULT/非 ULT 两条路径一致。SET 拿不到 `0x57`（超时）时**退回发一次 `0x56 GET`** 兜底，两条都失败才返回 false，`lastError_` 里同时带两条错误 |
| 4 | `:1022 buildEqStateReplyPayload()` + `:1221 EQEBB_SET_PARAM` | 仿真器补齐真机行为：`0x58` 落库后**也回一帧 `0x57`**（内容为刚写入的新值），`0x56 GET` 复用同一个构造函数。这样仿真器才具备复现"SET 回包残留"的条件 |

## 8.3 任务一：新增自检（`--test-sim`，9 条，全部挂在 `runStrictSimulationSelfTests`）

在 P1 持久化块之前插入 P0 回归块，复用 XM5 模拟器并把输出强制切成 **5 字节分片**（最恶劣的拆帧条件）：

```
  [PASS] P0 慢一拍回归前置：XM5 模拟连接成功（5 字节分片流）
  [PASS] P0 慢一拍：setEqualizer 返回时缓存已是本次写入的新值（SET 自带的 0x57 即回读结果）
  [PASS] P0 慢一拍：一次 EQ 写入只产生一帧设备数据回包（写入事务自带回读，不再另发 GET）
  [PASS] P0 慢一拍：写后第一次 syncEqualizerState 即为最新值（拖到第二拍才对即回归）
  [PASS] P0 慢一拍：缓冲里的旧 0x57 不会被写入事务当成应答（写入结果仍是本次新值）
  [PASS] P0 慢一拍：残留帧被消费并回 ACK（设备侧不会卡在未确认帧上）
  [PASS] P0 慢一拍：0x56 GET 不吃残留旧帧（sync 结果等于设备当前真实状态）
  [PASS] P0 慢一拍：清理后设备→主机队列已无残留帧（不会被下下一次命令再吃一次）
  [PASS] P0 慢一拍：ULT 与非 ULT 路径一致——ULT 组合帧 SET 自带 0x57，首次 sync 即最新值
```

其中"缓冲里的旧 0x57"那条是关键：用 `pushUnsolicitedNotificationForTest()` 先往主机队列塞一帧**旧值** `0x57`
（JAZZ），再 `setEqualizer(fresh)`，断言写入结果与随后的回读**都是 fresh 而不是 JAZZ**；
同时断言 `didHostAcknowledgeAllDeviceFrames()` 为真——残留帧被 drain 回 ACK 了，设备不会卡在"未确认帧"上。

另有两条旧断言因"设备对 SET 回 0x57"这一事实变化而改写（**条数不变**，语义更严）：
- `EQ 写入前置：仅下发 0x58（不自带 GET）时，写入后的缓存即为设备 0x57 回包里的新值`（原为"缓存不更新"）
- `ULT 同步前置：仅下发 0x58 时 ULT 模式已由设备 0x57 回包刷新`（原为"ULT 模式缓存不变"）

## 8.4 任务二/三：删除与措辞

| 位置 | 改动 |
| --- | --- |
| `sony-soundconnect-ui.cpp:330` | `enum Page { OVERVIEW, SOUND, EQUALIZER, DEVICE };`（去 `SCENES`） |
| `:331-336` | Action 去 `A_NAV_SCENES`、`A_SCENE` |
| `:393` 起 | 删 `int appliedScene = -1;` |
| `:838 drawOverview` | 删「快速场景」整块（原 4 张卡 + `addHit(t, A_SCENE, i)`）；`y += cardH + gap;` 之后直接是「最近操作」行 |
| 原 `:1224 drawScenes` | 整函数删除（含 `SceneDef`、卡片、`已应用` pill、"应用此场景"按钮） |
| `:1382 pageTitle` / `:1396 pageSubtitle` | 删 `case SCENES` 两个分支 |
| `:1449` | `const Nav navs[4]`（概览/声音控制/均衡器/设备设置）；`:1461` 循环 `i < 4`；图标 `switch` 的 `case 3` 由"三个圆点(场景)"改成设备矩形（原 `default`），不留死分支 |
| `:1588` 页面分发 | 删 `case SCENES: bottom = drawScenes(...)` |
| `:1811` / 原 `:1924` | 删 `case A_NAV_SCENES`、`case A_SCENE` 处理 |
| 原 `:2568 applyScene()` | 整函数删除（后端 `applySceneProfile()`/`SceneProfile` 保留：仍被自检与协议层使用，不是 UI 死代码） |
| `sony-soundconnect-cross-fixed.cpp:2959` / `:2994` | `ShotStartupSpec::page` 注释与范围 `0..4 → 0..3`；`--page=3` 现在是设备页 |
| `:3668` | 自检 `--page=4` → `--page=3`，断言同步改为 `tolerant.page == 3`（**条数不变**） |
| UI 设备页 原 `:1237-1239` | 删整句「先在 Windows 中配对耳机；MAC 与 RFCOMM 通道需填写真实值（本程序不做 SDP 服务发现）。」 |
| UI `:1296` | 「通道号为按键名推测的建议值，可在上方输入框手改（不做 SDP 发现）。」→ 「通道号为按键名推测的建议值，可在上方输入框手改。」 |
| UI `:2155` | 「请填写已确认的 RFCOMM 通道号（1–30）；当前程序不会自动做 SDP 服务发现。」→ 「请填写已确认的 RFCOMM 通道号（1–30）。」 |
| 后端 `:1788` / `:3709` / `:3713` / `:3807` | 同类用户可见文案同步去 SDP 字样（扫描状态句子、CLI 用法与参数错误），保留"通道为建议值、可在设备页手改"的实质信息。**界面已无 "SDP" 字样**（仅后端源码注释里保留解释性说明，不进入任何 UI/CLI 文案） |

## 8.5 本轮踩到的坑（记录，避免重复）

`drainBufferedFrames()` 第一版只做**一次** `receiveRawChunk(0)`。在 5 字节分片下这次只捞回 5 个字节，
残留的 `0x57` 仍不完整：既没被 drain 消费（因此没回 ACK），又被下一次 SET 事务当成"本次应答"用掉；
更糟的是仿真器"一次只允许一条未确认设备帧"，于是 SET 的回包发不出去 → SET 超时 → 3 条新断言当场 FAIL。
改成"循环读到没有为止"才真正成立。教训：**清理缓冲必须清到传输层空为止，且要回 ACK**，否则只是把慢一拍挪了个位置。

## 8.6 验收记录（原始输出）

**1) 三阶段构建（`cmd /c build_sony_ui.cmd`）**

```
[1/3] Building protocol simulator tests...
[2/3] Running simulator regression tests...
  [PASS] ...
[自检结果: 全部通过] 本地模拟器中的拆帧、序号、载荷校验与回读断言均通过。
(Exit Code = 0)

[3/3] Building Windows native UI...

[SUCCESS] Built: "C:\...\SonySoundConnectUI.exe"
Run SonySoundConnectUI.exe to open the window.
```
- `EXIT=0`；`[PASS]` 计数 **69**；`[FAIL]` 计数 **0**；输出里 `warning`/`警告` 计数 **0**（`-Wall -Wextra`，MinGW-W64 g++ 8.1.0）
- 完整日志：`build-out.txt`（本轮）；上一轮基线 `sim-test.log`

**2) 自检条数**：60 → **69**（+9 条 P0 断言；两条旧断言改写未增减；`--page` 断言只改取值未增减）

**3) 实机路径尝试（本机无可用蓝牙适配器，故按约定以仿真断言为准）**

```
> SonySoundConnectTest.exe --real A8:E6:E8:67:19:B5 --channel 18 --model ULT
[错误] 当前可执行文件未启用原生蓝牙支持，无法连接真实硬件。

> SonySoundConnectTest.exe（以 -DENABLE_NATIVE_BT 重新编译的副本 SonySoundConnectTestBT.exe）--real A8:E6:E8:67:19:B5 --channel 18 --model ULT
[连接失败] 无法连接到设备 A8:E6:E8:67:19:B5 -> 连接蓝牙设备失败，错误码: 10050
```
`10050 = WSAENETDOWN`（本机会话无可用蓝牙网络/适配器），因此**本轮无法在真机复现"set 后首次回读即新值"**。
替代证据：8.3 的 9 条断言在 5 字节分片 + 旧帧残留的最恶劣条件下证明"写入事务自带回读、GET 不吃残留帧"，
以及 8.7 截图里 UI 走生产路径（`setEqualizer` + 一次 `syncEqualizerState`）后显示的同步摘要即为本次写入值。

**4) 截图（`shot\run_shots_v4.ps1`，10 张，全部按启动参数驱动、可复现）**

| 文件 | 分辨率 | 证明 |
| --- | --- | --- |
| `v4_overview_1600x900.png` / `v4_overview_1280x720.png` | 1600×900 / 1280×720 | 左栏**只有四项**（概览/声音控制/均衡器/设备设置，无「场景预设」）；概览页**无「快速场景」**，只剩「聆听模式」与「均衡器」两张卡 |
| `v4_overview_1600x900_rail.png` | 1600×900 | 同一页在收成图标栏形态下的对照 |
| `v4_device_1600x900.png` / `v4_device_1280x720.png` | 1600×900 / 1280×720 | 设备页**无 SDP 整句**；发现卡片底部只剩「通道号为按键名推测的建议值，可在上方输入框手改。」 |
| `v4_device_1600x900_scrolled.png` | 1600×900 | 设备页下半屏（硬件选项 + 电量）无 SDP 字样 |
| `v4_eq_applied_1600x900.png` / `v4_eq_applied_1280x720.png` | 1600×900 / 1280×720 | 均衡器页：ULT 2 选中、预设芯片「重低音」高亮（`--preset=4`） |
| `v4_eq_syncnote_1600x900.png` / `v4_eq_syncnote_1280x720.png` | 1600×900 / 1280×720 | 同一页滚到底：`已与设备同步 · ULT 2 · 预设 重低音 · Clear Bass +0 · 频段 +0/+0/+0/+0/+0`——与上方芯片/推子同值，即"应用后第一次回读就是新值" |

截图脚本新增能力：`shot\Shot2.cs` 加 `RailToggle()`（点左栏底部热区展开/收起图标栏），
否则 1600×900 在 200% DPI 下 `compactRail` 为真，只显示图标、看不到"没有场景预设"这件事。

## 8.7 本轮已知限制

1. **实机验证缺口**：本机会话无可用蓝牙适配器（`--real` 返回 10050），"set 后首次回读即新值"的真机复现留给有耳机的机器。
   仿真器新增的"SET 也回 0x57"来自实机观察，但**回包字节布局仍是结构推断**（与 GET 的 0x57 同构）——若真机 SET 回执字段不同，
   症状会是"SET 事务解析失败 → 自动退回 0x56 GET"，功能不会错值，只是多一次往返。
2. **概览页仍保留「均衡器」摘要卡**：本轮只按要求删掉「快速场景」块；「均衡器」卡不是场景切换，且是均衡器页的入口与两维读数来源，
   故保留。如需连它一起收掉，请明确告知（会同时失去概览页的 ULT/EQ 两维读数）。
3. **`--page` 取值缩到 0..3**：外部脚本若仍在用 `--page=4`（旧设备页）会被 `parseShotStartupArgs` 拒绝并留痕（`rejectNote`），
   不会静默落到错误页面。

---

# v5 变更（用户反馈 m00360：保留概览的情景调节 + 修复聆听模式切换无效）

## 1. 聆听模式切换无效（TASK 1，真正的 bug）

**根因有两层，缺一层都修不好：**

1. **没有下发路径**：`onAction` 里 `A_MODE_OFF/A_MODE_ANC/A_MODE_AMBIENT/A_VOICE` 只改本地变量，随后只打一句
   「已更新界面选择；请点击应用或重新连接以提交。」；而概览页的「聆听模式」卡**只有三个分段按钮、没有应用按钮**，
   所以在概览页点击 = 什么命令都没发。
2. **分段索引被忽略**：旧实现 `case A_MODE_OFF: ambientMode = NcAsmMode::OFF;` 不看 `index`，
   于是点「降噪」「环境音」也被写成 OFF——即使走到应用按钮，发出去的也是错的模式。

**修法**（`sony-soundconnect-ui.cpp`）：

- `A_MODE_OFF` 按 `index` 映射：0=关闭 / 1=降噪 / 2=环境音；`A_MODE_ANC`、`A_MODE_AMBIENT` 两个独立动作也保留。
- 新增 `commitAmbientTap(NcAsmMode)` / `commitVoiceFocus(bool)`：**乐观更新界面 → 工作线程立即下发一条完整 0x68 帧 →
  成功后 `reloadState` 回读**；失败则 `hasAmbientRollback` 把界面与设备一起退回点击前的值。
  `finishAsync` 里回滚先落地、再由 `reloadLocalState()` 用设备真实值覆盖（顺序反了会把回滚值盖掉回读）。
- **只改 mode，不动等级**：载荷由新的纯函数 `sony::ambientTapConfig()` 生成，
  携带用户当前的环境音等级（夹到 1..20，0 会被 0x68 帧拒收），关闭降噪时清掉关注语音。
  切模式顺手把用户选的等级冲掉，同样会被当成"切换有问题"。
- 执行中禁用重复点击：`segmented()` 新增 `enabled` 参数，`busy` 时只画不登记命中区；
  两个提交函数入口再判一次 `busy` 并留日志。
- **滑块（环境音等级）保留「应用声音设置」按钮，不做拖动即下发**：拖动会产生大量帧，
  后端没有节流/合并，按钮是唯一不会把 RFCOMM 队列打满的做法。模式与关注语音走点击即下发。
- 顺带修掉一个取证参数缺陷：`--ult/--preset` 过去在**每次异步结束**都重放，
  连接后用户点情景卡/切模式会被启动参数又覆盖回去（截图出现"情景已套用但 ULT 读数没变"）。
  现在由 `shotStartupApplied` 保证只在刚连上时落位一次。

## 2. 「情景调节」回到概览页（TASK 2）

- `drawOverview` 在设备主卡下方新增**情景调节卡**：4 张（深度专注 / 办公协作 / 街头通勤 / Hi-Fi 鉴赏），
  宽屏一行 4 张、窄屏 2×2；每张卡标注该情景会写入的组合（ULT 机型与非 ULT 机型提示不同）。
- 点击 → `applySceneCard(i)` → 后端 `applySceneProfile()` 三维组合（降噪/环境音 + ULT Bass 模式 + EQ 预设，
  三维正交、互不覆盖），`stepLogs` 逐条进日志，成功后高亮该卡（`activeScene`）。
- 卡片序号 → 场景的映射抽成纯函数：`sony::kSceneCardCount` / `sceneProfileAtCard()` / `sceneCardLabelUtf8()`，
  越界索引自动夹取，UI 与 `--test-sim` 共用同一份实现。
- **左侧「场景预设」导航项与独立页面保持删除状态**：导航仍是四项（概览/声音控制/均衡器/设备设置），
  情景调节只出现在概览页。

## 3. 测试计数：69 → **74 PASS**

新增 5 条断言（`--test-sim`，全部通过）：

| 断言 | 证明 |
| --- | --- |
| 概览页聆听模式点按即下发：一次点击即写入降噪并回读一致（不依赖应用按钮） | 一次 `setAmbientSound(ambientTapConfig(...))` 即成帧 |
| 概览页聆听模式点按即下发：切换模式不改动用户已选的环境音等级（7 级保留） | NC 分支不改 asmLevel |
| 聆听模式点击载荷归一：等级夹到 1..20，关闭降噪的帧不携带关注语音 | 0→1 / 99→20 / OFF 清 voice |
| 概览页情景调节卡：4 张卡序号/标签与场景预设一一对应，越界索引自动夹取 | 卡片序号 ⇄ `SceneProfile` |
| 概览页情景卡点击：4 张卡各一次点击即写满降噪/环境音 + ULT + EQ 三维 | 三维正交、逐卡校验 |

## 4. 截图（`shot\run_shots_v5.ps1`，6 张）

| 文件 | 分辨率 | 证明 |
| --- | --- | --- |
| `v5_overview_1600x900.png` | 1600×900 | 情景调节 4 张卡回到概览页；聆听模式卡三个分段按钮 + 「点按任意选项立即下发到设备」 |
| `v5_overview_1280x720.png` | 1280×720 | 同上，1280×720 下不溢出 |
| `v5_overview_scene_click_1600x900.png` | 1600×900 | 点「街头通勤」：徽章=环境音、聆听模式=环境音、ULT Bass·ULT 1、EQ·重低音，状态栏「情景『街头通勤』已套用（3/3 步）并回读。」 |
| `v5_overview_mode_click_1600x900.png` | 1600×900 | 先点情景切到环境音，再点「降噪」：模式立即变主动降噪 + 「聆听模式已写入设备并回读。」——无应用按钮也生效 |
| `v5_overview_1040x640.png` | 1040×640（实际 1159×682） | 窗口最小尺寸下情景调节与聆听模式仍完整、可滚动 |
| `v5_sound_1600x900.png` | 1600×900 | 声音控制页：模式分段同样点按即下发，环境音等级仍走「应用声音设置」 |

`shot\Shot2.cs` 修了一个取证脚本的度量缺陷：被测 exe 无 DPI 清单（进程 DPI-unaware），
Windows 会把 `GetDpiForWindow` 虚拟化，脚本若按真实 DPI 计算会把点击坐标整体放大 1.75×（在 175% 缩放的机器上全部落空）。
新增 `EffDpi()` 固定按 96 DPI 计算，并新增 `OverviewSceneCard()` / `OverviewModeSegment()` 两个点击助手。

## 5. 剩余风险

1. **环境音等级仍需要「应用声音设置」**：概览页没有等级滑杆（ULT 协议本就不提供 1–20 级），
   非 ULT 机型要改等级必须进声音控制页——这是刻意保留的，不是遗漏。
2. **乐观更新与真实设备的竞态**：点击瞬间界面已是新值，若设备回包慢，用户可能在忙碌态看到"已切换"但状态栏还在转。
   失败回滚能纠正，但**没有做"下发超时"的兜底**（依赖 `setAmbientSound` 自身的超时失败）。
3. **情景卡多维部分失败**：`applySceneProfile` 是逐步骤下发，某一步失败时前几步不会回滚（如实显示），
   卡片不高亮但设备处于"部分应用"状态。是否需要事务化回滚需要产品决定。
4. **真机未验证**：本轮全部证据来自模拟器与截图，蓝牙适配器仍不可用（10050）。0x68 帧的 SET 回执布局仍是结构推断。
