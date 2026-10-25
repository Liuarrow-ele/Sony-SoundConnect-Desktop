# Sony Sound Connect 13.3.0（xapk）规范 vs 现有 Win32 实现 —— 差异清单

> 目标：把 `sony-soundconnect-ui.cpp`（原 945 行 GDI 手绘）重构到 Sound Connect 13.3.0 的设计语言，
> 并补齐缺失功能、无边框化、多分辨率适配。
>
> 本文所有 Sony 侧取值都标注了证据来源（APK 资源表 / 已解码 XML / 具体样式名），
> 拿不到的值会明确写「不可恢复」并给出推导规则，而不是编一个数字。

---

## 0. 取证链与可复现方式

| 环节 | 位置 / 命令 |
| --- | --- |
| 原始包 | `work\xapk\inner\com.sony.songpal.mdr.apk`（Sound Connect 13.3.0，104 MB，含 `resources.arsc`） |
| 已解码资源 | `work\xapk\decoded\`（`colors.txt` 470 行、`dimens.txt`、`strings.txt`、`values_simple.json`、`values_complex.json`、`xml\` 1488 个 layout/drawable） |
| 主题解析脚本 | `work\xapk\themedump.py`（按 config 展开 style，把 `@0x7f…` 解析成 `@color/…` 与最终字面值） |
| 布局解析脚本 | `work\xapk\spec.py`（`layout <name>` 打印已解析资源名的层级树） |
| 缺陷修复 | `work\xapk\andres.py` 第 368 行 `rid = int(val[2:], 16)` → `int(val[3:], 16)`（原来解析 `@0x7f…` 直接抛异常，所以 `decoded\xml\*` 里残留的是未解析的 `@0x7f…`，**不能直接当文本读**） |

主题链（证据）：
`style/AppTheme.FollowSystem` → `style/Base.AppTheme.FollowSystem` → `style/Base.vAll.AppTheme.FollowSystem`（该 style 有 **两套 config**：默认浅色 + 夜间深色）。本项目取**夜间深色**（理由见 §5 冲突 1）。

---

## 1. 结论先行：xapk 里哪些东西是真的、哪些是假的

| 资源类别 | 能否恢复 | 依据 |
| --- | --- | --- |
| **配色令牌** | ✅ 完全可恢复 | `Base.vAll.AppTheme.FollowSystem` 的 `<item>` 全是字面 `#AARRGGBB` |
| **排版体系（命名/层级）** | ✅ 可恢复 | 38 个 `TSS_*/TLL_*/TS_*` 样式，`T{字号}_{对齐}_{色族}_{字重}` |
| **组件清单 / 变体** | ✅ 可恢复 | layout + drawable selector 可解析 |
| **信息架构** | ✅ 可恢复（移动端形态） | 790 个业务 layout 的层级骨架 |
| **间距 dp 值** | ❌ 不可恢复 | 见下 |
| **圆角半径** | ❌ 不可恢复 | 见下 |
| **字号 sp 值** | ❌ 不可恢复 | 见下 |
| **控件高度/图标尺寸** | ❌ 不可恢复 | 见下 |

### 决定性证据：xapk 的 `dimen` 被整体清零

`values_simple.json` 里共有 **895 个 dimen**，取值分布：

```
0dp  = 773
0sp  =  60
nandp=  11
0    =   7
0.26 =   3
0px  =   3
0.38 =   3
1    =   3
非零 dp 总数 = 0
```

抽查确认（`themedump.py dimen`）：

- `dimen/stm_update_text_left_margin` = `0dp`
- `dimen/device_settings_done_button_margin_top` = `0dp`
- `dimen/ui_common_font_size_SS / S / M / L / LL / 2S / 3S / 3L / 4S / 4L / 42` **全部 `0sp`**，且只有 default 一个 config。
- `style/TSS_L_C2_Re` = `{textSize → @dimen/ui_common_font_size_SS (=0sp), textStyle=normal, textColor → ?attr/ui_common_color_C2}`
- `drawable` 里的 `<corners radius="@0x7f070032">` 解析出来是 `dimen/abc_control_corner_material` —— AppCompat 占位值，不是 Sony 自己的圆角。

**推断**：Sony 打包时把真实 dp/sp 注入到内部构建产物，xapk 释放出来的是 stub。因此：

