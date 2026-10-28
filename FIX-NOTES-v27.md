# v27 变更说明：音效调节页精简 + 单页排版 + 关键文字放大

生成时间：2026-10-28
基线：v26（`SonySoundConnect-v26-ambient-display-icon`）。
本轮只改 UI 层（`sony-soundconnect-ui.cpp`），**未改** `sony-soundconnect-cross-fixed.cpp` 与蓝牙下发路径。

---

## 1. 删了什么

| 删除对象 | 原位置 | 连带清理 |
|---|---|---|
| 「ULT Bass · ULT WEAR 专属」卡片 | `drawEqualizer()` 开头（原 1158-1171） | 分段控件（关闭/ULT 1/ULT 2）、当前模式说明行、该卡占用的纵向空间与 `y +=` 推进 |
| 「预设音色」预设 EQ 列表 | 原 1173-1215 | 15 个预设芯片、芯片换行布局缓存（`eqPresetChipWidths` / `eqPresetRows` 不再被绘制路径引用）、卡片标题与长说明、`y +=` 推进 |

保留但不再有 UI 入口（**行为不变**，见第 4 节边界）：
- `ultBassMode`（ULT 模式）仍随 `0x03` 帧下发，仍持久化，仍可被 `--ult=` 启动参数设置；
- `selectedPreset`（EQ 预设）仍随帧下发、仍持久化、仍可被 `--preset=` 设置；
- `A_ULT_MODE` / `A_EQ_PRESET` 的动作处理分支保留（避免影响既有键盘/程序化路径），只是本页不再生成对应命中区。

## 2. 单页无滚动的排版

删除两张卡后本页只剩三段：五段均衡器卡 → Clear Bass 卡 → 操作行。

| 参数 | 原值 | 新值 | 说明 |
|---|---|---|---|
| `graphH`（曲线高度） | `P(420)` | `P(370)` | 与推子列等高比例 |
| `faderH`（推子列总高） | `P(430)` | `P(380)` | 控件行加高 78 后仍留 302px 行程 |
| 推子列间距 `cg` | `P(5)` | `P(8)` | 字号变大后列间不再拥挤 |
| 卡片副标题字体 | `F_CAP`(11dp) | `F_BODY`(13dp) | 说明文字更易读 |

**结论口径**：在 **1600×900 客户区**（截图 `shot-eq-single-page-1600x900.png`）与 **程序默认窗口 1403×814 逻辑像素 / 2455×1425 物理像素 @175% DPI**（截图 `shot-eq-default-size.png`）下，整页完整显示、无滚动条。

判据（非肉眼）：
- 右边缘第 1580–1598 列像素扫描，非背景像素数全为 **0** → 渲染层没有绘制滚动条（有滚动时会画高亮轨道）；
- 操作行按钮与状态栏之间留有空白，未出现内容压到状态栏。

边界：**1280×720 仍会滚动**（`shot-eq-single-page-1280x720.png`，Clear Bass 需下滚）。原因是 p() 缩放同时缩小窗口与内容，但页头/状态栏占固定比例；本轮的验收尺寸是上面的两个，不是 1280×720。

## 3. 放大了哪些文字

参照用户提供的截图（五段推子下方的 `+5 / 400` 与 `−/+` 按钮），把「对调 EQ 有帮助」的三类文字放大：

| 文字项 | 原字体 | 新字体 | 行高 |
|---|---|---|---|
| 每段增益数值（如 `+5`、`0`） | `F_CAP` 11dp | `F_TITLE_S` 15dp | 18 → 24 |
| 频点标签（`400 / 1k / 2.5k / 6.3k / 16k`） | `F_CAP` 11dp | `F_BODY_M` 13dp | 18 → 20 |
| `−` / `+` 按钮 | 16×16（半径 5px 圆角） | 22×22（半径 6px 圆角），命中区随之扩大到 22px | — |

未放大：卡片标题（17dp 已足够）、Clear Bass 的步进器数字（`F_TITLE` 17dp）、底部同步摘要（`F_CAP`，一行内需容纳长文本，放大将截断）。

## 4. 牵连点与处理（按要求先列后改）

| 牵连点 | 是否受影响 | 处理 |
|---|---|---|
| `sony-soundconnect-cross-fixed.cpp` 的 ULT 帧构造 / 预设枚举 | 否 | 未改一行；`buildUltBassSetPayload` 仍接收 `ultBassMode_` |
| 配置持久化 `%LOCALAPPDATA%\SonySoundConnectUI\last-state.txt` | 否 | `ult=` / `preset=` 字段照旧读写；界面上看不到入口但值仍生效 |
| 默认值 | 否 | `ultBassMode=0`（关闭）、`selectedPreset=0`（关闭）不变 |
| `--test-sim` 断言 | 否 | 98 PASS / 0 FAIL，与改动前一致 |
| `--page/--ult/--preset` 取证启动参数 | 否 | 仍可用，只是页面上不再有对应控件 |

## 5. 验证

| 项 | 结果 |
|---|---|
| 构建 | `SonySoundConnectTest.exe`、`SonySoundConnectUI.exe` 均 exit 0，**warning 0 / error 0**（`build-test.log`、`build-ui.log` 为空） |
| 自检 | `--test-sim` **98 PASS / 0 FAIL**（无新增失败） |
| 无滚动 | `shot-eq-single-page-1600x900.png`（1600×900）、`shot-eq-default-size.png`（默认窗口 @175% DPI） |
| 边界尺寸 | `shot-eq-single-page-1280x720.png`（该尺寸仍滚动，已说明） |
| 补丁 | `v27-eq-page-simplify.diff`（1 文件 / 2 hunk，`git apply --reverse --check` = 0） |

## 6. 本轮未做

- 未真机复测（本轮为纯布局/字号改动，不触碰协议；真机行为与 v26 相同）。
- 未新增字号档位，只复用现有阶梯（`F_BODY` / `F_BODY_M` / `F_TITLE_S`），避免动 `FontKind` 与 `kFonts` 表引发全局观感变化。
- 未删除 `A_EQ_PRESET` / `A_ULT_MODE` 的后端处理分支（保留可程序化触发的路径），如需彻底移除请另行确认。