> **数值型规范（间距 / 圆角 / 字号 / 控件高度）无法从这份 xapk 恢复。**

本项目的推导规则（写进代码注释，可审计）：

1. **间距**：8dp 栅格为主（4/8/12/16/20/24/32），与 Android Material/SCA 组件默认节奏一致；
2. **字号**：按 Sony 自己的字号阶梯命名排序推导 —— `SS < S < M < L < LL < 2S? < 3L < 4L`，
   落到 11/13/13-600/15/17/20/26/34（logical px），保证单调递增且与 `TSS_L_C2_Re`（正文小字）、
   `TLL_L_C1_Re`（大标题）、`T3S_C_C1_MeStyle`（模式按钮居中中号）三者的**相对关系**一致；
3. **圆角**：卡片 16、按钮胶囊 `h/2`、输入框 12、分段控件 12、开关 `h/2` —— 与 Sony 的
   "卡片大圆角 + 全胶囊按钮 + 方形描边分段块" 三种形态区分开。

---

## 2. 配色差异

### 2.1 Sony 深色主题令牌（**精确值**，`#AARRGGBB`）

| Sony 属性名 | 值 | 新实现令牌 | 旧实现 | 判定 |
| --- | --- | --- | --- | --- |
| `surface` | `#FF0E1010` | `T_SURFACE` | `BG` `#0A0F16`（偏蓝） | **改**：Sony 是中性近黑，不是蓝黑 |
| `surf_container` / `ui_common_bg_color` 邻域 | `#FF1C2021` | `T_CONTAINER`（侧栏/标题栏） | `SIDEBAR` `#0E151E` | **改** |
| `surface_container_high` | `#FF282D2F` | `T_CONT_HIGH`（hover/次级面） | — | **新增**（旧实现无 hover 面色） |
| `surf_container_highest` = `sca_card_background_layer1/2` | `#FF36393A` | `T_CARD`（卡片） | `PANEL` `#131C27` + `PANEL2` `#182330` | **改**：卡片必须比背景**亮**，旧实现卡片比背景还暗/接近 |
| `ui_common_bg_color_card_screen` | `#FF121212` | `T_SCREEN`（输入框/图形空槽） | — | **新增**（旧实现用 `PANEL2` 顶替） |
| `on_surface` | `#FFE1E3E3` | `T_ON` | `TEXT` `#EEF4F9` | **改**：Sony 主文字不是纯白 |
| `on_surface_variant` | `#FFC4C7C7` | `T_ON_VAR` | `MUTED` `#8FA0B1` | **改**：旧实现对比度不足（`#8FA0B1` 在 `#0A0F16` 上约 6.2:1，Sony 的 `#C4C7C7` 是 9.9:1） |
| `on_surface_disabled` | `#61E1E3E3`（38% α） | `T_DIS` ≈ `blend(T_ON,38,T_CARD)` | — | **新增** |
| `outline` | `#FF8E9192` | `T_OUTLINE` | — | **新增** |
| `outline_variant` | `#FF444748` | `T_OUTLINE_VAR` | `BORDER` `#2A3949`（偏蓝） | **改** |
| `ui_common_divider_color` = `sca_divider` | `#40FFFFFF`（25% α） | `T_DIV_CARD = blend(white,25,T_CARD)`、`T_DIV_CONT` | — | **新增**：旧实现没有分隔线令牌，直接复用 `BORDER` |
| `ui_common_press_color` | `#1FFFFFFF`（12% α） | `T_PRESS` | — | **新增** |
| `primary` = `theme_color_accent` = `theme_color_app` = `sca_primary` = `ui_common_color_A1/A2` | `#FFC5D4FF` | `T_ACCENT` | `BLUE` `#6797FF` / `TEAL` `#5CE0C6` | **改**：Sony 主色是**淡长春花蓝**，不是高饱和蓝，也不是青色；且旧实现同时用蓝+青+紫三主色 |
| `primary_container` | `#FF005141` | `T_PC_GREEN` | — | 新增 |
| `sca_primary_container` | `#FF364FA8` | `T_PC_BLUE` | — | 新增 |
| `theme_color_tab_indicator` / `theme_color_tab_text_selected` | `#FF5FDBBA`（薄荷） | `T_MINT` | `TEAL` `#5CE0C6` | **改**：Sony 的"选中/在线"绿，仅用于状态，不当主色 |
| `theme_color_tab_text` / `theme_color_toolbar_icon` | `#B3FFFFFF`（70% α） | `T_ON_VAR` 一档 | `MUTED` | **改** |
| `ui_common_color_C1 / C2 / C3 / C4` | `#FFFFFFFF / #B3FFFFFF / #80FFFFFF / #40FFFFFF` | 文字四级阶梯 | 旧实现只有 2 级（TEXT/MUTED） | **新增 2 级** |
| `icon_big_header` / `icon_unelected` | `#B2FFFFFF` | 图标色 | — | 新增 |
| multicolor 01–07（深色） | `#7384FC` `#F47F95` `#FF884B` `#04AFBC` `#A0D4FC` `#F5F5F5` `#9A9A9A` | `T_INDIGO/T_PINK/T_ORANGE/T_CYAN/T_SKY/T_GREY` | `PURPLE/GREEN/AMBER/RED` | **改**：旧实现是自造语义色，Sony 有固定 7 色数据可视化色板 |
| `eq_graph_center_line_color` | `#FF909090` | `T_EQ_CENTER` | — | **新增** |
| `eq_graph_line_color` | `#FF646464` | `T_EQ_LINE` | — | **新增** |
| `fyeq_selection_color_gold` | `#FFAA8940` | `T_GOLD` | `AMBER` `#F9C15D` | **改**：EQ 金色是 Sony 旧视觉残留（`ui_common_color_a1_5 #0DAA8940`） |

**旧实现整体问题**：整套色板是"蓝黑底 + 高饱和青/紫/蓝"的自造现代风，与 Sony 的
"中性近黑底 + 灰阶层级 + 单一淡蓝主色 + 薄荷状态色"两套体系不兼容 —— 12 个色值里只有 0 个对得上。

### 2.2 透明度处理方式差异

Sony 大量使用 8 位 α（`#40FFFFFF`、`#B3FFFFFF`、`#1FFFFFFF`）。
GDI 的 `COLORREF` 不带 α，`AlphaBlend` 又需要额外链路，所以新实现用
`constexpr COLORREF blend(COLORREF fg, int percent, COLORREF bg)` 在**编译期预合成**：

```cpp
constexpr COLORREF T_DIV_CARD = blend(T_ON, 25, T_CARD);   // #40FFFFFF over #36393A
constexpr COLORREF T_PRESS    = blend(T_ON, 12, T_CARD);   // #1FFFFFFF over card
constexpr COLORREF T_DIS_CARD = blend(T_ON, 38, T_CARD);   // #61E1E3E3
```

好处：单文件、零依赖、色彩结果与 Android 上 `#αRRGGBB` 叠加在对应底色上**逐像素一致**。

---

## 3. 排版差异

Sony 的字体样式命名（38 个键）解码为：

```
T { 字号族 } _ { 对齐 } _ { 色族 } _ { 字重 }
例：TSS_L_C2_Re  = 字号 SS，左对齐，色族 C2(#B3FFFFFF)，Regular
    TLL_L_C1_Re  = 字号 LL，左对齐，色族 C1(#FFFFFF)，Regular（设备名/大标题）
    T3S_C_C1_MeStyle = 3S 号，居中，C1，Medium（模式按钮）
    TS_R_C2_Re   = S 号，右对齐（"编辑"文本按钮）
```

| 项 | Sony（可恢复） | 旧实现 | 新实现 |
| --- | --- | --- | --- |
| 字号阶梯 | `SS < S < M < L < LL < 3L < 4L`（值不可恢复） | 12/15/17/20/25/34 px 平铺 | 11/13/13/15/17/20/26/34（logical，×DPI） |
| 字重维度 | `Re/Me/Bo/Li` 四档 | 只有 400/500/600/650/700 混用，无 Li | 400/500/600/700 四档，语义化命名 `F_CAP/F_BODY/F_BODY_M/F_TITLE_S/F_TITLE/F_TITLE_M/F_HEADLINE/F_DISPLAY` |
| 色族绑定 | **样式自带颜色**（`C1/C2/C3/A1/…`） | 颜色与字号分离，调用点手填 | `textIn(dc, rect, s, color, FontKind)` 保持分离，但颜色只用 §2 的语义令牌 |
| 对齐位 | 样式名的第 2 段就是对齐 | 每次调用手写 `DT_LEFT/DT_CENTER` | 保留 `DT_*`，但组件封装（`button/pill/cardTitle`）内建对齐 |
| DPI | Android 自动 sp→px | **完全无 DPI 感知**（96 DPI 假设） | `P(dp) = round(dp × g_scale)`，`g_scale = 响应式系数 × dpi/96` |

> 字号阶梯之所以"可信"：Sony 的 `SS(小) → 4L(特大)` 命名本身就规定了单调序，
> 本项目只把相邻档位的差值按 2/2/2/2/3/3 的节奏铺开，跨越整个 11→34 区间。

---

## 4. 间距 / 圆角 / 尺寸差异

| 项 | 旧实现（硬编码 px） | 新实现（design dp × P()） | 差异原因 |
| --- | --- | --- | --- |
| 窗口边框 | `WS_OVERLAPPEDWINDOW`（系统标题栏 + 粗边框） | `WS_POPUP | WS_THICKFRAME` + `WM_NCCALCSIZE` 归零 + DWM 1px 扩展帧 + 圆角 | 无边框要求（§6） |
| 最小尺寸 | 钉死 `1120 × 760` | `max(420, P(680)) × max(300, P(400))` | 1280×720 / 1366×768 必须可缩小 |
| 侧栏宽 | 232 固定 | `P(236)`；`logicalW < 1200` 时自动收成 `P(76)` 图标栏 | 响应式（索尼移动端无侧栏，见 §5 冲突 3） |
| 页头高 | 84 固定 | `titleH P(44)` + `toolH P(58)` 两段（自绘标题栏 + 页面工具条） | 无边框后必须自己画标题栏 |
| 状态条高 | 31 固定 | `P(28)` | 随 DPI 缩放 |
| 卡片圆角 | 14 / 18 混用 | `P(16)` 统一 | Sony 卡片层级统一圆角 |
| 按钮圆角 | 12 | 胶囊 `h/2`（RaisedButton）/ 无底（FlatButton） | `sca_ui_common_raised_button` / `sca_ui_common_flat_button` |
| 卡片内边距 | 22/16 混用 | `P(20)` 标题 + `P(44)` 副标题基线 | — |
| 网格间距 | 16 | `gapY = P(16)`（8 栅格 ×2） | — |
| 开关尺寸 | 46 × 27 | 44 × 24（轨道）+ 圆钮，选中填充 `T_ACCENT` | Sony 用 checkbox/switch 两种，桌面端取 switch |
| 滑杆 | 轨道 5、钮 r=7 | 轨道 `P(5)`、钮 `P(7)`，命中区扩到 `±13` | 触控/鼠标安全区 |
| 滚动 | 无（内容溢出直接裁掉） | 视口裁剪 + 滚动条（`P(4)` 轨道 + thumb）+ 滚轮 `P(64)/格` | 1280×720 必须能看全内容 |

**8dp 栅格的落地**：所有尺寸都写成 `P(4/8/12/16/20/24/28/32/44/58/… )`，
在 `g_scale = 1` 时正好落在 4 的倍数上，与 Android 的 `4dp` 基线一致。

---

## 5. 无法调和的冲突与裁决（**先写清再动手**）

### 冲突 1：浅色 vs 深色主题

- xapk 的 `Base.vAll.AppTheme.FollowSystem` 有 **两套 config**（默认浅色 + 夜间深色），
  且是 `FollowSystem`（跟随系统）。
- 旧桌面实现是深色。
- **裁决**：取**深色那套的令牌**，理由：
  1. 旧实现、`README`、用户既有截图都是深色，改浅色等于重做产品调性；
  2. 深色令牌完整（surface/container/high/highest/outline/divider 六层齐全），浅色套缺 `sca_card_background_layer*`；
  3. 桌面工具类应用深色更常见。
- 影响：本文 §2.1 全部取值来自深色 config；浅色主题**未实现**（见 §7「未补」）。

### 冲突 2：声控模式 4 段 vs 后端 3 个枚举（**不可调和，已按后端裁剪**）

- xapk：`nc_mode_nc_ncss_asm_layout_ambient_sound_control_mode_buttons` 里是 **4 个等宽按钮**：
  `modeButtonNC / modeButtonNCSS / modeButtonASM / modeButtonOFF`，
  背景 `asc_mode_button_background_selector`（checked → `ui_common_color_a1_5` 底 + `ui_common_color_a1` 描边）。
- 后端：`sony-soundconnect-cross-fixed.cpp:182-186`
  ```cpp
  enum class NcAsmMode { OFF = 0x00, NOISE_CANCEL = 0x01, AMBIENT_SOUND = 0x02 };
  ```
  **没有 NCSS（噪声抑制/自适应）枚举，也没有对应操作码。**
- **裁决**：分段控件保留 **3 段（关闭 / 主动降噪 / 环境音）**，视觉按 Sony 的 checked 态实现，
  **不伪造第 4 个按钮**。清单里标为「未补 · 协议无对应枚举」。
- 备选方案（未采纳）：画 4 段但 NCSS 置灰 —— 会造成"功能存在但点不动"的误解，比不画更差。

### 冲突 3：移动端信息架构（toolbar + 页面栈） vs 桌面端（侧栏 + 多页面常驻）

- xapk 里**没有 bottom-navigation**，只有 `ui_common_toolbar` + drawer/activity 栈
  （`device_detail_activity`、`*_fragment`、`*_third_layer_fragment`）。
- 桌面窗口 1280×720 以上，用移动端 stack 导航会浪费横向空间。
- **裁决**：保留**侧栏 + 5 页常驻**（旧实现的 IA），但：
  - 侧栏项改成 Sony 的"图标 + 主标题 + 副说明"两行结构（对应 `TSS_L_C1_Re` + `TSS_L_C2_Re`）；
  - 选中态用 `blend(T_ACCENT,14,T_CONTAINER)` 填充 + `P(3)` 宽 `T_ACCENT` 竖条（Sony tab indicator 的等价物）；
  - 窄窗口自动收成图标栏（见 §4），保留移动端"一屏一功能"的密度感。

### 冲突 4：Sony 用矢量图标，旧实现零图标资源

- xapk：`drawable/a_mdr_connect_icon_bt_s`、`a_mdr_codec_sbc_light`、`a_mdr_dseehx_light` 等 PNG/vector。
- 约束：不引入第三方库、保持单文件、不能内嵌二进制资源。
- **裁决**：用 GDI 几何图元画等价图标（环/圆/竖条/圆点/矩形 + 小尾巴），
  不引入图标字体。清单标为「部分补 · 几何等价物」。

---

## 6. 功能补齐清单（已补 / 部分补 / 未补）

| # | 功能点 | Sony 依据 | 状态 | 说明 |
| --- | --- | --- | --- | --- |
| 1 | 无边框窗口 + 自绘标题栏 | 桌面端要求 | ✅ 已补 | `WS_POPUP|WS_THICKFRAME`，`WM_NCCALCSIZE` 归零，`WM_NCHITTEST` 自绘 8 向缩放边 + `HTCAPTION` 拖动区 |
| 2 | 最小化/最大化/关闭按钮 | 桌面端要求 | ✅ 已补 | 自绘三按钮，hover 高亮，关闭 hover 变红 `#C42B1C` |
| 3 | 窗口圆角 + 投影 | 桌面端要求 | ✅ 已补 | `DwmSetWindowAttribute(DWMWA_WINDOW_CORNER_PREFERENCE=2)` + `DwmExtendFrameIntoClientArea(1,1,1,1)` |
| 4 | 最大化不盖任务栏 | 桌面端要求 | ✅ 已补 | `WM_GETMINMAXINFO` 用 `rcWork` 钉 `ptMaxSize/ptMaxPosition` |
| 5 | DPI 感知（Per-Monitor V2） | 桌面端要求 | ✅ 已补 | 三级降级：`SetProcessDpiAwarenessContext(-4)` → `shcore!SetProcessDpiAwareness(2)` → `SetProcessDPIAware` |
| 6 | `WM_DPICHANGED` 处理 | 桌面端要求 | ✅ 已补 | 按建议矩形重排 + 重建字体 + 重算度量 |
| 7 | 基于基准宽度的缩放因子 | 桌面端要求 | ✅ 已补 | `sf = clamp(min(logicalW/1440, logicalH/900×1.12), 0.62, 1.55)` |
| 8 | 窄窗口自动收侧栏 | 移动端 IA 借鉴 | ✅ 已补 | `logicalW < 1200` → `P(76)` 图标栏（`A_RAIL_TOGGLE` 可手动展开） |
| 9 | 内容滚动 + 滚动条 | — | ✅ 已补 | 视口裁剪 + 滚轮 + 滚动条，任意尺寸不裁切 |
| 10 | 自绘输入框（MAC / 通道） | `device_detail_fragment` 的 `device_name_change_btn` | ✅ 已补 | 删除原生 `EDIT`（`WS_EX_CLIENTEDGE`），改为 `Field` 结构：焦点描边、占位符、500ms 闪烁光标、十六进制自动插冒号、键盘编辑（←/→/Home/End/Delete/Esc/Enter） |
| 11 | 设备状态徽章行 | `device_card_custom_view_connected`：BT + 电量 + 编码 + DSEE 四枚 | ✅ 已补 | 概览 hero 卡显示 `● 已连接 / 86% / MDR 2 / 主动降噪` 四枚 pill |
| 12 | 电量可视化 | `BatteryView` | ✅ 已补 | `getBatteryState()`（单电池 / 左右耳 / 充电态）→ 百分比 + 进度条 + 徽章 |
| 13 | EQ 响应曲线 | `eq_graph_view` / `eq_graph_center_line_color` / `eq_graph_line_color` | ✅ 已补 | `#121212` 空槽 + 4 条水平网格 `T_EQ_LINE` + 控制点线性插值 + 14% 强调色面积填充 |
| 14 | 5 段垂直推子 + 步进 | `custom_eq_fragment` / `eq_graph_band_button` | ✅ 已补 | 拖动 + `±` 按钮（到边界自动置灰） |
| 15 | Clear Bass 步进器 | `eq_basseffect_buttons` | ✅ 已补 | `-10..+10 dB`，滑杆与步进双向同步 |
| 16 | 场景"已应用"态 | `visualize_scene_select_hide_fragment` | ✅ 已补 | 应用后卡片描边 + 「已应用」pill + 按钮文案变「重新应用」 |
| 17 | 声控模式分段控件（checked 态） | `asc_mode_button_background_selector` | ⚠️ 部分补 | 3 段（缺 NCSS，见 §5 冲突 2） |
| 18 | 硬件功能开关（DSEE / 佩戴感应 / 连接质量 / 免摘对话） | `ncasm_ctrl_sound_experience_toggle_setting_layout` 的 `SCAUICommonCheckbox` | ✅ 已补 | 仅当 `ModelProfile` 声明支持时才出现（避免给 ULT 发别的机型命令） |
| 19 | 真实设备 MAC/通道校验 | `device_detail_fragment` | ✅ 已补 | `sony::validateBluetoothMacAddress` + 通道 `1..30` 校验，失败在状态栏提示 |
| 20 | 连接状态 / busy 指示 | toolbar | ✅ 已补 | 标题栏 pill（`● 模拟设备` / `● 设备已连接` / `○ 未连接`）+ `处理中…` |
| 21 | 键盘可用性 | 无障碍 | ✅ 已补 | `Tab` 在 MAC/通道间切换，`Enter` 触发连接，`Esc` 取消焦点，输入框有光标 |
| 22 | 鼠标指针语义 | 无障碍 | ✅ 已补 | `WM_SETCURSOR`：输入框 → `IDC_IBEAM`，可点控件 → `IDC_HAND` |
| 23 | 浅色主题 | 另一套 config | ❌ 未补 | 见 §5 冲突 1；令牌已按深色落地，加浅色需再取一套 `Base.vAll.AppTheme.FollowSystem` default config 的 60+ 属性 |
| 24 | NCSS / 自适应降噪 | 4 段按钮 | ❌ 未补 | 后端无枚举、无操作码（§5 冲突 2） |
| 25 | 矢量图标集 | `res/drawable*` 的 PNG/vector | ⚠️ 部分补 | 几何图元等价物（§5 冲突 4） |
| 26 | 多设备管理（`device_selection`） | `shape_device_selection_selected` | ❌ 未补 | 后端是单 RFCOMM 会话模型，无多设备注册表；加会动协议层 |
| 27 | 固件升级 / 语言包 | `*_fragment` 若干 | ❌ 未补 | 需要 MDR 升级协议（V1 之外），超出「不改协议逻辑」约束 |
| 28 | 空间音频 / 360RA 开关 | `activity_device_capability` | ❌ 未补 | 后端 `ModelProfile` 无对应能力位 |

---

## 7. 交互差异

| 交互 | 旧实现 | 新实现 | Sony 依据 |
| --- | --- | --- | --- |
| 悬停反馈 | 只有按钮有 `hover` 布尔 | 全部可点元素：卡片、导航、开关、滑杆钮、窗口按钮、`±` | `ui_common_press_color #1FFFFFFF` 同义 |
| 按压反馈 | 无 | 选中/按下用 12% 白叠加 `T_PRESS` | 同上 |
| 拖动 | 只有滑杆拖动 | 滑杆 + 垂直推子 + 标题栏拖动窗口 + 8 向缩放 | — |
| 滚轮 | 无 | `WM_MOUSEWHEEL`，`P(64)`/格，边界夹紧 | — |
| 焦点 | 无（原生 EDIT 自己处理） | `focusField` + 光标闪烁 + `Tab` 循环 | Android focus 态 |
| 危险操作 | 直接执行 | `A_POWER_OFF` 双重确认 `MessageBox` | — |
| 长文本 | 直接裁切 | `DT_END_ELLIPSIS` + 提示文案换行 | — |
| 禁用态 | 无视觉区分 | `T_DIS_CARD` 文字 + 不响应点击 | `on_disabled #61E1E3E3` |

---

## 8. 信息架构差异

| Sony（移动端） | 本项目（桌面端） | 说明 |
| --- | --- | --- |
| `ui_common_toolbar` + 返回栈 | 自绘标题栏（品牌 + 状态 + 窗口按钮） | §5 冲突 3 |
| `device_detail_activity`（设备详情） | 「设备设置」页 | 型号行 / MAC / 通道 / 连接 / 断开 / 重读 / 关机 |
| `ncasm_ctrl_sound_experience_layout`（声控） | 「声音控制」页 | 模式分段 + 关注语音 + Speak-to-Chat + 连接状态 |
| `custom_eq_fragment` + `eq_graph_view` | 「均衡器」页 | 预设 / 曲线 / 5 推子 / Clear Bass |
| `scene_based_listening_fragment` | 「场景预设」页 | 4 张场景卡（2×2 / 1 列自适应） |
| `device_card_custom_view_connected` | 「概览」页 hero 卡 | 型号 + 状态徽章行 + 快捷入口 |
| 无对应（Sony 首页是设备列表） | 「概览」聚合页 | 桌面端需要一个总览入口 |

---

## 9. 复核方式

```bat
:: 1) 编译 UI（0 warning 0 error）
g++ -std=c++17 -pthread -DUNICODE -D_UNICODE -DENABLE_NATIVE_BT ^
    -finput-charset=UTF-8 -fexec-charset=UTF-8 -O2 -Wall -Wextra ^
    sony-soundconnect-ui.cpp -o SonySoundConnectUI.exe ^
    -mwindows -lws2_32 -lgdi32 -luser32

:: 2) 协议回归（13 + 10 条断言全 PASS）
g++ -std=c++17 -pthread -finput-charset=UTF-8 -fexec-charset=UTF-8 -O2 -Wall -Wextra ^
    sony-soundconnect-cross-fixed.cpp -o SonySoundConnectTest.exe
SonySoundConnectTest.exe --test-sim

:: 3) 多分辨率截图取证（自动改尺寸 + 点导航 + PrintWindow）
powershell -NoProfile -ExecutionPolicy Bypass -File shot\run_shots.ps1
```

截图产物在 `shot\`：`res_1280x720.png`、`res_1366x768.png`、`res_1600x900.png`、
`res_1920x1080.png`、`res_2560x1440.png`、`page_0..4_*.png`、`page_eq_nonult.png`、
`free_1040x640.png` / `free_1180x700.png` / `free_2000x1000.png`。

---

## 10. 一句话总结

可恢复的（配色 60+ 令牌、排版体系、组件清单、IA）全部落地；
不可恢复的（dp/sp 数值）按 8dp 栅格 + 有序字号阶梯推导并在代码注释里写明依据；
不可调和的（NCSS、浅色主题、多设备）显式标为「未补」并给出理由，不伪造。
