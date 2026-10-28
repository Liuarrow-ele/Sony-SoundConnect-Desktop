// ============================================================================
// Sony SoundConnect — Windows native desktop front-end (rebuild v5.1)
//
// 设计基线：Sony Sound Connect 13.3.0 的 dark / FollowSystem 主题令牌
//           （由 xapk resources.arsc 解码取得，见 SPEC-DIFF.md）
// 结构：无边框窗口 + 自绘标题栏 + DPI 感知 + 基准宽度等比缩放 + 可滚动内容区
// 依赖：仅 Win32 + GDI（dwmapi / shcore 通过 LoadLibrary 动态调用，无需新增链接库）
// 本文件 #include 后端协议源，保持单文件可编译。
// ============================================================================
#define main sony_soundconnect_console_main
#include "sony-soundconnect-cross-fixed.cpp"
#undef main

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <functional>
#include <thread>
#include <chrono>
#include <exception>

namespace {

// 老版本 MinGW 头文件没有这些常量，按 Win32 官方取值补齐。
#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL
#define DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL 33
#endif

constexpr UINT WM_APP_ASYNC_DONE = WM_APP + 41;

// 从 DLL 取函数指针：走 memcpy 规避 -Wcast-function-type，语义等价且无告警。
template <typename Fn>
Fn procAddr(HMODULE module, const char* name) {
    FARPROC raw = GetProcAddress(module, name);
    Fn fn = nullptr;
    static_assert(sizeof(fn) == sizeof(raw), "function pointer size mismatch");
    std::memcpy(&fn, &raw, sizeof(raw));
    return fn;
}

// ============================================================================
// 0. DPI 感知 —— 运行时动态解析，避免引入额外导入库
// ============================================================================
using PFN_SetProcessDpiAwarenessContext = BOOL (WINAPI*)(HANDLE);
using PFN_SetProcessDpiAwareness        = HRESULT (WINAPI*)(int);
using PFN_SetProcessDPIAware           = BOOL (WINAPI*)(void);
using PFN_GetDpiForWindow              = UINT (WINAPI*)(HWND);
using PFN_GetDpiForMonitor             = HRESULT (WINAPI*)(HMONITOR, int, UINT*, UINT*);

UINT g_dpi = 96;            // 当前窗口 DPI
double g_scale = 1.0;       // 综合缩放 = 响应式系数 × DPI 系数

// 设计稿 → 设备像素
int P(int designPx) {
    int v = static_cast<int>(std::lround(designPx * g_scale));
    return v;
}

void enableDpiAwareness() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto setCtx = procAddr<PFN_SetProcessDpiAwarenessContext>(user32, "SetProcessDpiAwarenessContext");
        if (setCtx && setCtx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)))) return; // PER_MONITOR_AWARE_V2
    }
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore) {
        auto setAware = procAddr<PFN_SetProcessDpiAwareness>(shcore, "SetProcessDpiAwareness");
        if (setAware && SUCCEEDED(setAware(2))) { FreeLibrary(shcore); return; } // PER_MONITOR
        FreeLibrary(shcore);
    }
    HMODULE user32b = GetModuleHandleW(L"user32.dll");
    if (user32b) {
        auto sysAware = procAddr<PFN_SetProcessDPIAware>(user32b, "SetProcessDPIAware");
        if (sysAware) sysAware();
    }
}

UINT queryWindowDpi(HWND hwnd) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        auto getDpi = procAddr<PFN_GetDpiForWindow>(user32, "GetDpiForWindow");
        if (getDpi && hwnd) {
            UINT d = getDpi(hwnd);
            if (d >= 48) return d;
        }
    }
    HDC dc = GetDC(hwnd);
    UINT d = 96;
    if (dc) { d = static_cast<UINT>(GetDeviceCaps(dc, LOGPIXELSX)); ReleaseDC(hwnd, dc); }
    return d < 48 ? 96u : d;
}

// ---- DWM：阴影 / 圆角 / 深色边框（全部动态加载，无新增链接依赖）----
struct MyMargins { int left, right, top, bottom; };
using PFN_DwmExtendFrameIntoClientArea = HRESULT (WINAPI*)(HWND, const MyMargins*);
using PFN_DwmSetWindowAttribute        = HRESULT (WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

void applyDwmWindowLook(HWND hwnd) {
    static HMODULE dwm = nullptr;
    static bool tried = false;
    if (!tried) { tried = true; dwm = LoadLibraryW(L"dwmapi.dll"); }
    if (!dwm) return;
    auto extend = procAddr<PFN_DwmExtendFrameIntoClientArea>(dwm, "DwmExtendFrameIntoClientArea");
    if (extend) {
        // 1px 扩展帧：让 DWM 负责投影，同时把系统边框压到 1 像素且与本窗口底色一致。
        MyMargins m{1, 1, 1, 1};
        extend(hwnd, &m);
    }
    auto setAttr = procAddr<PFN_DwmSetWindowAttribute>(dwm, "DwmSetWindowAttribute");
    if (setAttr) {
        const DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
        const DWORD DWMWA_WINDOW_CORNER_PREFERENCE = 33;
        BOOL dark = TRUE;
        setAttr(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        int corner = 2; // DWMWCP_ROUND（Win11 22000+；旧系统调用失败不影响外观）
        setAttr(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    }
}

// ============================================================================
// 1. 设计令牌（xapk dark 主题，值 = resources.arsc 解出的 ARGB）
// ============================================================================
constexpr int cR(COLORREF c) { return static_cast<int>(c & 0xFF); }
constexpr int cG(COLORREF c) { return static_cast<int>((c >> 8) & 0xFF); }
constexpr int cB(COLORREF c) { return static_cast<int>((c >> 16) & 0xFF); }

// GDI 无 alpha：按 Sony 的 alpha 值预先合成到父表面上。
constexpr COLORREF blend(COLORREF fg, int percent, COLORREF bg) {
    const int r = cR(bg) + (cR(fg) - cR(bg)) * percent / 100;
    const int g = cG(bg) + (cG(fg) - cG(bg)) * percent / 100;
    const int b = cB(bg) + (cB(fg) - cB(bg)) * percent / 100;
    return RGB(r < 0 ? 0 : (r > 255 ? 255 : r),
               g < 0 ? 0 : (g > 255 ? 255 : g),
               b < 0 ? 0 : (b > 255 ? 255 : b));
}

constexpr COLORREF T_SURFACE    = RGB(0x0E, 0x10, 0x10); // surface
constexpr COLORREF T_CONTAINER  = RGB(0x1C, 0x20, 0x21); // surf_container
constexpr COLORREF T_CONT_HIGH  = RGB(0x28, 0x2D, 0x2F); // surface_container_high
constexpr COLORREF T_CARD       = RGB(0x36, 0x39, 0x3A); // surf_container_highest / sca_card_layer1
constexpr COLORREF T_SCREEN     = RGB(0x12, 0x12, 0x12); // ui_common_bg_color_card_screen
constexpr COLORREF T_ON         = RGB(0xE1, 0xE3, 0xE3); // on_surface
constexpr COLORREF T_ON_VAR     = RGB(0xC4, 0xC7, 0xC7); // on_surface_variant
constexpr COLORREF T_OUTLINE    = RGB(0x8E, 0x91, 0x92); // outline
constexpr COLORREF T_OUTLINE_VAR= RGB(0x44, 0x47, 0x48); // outline_variant
constexpr COLORREF T_ACCENT     = RGB(0xC5, 0xD4, 0xFF); // primary / theme_color_accent
constexpr COLORREF T_MINT       = RGB(0x5F, 0xDB, 0xBA); // theme_color_tab_indicator
constexpr COLORREF T_PC_GREEN   = RGB(0x00, 0x51, 0x41); // primary_container
constexpr COLORREF T_PC_BLUE    = RGB(0x36, 0x4F, 0xA8); // sca_primary_container
constexpr COLORREF T_INDIGO     = RGB(0x73, 0x84, 0xFC); // multicolor_01
constexpr COLORREF T_PINK       = RGB(0xF4, 0x7F, 0x95); // multicolor_02
constexpr COLORREF T_ORANGE     = RGB(0xFF, 0x88, 0x4B); // multicolor_03
constexpr COLORREF T_CYAN       = RGB(0x04, 0xAF, 0xBC); // multicolor_04
constexpr COLORREF T_SKY        = RGB(0xA0, 0xD4, 0xFC); // multicolor_05
constexpr COLORREF T_GREY       = RGB(0x9A, 0x9A, 0x9A); // multicolor_07
constexpr COLORREF T_EQ_CENTER  = RGB(0x90, 0x90, 0x90); // eq_graph_center_line_color
constexpr COLORREF T_EQ_LINE    = RGB(0x64, 0x64, 0x64); // eq_graph_line_color
constexpr COLORREF T_GOLD       = RGB(0xAA, 0x89, 0x40); // fyeq_selection_color_gold

// 派生色（对应 #40FFFFFF / #1FFFFFFF / #61E1E3E3 等带 alpha 的令牌）
constexpr COLORREF T_DIV_CARD   = blend(RGB(255,255,255), 25, T_CARD);       // ui_common_divider_color on card
constexpr COLORREF T_DIV_CONT   = blend(RGB(255,255,255), 25, T_CONTAINER);
constexpr COLORREF T_PRESS      = blend(RGB(255,255,255), 12, T_CARD);       // ui_common_press_color
constexpr COLORREF T_DIS_CARD   = blend(T_ON, 38, T_CARD);                   // on_surface_disabled
constexpr COLORREF T_DIS_CONT   = blend(T_ON, 38, T_CONTAINER);
constexpr COLORREF T_DIS_VAR    = blend(T_ON_VAR, 38, T_CARD);

// ============================================================================
// 2. 基础几何 / 文本
// ============================================================================
struct Rect { int x, y, w, h; };

RECT toRECT(const Rect& r) {
    RECT q{r.x, r.y, r.x + r.w, r.y + r.h};
    return q;
}
inline Rect inset(const Rect& r, int d) { return Rect{r.x + d, r.y + d, r.w - 2 * d, r.h - 2 * d}; }
inline Rect shift(const Rect& r, int dx, int dy) { return Rect{r.x + dx, r.y + dy, r.w, r.h}; }
inline bool inside(const Rect& r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

std::wstring W(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return std::wstring(s.begin(), s.end());
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, out.data(), n);
    if (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

void fillRect(HDC dc, Rect r, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color);
    RECT q = toRECT(r);
    FillRect(dc, &q, b);
    DeleteObject(b);
}

void roundRect(HDC dc, Rect r, COLORREF fill, COLORREF edge, int radius, int edgeW = 1) {
    if (r.w <= 0 || r.h <= 0) return;
    const int rad = std::min(radius, std::min(r.w, r.h));
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = CreatePen(PS_SOLID, edgeW, edge);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    RoundRect(dc, r.x, r.y, r.x + r.w, r.y + r.h, rad, rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

inline void roundFill(HDC dc, Rect r, COLORREF fill, int radius) { roundRect(dc, r, fill, fill, radius); }

void hline(HDC dc, int x1, int x2, int y, COLORREF color, int width = 1) {
    if (x2 <= x1) return;
    HPEN p = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old = SelectObject(dc, p);
    MoveToEx(dc, x1, y, nullptr);
    LineTo(dc, x2, y);
    SelectObject(dc, old);
    DeleteObject(p);
}

void vline(HDC dc, int x, int y1, int y2, COLORREF color, int width = 1) {
    if (y2 <= y1) return;
    HPEN p = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old = SelectObject(dc, p);
    MoveToEx(dc, x, y1, nullptr);
    LineTo(dc, x, y2);
    SelectObject(dc, old);
    DeleteObject(p);
}

void ellipseFill(HDC dc, int cx, int cy, int rad, COLORREF color) {
    if (rad <= 0) return;
    HBRUSH b = CreateSolidBrush(color);
    HPEN p = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

void ellipseRing(HDC dc, int cx, int cy, int rad, COLORREF color, int width) {
    if (rad <= 0) return;
    HBRUSH b = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN p = CreatePen(PS_SOLID, width, color);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    Ellipse(dc, cx - rad, cy - rad, cx + rad, cy + rad);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(p);
}

void polylineTo(HDC dc, const std::vector<POINT>& pts, COLORREF color, int width) {
    if (pts.size() < 2) return;
    HPEN p = CreatePen(PS_SOLID, width, color);
    HGDIOBJ old = SelectObject(dc, p);
    MoveToEx(dc, pts[0].x, pts[0].y, nullptr);
    for (size_t i = 1; i < pts.size(); ++i) LineTo(dc, pts[i].x, pts[i].y);
    SelectObject(dc, old);
    DeleteObject(p);
}

void fillPolygon(HDC dc, const std::vector<POINT>& pts, COLORREF color) {
    if (pts.size() < 3) return;
    HBRUSH b = CreateSolidBrush(color);
    HPEN p = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, p);
    Polygon(dc, pts.data(), static_cast<int>(pts.size()));
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(b);
    DeleteObject(p);
}

// Sony 字体阶梯：T{size}_{align}_{colorFamily}_{weight} 的 size → px 映射
// （xapk 中 dimen/ui_common_font_size_* 全部被置为 0sp，故按有序阶梯推导，见 SPEC-DIFF.md）
enum FontKind {
    F_CAP = 0,     // SS  caption / 辅助说明
    F_BODY,        // S   body
    F_BODY_M,      // S   body medium（强调）
    F_TITLE_S,     // M   title small
    F_TITLE,       // L   title
    F_TITLE_M,     // LL  title medium
    F_HEADLINE,    // 3L  headline
    F_DISPLAY,     // 4L  display
    F_COUNT
};

struct FontSpec { int dp; int weight; };
const FontSpec kFonts[F_COUNT] = {
    {11, 400}, {13, 400}, {13, 600}, {15, 500},
    {17, 500}, {20, 600}, {26, 600}, {34, 700}
};

// ============================================================================
// 3. 应用主体
// ============================================================================
class SoundConnectApp {
public:
    HWND hwnd = nullptr;
    HFONT fonts[F_COUNT]{};
    HDC measureDc = nullptr;
    HBRUSH bgBrush = nullptr;

    enum Page { OVERVIEW, SOUND, EQUALIZER, DEVICE };
    enum Action {
        A_NONE, A_NAV_OVERVIEW, A_NAV_SOUND, A_NAV_EQ, A_NAV_DEVICE,
        A_CONNECT_SIM, A_CONNECT_REAL, A_DISCONNECT, A_REFRESH, A_POWER_OFF,
        A_MODEL_NEXT, A_MODE_OFF, A_MODE_ANC, A_MODE_AMBIENT, A_VOICE,
        A_SOUND_APPLY, A_EQ_PRESET, A_EQ_APPLY, A_DSEE, A_WEAR, A_CODEC, A_STC,
        A_SLIDER_AMBIENT, A_SLIDER_BASS, A_SLIDER_BAND, A_ULT_MODE,
        // ---- 新增（无边框窗口 + 自绘输入 + 垂直推子）----
        A_TITLE_MIN, A_TITLE_MAX, A_TITLE_CLOSE,
        A_INPUT_MAC, A_INPUT_CHANNEL,
        A_VSLIDER_BAND, A_BAND_UP, A_BAND_DOWN, A_BASS_UP, A_BASS_DOWN,
        A_RAIL_TOGGLE,
        // ---- 新增（功能 A 设备发现 / 功能 B 同步 EQ）----
        A_SCAN_SIM, A_SCAN_REAL, A_DEVICE_PICK, A_EQ_SYNC,
        // ---- 新增（概览页「情景调节」4 张卡片；索引 0..3 由 sony::sceneProfileAtCard 映射）----
        A_SCENE_CARD
    };

    struct Hit { Rect r; Action action; int index; };
    struct AsyncResult {
        std::unique_ptr<sony::HeadphoneController> controller;
        bool simulation = true;
        bool reloadState = false;
        std::string status;
        std::vector<std::string> messages;
        // ---- 功能 A：扫描结果回传（在工作线程里枚举，回到 UI 线程再落地）----
        bool hasScanResult = false;
        bool scanUsesNative = false;
        sony::DeviceScanResult scan;
        // ---- 功能 B：EQ 同步反馈 ----
        bool hasSyncNote = false;
        bool syncOk = false;
        std::string syncNote;
        // ---- 聆听模式「点击即下发」失败回滚：把界面退回到点击前（或设备回读值）----
        bool hasAmbientRollback = false;
        sony::AmbientSoundConfig rollbackAmbient{};
        // ---- 概览页情景卡：本次点击的卡片序号与结果（-1 = 本次未点情景卡）----
        int sceneCard = -1;
        bool sceneOk = false;
        long long durationMs = 0;
    };

    std::vector<Hit> hits;
    Action activeSlider = A_NONE;
    int activeSliderIndex = 0;
    Action hoverAction = A_NONE;
    int hoverIndex = -1;
    int vShift = 0;              // 内容区滚动位移（hit 记录时补偿）

    std::unique_ptr<sony::HeadphoneController> controller;
    std::string modelCode = "ULT";
    std::string status = "正在初始化模拟设备…";
    std::string lastAction = "欢迎使用 SoundConnect";
    std::vector<std::string> logs;
    Page page = OVERVIEW;

    int ambientLevel = 10;
    int clearBass = 0;
    int bands[5] = {0, 0, 0, 0, 0};
    int selectedPreset = 0;
    bool presetSelectionTouched = false; // only user selection should replace a hidden/current device preset
    int ultBassMode = 0;
    // 取证启动参数（--ult / --preset）的暂存值；-1 表示命令行没有指定，保持原状态
    int shotUltBass = -1;
    int shotPreset = -1;
    int shotAmbient = -1;  // --ambient=0..2：取证截图时把聆听模式摆到指定档位
    bool shotStartupApplied = false;  // 取证参数只落位一次（见 finishAsync）
    sony::NcAsmMode ambientMode = sony::NcAsmMode::NOISE_CANCEL;
    bool voiceFocus = false;
    int activeScene = -1;         // 上次成功应用的概览页情景卡序号（-1 = 尚未应用）
    bool speakToChat = false;
    bool dsee = false;
    bool wear = true;
    bool codecQuality = true;
    bool simulation = true;
    bool busy = false;

    // ---- 功能 A：本机蓝牙设备发现结果 ----
    std::vector<sony::BluetoothAudioDevice> devices;
    std::string deviceScanNote = "尚未扫描：点「扫描模拟设备」或「扫描本机蓝牙」列出候选耳机。";
    bool deviceScanOk = false;      // 上次扫描是否成功（决定列表是否可信）
    bool deviceScanNative = false;  // 上次扫描是否走的原生枚举
    bool deviceScanDenied = false;  // 上次扫描是否因权限/适配器问题失败（决定文案颜色）
    int selectedDevice = -1;

    // ---- 功能 B：同步 EQ 状态反馈 ----
    bool eqSynced = false;
    std::string eqSyncNote = "尚未与设备同步 EQ 状态。";

    // ---- 窗口 / 布局状态 ----
    bool maximized = false;
    bool windowActive = true;
    int scrollY = 0;
    // Cache EQ curve geometry across WM_PAINT. During mouse drag, slider values quantize to
    // integer steps; repeated paints at the same step re-use this polyline rather than resampling.
    bool eqCurveCacheValid = false;
    Rect eqCurveCachedPlot{};
    std::array<int, 5> eqCurveCachedBands{{0, 0, 0, 0, 0}};
    std::vector<POINT> eqCurveCachedPoints;
    std::vector<POINT> eqCurveCachedArea;
    int eqPresetLayoutWidth = -1;
    double eqPresetLayoutScale = -1.0;
    int eqPresetLayoutRows = 1;
    std::vector<int> eqPresetChipWidths;
    std::vector<int> eqPresetRows;
    int contentH = 0;
    bool compactRail = false;
    bool railExpanded = false;
    int titleH = 0, toolH = 0, statusH = 0, sidebarW = 0, padX = 0, gapY = 0;
    int lastScaleKey = -1;
    Rect contentRect{};

    // ---- 自绘输入框（替代原生 EDIT + WS_EX_CLIENTEDGE）----
    struct Field {
        std::wstring text;      // 仅存有效字符（MAC 存 12 位十六进制，通道存数字）
        int caret = 0;
        bool focused = false;
        bool numeric = false;
        bool colon = false;     // MAC 的 XX:XX:... 显示格式
        size_t maxDigits = 12;
        Rect rect{};
        Action action = A_NONE;

        std::wstring formatted() const {
            if (!colon) return text;
            std::wstring out;
            for (size_t i = 0; i < text.size(); ++i) {
                if (i != 0 && i % 2 == 0) out.push_back(L':');
                out.push_back(text[i]);
            }
            return out;
        }
        int fmtIndex(int digits) const {
            if (!colon) return digits;
            if (digits <= 0) return 0;
            int colons = 0;
            for (int j = 0; j < digits && j < static_cast<int>(text.size()); ++j) {
                if (j != 0 && j % 2 == 0) ++colons;
            }
            int idx = digits + colons;
            if (digits % 2 == 0) ++idx;          // 光标落在分隔符之后
            const int len = static_cast<int>(formatted().size());
            return idx > len ? len : idx;
        }
    };
    Field macField;
    Field chField;
    Field* focusField = nullptr;
    bool caretOn = true;

    SoundConnectApp() {
        logs.push_back("UI 初始化完成");
        macField.action = A_INPUT_MAC;
        macField.numeric = false;
        macField.colon = true;
        macField.maxDigits = 12;
        chField.action = A_INPUT_CHANNEL;
        chField.numeric = true;
        chField.colon = false;
        chField.maxDigits = 2;
        chField.text = L"18";
        chField.caret = 2;
        restorePersistedSettings();
    }

    ~SoundConnectApp() {
        for (auto& f : fonts) if (f) DeleteObject(f);
        if (bgBrush) DeleteObject(bgBrush);
        if (measureDc) DeleteDC(measureDc);
    }

    // ---------------------------------------------------------------- 字体
    void initFonts() {
        for (auto& f : fonts) if (f) { DeleteObject(f); f = nullptr; }
        for (int i = 0; i < F_COUNT; ++i) {
            int px = P(kFonts[i].dp);
            if (px < 11) px = 11;
            fonts[i] = CreateFontW(-px, 0, 0, 0, kFonts[i].weight, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
            if (!fonts[i]) {
                fonts[i] = CreateFontW(-px, 0, 0, 0, kFonts[i].weight, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            }
        }
        if (!measureDc) measureDc = CreateCompatibleDC(nullptr);
    }

    int textW(const std::wstring& s, int fk) const {
        if (!measureDc || !fonts[fk] || s.empty()) return 0;
        HGDIOBJ old = SelectObject(measureDc, fonts[fk]);
        SIZE sz{};
        GetTextExtentPoint32W(measureDc, s.c_str(), static_cast<int>(s.size()), &sz);
        SelectObject(measureDc, old);
        return sz.cx;
    }

    // 量一段自动换行文字的像素高度（用于先定卡片高度、再画文字）
    int wrappedTextHeight(const std::wstring& s, int width, int fk) const {
        if (!measureDc || !fonts[fk] || s.empty() || width <= 0) return 0;
        HGDIOBJ old = SelectObject(measureDc, fonts[fk]);
        RECT q{0, 0, width, 0};
        DrawTextW(measureDc, s.c_str(), static_cast<int>(s.size()), &q,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        SelectObject(measureDc, old);
        return q.bottom - q.top;
    }

    void textIn(HDC dc, Rect r, const std::wstring& s, COLORREF color, int fk,
                UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE) const {
        if (!fonts[fk]) return;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, color);
        HGDIOBJ old = SelectObject(dc, fonts[fk]);
        RECT q = toRECT(r);
        DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &q, flags | DT_NOPREFIX);
        SelectObject(dc, old);
    }

    // ------------------------------------------------------------ 命中区域
    void addHit(Rect r, Action a, int index = 0) {
        hits.push_back(Hit{Rect{r.x, r.y + vShift, r.w, r.h}, a, index});
    }
    Hit* hitAt(int x, int y) {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
            if (inside(it->r, x, y)) return &(*it);
        }
        return nullptr;
    }

    // ======================================================================
    // 4. 度量
    // ======================================================================
    void updateMetrics(int cw, int ch) {
        const double logicalW = cw * 96.0 / static_cast<double>(g_dpi);
        const double logicalH = ch * 96.0 / static_cast<double>(g_dpi);
        double sf = std::min(logicalW / 1440.0, (logicalH / 900.0) * 1.12);
        sf = std::clamp(sf, 0.62, 1.55);
        g_scale = sf * (static_cast<double>(g_dpi) / 96.0);

        compactRail = logicalW < 1200.0;
        titleH = P(44);
        toolH = P(58);
        statusH = P(28);
        sidebarW = (compactRail && !railExpanded) ? P(76) : P(236);
        padX = P(24);
        gapY = P(16);
    }

    Rect contentViewport(int w, int h) const {
        Rect v{sidebarW, titleH + toolH, w - sidebarW, h - titleH - toolH - statusH};
        if (v.w < 0) v.w = 0;
        if (v.h < 0) v.h = 0;
        return v;
    }

    // ======================================================================
    // 5. 组件
    // ======================================================================
    void card(HDC dc, Rect r, COLORREF bg = T_CARD) {
        roundFill(dc, r, bg, P(16));
    }

    void cardTitle(HDC dc, Rect cardR, const std::wstring& title, const std::wstring& subtitle) {
        const int px = P(20);
        textIn(dc, Rect{cardR.x + px, cardR.y + P(16), cardR.w - px * 2, P(26)}, title, T_ON, F_TITLE);
        if (!subtitle.empty())
            textIn(dc, Rect{cardR.x + px, cardR.y + P(44), cardR.w - px * 2, P(20)}, subtitle, T_ON_VAR, F_CAP);
    }

    void pill(HDC dc, Rect r, const std::wstring& label, COLORREF fg, COLORREF bg, COLORREF edge) {
        roundRect(dc, r, bg, edge, r.h / 2);
        textIn(dc, r, label, fg, F_CAP, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void filledButton(HDC dc, Rect r, const std::wstring& label, Action a, int idx = 0, bool enabled = true) {
        const bool hov = enabled && hoverAction == a && hoverIndex == idx;
        COLORREF bg = enabled ? (hov ? blend(T_SURFACE, 14, T_ACCENT) : T_ACCENT) : T_CONT_HIGH;
        COLORREF fg = enabled ? T_SURFACE : T_DIS_CONT;
        roundFill(dc, r, bg, r.h / 2);
        textIn(dc, r, label, fg, F_BODY_M, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (enabled) addHit(r, a, idx);
    }

    void outlineButton(HDC dc, Rect r, const std::wstring& label, Action a, int idx = 0, bool enabled = true) {
        const bool hov = enabled && hoverAction == a && hoverIndex == idx;
        COLORREF bg = !enabled ? T_CONTAINER : (hov ? T_CONT_HIGH : T_CONTAINER);
        COLORREF edge = enabled ? (hov ? T_OUTLINE : T_OUTLINE_VAR) : T_DIV_CONT;
        COLORREF fg = enabled ? T_ON : T_DIS_CONT;
        roundRect(dc, r, bg, edge, r.h / 2);
        textIn(dc, r, label, fg, F_BODY_M, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (enabled) addHit(r, a, idx);
    }

    void accentTextButton(HDC dc, Rect r, const std::wstring& label, Action a, int idx = 0,
                          bool enabled = true, int fk = F_BODY_M) {
        const bool hov = enabled && hoverAction == a && hoverIndex == idx;
        if (hov) roundFill(dc, r, T_PRESS, P(10));
        textIn(dc, r, label, enabled ? T_ACCENT : T_DIS_CARD, fk,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (enabled) addHit(r, a, idx);
    }

    // 分段选择器（对应 xapk 的 SCAUICommonRadioButton 组）
    // enabled=false 时只做静态绘制、不登记命中区：用于「命令执行中」禁止重复点击。
    void segmented(HDC dc, Rect r, const std::vector<std::wstring>& items, int sel, Action a,
                   bool enabled = true) {
        const int inner = P(3);
        roundFill(dc, r, T_CONTAINER, P(12));
        const int n = static_cast<int>(items.size());
        if (n <= 0) return;
        const int iw = (r.w - inner * 2) / n;
        for (int i = 0; i < n; ++i) {
            Rect cell{r.x + inner + i * iw, r.y + inner, (i == n - 1) ? (r.w - inner * 2 - i * iw) : iw,
                      r.h - inner * 2};
            const bool on = (i == sel);
            const bool hov = enabled && hoverAction == a && hoverIndex == i;
            COLORREF bg = on ? blend(T_ACCENT, enabled ? 18 : 8, T_CONTAINER) : (hov ? T_CONT_HIGH : T_CONTAINER);
            COLORREF edge = on ? (enabled ? T_ACCENT : blend(T_ACCENT, 40, T_CONTAINER))
                               : (hov ? T_OUTLINE_VAR : T_CONTAINER);
            roundRect(dc, cell, bg, edge, P(9));
            textIn(dc, cell, items[static_cast<size_t>(i)],
                   on ? (enabled ? T_ACCENT : blend(T_ACCENT, 45, T_CONTAINER)) : T_ON_VAR, F_BODY_M,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (enabled) addHit(cell, a, i);
        }
    }

    // 开关行（Sony 设置页的 on/off 行）
    void toggleRow(HDC dc, Rect r, const std::wstring& title, const std::wstring& sub,
                   bool on, Action a, bool enabled = true) {
        const bool hov = enabled && hoverAction == a;
        roundFill(dc, r, hov ? T_CONT_HIGH : T_SCREEN, P(12));
        const int tw = P(44), th = P(24);
        const int textW = r.w - tw - P(44);
        textIn(dc, Rect{r.x + P(14), r.y + P(8), textW, P(20)}, title,
               enabled ? T_ON : T_DIS_CARD, F_BODY_M);
        if (!sub.empty())
            textIn(dc, Rect{r.x + P(14), r.y + P(28), textW, P(18)}, sub,
                   enabled ? T_ON_VAR : T_DIS_VAR, F_CAP);
        Rect tr{r.x + r.w - tw - P(16), r.y + (r.h - th) / 2, tw, th};
        const COLORREF track = on ? T_ACCENT : blend(T_ON_VAR, 45, T_SCREEN);
        roundFill(dc, tr, enabled ? track : T_DIV_CARD, th / 2);
        const int kr = th / 2 - P(3);
        const int kx = on ? (tr.x + tr.w - P(3) - kr) : (tr.x + P(3) + kr);
        ellipseFill(dc, kx, tr.y + th / 2, kr, on ? T_SURFACE : T_ON_VAR);
        if (enabled) addHit(r, a);
    }

    // 水平滑杆
    // displayOffset：只影响右侧打印出来的数字，不参与几何与命中计算。
    // 用途：设备原始等级 1..20 对应滑杆 progress 0..19，用户要求看到 1..20，
    // 因此传 displayOffset = range.minimum 把打印值抬到与设备原始值一致的区间。
    void hSlider(HDC dc, Rect r, const std::wstring& label, int value, int mn, int mx,
                 Action a, int idx, const std::wstring& suffix, bool enabled = true,
                 int displayOffset = 0) {
        const int valW = P(84);
        textIn(dc, Rect{r.x, r.y, r.w - valW, P(20)}, label, enabled ? T_ON_VAR : T_DIS_VAR, F_CAP);
        const std::wstring val = std::to_wstring(value + displayOffset) + suffix;
        textIn(dc, Rect{r.x + r.w - valW, r.y, valW, P(20)}, val, enabled ? T_ACCENT : T_DIS_VAR,
               F_BODY_M, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        const int ty = r.y + P(32);
        const int tx = r.x + P(10);
        const int tw = r.w - P(20);
        roundFill(dc, Rect{tx, ty, tw, P(4)}, blend(T_ON_VAR, 30, T_CARD), P(2));
        const double pct = (mx == mn) ? 0.0 : static_cast<double>(value - mn) / (mx - mn);
        const int fw = static_cast<int>(pct * tw);
        if (fw > 0) roundFill(dc, Rect{tx, ty, fw, P(4)}, enabled ? T_ACCENT : T_DIS_CARD, P(2));
        const int kr = P(9);
        ellipseFill(dc, tx + fw, ty + P(2), kr, enabled ? T_ACCENT : T_DIS_CARD);
        ellipseFill(dc, tx + fw, ty + P(2), kr - P(3), enabled ? T_SURFACE : T_CARD);
        if (enabled) addHit(Rect{tx - P(10), ty - P(16), tw + P(20), P(32)}, a, idx);
    }

    // 垂直推子（Sony EQ 的频段推子）
    void vFader(HDC dc, Rect cell, int value, int mn, int mx, Action a, int idx, bool interactive = true) {
        const int cx = cell.x + cell.w / 2;
        const int top = cell.y;
        const int trackH = cell.h;
        roundFill(dc, Rect{cx - P(2), top, P(4), trackH}, interactive ? blend(T_ON_VAR, 30, T_CARD) : T_DIV_CARD, P(2));
        const double pct = (mx == mn) ? 0.0 : static_cast<double>(value - mn) / (mx - mn);
        const int kh = static_cast<int>(pct * trackH);
        if (kh > 0) roundFill(dc, Rect{cx - P(2), top + trackH - kh, P(4), kh}, interactive ? T_ACCENT : T_DIS_CARD, P(2));
        hline(dc, cx - P(8), cx + P(8), top + trackH / 2, T_EQ_CENTER);
        const int ky = top + trackH - kh;
        const int kr = P(10);
        ellipseFill(dc, cx, ky, kr, interactive && hoverAction == a && hoverIndex == idx ? T_ON : (interactive ? T_ACCENT : T_DIS_CARD));
        ellipseFill(dc, cx, ky, kr - P(4), T_SURFACE);
        if (interactive) addHit(Rect{cx - P(18), top - P(6), P(36), trackH + P(12)}, a, idx);
    }

    // ± 步进器
    void stepper(HDC dc, Rect r, int value, Action up, Action down, int idx,
                 const std::wstring& unit, int mn, int mx, bool interactive = true) {
        const int bs = r.h;
        Rect minus{r.x, r.y, bs, bs};
        Rect plus{r.x + r.w - bs, r.y, bs, bs};
        const bool hovM = interactive && hoverAction == down && hoverIndex == idx;
        const bool hovP = interactive && hoverAction == up && hoverIndex == idx;
        roundRect(dc, minus, hovM ? T_CONT_HIGH : T_SCREEN, T_OUTLINE_VAR, bs / 2);
        roundRect(dc, plus, hovP ? T_CONT_HIGH : T_SCREEN, T_OUTLINE_VAR, bs / 2);
        hline(dc, minus.x + bs / 3, minus.x + bs - bs / 3, minus.y + bs / 2, value > mn ? T_ON : T_DIS_CARD, P(2));
        hline(dc, plus.x + bs / 3, plus.x + bs - bs / 3, plus.y + bs / 2, value < mx ? T_ON : T_DIS_CARD, P(2));
        vline(dc, plus.x + bs / 2, plus.y + bs / 3, plus.y + bs - bs / 3, value < mx ? T_ON : T_DIS_CARD, P(2));
        const std::wstring txt = (value > 0 ? L"+" : L"") + std::to_wstring(value) + unit;
        textIn(dc, Rect{minus.x + bs, r.y, r.w - bs * 2, r.h}, txt, T_ACCENT, F_TITLE,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (interactive) { addHit(minus, down, idx); addHit(plus, up, idx); }
    }

    // 自绘输入框
    void textField(HDC dc, Field& f, Rect r, const std::wstring& label, const std::wstring& placeholder) {
        textIn(dc, Rect{r.x, r.y - P(20), r.w, P(18)}, label, T_ON_VAR, F_CAP);
        const bool hov = (hoverAction == f.action);
        const COLORREF edge = f.focused ? T_ACCENT : (hov ? T_OUTLINE : T_OUTLINE_VAR);
        roundRect(dc, r, T_SCREEN, edge, P(10));
        const int tx = r.x + P(14);
        const std::wstring shown = f.formatted();
        const int availW = r.w - P(28);
        if (shown.empty() && !f.focused) {
            textIn(dc, Rect{tx, r.y, availW, r.h}, placeholder, blend(T_ON_VAR, 45, T_SCREEN), F_BODY);
        } else {
            textIn(dc, Rect{tx, r.y, availW, r.h}, shown, T_ON, F_BODY,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        if (f.focused && caretOn) {
            const int caretIdx = f.fmtIndex(f.caret);
            const int cx = tx + textW(shown.substr(0, static_cast<size_t>(caretIdx)), F_BODY);
            vline(dc, cx, r.y + P(9), r.y + r.h - P(9), T_ACCENT, std::max(1, P(2)));
        }
        f.rect = Rect{r.x, r.y + vShift, r.w, r.h};
        addHit(r, f.action, 0);
    }

    // 顶栏窗口按钮用矢量字形（不依赖图标字体）
    void windowButton(HDC dc, Rect r, int kind, Action a, bool hot) {
        if (hot) roundFill(dc, r, kind == 2 ? RGB(0xC4, 0x2B, 0x1C) : T_CONT_HIGH, P(8));
        const int cx = r.x + r.w / 2, cy = r.y + r.h / 2;
        const int s = P(5);
        const COLORREF fg = (kind == 2 && hot) ? RGB(255, 255, 255) : T_ON;
        if (kind == 0) {                       // minimize
            hline(dc, cx - s, cx + s, cy + P(1), fg, std::max(1, P(1)));
        } else if (kind == 1) {                // maximize / restore
            const bool zoom = maximized;
            if (zoom) {
                Rect a1{cx - s, cy - s + P(2), s * 2 - P(2), s * 2 - P(2)};
                Rect a2{cx - s + P(3), cy - s, s * 2 - P(2), s * 2 - P(2)};
                roundRect(dc, a2, T_SURFACE, fg, P(1), std::max(1, P(1)));
                roundRect(dc, a1, T_SURFACE, fg, P(1), std::max(1, P(1)));
            } else {
                roundRect(dc, Rect{cx - s, cy - s, s * 2, s * 2}, T_SURFACE, fg, P(1), std::max(1, P(1)));
            }
        } else {                               // close
            HPEN p = CreatePen(PS_SOLID, std::max(1, P(1)), fg);
            HGDIOBJ old = SelectObject(dc, p);
            MoveToEx(dc, cx - s, cy - s, nullptr); LineTo(dc, cx + s + 1, cy + s + 1);
            MoveToEx(dc, cx + s, cy - s, nullptr); LineTo(dc, cx - s - 1, cy + s + 1);
            SelectObject(dc, old);
            DeleteObject(p);
        }
        addHit(r, a, 0);
    }

    // ======================================================================
    // 6. 页面
    // ======================================================================
    std::wstring modelName() const {
        if (controller) return W(controller->getModelProfile().displayName);
        try { return W(sony::getProfileByModelCode(modelCode).displayName); } catch (...) { return W(modelCode); }
    }

    // 聆听模式的可选集合。ULT WEAR 实机固件不接受「关闭」帧（见 DeviceModelProfile::supportsAmbientOff），
    // 所以该机型只提供「降噪 / 环境音」两段；索引到模式的映射也走同一张表，避免 UI 与协议各写一份。
    // UI 侧按机型代码取能力（本类只持有 modelCode，控制器由后台线程持有）。
    bool ambientOffSupported() const {
        try { return sony::getProfileByModelCode(modelCode).supportsAmbientOff; }
        catch (...) { return true; }
    }

    std::vector<sony::NcAsmMode> modeTapTargets() const {
        std::vector<sony::NcAsmMode> targets;
        if (ambientOffSupported()) targets.push_back(sony::NcAsmMode::OFF);
        targets.push_back(sony::NcAsmMode::NOISE_CANCEL);
        targets.push_back(sony::NcAsmMode::AMBIENT_SOUND);
        return targets;
    }

    std::vector<std::wstring> modeTapLabels(bool longForm) const {
        std::vector<std::wstring> labels;
        if (ambientOffSupported()) labels.push_back(longForm ? L"声音关闭" : L"关闭");
        labels.push_back(longForm ? L"主动降噪" : L"降噪");
        labels.push_back(L"环境音");
        return labels;
    }

    int modeTapIndexOf(sony::NcAsmMode mode) const {
        const std::vector<sony::NcAsmMode> targets = modeTapTargets();
        for (size_t i = 0; i < targets.size(); ++i) {
            if (targets[i] == mode) return static_cast<int>(i);
        }
        return -1;
    }

    std::wstring modeName() const {
        switch (ambientMode) {
            case sony::NcAsmMode::NOISE_CANCEL:  return L"主动降噪";
            case sony::NcAsmMode::AMBIENT_SOUND: return L"环境音";
            default:                             return L"声音关闭";
        }
    }

    // EQ 预设全表（P0/P1）：所有索尼机型都支持 EQ，因此这张表对所有机型都可用；
    // ULT WEAR 也一样，只是它的 EQ 维度与 ULT 模式维度同帧下发（见 CHANGES.md 第 7.2 节）。
    struct PresetOpt { const wchar_t* label; sony::EqPresetId id; };
    static const std::vector<PresetOpt>& presetOptions() {
        // Keep only the Chinese-labeled choices requested in the review note.
        // Wire IDs are sourced from Sound Connect 13.3.0 classes4.dex EqPresetId.
        // English-only / gaming / FPS / partner presets remain in the backend enum but are not shown here.
        static const std::vector<PresetOpt> opts{
            {L"关闭", sony::EqPresetId::OFF},
            {L"明快", sony::EqPresetId::BRIGHT},
            {L"舒缓", sony::EqPresetId::RELAXED},
            {L"人声", sony::EqPresetId::VOCAL},
            {L"重低音", sony::EqPresetId::BASS_BOOST},
            {L"摇滚", sony::EqPresetId::ROCK},
            {L"流行", sony::EqPresetId::POP},
            {L"爵士", sony::EqPresetId::JAZZ},
            {L"舞曲", sony::EqPresetId::DANCE},
            {L"原声", sony::EqPresetId::ACOUSTIC},
            {L"激昂", sony::EqPresetId::EXCITED},
            {L"醇厚", sony::EqPresetId::MELLOW},
            {L"高音增强", sony::EqPresetId::TREBLE_BOOST},
            {L"语音", sony::EqPresetId::SPEECH},
            {L"自定义", sony::EqPresetId::CUSTOM},
            {L"用户设定 1", sony::EqPresetId::USER_SETTING1},
            {L"用户设定 2", sony::EqPresetId::USER_SETTING2},
            {L"用户设定 3", sony::EqPresetId::USER_SETTING3},
            {L"用户设定 4", sony::EqPresetId::USER_SETTING4},
            {L"用户设定 5", sony::EqPresetId::USER_SETTING5},
        };
        return opts;
    }

    int presetIndexOf(sony::EqPresetId id) const {
        const auto& opts = presetOptions();
        for (size_t i = 0; i < opts.size(); ++i) {
            if (opts[i].id == id) return static_cast<int>(i);
        }
        return -1; // Device may currently use a valid XAPK preset not exposed in this Chinese-only UI.
    }

    sony::EqPresetId presetIdAt(int index) const {
        const auto& opts = presetOptions();
        if (index >= 0 && index < static_cast<int>(opts.size())) return opts[static_cast<size_t>(index)].id;
        return sony::EqPresetId::OFF;
    }

    std::wstring presetNameW() const {
        const auto& opts = presetOptions();
        if (selectedPreset >= 0 && selectedPreset < static_cast<int>(opts.size())) {
            return opts[static_cast<size_t>(selectedPreset)].label;
        }
        return L"设备当前预设（未列入中文列表）";
    }

    std::wstring bassNameW() const {
        switch (ultBassMode) {
            case 1: return L"ULT 1";
            case 2: return L"ULT 2";
            default: return L"关闭";
        }
    }

    // ---- 通用：卡片内的一行分隔线 ----
    void divider(HDC dc, Rect cardR, int y, COLORREF color = T_DIV_CARD) {
        hline(dc, cardR.x + P(20), cardR.x + cardR.w - P(20), y, color);
    }

    // ---------------- OVERVIEW ----------------
    int drawOverview(HDC dc, Rect vp) {
        const int x = vp.x, w = vp.w, gap = gapY;
        int y = vp.y;
        const bool two = w >= P(780);

        // 设备主卡
        Rect hero{x, y, w, P(132)};
        card(dc, hero, T_CARD);
        vline(dc, hero.x + P(20), hero.y + P(20), hero.y + hero.h - P(20), T_ACCENT, P(3));
        textIn(dc, Rect{hero.x + P(34), hero.y + P(18), w - P(360), P(38)}, modelName(), T_ON, F_HEADLINE,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        textIn(dc, Rect{hero.x + P(34), hero.y + P(56), w - P(360), P(20)},
               controller ? (simulation ? L"模拟设备 · 本地协议会话" : L"真实设备 · 蓝牙 RFCOMM")
                          : L"尚未连接设备",
               T_ON_VAR, F_CAP);
        // 状态徽章行（对应 device_card_custom_view_connected：BT / 电量 / 编码 / DSEE）
        {
            int bx = hero.x + P(34);
            const int by = hero.y + P(84), bh = P(26);
            auto chip = [&](const std::wstring& s, COLORREF fg, COLORREF edge) {
                const int cw = textW(s, F_CAP) + P(20);
                pill(dc, Rect{bx, by, cw, bh}, s, fg, T_SCREEN, edge);
                bx += cw + P(8);
            };
            const bool live = controller && controller->isConnected();
            chip(live ? L"● 已连接" : L"○ 未连接", live ? T_MINT : T_ON_VAR, live ? blend(T_MINT, 55, T_CARD) : T_OUTLINE_VAR);
            if (controller) {
                const auto& b = controller->getBatteryState();
                if (b.singleHeadphonePercent) {
                    chip(std::to_wstring(static_cast<int>(*b.singleHeadphonePercent)) + L"%",
                         b.isCharging ? T_MINT : T_ON, T_OUTLINE_VAR);
                } else if (b.leftEarbudPercent || b.rightEarbudPercent) {
                    chip(L"L " + std::to_wstring(static_cast<int>(b.leftEarbudPercent.value_or(0))) +
                         L" / R " + std::to_wstring(static_cast<int>(b.rightEarbudPercent.value_or(0))),
                         T_ON, T_OUTLINE_VAR);
                }
                const auto& prof = controller->getModelProfile();
                std::wstring codec = L"MDR " + std::to_wstring(static_cast<int>(prof.generation) == 1 ? 1 : 2);
                chip(codec, T_ON_VAR, T_OUTLINE_VAR);
            }
            chip(modeName(), T_ACCENT, blend(T_ACCENT, 45, T_CARD));
        }
        // 右侧操作
        {
            const int bw = P(180), bh = P(40);
            const int bx = hero.x + hero.w - bw - P(20);
            filledButton(dc, Rect{bx, hero.y + P(22), bw, bh}, L"打开声音设置", A_NAV_SOUND);
            outlineButton(dc, Rect{bx, hero.y + P(72), bw, bh}, L"均衡器", A_NAV_EQ);
        }
        y = hero.y + hero.h + gap;

        // ---- 情景调节（4 张卡片，点击即应用三维组合：降噪/环境音 + ULT 模式 + EQ 预设）----
        // 注意：左侧「场景预设」导航项与独立页面保持删除状态，情景调节只出现在概览页。
        {
            const bool ult = (modelCode == "ULT");
            const bool grid = two;                       // 双列布局时一行 4 张，窄屏退回 2x2
            const int cols = grid ? 4 : 2;
            const int rows = grid ? 1 : 2;
            const int tileGap = P(10), tileH = P(76), padIn = P(20);
            const int sceneH = P(72) + rows * tileH + (rows - 1) * tileGap + P(18);
            Rect scene{x, y, w, sceneH};
            card(dc, scene);
            cardTitle(dc, scene, L"情景调节",
                      ult ? L"一次点击写入 降噪/环境音 + ULT 模式 + EQ 预设（三维正交，互不覆盖）"
                          : L"一次点击写入 降噪/环境音 + EQ 预设 + 连接相关选项（组合下发）");
            const int tileW = (w - padIn * 2 - tileGap * (cols - 1)) / cols;
            static const wchar_t* kHintUlt[4] = {
                L"降噪 · ULT 关闭 · 舒缓", L"环境音 + 语音 · ULT 关闭 · 人声",
                L"环境音 · ULT 1 · 重低音", L"降噪 · ULT 2 · 明快"
            };
            static const wchar_t* kHintStd[4] = {
                L"降噪 · 舒缓 · 免摘关闭", L"环境音 14 + 语音 · 人声 · 高灵敏免摘",
                L"环境音 8 · 重低音 · 稳定连接", L"降噪 · 明快 · LDAC + DSEE"
            };
            for (int i = 0; i < sony::kSceneCardCount; ++i) {
                const int c = i % cols, r = i / cols;
                Rect t{scene.x + padIn + c * (tileW + tileGap),
                       scene.y + P(72) + r * (tileH + tileGap), tileW, tileH};
                const bool on = activeScene == i;
                const bool hov = hoverAction == A_SCENE_CARD && hoverIndex == i;
                roundRect(dc, t, on ? blend(T_ACCENT, 16, T_SCREEN) : (hov ? T_CONT_HIGH : T_SCREEN),
                          on ? T_ACCENT : T_OUTLINE_VAR, P(12));
                textIn(dc, Rect{t.x + P(12), t.y + P(12), t.w - P(24), P(24)},
                       W(sony::sceneCardLabelUtf8(i)), on ? T_ACCENT : T_ON, F_BODY_M,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                textIn(dc, Rect{t.x + P(12), t.y + P(38), t.w - P(24), P(18)},
                       ult ? kHintUlt[i] : kHintStd[i], T_ON_VAR, F_CAP,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                if (!busy) addHit(t, A_SCENE_CARD, i);
            }
            y += sceneH + gap;
        }

        // 双卡：聆听模式 / 均衡器
        const int colGap = gap;
        const int leftW = two ? static_cast<int>(w * 0.58) - colGap / 2 : w;
        const int rightW = two ? w - leftW - colGap : w;
        const int bx = two ? x + leftW + colGap : x;
        const int cardH = P(186);

        Rect cardA{x, y, leftW, cardH};
        card(dc, cardA);
        cardTitle(dc, cardA, L"聆听模式", L"点按任意选项立即下发到设备");
        textIn(dc, Rect{cardA.x + P(20), cardA.y + P(74), leftW - P(40), P(34)}, modeName(), T_ON, F_TITLE_M);
        textIn(dc, Rect{cardA.x + P(20), cardA.y + P(110), leftW - P(40), P(20)},
               ambientMode == sony::NcAsmMode::AMBIENT_SOUND
                   ? L"环境音原始值 " + std::to_wstring(ambientLevel)
                   : (ambientMode == sony::NcAsmMode::NOISE_CANCEL ? std::wstring(L"降低环境噪声，专注内容")
                                                                  : std::wstring(L"关闭降噪与环境音增强")),
               T_ON_VAR, F_CAP);
        {
            std::vector<std::wstring> items = modeTapLabels(false);
            segmented(dc, Rect{cardA.x + P(20), cardA.y + cardH - P(58), leftW - P(40), P(40)},
                      items, modeTapIndexOf(ambientMode), A_MODE_OFF, !busy);
        }

        if (two) {
            Rect cardB{bx, y, rightW, cardH};
            card(dc, cardB);
            cardTitle(dc, cardB, L"均衡器", modelCode == "ULT" ? L"ULT 模式 + EQ 预设（两个独立维度）"
                                                              : L"当前音色预设");
            textIn(dc, Rect{cardB.x + P(20), cardB.y + P(74), rightW - P(40), P(34)},
                   modelCode == "ULT" ? (L"ULT Bass · " + bassNameW() + L"   |   EQ · " + presetNameW())
                                      : presetNameW(), T_ON, F_TITLE_M,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            textIn(dc, Rect{cardB.x + P(20), cardB.y + P(110), rightW - P(40), P(20)},
                   modelCode == "ULT" ? std::wstring(L"两者可同时启用，同帧下发互不覆盖")
                                      : L"Clear Bass " + std::to_wstring(clearBass),
                   T_ON_VAR, F_CAP, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            outlineButton(dc, Rect{cardB.x + P(20), cardB.y + cardH - P(58), rightW - P(40), P(40)},
                          L"打开均衡器", A_NAV_EQ);
        }
        y += cardH + gap;

        textIn(dc, Rect{x, y, w, P(20)}, L"最近操作：" + W(lastAction), T_ON_VAR, F_CAP,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return y + P(20);
    }

    // ---------------- SOUND ----------------
    int drawSound(HDC dc, Rect vp) {
        const int x = vp.x, w = vp.w, gap = gapY;
        int y = vp.y;
        const bool two = w >= P(760);
        const int leftW = two ? static_cast<int>(w * 0.58) - gap / 2 : w;
        const int rightW = two ? w - leftW - gap : w;
        const int rx = two ? x + leftW + gap : x;

        Rect modeCard{x, y, leftW, P(196)};
        card(dc, modeCard);
        cardTitle(dc, modeCard, L"聆听模式", L"点按立即下发；等级在下方调整后点「应用声音设置」");
        {
            std::vector<std::wstring> items = modeTapLabels(true);
            segmented(dc, Rect{modeCard.x + P(20), modeCard.y + P(76), leftW - P(40), P(46)}, items,
                      modeTapIndexOf(ambientMode), A_MODE_OFF, !busy);
        }
        textIn(dc, Rect{modeCard.x + P(20), modeCard.y + P(132), leftW - P(40), P(44)},
               ambientMode == sony::NcAsmMode::NOISE_CANCEL ? L"降低周围噪声，专注当前内容。"
             : ambientMode == sony::NcAsmMode::AMBIENT_SOUND ? L"保留环境信息，听见周围声音。"
                                                             : L"关闭降噪与环境音增强。",
               T_ON_VAR, F_BODY, DT_LEFT | DT_TOP | DT_WORDBREAK);
        y += modeCard.h + gap;

        // 官方 13.3.0: device capability range (min,max,step) -> slider progress.
        // Real-device range is not fabricated from APK: it must come from the device capability table.
        {
            const sony::AmbientLevelRange range = controller ? controller->getAmbientLevelRange(voiceFocus)
                : (simulation ? sony::AmbientLevelRange{1, 20, 1, true} : sony::AmbientLevelRange{});
            const bool rangeReady = range.valid();
            const int progressMax = rangeReady ? range.sliderMax() : 0;
            const int progress = rangeReady ? range.progressFromRaw(ambientLevel) : 0;
            Rect lvl{x, y, leftW, P(126)};
            card(dc, lvl);
            const bool simulatorRange = controller ? controller->isSimulationMode() : simulation;
            // 显示口径：设备原始等级区间（1..20）优先。滑杆几何仍按 progress(0..19) 计算，
            // 只把打印出来的数字抬到 raw 区间，这样既保持官方 progress 映射，也满足用户
            // 「软件里显示 1–20」的要求。
            const int displayMin = rangeReady ? range.minimum : 0;
            const int displayMax = rangeReady ? range.maximum : 0;
            cardTitle(dc, lvl, L"环境音等级",
                      rangeReady ? (std::wstring(simulatorRange ? L"模拟测试范围（非官方机型值）"
                                                                : L"设备能力范围") +
                                    L" · 等级 " + std::to_wstring(ambientLevel) +
                                    L" / " + std::to_wstring(displayMax))
                                 : L"当前原始设备值 " + std::to_wstring(ambientLevel) +
                                   L"；未读取设备能力 min/max/step，暂不伪造官方滑块位置" +
                                   (controller ? (L" · " + W(controller->getAmbientCapabilityNote())) : L""));
            hSlider(dc, Rect{lvl.x + P(20), lvl.y + P(72), leftW - P(40), P(34)},
                    rangeReady ? L"环境音" : L"范围未同步", progress, 0,
                    std::max(1, progressMax), A_SLIDER_AMBIENT, 0,
                    rangeReady ? (L" / " + std::to_wstring(displayMax)) : L"",
                    rangeReady && ambientMode == sony::NcAsmMode::AMBIENT_SOUND,
                    displayMin);
            y += lvl.h + gap;
        }

        filledButton(dc, Rect{x, y, leftW, P(44)}, L"应用声音设置", A_SOUND_APPLY, 0, !busy);
        y += P(44);

        // 右列
        int ry = vp.y;
        Rect talk{rx, ry, rightW, P(206)};
        card(dc, talk);
        cardTitle(dc, talk, L"语音与对话", L"让人声更容易被听见");
        {
            const int rw = rightW - P(40);
            toggleRow(dc, Rect{rx + P(20), ry + P(74), rw, P(52)}, L"关注语音", L"仅在环境音模式下可用",
                      voiceFocus, A_VOICE, ambientMode == sony::NcAsmMode::AMBIENT_SOUND && !busy);
            toggleRow(dc, Rect{rx + P(20), ry + P(134), rw, P(52)}, L"Speak-to-Chat", L"说话时自动切换环境音",
                      speakToChat, A_STC, controller && controller->getModelProfile().supportsSpeakToChat);
        }
        ry += talk.h + gap;

        if (controller && controller->getModelProfile().supportsDseeControl) {
            Rect enh{rx, ry, rightW, P(112)};
            card(dc, enh);
            toggleRow(dc, Rect{rx + P(20), ry + P(20), rightW - P(40), P(56)}, L"DSEE Extreme",
                      L"数字声音增强引擎", dsee, A_DSEE);
            ry += enh.h + gap;
        }

        Rect info{rx, ry, rightW, P(196)};
        card(dc, info);
        cardTitle(dc, info, L"连接状态", simulation ? L"模拟设备 · 本地协议测试" : L"真实设备 · 蓝牙 RFCOMM");
        textIn(dc, Rect{rx + P(20), ry + P(74), rightW - P(40), P(20)}, L"协议",
               T_ON_VAR, F_CAP);
        textIn(dc, Rect{rx + P(20), ry + P(96), rightW - P(40), P(22)},
               controller ? W(controller->getNegotiatedProtocolText()) : std::wstring(L"尚未协商"),
               T_ON, F_BODY_M, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        textIn(dc, Rect{rx + P(20), ry + P(122), rightW - P(40), P(20)},
               controller ? W(controller->getMacAddress() + "  ·  通道 " + std::to_string(controller->getRfcommChannel()))
                          : std::wstring(L"未连接"),
               T_ON_VAR, F_CAP, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        outlineButton(dc, Rect{rx + P(20), ry + info.h - P(58), rightW - P(40), P(40)},
                      L"刷新设备状态", A_REFRESH, 0, !busy);
        ry += info.h;

        return std::max(y, ry) + P(8);
    }

    // ---------------- EQUALIZER ----------------
    int drawEqualizer(HDC dc, Rect vp) {
        const int x = vp.x, w = vp.w, gap = gapY;
        int y = vp.y;

        const bool ult = (modelCode == "ULT");

        // 本轮按用户要求精简音效调节页：
        //   1) 删除「ULT Bass」卡（含其分段控件与当前模式说明）——ULT 模式仍保存在后端状态与
        //      持久化文件里，只是本页不再提供入口；
        //   2) 删除「预设音色」预设 EQ 列表（15 个芯片及其换行布局缓存）——selectedPreset 仍由
        //      持久化/启动参数维护，且应用均衡器时照旧随帧下发，行为不变。
        // 删除后本页只剩「五段均衡器 + Clear Bass + 操作行」，目标是在 1600×900 窗口内单页无滚动。

        // EQ 改为固定左右布局；曲线与推子行程均显著加高。
        // 手动增益从设备 GET 回包初始化，SET 只写用户调整后的独立 band/Clear Bass 字段；
        // 预设的静态数值曲线未从 XAPK 找到，因此不再臆造“预设曲线数组”，但不禁用原本可用的手动调节。
        const bool canEditBands = true;
        // 字体阶梯上调后的尺寸预算（本轮）：
        //   dB 数值 F_TITLE_S(15) 24px、频点 F_BODY_M(13) 20px、± 按钮 22px，
        //   控件行总高 78px；推子 450px 仍留出 370px 行程。
        const int graphH = P(370);
        const int faderH = P(380);
        const int bodyH = faderH;
        Rect eqCard{x, y, w, P(66) + bodyH + P(18)};
        card(dc, eqCard);
        textIn(dc, Rect{eqCard.x + P(20), eqCard.y + P(16), w - P(40), P(24)}, L"五段均衡器", T_ON, F_TITLE);
        textIn(dc, Rect{eqCard.x + P(20), eqCard.y + P(42), w - P(40), P(20)},
               ult ? L"推子编辑设备回读的五段增益；Clear Bass 独立，不叠加到整条曲线"
                   : L"曲线显示当前回读/本地预览的五段增益；Clear Bass 独立，不作为全频段偏移",
               T_ON_VAR, F_BODY);
        const Rect body{eqCard.x + P(20), eqCard.y + P(66), w - P(40), bodyH};
        {
            const int gapX = P(12);
            const int minGraphW = P(180);
            const int minFaderW = P(250);
            const int maxGraphW = std::max(minGraphW, body.w - minFaderW - gapX);
            const int leftW = std::clamp(body.w * 40 / 100, minGraphW, maxGraphW);
            Rect graph{body.x, body.y, leftW, graphH};
            roundFill(dc, graph, T_SCREEN, P(10));
            drawEqCurve(dc, graph, bands, clearBass);
            const int faderX = body.x + leftW + gapX;
            const int faderW = std::max(1, body.w - leftW - gapX);
            const wchar_t* freq[5] = {L"400", L"1k", L"2.5k", L"6.3k", L"16k"};
            const int cg = P(8);
            const int colW = std::max(P(34), (faderW - cg * 4) / 5);
            for (int i = 0; i < 5; ++i) {
                Rect cell{faderX + i * (colW + cg), body.y, colW, faderH};
                const int controlsH = P(78);
                vFader(dc, Rect{cell.x, cell.y, cell.w, cell.h - controlsH}, bands[i], -10, 10,
                       A_VSLIDER_BAND, i, canEditBands);
                // dB 数值：11dp → 15dp（F_TITLE_S），行高 24
                textIn(dc, Rect{cell.x, cell.y + cell.h - P(80), cell.w, P(24)},
                       (bands[i] > 0 ? L"+" : L"") + std::to_wstring(bands[i]),
                       canEditBands ? T_ACCENT : T_DIS_VAR, F_TITLE_S,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                // 频点标签：11dp → 13dp（F_BODY_M），行高 20
                textIn(dc, Rect{cell.x, cell.y + cell.h - P(52), cell.w, P(20)}, freq[i], T_ON_VAR, F_BODY_M,
                       DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                // ± 按钮：16px → 22px，命中区随之外扩
                const int bs = P(22);
                Rect minus{cell.x + cell.w / 2 - bs - P(2), cell.y + cell.h - P(28), bs, bs};
                Rect plus{cell.x + cell.w / 2 + P(2), cell.y + cell.h - P(28), bs, bs};
                roundRect(dc, minus, canEditBands && hoverAction == A_BAND_DOWN && hoverIndex == i ? T_CONT_HIGH : T_SCREEN,
                          canEditBands ? T_OUTLINE_VAR : T_DIV_CARD, P(6));
                roundRect(dc, plus, canEditBands && hoverAction == A_BAND_UP && hoverIndex == i ? T_CONT_HIGH : T_SCREEN,
                          canEditBands ? T_OUTLINE_VAR : T_DIV_CARD, P(6));
                const COLORREF bandControl = canEditBands ? T_ON : T_DIS_CARD;
                hline(dc, minus.x + P(6), minus.x + bs - P(6), minus.y + minus.h / 2,
                      canEditBands && bands[i] > -10 ? bandControl : T_DIS_CARD, P(1));
                hline(dc, plus.x + P(6), plus.x + bs - P(6), plus.y + plus.h / 2,
                      canEditBands && bands[i] < 10 ? bandControl : T_DIS_CARD, P(1));
                vline(dc, plus.x + bs / 2, plus.y + P(6), plus.y + plus.h - P(6),
                      canEditBands && bands[i] < 10 ? bandControl : T_DIS_CARD, P(1));
                if (canEditBands) { addHit(minus, A_BAND_DOWN, i); addHit(plus, A_BAND_UP, i); }
            }
        }
        y += eqCard.h + gap;

        // Clear Bass is an independent field in the EQ payload and stays independent from the curve.
        // Current UI values are initialized from the last device GET/RET state; no preset curve is invented.
        Rect bass{x, y, w, P(112)};
        card(dc, bass);
        cardTitle(dc, bass, L"Clear Bass", controller && !controller->isSimulationMode()
                  ? L"独立字段 · 根据设备回读值调整并写入后回读"
                  : L"独立字段 · 模拟值用于回归；不叠加到五段 EQ 曲线");
        stepper(dc, Rect{x + w - P(20) - P(140), bass.y + P(28), P(140), P(36)},
                clearBass, A_BASS_UP, A_BASS_DOWN, 0, L"", -10, 10);
        hSlider(dc, Rect{bass.x + P(20), bass.y + P(68), w - P(200), P(34)},
                L"Clear Bass（独立预览）", clearBass, -10, 10, A_SLIDER_BASS, 0, L"", true);
        y += bass.h + gap;

        filledButton(dc, Rect{x, y, P(190), P(44)},
                     ult ? L"应用均衡器 + ULT" : L"应用均衡器", A_EQ_APPLY, 0, !busy);
        outlineButton(dc, Rect{x + P(202), y, P(160), P(44)}, L"从设备同步", A_EQ_SYNC, 0,
                      !busy && controller != nullptr);
        // 功能 B 的同步反馈放在按钮同一行右侧：不额外占用垂直空间，避免小窗口下推挤底部操作行
        textIn(dc, Rect{x + P(374), y, w - P(374), P(44)}, eqSyncLineW(),
               eqSynced ? T_MINT : T_ON_VAR, F_CAP,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return y + P(44) + P(8);
    }

    // 同步状态行（功能 B）：已同步用实心圆点 + 摘要，未同步给出原因
    std::wstring eqSyncLineW() const {
        if (eqSynced) return L"● 已与设备同步 · " + W(eqSyncNote);
        return L"○ 未同步 · " + W(eqSyncNote);
    }

    // 五段 EQ 曲线使用单调三次 Hermite 插值；Clear Bass 在 XAPK 中是独立 ClearBassSeekBar，
    // 不作为整体增益叠加到五段曲线。缓存图线坐标，减少 WM_MOUSEMOVE/WM_PAINT 的重复计算。
    void drawEqCurve(HDC dc, Rect g, const int (&band)[5], int bass) {
        (void)bass;
        const int inner = P(16);
        Rect plot{g.x + inner, g.y + inner, std::max(1, g.w - inner * 2), std::max(1, g.h - inner * 2)};
        for (int i = 0; i <= 4; ++i) {
            const int yy = plot.y + plot.h * i / 4;
            hline(dc, plot.x, plot.x + plot.w, yy, i == 2 ? T_EQ_CENTER : T_EQ_LINE);
        }
        std::array<int, 5> values{};
        double ctrl[5]{};
        for (int i = 0; i < 5; ++i) {
            values[i] = std::clamp(band[i], -10, 10);
            ctrl[i] = static_cast<double>(values[i]);
        }
        const bool sameGeometry = eqCurveCacheValid &&
            eqCurveCachedPlot.x == plot.x && eqCurveCachedPlot.y == plot.y &&
            eqCurveCachedPlot.w == plot.w && eqCurveCachedPlot.h == plot.h;
        if (!sameGeometry || eqCurveCachedBands != values || eqCurveCachedPoints.empty()) {
            const int samples = std::clamp(plot.w / std::max(1, P(3)), 120, 240);
            // Monotone cubic Hermite spline: C1 smooth at control points without Catmull-Rom
            // overshoot/ringing around sharp peaks, while preserving each actual band value.
            double delta[4]{};
            double tangent[5]{};
            for (int i = 0; i < 4; ++i) delta[i] = ctrl[i + 1] - ctrl[i];
            tangent[0] = delta[0];
            tangent[4] = delta[3];
            for (int i = 1; i < 4; ++i) {
                tangent[i] = (delta[i - 1] * delta[i] <= 0.0)
                    ? 0.0 : (2.0 * delta[i - 1] * delta[i]) / (delta[i - 1] + delta[i]);
            }
            eqCurveCachedPoints.clear();
            eqCurveCachedArea.clear();
            eqCurveCachedPoints.reserve(static_cast<size_t>(samples) + 1);
            eqCurveCachedArea.reserve(static_cast<size_t>(samples) + 3);
            for (int s = 0; s <= samples; ++s) {
                const double t = static_cast<double>(s) / samples * 4.0;
                const int i = std::min(3, static_cast<int>(t));
                const double f = t - i;
                const double f2 = f * f, f3 = f2 * f;
                const double h00 = 2.0 * f3 - 3.0 * f2 + 1.0;
                const double h10 = f3 - 2.0 * f2 + f;
                const double h01 = -2.0 * f3 + 3.0 * f2;
                const double h11 = f3 - f2;
                const double v = h00 * ctrl[i] + h10 * tangent[i] +
                                 h01 * ctrl[i + 1] + h11 * tangent[i + 1];
                const double bounded = std::clamp(v, -10.0, 10.0);
                eqCurveCachedPoints.push_back(POINT{
                    plot.x + static_cast<LONG>(plot.w * s / samples),
                    plot.y + static_cast<LONG>(plot.h / 2.0 - bounded / 12.0 * (plot.h / 2.0))});
            }
            if (!eqCurveCachedPoints.empty()) {
                eqCurveCachedArea.insert(eqCurveCachedArea.end(), eqCurveCachedPoints.begin(), eqCurveCachedPoints.end());
                eqCurveCachedArea.push_back(POINT{eqCurveCachedPoints.back().x, plot.y + plot.h});
                eqCurveCachedArea.push_back(POINT{eqCurveCachedPoints.front().x, plot.y + plot.h});
            }
            eqCurveCachedPlot = plot;
            eqCurveCachedBands = values;
            eqCurveCacheValid = true;
        }
        if (!eqCurveCachedArea.empty()) fillPolygon(dc, eqCurveCachedArea, blend(T_ACCENT, 14, T_SCREEN));
        polylineTo(dc, eqCurveCachedPoints, T_ACCENT, std::max(1, P(2)));
        for (int i = 0; i < 5; ++i) {
            const int px = plot.x + plot.w * i / 4;
            const int py = plot.y + static_cast<int>(plot.h / 2.0 - ctrl[i] / 12.0 * (plot.h / 2.0));
            ellipseFill(dc, px, py, P(3), T_ACCENT);
        }
    }

    // ---------------- DEVICE ----------------
    int drawDevice(HDC dc, Rect vp) {
        const int x = vp.x, w = vp.w, gap = gapY;
        int y = vp.y;
        const bool two = w >= P(820);
        const int leftW = two ? std::min(P(560), w - P(300)) : w;
        const int rightW = two ? w - leftW - gap : w;
        const int rx = two ? x + leftW + gap : x;

        // 设备连接卡
        Rect conn{x, y, leftW, P(376)};
        card(dc, conn);
        cardTitle(dc, conn, L"设备连接", L"选择模拟模式或连接已配对的真实耳机");

        // 型号行（Sony device_detail 的 name + change 行）
        {
            Rect row{conn.x + P(20), conn.y + P(76), leftW - P(40), P(48)};
            roundFill(dc, row, T_SCREEN, P(12));
            textIn(dc, Rect{row.x + P(14), row.y, row.w - P(120), row.h}, modelName(), T_ON, F_BODY_M,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            accentTextButton(dc, Rect{row.x + row.w - P(96), row.y + P(8), P(84), P(32)},
                             L"切换型号", A_MODEL_NEXT, 0, !busy, F_CAP);
        }
        // MAC / 通道（自绘输入）
        {
            const int fw = leftW - P(40);
            const int macW = std::max(P(200), fw - P(120));
            textField(dc, macField, Rect{conn.x + P(20), conn.y + P(168), macW, P(40)},
                      L"蓝牙 MAC 地址", L"XX:XX:XX:XX:XX:XX");
            textField(dc, chField, Rect{conn.x + P(20) + macW + P(16), conn.y + P(168), P(104), P(40)},
                      L"通道", L"18");
        }
        // 三个主操作
        {
            const int bh = P(42);
            const int by = conn.y + P(232);
            const int bw = (leftW - P(40) - gap) / 2;
            outlineButton(dc, Rect{conn.x + P(20), by, bw, bh}, L"启动模拟设备", A_CONNECT_SIM, 0, !busy);
            filledButton(dc, Rect{conn.x + P(20) + bw + gap, by, bw, bh}, L"连接真实耳机", A_CONNECT_REAL, 0, !busy);
            const int sw = (leftW - P(40) - gap * 2) / 3;
            const int sy = by + bh + P(12);
            outlineButton(dc, Rect{conn.x + P(20), sy, sw, bh}, L"断开", A_DISCONNECT, 0, !busy && controller != nullptr);
            outlineButton(dc, Rect{conn.x + P(20) + sw + gap, sy, sw, bh}, L"重新读取", A_REFRESH, 0, !busy);
            outlineButton(dc, Rect{conn.x + P(20) + (sw + gap) * 2, sy, sw, bh}, L"发送关机", A_POWER_OFF, 0,
                          !busy && controller != nullptr);
        }
        y += conn.h + gap;

        // ---- 功能 A：蓝牙设备发现卡（点选即填入 MAC + 建议通道并连接）----
        {
            const int shown = std::min<int>(4, static_cast<int>(devices.size()));
            const int rowH = P(52);
            const int listH = shown * rowH;
            // 扫描文案长度随状态变化（成功/无适配器/无权限各不同），先量高度再定卡片尺寸，
            // 避免窄窗下换行文字压到下面的免责行。
            const std::wstring scanNoteW = W(deviceScanNote);
            const int noteH = std::clamp(wrappedTextHeight(scanNoteW, leftW - P(40), F_CAP), P(18), P(58));
            Rect disc{x, y, leftW, P(70) + P(44) + P(8) + listH + noteH + P(36)};
            card(dc, disc);
            cardTitle(dc, disc, L"蓝牙设备发现", L"读取本机已配对/已知设备，点选即自动填入并连接");

            // 两条扫描入口：模拟列表（回归可跑）与本机真实枚举（bthprops）
            {
                const int bh = P(36);
                const int by = disc.y + P(66);
                const int bw = (leftW - P(40) - gap) / 2;
                outlineButton(dc, Rect{disc.x + P(20), by, bw, bh}, L"扫描模拟设备", A_SCAN_SIM, 0, !busy);
                outlineButton(dc, Rect{disc.x + P(20) + bw + gap, by, bw, bh}, L"扫描本机蓝牙", A_SCAN_REAL, 0,
                              !busy);
            }

            // 设备列表（空列表时不占位，直接由下面的状态说明承担提示，避免重复文案）
            const int ly = disc.y + P(66) + P(44) + P(8);
            for (int i = 0; i < shown; ++i) {
                const auto& dev = devices[static_cast<size_t>(i)];
                Rect row{disc.x + P(20), ly + i * rowH, leftW - P(40), rowH - P(6)};
                const bool sel = (selectedDevice == i);
                if (sel) {
                    // 选中态：accent 描边 + 淡色底，与分段控件的选中视觉保持一致
                    roundRect(dc, row, blend(T_ACCENT, 45, T_SCREEN), blend(T_ACCENT, 45, T_SCREEN), P(10));
                    roundFill(dc, Rect{row.x + 1, row.y + 1, row.w - 2, row.h - 2},
                              blend(T_ACCENT, 16, T_SCREEN), P(9));
                } else {
                    roundFill(dc, row, T_SCREEN, P(10));
                }
                textIn(dc, Rect{row.x + P(12), row.y + P(4), row.w - P(180), P(20)}, W(dev.name), T_ON,
                       F_BODY_M, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                textIn(dc, Rect{row.x + P(12), row.y + P(22), row.w - P(180), P(18)},
                       W(dev.macAddress) + L"   ·   通道 " + std::to_wstring(dev.suggestedRfcommChannel),
                       T_ON_VAR, F_CAP, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                const std::wstring badge = dev.connected ? L"已连接"
                                          : dev.paired  ? L"已配对"
                                                        : L"未配对";
                const COLORREF badgeFg = dev.connected ? T_MINT : (dev.paired ? T_ACCENT : T_ON_VAR);
                const int badgeW = P(58);
                pill(dc, Rect{row.x + row.w - badgeW - P(10), row.y + (row.h - P(22)) / 2, badgeW, P(22)},
                     badge, badgeFg, blend(badgeFg, 14, T_SCREEN), blend(badgeFg, 40, T_SCREEN));
                addHit(row, A_DEVICE_PICK, i);
            }

            // 状态说明 + 通道来源免责
            const int ny = ly + listH + P(2);
            textIn(dc, Rect{disc.x + P(20), ny, leftW - P(40), noteH}, scanNoteW,
                   deviceScanDenied ? T_PINK : T_ON_VAR, F_CAP, DT_LEFT | DT_TOP | DT_WORDBREAK);
            textIn(dc, Rect{disc.x + P(20), ny + noteH + P(2), leftW - P(40), P(18)},
                   L"通道号为按键名推测的建议值，可在上方输入框手改。",
                   blend(T_ON_VAR, 70, T_CARD), F_CAP, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += disc.h + gap;
        }

        // 硬件选项
        int ry = vp.y;
        const bool ult = (modelCode == "ULT");
        if (ult) {
            Rect ro{rx, ry, rightW, P(196)};
            card(dc, ro);
            cardTitle(dc, ro, L"ULT WEAR 已配置的功能", L"仅在协议确认后开放写入");
            textIn(dc, Rect{ro.x + P(20), ro.y + P(76), rightW - P(40), P(96)},
                   L"• 主动降噪 / 环境音 / 关注语音\n• ULT Bass：关闭、ULT 1、ULT 2（EQ 子类型 0x03 的 [3] 位）\n• 均衡器：预设 / 五段推子 / Clear Bass（与 ULT 模式同帧、互不覆盖）\n• 电池百分比与充电状态",
                   T_ON, F_BODY, DT_LEFT | DT_TOP | DT_WORDBREAK);
            ry += ro.h + gap;
            Rect warn{rx, ry, rightW, P(128)};
            card(dc, warn, T_CONTAINER);
            textIn(dc, Rect{warn.x + P(20), warn.y + P(18), rightW - P(40), P(96)},
                   L"DSEE、编码优先、佩戴感应等命令未在该机型配置中核实，故不显示可写开关，避免误发其它机型的命令。",
                   T_ON_VAR, F_CAP, DT_LEFT | DT_TOP | DT_WORDBREAK);
            ry += warn.h + gap;
        } else {
            const auto prof = controller ? controller->getModelProfile() : sony::getProfileByModelCode(modelCode);
            std::vector<std::pair<std::wstring, std::pair<std::wstring, bool>>> rows;
            if (prof.supportsDseeControl)
                rows.push_back({L"DSEE Extreme", {L"数字声音增强", dsee}});
            if (prof.supportsWearDetectionControl)
                rows.push_back({L"佩戴感应", {L"取下耳机时自动暂停", wear}});
            if (prof.supportsCodecQualityControl)
                rows.push_back({L"音质优先", {L"LDAC / 稳定连接模式", codecQuality}});
            if (prof.supportsSpeakToChat)
                rows.push_back({L"Speak-to-Chat", {L"智能免摘对话", speakToChat}});
            const int rowH = P(60);
            Rect hw{rx, ry, rightW, P(74) + static_cast<int>(rows.size()) * rowH + P(8)};
            card(dc, hw);
            cardTitle(dc, hw, L"硬件选项", L"开关将尝试写入并回读设备状态");
            const Action acts[4] = {A_DSEE, A_WEAR, A_CODEC, A_STC};
            for (size_t i = 0; i < rows.size() && i < 4; ++i) {
                toggleRow(dc, Rect{hw.x + P(20), hw.y + P(74) + static_cast<int>(i) * rowH,
                                   rightW - P(40), rowH - P(6)},
                          rows[i].first, rows[i].second.first, rows[i].second.second, acts[i]);
            }
            ry += hw.h + gap;
        }

        // 电量卡
        Rect bat{rx, ry, rightW, P(150)};
        card(dc, bat);
        cardTitle(dc, bat, L"电量", L"来自设备的实时回读");
        {
            const int barY = bat.y + P(80);
            const int barH = P(10);
            const int barW = rightW - P(40);
            auto bar = [&](int pct, const std::wstring& label) {
                textIn(dc, Rect{bat.x + P(20), barY - P(22), P(120), P(18)}, label, T_ON_VAR, F_CAP);
                textIn(dc, Rect{bat.x + rightW - P(80), barY - P(22), P(60), P(18)},
                       std::to_wstring(pct) + L"%", T_ON, F_CAP, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
                roundFill(dc, Rect{bat.x + P(20), barY, barW, barH}, blend(T_ON_VAR, 25, T_CARD), barH / 2);
                const int fw = barW * std::clamp(pct, 0, 100) / 100;
                if (fw > 0) roundFill(dc, Rect{bat.x + P(20), barY, fw, barH},
                                      pct <= 20 ? T_PINK : T_MINT, barH / 2);
            };
            if (controller) {
                const auto& b = controller->getBatteryState();
                if (b.singleHeadphonePercent) {
                    bar(static_cast<int>(*b.singleHeadphonePercent),
                        b.isCharging ? L"耳机 · 充电中" : L"耳机");
                } else if (b.leftEarbudPercent || b.rightEarbudPercent) {
                    bar(static_cast<int>(b.leftEarbudPercent.value_or(0)), L"左耳");
                    bar(static_cast<int>(b.rightEarbudPercent.value_or(0)), L"右耳");
                } else {
                    textIn(dc, Rect{bat.x + P(20), barY, barW, P(20)}, L"尚未回读电量", T_ON_VAR, F_CAP);
                }
            } else {
                textIn(dc, Rect{bat.x + P(20), barY, barW, P(20)}, L"尚未连接设备", T_ON_VAR, F_CAP);
            }
        }
        ry += bat.h;

        return std::max(y, ry) + P(8);
    }

    // ======================================================================
    // 7. 窗口外壳
    // ======================================================================
    std::wstring pageTitle() const {
        switch (page) {
            case OVERVIEW:  return L"概览";
            case SOUND:     return L"声音控制";
            case EQUALIZER: return L"均衡器";
            case DEVICE:    return L"设备设置";
        }
        return L"SoundConnect";
    }

    std::wstring pageSubtitle() const {
        switch (page) {
            case OVERVIEW:  return L"设备状态与快捷操作";
            case SOUND:     return L"降噪与环境音";
            case EQUALIZER: return L"定制你的声音";
            case DEVICE:    return L"连接与硬件选项";
        }
        return L"";
    }

    // 自绘标题栏
    void drawTitleBar(HDC dc, int w) {
        fillRect(dc, Rect{0, 0, w, titleH}, T_SURFACE);
        // 品牌标记
        const int lx = P(24), ly = titleH / 2;
        ellipseRing(dc, lx + P(9), ly, P(10), T_ACCENT, std::max(1, P(2)));
        ellipseFill(dc, lx + P(9) + P(4), ly, P(3), T_ACCENT);
        textIn(dc, Rect{lx + P(28), 0, P(220), titleH}, L"Sound Connect", T_ON, F_BODY_M,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        textIn(dc, Rect{lx + P(28) + textW(L"Sound Connect", F_BODY_M) + P(10), 0, P(180), titleH},
               L"Desktop", T_ON_VAR, F_CAP, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // 右侧连接状态 + 窗口按钮
        const int bsq = P(30), bgap = P(4);
        int bx = w - P(12) - bsq;
        windowButton(dc, Rect{bx, (titleH - bsq) / 2, bsq, bsq}, 2, A_TITLE_CLOSE, hoverAction == A_TITLE_CLOSE);
        bx -= bsq + bgap;
        windowButton(dc, Rect{bx, (titleH - bsq) / 2, bsq, bsq}, 1, A_TITLE_MAX, hoverAction == A_TITLE_MAX);
        bx -= bsq + bgap;
        windowButton(dc, Rect{bx, (titleH - bsq) / 2, bsq, bsq}, 0, A_TITLE_MIN, hoverAction == A_TITLE_MIN);
        bx -= P(16);

        const bool live = controller && controller->isConnected();
        const std::wstring chipText = live ? (simulation ? L"● 模拟设备" : L"● 设备已连接") : L"○ 未连接";
        const int cw = textW(chipText, F_CAP) + P(22);
        bx -= cw;
        pill(dc, Rect{bx, (titleH - P(24)) / 2, cw, P(24)}, chipText,
             live ? T_MINT : T_ON_VAR, T_CONTAINER,
             live ? blend(T_MINT, 55, T_CONTAINER) : T_OUTLINE_VAR);
        bx -= P(12);
        if (busy) {
            const std::wstring b = L"处理中…";
            const int bwid = textW(b, F_CAP) + P(22);
            bx -= bwid;
            pill(dc, Rect{bx, (titleH - P(24)) / 2, bwid, P(24)}, b, T_ACCENT, T_CONTAINER,
                 blend(T_ACCENT, 45, T_CONTAINER));
        }
        hline(dc, 0, w, titleH - 1, windowActive ? T_DIV_CONT : T_CONTAINER);
    }

    // 侧栏 / 图标栏
    void drawSidebar(HDC dc, int w, int h) {
        (void)w;
        fillRect(dc, Rect{0, titleH, sidebarW, h - titleH}, T_CONTAINER);
        vline(dc, sidebarW - 1, titleH, h, T_DIV_CONT);

        struct Nav { const wchar_t* label; const wchar_t* sub; Action action; Page p; };
        const Nav navs[4] = {
            {L"概览", L"设备状态与快捷操作", A_NAV_OVERVIEW, OVERVIEW},
            {L"声音控制", L"降噪与环境音", A_NAV_SOUND, SOUND},
            {L"均衡器", L"定制你的声音", A_NAV_EQ, EQUALIZER},
            {L"设备设置", L"连接与硬件选项", A_NAV_DEVICE, DEVICE}
        };
        const bool rail = compactRail && !railExpanded;
        int y = titleH + P(20);
        if (!rail)
            textIn(dc, Rect{P(22), y, sidebarW - P(40), P(18)}, L"工作台", T_ON_VAR, F_CAP);
        y += rail ? 0 : P(24);

        for (int i = 0; i < 4; ++i) {
            const bool sel = page == navs[i].p;
            const bool hov = hoverAction == navs[i].action;
            Rect r{rail ? P(10) : P(13), y, sidebarW - (rail ? P(20) : P(26)), rail ? P(46) : P(52)};
            if (sel) roundFill(dc, r, blend(T_ACCENT, 14, T_CONTAINER), P(12));
            else if (hov) roundFill(dc, r, T_CONT_HIGH, P(12));
            if (sel) roundFill(dc, Rect{r.x, r.y + P(12), std::max(1, P(3)), r.h - P(24)}, T_ACCENT, P(2));
            // 图标：用几何图形区分四个页面（不依赖图标字体）
            const int icx = r.x + (rail ? r.w / 2 : P(26));
            const int icy = r.y + r.h / 2;
            const COLORREF ic = sel ? T_ACCENT : T_ON_VAR;
            switch (i) {
                case 0: ellipseRing(dc, icx, icy, P(7), ic, std::max(1, P(2))); break;
                case 1: ellipseRing(dc, icx, icy, P(7), ic, std::max(1, P(2)));
                        hline(dc, icx + P(5), icx + P(11), icy + P(5), ic, std::max(1, P(2))); break;
                case 2: for (int k = -1; k <= 1; ++k)
                            vline(dc, icx + k * P(5), icy - P(7) + (k + 1) * P(3),
                                  icy + P(7) - (k + 1) * P(2), ic, std::max(1, P(2)));
                        break;
                default: roundRect(dc, Rect{icx - P(7), icy - P(5), P(14), P(10)}, T_CONTAINER, ic, P(3),
                                   std::max(1, P(1)));
                        vline(dc, icx, icy + P(5), icy + P(8), ic, std::max(1, P(2))); break;
            }
            if (!rail) {
                textIn(dc, Rect{r.x + P(46), r.y + P(6), r.w - P(56), P(24)}, navs[i].label,
                       sel ? T_ACCENT : T_ON, F_BODY_M, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                textIn(dc, Rect{r.x + P(46), r.y + P(28), r.w - P(56), P(18)}, navs[i].sub,
                       T_ON_VAR, F_CAP, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            addHit(r, navs[i].action);
            y += r.h + P(6);
        }

        // 底部设备胶囊
        if (!rail) {
            Rect b{P(16), h - statusH - P(112), sidebarW - P(32), P(96)};
            roundFill(dc, b, T_SURFACE, P(14));
            pill(dc, Rect{b.x + P(12), b.y + P(12), std::min(P(120), b.w - P(24)), P(22)},
                 simulation ? L"SIMULATION" : L"REAL DEVICE", simulation ? T_ORANGE : T_MINT,
                 T_CONTAINER, simulation ? blend(T_ORANGE, 50, T_CONTAINER) : blend(T_MINT, 50, T_CONTAINER));
            textIn(dc, Rect{b.x + P(12), b.y + P(42), b.w - P(24), P(20)},
                   simulation ? L"模拟协议环境" : L"原生蓝牙会话", T_ON, F_CAP,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            textIn(dc, Rect{b.x + P(12), b.y + P(64), b.w - P(24), P(18)},
                   W(controller ? controller->getNegotiatedProtocolText() : std::string("未协商")),
                   T_ON_VAR, F_CAP, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else {
            addHit(Rect{P(10), h - statusH - P(52), sidebarW - P(20), P(44)}, A_RAIL_TOGGLE);
        }
        if (compactRail && !rail) {
            // 展开态下提供一个收起入口
            addHit(Rect{P(13), h - statusH - P(52), sidebarW - P(26), P(44)}, A_RAIL_TOGGLE);
        }
    }

    void drawToolbar(HDC dc, int w) {
        const int y = titleH;
        fillRect(dc, Rect{sidebarW, y, w - sidebarW, toolH}, T_SURFACE);
        const int tx = sidebarW + padX;
        textIn(dc, Rect{tx, y + P(8), std::max(P(80), w - sidebarW - padX * 2 - P(200)), P(30)},
               pageTitle(), T_ON, F_TITLE_M, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        textIn(dc, Rect{tx, y + P(34), std::max(P(80), w - sidebarW - padX * 2 - P(200)), P(18)},
               pageSubtitle(), T_ON_VAR, F_CAP, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        hline(dc, sidebarW, w, y + toolH - 1, T_DIV_CONT);
    }

    void drawStatusBar(HDC dc, int w, int h) {
        const int y = h - statusH;
        fillRect(dc, Rect{sidebarW, y, w - sidebarW, statusH}, T_SURFACE);
        hline(dc, sidebarW, w, y, T_DIV_CONT);
        const std::wstring left = W(status.empty() ? lastAction : status);
        textIn(dc, Rect{sidebarW + padX, y, std::max(P(60), w - sidebarW - padX * 2 - P(240)), statusH},
               left, busy ? T_ACCENT : T_ON_VAR, F_CAP,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const std::wstring right = L"C++17 · MDR protocol · " + std::to_wstring(logs.size()) + L" 条记录";
        textIn(dc, Rect{w - padX - P(230), y, P(230), statusH}, right, T_ON_VAR, F_CAP,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // 滚动条（内容超出视口时显示）
    void drawScrollbar(HDC dc, Rect vp) {
        if (contentH <= vp.h || vp.h <= 0) return;
        const int trackX = vp.x + vp.w - P(10);
        const int trackW = P(4);
        roundFill(dc, Rect{trackX, vp.y + P(6), trackW, vp.h - P(12)}, blend(T_ON_VAR, 18, T_SURFACE), trackW / 2);
        const int maxScroll = contentH - vp.h;
        const int thumbH = std::max(P(28), (vp.h - P(12)) * vp.h / contentH);
        const int travel = (vp.h - P(12)) - thumbH;
        const int thumbY = vp.y + P(6) + (maxScroll > 0 ? travel * scrollY / maxScroll : 0);
        roundFill(dc, Rect{trackX, thumbY, trackW, thumbH}, blend(T_ON_VAR, 55, T_SURFACE), trackW / 2);
    }

    // ======================================================================
    // 8. 主绘制
    // ======================================================================
    void draw(HDC dc, int w, int h) {
        hits.clear();
        updateMetrics(w, h);
        const int scaleKey = P(1000);
        if (scaleKey != lastScaleKey) {
            lastScaleKey = scaleKey;
            initFonts();
        }

        fillRect(dc, Rect{0, 0, w, h}, T_SURFACE);
        vShift = 0;
        drawTitleBar(dc, w);
        drawSidebar(dc, w, h);
        drawToolbar(dc, w);

        contentRect = contentViewport(w, h);
        if (contentRect.w > 0 && contentRect.h > 0) {
            // 先按上一帧内容高度约束滚动位置，避免越界
            const int maxScrollPrev = std::max(0, contentH - contentRect.h);
            scrollY = std::clamp(scrollY, 0, maxScrollPrev);

            const int saved = SaveDC(dc);
            IntersectClipRect(dc, contentRect.x, contentRect.y,
                              contentRect.x + contentRect.w, contentRect.y + contentRect.h);
            SetViewportOrgEx(dc, 0, -scrollY, nullptr);
            vShift = -scrollY;

            Rect vp{contentRect.x + padX, contentRect.y + P(4),
                    contentRect.w - padX * 2, contentRect.h - P(8)};
            if (vp.w < P(120)) vp.w = std::max(P(40), contentRect.w / 2);
            int bottom = vp.y;
            switch (page) {
                case OVERVIEW:  bottom = drawOverview(dc, vp); break;
                case SOUND:     bottom = drawSound(dc, vp); break;
                case EQUALIZER: bottom = drawEqualizer(dc, vp); break;
                case DEVICE:    bottom = drawDevice(dc, vp); break;
            }
            contentH = bottom - vp.y + P(16);
            vShift = 0;
            RestoreDC(dc, saved);
            const int maxScroll = std::max(0, contentH - contentRect.h);
            scrollY = std::clamp(scrollY, 0, maxScroll);
            drawScrollbar(dc, contentRect);
        }
        drawStatusBar(dc, w, h);
    }

    // ======================================================================
    // 9. 输入 / 命中
    // ======================================================================
    Rect titleBarArea(int w) const { (void)w; return Rect{0, 0, 100000, titleH}; }
    Rect titleButtons{};

    bool isWindowButtonHit(int x, int y) const {
        return hoverAction == A_TITLE_MIN || hoverAction == A_TITLE_MAX || hoverAction == A_TITLE_CLOSE
            ? (x >= 0 && y < titleH ? true : false) : false;
    }

    void onMouseDown(int x, int y) {
        Hit* hit = hitAt(x, y);
        if (!hit) {
            // 点击空白处收起输入焦点
            if (focusField) { focusField->focused = false; focusField = nullptr; invalidate(); }
            return;
        }
        switch (hit->action) {
            case A_TITLE_MIN: ShowWindow(hwnd, SW_MINIMIZE); return;
            case A_TITLE_MAX: toggleMaximize(); return;
            case A_TITLE_CLOSE: PostMessageW(hwnd, WM_CLOSE, 0, 0); return;
            case A_INPUT_MAC:
                setFocusField(&macField, x);
                return;
            case A_INPUT_CHANNEL:
                setFocusField(&chField, x);
                return;
            case A_SLIDER_AMBIENT:
            case A_SLIDER_BASS:
            case A_SLIDER_BAND:
            case A_VSLIDER_BAND:
                activeSlider = hit->action;
                activeSliderIndex = hit->index;
                updateSlider(hit->action, hit->index, x, y);
                return;
            default: break;
        }
        if (focusField) { focusField->focused = false; focusField = nullptr; }
        onAction(hit->action, hit->index, x, y);
    }

    void setFocusField(Field* f, int x) {
        if (focusField && focusField != f) focusField->focused = false;
        focusField = f;
        f->focused = true;
        f->caret = caretFromX(*f, x);
        caretOn = true;
        SetTimer(hwnd, 1, 500, nullptr);
        invalidate();
    }

    int caretFromX(const Field& f, int x) const {
        const int tx = f.rect.x + P(14);
        const std::wstring s = f.formatted();
        int best = 0, bestDist = 1 << 30;
        for (int d = 0; d <= static_cast<int>(f.text.size()); ++d) {
            const int idx = f.fmtIndex(d);
            const int cx = tx + textW(s.substr(0, static_cast<size_t>(idx)), F_BODY);
            const int dist = std::abs(cx - x);
            if (dist < bestDist) { bestDist = dist; best = d; }
        }
        return best;
    }

    void toggleMaximize() {
        if (!hwnd) return;
        maximized = !maximized;
        ShowWindow(hwnd, maximized ? SW_MAXIMIZE : SW_RESTORE);
    }

    void onChar(wchar_t ch) {
        if (!focusField) return;
        Field& f = *focusField;
        if (ch == L'\t') {
            setFocusField(&f, f.rect.x);
            return;
        }
        if (ch == L'\b') {
            if (f.caret > 0) {
                f.text.erase(static_cast<size_t>(f.caret - 1), 1);
                --f.caret;
            }
            invalidate();
            return;
        }
        if (ch < 32) return;
        wchar_t c = ch;
        if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - L'a' + L'A');
        const bool digit = (c >= L'0' && c <= L'9');
        const bool hex = digit || (c >= L'A' && c <= L'F');
        if (f.numeric) {
            if (!digit) return;
        } else if (!hex) {
            return;
        }
        if (f.text.size() >= f.maxDigits) return;
        if (f.caret > static_cast<int>(f.text.size())) f.caret = static_cast<int>(f.text.size());
        f.text.insert(f.text.begin() + f.caret, c);
        ++f.caret;
        invalidate();
        // 12 位十六进制输完后自动提交格式提示
        if (!f.numeric && f.text.size() == f.maxDigits) {
            focusField->focused = false;
            focusField = nullptr;
        }
    }

    void onKeyDown(int vk) {
        if (!focusField) return;
        Field& f = *focusField;
        switch (vk) {
            case VK_LEFT:  if (f.caret > 0) --f.caret; break;
            case VK_RIGHT: if (f.caret < static_cast<int>(f.text.size())) ++f.caret; break;
            case VK_HOME:  f.caret = 0; break;
            case VK_END:   f.caret = static_cast<int>(f.text.size()); break;
            case VK_DELETE:
                if (f.caret < static_cast<int>(f.text.size()))
                    f.text.erase(static_cast<size_t>(f.caret), 1);
                break;
            case VK_BACK:
                if (f.caret > 0) { f.text.erase(static_cast<size_t>(f.caret - 1), 1); --f.caret; }
                break;
            case VK_ESCAPE:
                f.focused = false;
                focusField = nullptr;
                break;
            case VK_RETURN:
                if (page == DEVICE) connectTo(false);
                break;
            default: return;
        }
        caretOn = true;
        invalidate();
    }

    void updateSlider(Action a, int index, int x, int y) {
        bool changed = false;
        for (const auto& hit : hits) {
            if (hit.action != a || hit.index != index) continue;
            if (a == A_VSLIDER_BAND) {
                const int trackTop = hit.r.y + P(6);
                const int trackH = std::max(1, hit.r.h - P(12));
                const double t = std::clamp(1.0 - static_cast<double>(y - trackTop) / trackH, 0.0, 1.0);
                const int v = -10 + static_cast<int>(std::lround(t * 20.0));
                if (index >= 0 && index < 5 && bands[index] != v) {
                    bands[index] = v;
                    changed = true;
                }
            } else if (a == A_SLIDER_AMBIENT) {
                const auto range = controller ? controller->getAmbientLevelRange(voiceFocus)
                    : (simulation ? sony::AmbientLevelRange{1, 20, 1, true} : sony::AmbientLevelRange{});
                if (!range.valid()) break;
                const int x0 = hit.r.x + P(10);
                const int width = std::max(1, hit.r.w - P(20));
                const double t = std::clamp(static_cast<double>(x - x0) / width, 0.0, 1.0);
                const int progress = static_cast<int>(std::lround(t * range.sliderMax()));
                const int next = range.rawFromProgress(progress);
                if (ambientLevel != next) { ambientLevel = next; changed = true; }
            } else {
                const int x0 = hit.r.x + P(10);
                const int width = std::max(1, hit.r.w - P(20));
                const double t = std::clamp(static_cast<double>(x - x0) / width, 0.0, 1.0);
                const int v = -10 + static_cast<int>(std::lround(t * 20.0));
                if (a == A_SLIDER_BASS) {
                    if (clearBass != v) { clearBass = v; changed = true; }
                } else if (index >= 0 && index < 5 && bands[index] != v) {
                    bands[index] = v;
                    changed = true;
                }
            }
            break;
        }
        // Dragging within the same quantized step produces no paint work; repaint only when value changed.
        if (changed) invalidate();
    }

    void onWheel(int delta) {
        if (contentH <= contentRect.h) return;
        scrollY -= (delta / 120) * P(64);
        scrollY = std::clamp(scrollY, 0, std::max(0, contentH - contentRect.h));
        invalidate();
    }

    void tickCaret() {
        if (!focusField) return;
        caretOn = !caretOn;
        invalidate();
    }

    void onHover(int x, int y) {
        Hit* h = hitAt(x, y);
        const Action a = h ? h->action : A_NONE;
        const int i = h ? h->index : -1;
        if (a != hoverAction || i != hoverIndex) {
            hoverAction = a;
            hoverIndex = i;
            invalidate();
        }
        wchar_t* cur = h ? (h->action == A_INPUT_MAC || h->action == A_INPUT_CHANNEL ? IDC_IBEAM : IDC_HAND)
                         : IDC_ARROW;
        SetCursor(LoadCursorW(nullptr, cur));
    }

    // ======================================================================
    // 10. 动作（与原实现一一对应，全部保留）
    // ======================================================================
    void onAction(Action a, int index, int x, int y) {
        (void)x; (void)y;
        switch (a) {
            case A_RAIL_TOGGLE: {
                if (compactRail == false) { railExpanded = false; break; }
                railExpanded = !railExpanded;
                invalidate();
                return;
            }
            case A_NAV_OVERVIEW: page = OVERVIEW; scrollY = 0; break;
            case A_NAV_SOUND: page = SOUND; scrollY = 0; break;
            case A_NAV_EQ: page = EQUALIZER; scrollY = 0; break;
            case A_NAV_DEVICE: page = DEVICE; scrollY = 0; break;
            case A_CONNECT_SIM: connectTo(true); return;
            case A_CONNECT_REAL: connectTo(false); return;
            case A_SCAN_SIM: scanDevices(false); return;
            case A_SCAN_REAL: scanDevices(true); return;
            case A_DEVICE_PICK: pickDevice(index); return;
            case A_EQ_SYNC: syncEqFromDevice(); return;
            case A_DISCONNECT:
                if (controller) {
                    startAsync("正在断开连接…", [](AsyncResult& result) {
                        if (result.controller) result.controller->disconnectDevice();
                        result.controller.reset();
                        result.reloadState = false;
                        result.status = "连接已断开。";
                        result.messages.push_back(result.status);
                    });
                } else {
                    log("当前没有活动连接。");
                }
                return;
            case A_REFRESH: doRefresh(); return;
            case A_POWER_OFF:
                if (!ensureConnected()) return;
                if (!simulation) {
                    MessageBoxW(hwnd,
                        L"当前版本的真实关机命令尚未经过目标型号实机验证，因此为避免发送未确认的命令，真实设备关机操作已禁用。",
                        L"功能尚未验证", MB_OK | MB_ICONWARNING);
                    return;
                }
                if (MessageBoxW(hwnd, L"确认向模拟器发送关机指令？", L"确认操作",
                                MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    startAsync("正在运行模拟关机回归…", [](AsyncResult& result) {
                        if (!result.controller) {
                            result.status = "模拟器连接不存在。";
                            result.messages.push_back(result.status);
                            return;
                        }
                        const bool ok = result.controller->powerOffHeadphone();
                        const std::string message = ok ? "模拟关机回归操作成功。"
                                                       : "模拟关机失败：" + result.controller->getLastError();
                        result.messages.push_back(message);
                        result.status = message;
                        if (ok) { result.controller->disconnectDevice(); result.controller.reset(); }
                    });
                }
                return;
            case A_MODEL_NEXT: {
                const char* codes[4] = {"ULT", "XM5", "XM4", "WF-XM5"};
                int i = 0;
                for (; i < 4; ++i) if (modelCode == codes[i]) break;
                modelCode = codes[(i + 1) % 4];
                try {
                    const auto selected = sony::getProfileByModelCode(modelCode);
                    chField.text = std::to_wstring(selected.defaultRfcommChannel);
                    chField.caret = static_cast<int>(chField.text.size());
                } catch (...) {}
                log("已选择机型：" + modelCode + "；默认 RFCOMM 通道已按机型更新（重新连接后生效）");
                break;
            }
            case A_ULT_MODE: {
                const uint8_t requested = static_cast<uint8_t>(std::clamp(index, 0, 2));
                runControllerAction("正在切换 ULT Bass 模式…",
                    [requested](sony::HeadphoneController& c) { return c.setUltBassMode(requested); },
                    "ULT Bass 模式已回读。", "ULT Bass 操作失败：");
                return;
            }
            // 概览页与声音控制页共用同一组分段按钮：index 就是段序号，
            // 0=关闭 / 1=降噪 / 2=环境音。旧实现忽略 index 一律写 OFF，
            // 这正是「点降噪没反应」的第二层原因。
            case A_MODE_OFF: {
                const std::vector<sony::NcAsmMode> targets = modeTapTargets();
                if (index < 0 || static_cast<size_t>(index) >= targets.size()) return;
                commitAmbientTap(targets[static_cast<size_t>(index)]);
                return;
            }
            case A_MODE_ANC:
                commitAmbientTap(sony::NcAsmMode::NOISE_CANCEL);
                return;
            case A_MODE_AMBIENT:
                commitAmbientTap(sony::NcAsmMode::AMBIENT_SOUND);
                return;
            case A_VOICE:
                commitVoiceFocus(!voiceFocus);
                return;
            case A_SCENE_CARD:
                applySceneCard(index);
                return;
            case A_SOUND_APPLY: applySound(); return;
            case A_EQ_PRESET:
                selectedPreset = index;
                presetSelectionTouched = (index >= 0 && index < static_cast<int>(presetOptions().size()));
                break;
            case A_EQ_APPLY: applyEq(); return;
            case A_BAND_UP:
            case A_BAND_DOWN:
                if (index >= 0 && index < 5) {
                    bands[index] = std::clamp(bands[index] + (a == A_BAND_UP ? 1 : -1), -10, 10);
                }
                break;
            case A_BASS_UP: clearBass = std::clamp(clearBass + 1, -10, 10); break;
            case A_BASS_DOWN: clearBass = std::clamp(clearBass - 1, -10, 10); break;
            case A_DSEE: {
                const bool requested = !dsee;
                runControllerAction("正在修改 DSEE Extreme…",
                    [requested](sony::HeadphoneController& c) { return c.setDseeExtreme(requested); },
                    "DSEE 状态已回读。", "DSEE 操作失败：");
                return;
            }
            case A_WEAR: {
                const bool requested = !wear;
                runControllerAction("正在修改佩戴感应…",
                    [requested](sony::HeadphoneController& c) { return c.setWearDetection(requested); },
                    "佩戴感应状态已回读。", "佩戴感应操作失败：");
                return;
            }
            case A_CODEC: {
                const auto requested = codecQuality
                    ? sony::BluetoothConnectionQuality::PRIORITIZE_STABLE_CONNECTION
                    : sony::BluetoothConnectionQuality::PRIORITIZE_SOUND_QUALITY;
                runControllerAction("正在修改蓝牙音质模式…",
                    [requested](sony::HeadphoneController& c) { return c.setBluetoothConnectionQuality(requested); },
                    "连接音质模式已回读。", "音质模式操作失败：");
                return;
            }
            case A_STC: {
                if (!ensureConnected()) return;
                const auto old = controller->getSpeakToChat();
                const sony::SpeakToChatConfig requested{!old.enabled, old.sensitivity, old.timeout};
                runControllerAction("正在修改 Speak-to-Chat…",
                    [requested](sony::HeadphoneController& c) { return c.setSpeakToChat(requested); },
                    "Speak-to-Chat 状态已回读。", "Speak-to-Chat 操作失败：");
                return;
            }
            default: break;
        }
        if (a == A_EQ_PRESET || a == A_MODEL_NEXT) {
            log("已更新界面选择；请点击应用或重新连接以提交。");
        }
        invalidate();
    }

    // ======================================================================
    // 11. 后端交互（逻辑与原实现一致）
    // ======================================================================
    void initFontsPublic() { initFonts(); }
    void invalidate() { if (hwnd) InvalidateRect(hwnd, nullptr, FALSE); }

    void log(const std::string& s) {
        lastAction = s;
        logs.push_back(s);
        if (logs.size() > 80) logs.erase(logs.begin(), logs.begin() + 20);
    }

    // ======================================================================
    // 10.4 P1 本地设置持久化（离线功能）：把上次的机型 / MAC / ULT 模式 / EQ
    //      （预设 + Clear Bass + 五段）/ 环境音写进本机文件，下次启动直接恢复。
    //      与蓝牙协议无关，纯本地文件；格式见后端 serializePersistedState()。
    // ======================================================================
    static std::string settingsPath() {
        wchar_t buf[MAX_PATH]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) : std::wstring(L".");
        dir += L"\\SonySoundConnectUI";
        CreateDirectoryW(dir.c_str(), nullptr);
        return WtoA(dir + L"\\last-state.txt");
    }

    void persistSettings() {
        if (!controller || !controller->isConnected()) return;
        const sony::PersistedState state =
            sony::makePersistedState(*controller, modelCode, WtoA(macField.formatted()));
        if (sony::savePersistedState(settingsPath(), state)) {
            log("已保存本地设置（机型 / EQ / ULT 模式 / 环境音，离线可恢复）");
        } else {
            log("本地设置保存失败：无法写入设置文件（不影响设备控制）。");
        }
    }

    void restorePersistedSettings() {
        sony::PersistedState state{};
        if (!sony::loadPersistedState(settingsPath(), state)) {
            logs.push_back("未找到本地设置文件，使用默认值");
            return;
        }
        modelCode = state.modelCode;
        if (!state.macAddress.empty()) setMacField(state.macAddress);
        ultBassMode = state.ultBassMode;
        clearBass = state.equalizer.clearBass;
        for (int i = 0; i < 5; ++i) bands[i] = state.equalizer.bands[i];
        selectedPreset = presetIndexOf(state.equalizer.preset);
        presetSelectionTouched = false;
        if (state.hasAmbient) {
            ambientMode = state.ambient.mode;
            ambientLevel = state.ambient.ambientLevel;
            voiceFocus = state.ambient.voicePassthrough;
        }
        logs.push_back("已恢复上次本地设置（机型 " + state.modelCode + "；离线恢复，尚未连接设备）");
        status = "已恢复上次本地设置；连接设备后可重新下发。";
    }

    void reloadLocalState() {
        if (!controller) return;
        const auto& a = controller->getAmbientSound();
        ambientMode = a.mode;
        ambientLevel = a.ambientLevel;
        voiceFocus = a.voicePassthrough;
        ultBassMode = controller->getUltBassMode();
        const auto& e = controller->getEqualizer();
        clearBass = e.clearBass;
        for (int i = 0; i < 5; ++i) bands[i] = e.bands[i];
        // Selected index is -1 when the device uses a protocol preset deliberately hidden from the Chinese-only chip list.
        selectedPreset = presetIndexOf(e.preset);
        presetSelectionTouched = false;
        const auto& hw = controller->getHardwareConfig();
        dsee = hw.dseeExtremeEnabled;
        wear = hw.pauseWhenTakenOff;
        codecQuality = hw.connectionQuality == sony::BluetoothConnectionQuality::PRIORITIZE_SOUND_QUALITY;
        speakToChat = controller->getSpeakToChat().enabled;
    }

    // 所有可能阻塞的蓝牙/协议操作都在工作线程执行；控制器所有权转移给结果对象。
    bool startAsync(const std::string& progress, std::function<void(AsyncResult&)> job) {
        if (busy) return false;
        busy = true;
        status = progress;
        log(progress);
        invalidate();

        auto result = std::make_shared<AsyncResult>();
        result->controller = std::move(controller);
        result->simulation = simulation;
        HWND target = hwnd;
        try {
            std::thread([target, result, job = std::move(job)]() mutable {
                const auto taskStarted = std::chrono::steady_clock::now();
                try {
                    job(*result);
                } catch (const std::exception& e) {
                    result->status = std::string("后台操作发生异常：") + e.what();
                    result->messages.push_back(result->status);
                    result->reloadState = false;
                } catch (...) {
                    result->status = "后台操作发生未知异常。";
                    result->messages.push_back(result->status);
                    result->reloadState = false;
                }
                result->durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - taskStarted).count();
                auto* posted = new std::shared_ptr<AsyncResult>(result);
                if (!PostMessageW(target, WM_APP_ASYNC_DONE, 0, reinterpret_cast<LPARAM>(posted))) {
                    delete posted;
                }
            }).detach();
        } catch (const std::exception& e) {
            controller = std::move(result->controller);
            busy = false;
            status = std::string("无法启动后台任务：") + e.what();
            log(status);
            invalidate();
            return false;
        }
        return true;
    }

    // 取证/演示专用：把「本地视图状态」摆到指定两维组合。
    // 只改 UI 视图变量，不发送任何协议帧，也不伪造"已同步"——同步线路仍只由设备回包驱动
    // （result->hasSyncNote → eqSynced/eqSyncNote）。协议层的两维正交性由 --test-sim 自检证明。
    void applyShotStartupState() {
        if (shotUltBass >= 0 && shotUltBass <= 2) ultBassMode = shotUltBass;
        if (shotPreset >= 0 && shotPreset < static_cast<int>(presetOptions().size())) {
            selectedPreset = shotPreset;
            presetSelectionTouched = true;
        }
        if (shotAmbient >= 0 && shotAmbient <= 2) ambientMode = static_cast<sony::NcAsmMode>(shotAmbient);
    }

    // 命令行启动参数（便于取证脚本与无鼠标环境）：
    //   --page=N(0..3) --model=ULT|XM5|XM4|WF-XM5 --ult=0..2 --preset=0..19 --connect-sim
    // 解析在 sony::parseShotStartupArgs()（后端纯函数，已被 --test-sim 覆盖），UI 只负责落地。
    // 构造函数已执行 restorePersistedSettings()，这里的显式参数优先级最高。
    void applyStartupArgs(const std::vector<std::string>& args) {
        const sony::ShotStartupSpec spec = sony::parseShotStartupArgs(args);
        if (spec.page >= 0) page = static_cast<Page>(spec.page);
        if (!spec.modelCode.empty()) modelCode = spec.modelCode;
        if (spec.ultBassMode >= 0) shotUltBass = spec.ultBassMode;
        if (spec.presetIndex >= 0) shotPreset = spec.presetIndex;
        if (spec.ambientMode >= 0) shotAmbient = spec.ambientMode;
        if (!spec.rejectNote.empty()) log("启动参数已忽略（取值非法）：" + spec.rejectNote);
        applyShotStartupState();
        invalidate();
        if (spec.connectSim && !controller) connectTo(true);
    }

    // 取证参数不仅"摆视图"，还把同一组两维值真的写进模拟设备并回读一次，
    // 这样界面上的 ULT 卡、预设芯片与右下角"已与设备同步"读数是同一份状态，
    // 截图不会出现"视图说 ULT 2、同步行说 ULT 关闭"的自相矛盾。
    // 走的是生产路径（setUltBassMode / setEqualizer / syncEqualizerState），不是旁路赋值。
    void applyShotStartupToDevice() {
        if (!controller || (shotUltBass < 0 && shotPreset < 0 && shotAmbient < 0)) return;
        if (shotAmbient >= 0) {
            const sony::AmbientSoundConfig ambientCfg =
                sony::ambientTapConfig(static_cast<sony::NcAsmMode>(shotAmbient), ambientLevel, voiceFocus);
            if (!controller->setAmbientSound(ambientCfg)) {
                log("取证参数（聆听模式）应用失败：" + controller->getLastError());
            } else {
                reloadLocalState();  // 用设备回读值刷新，截图上的等级是设备真实值
            }
        }
        if (shotUltBass >= 0 && !controller->setUltBassMode(static_cast<uint8_t>(shotUltBass))) {
            log("取证参数（ULT 模式）应用失败：" + controller->getLastError());
        }
        if (shotPreset >= 0) {
            sony::EqualizerConfig eq;
            eq.preset = presetIdAt(selectedPreset);
            eq.clearBass = static_cast<int8_t>(std::clamp(clearBass, -10, 10));
            for (int i = 0; i < 5; ++i) eq.bands[i] = static_cast<int8_t>(std::clamp(bands[i], -10, 10));
            if (!controller->setEqualizer(eq)) log("取证参数（EQ 预设）应用失败：" + controller->getLastError());
        }
        (void)controller->pollDeviceNotifications();
        if (controller->syncEqualizerState()) {
            eqSynced = true;
            eqSyncNote = describeEqState(*controller);
            reloadLocalState();  // 回读后视图与设备完全一致
        }
    }

    void finishAsync(const std::shared_ptr<AsyncResult>& result) {
        if (!result) return;
        controller = std::move(result->controller);
        simulation = result->simulation;
        if (!result->status.empty()) status = result->status;
        // 点按即下发的失败回滚：先把界面退回点击前的值（乐观更新已把它改掉了），
        // 再交给 reloadState 用设备真实值覆盖——顺序不能反，否则滚回值会盖掉回读。
        if (result->hasAmbientRollback) {
            ambientMode = result->rollbackAmbient.mode;
            ambientLevel = result->rollbackAmbient.ambientLevel;
            voiceFocus = result->rollbackAmbient.voicePassthrough;
        }
        // 情景调节：记录最近一次套用的卡片（仅作为概览页高亮，不改动任何设备状态）
        if (result->sceneCard >= 0) activeScene = result->sceneOk ? result->sceneCard : -1;
        if (result->reloadState && controller) {
            reloadLocalState();
            persistSettings();  // P1 离线功能：状态变化后落盘，重启可恢复
        }
        // 功能 A：扫描结果落地（列表 + 可读状态文案）
        if (result->hasScanResult) {
            devices = result->scan.devices;
            deviceScanNote = result->scan.message;
            deviceScanOk = result->scan.succeeded();
            deviceScanNative = result->scanUsesNative;
            deviceScanDenied = result->scan.status == sony::DeviceScanStatus::ACCESS_DENIED ||
                               result->scan.status == sony::DeviceScanStatus::UNSUPPORTED;
            selectedDevice = -1;
        }
        // 功能 B：同步反馈
        if (result->hasSyncNote) {
            eqSynced = result->syncOk;
            eqSyncNote = result->syncNote;
        }
        for (const auto& message : result->messages) log(message);
        log("[性能] 本次后台操作耗时 " + std::to_string(result->durationMs) + " ms");
        // 取证参数（--ult/--preset）只在"刚连上"的那一次落位一次。
        // 旧实现每次异步结束都重放，导致连接后用户点情景卡/切模式时被启动参数
        // 又覆盖回去——截图会出现"情景已套用但 ULT 读数没变"的自相矛盾。
        if (!shotStartupApplied && (shotUltBass >= 0 || shotPreset >= 0 || shotAmbient >= 0)) {
            shotStartupApplied = true;
            applyShotStartupState();   // 取证参数在连接回填之后重新落位（否则会被 reloadLocalState 覆盖）
            applyShotStartupToDevice();
        }
        busy = false;
        activeSlider = A_NONE;
        invalidate();
    }

    bool connectTo(bool sim) {
        if (busy) return false;
        sony::DeviceModelProfile profile;
        try {
            profile = sony::getProfileByModelCode(modelCode);
        } catch (const std::exception& e) {
            status = e.what();
            log(status);
            invalidate();
            return false;
        }

        std::string mac = "AC:80:0A:12:34:56"; // 仅模拟器占位
        int channel = profile.defaultRfcommChannel;
        if (!sim) {
            mac = WtoA(macField.formatted());
            const std::string ch = WtoA(chField.text);
            std::array<uint8_t, 6> parsedMac{};
            if (!sony::validateBluetoothMacAddress(mac, parsedMac)) {
                status = "请输入真实耳机的蓝牙 MAC 地址（格式 XX:XX:XX:XX:XX:XX）。";
                log(status);
                invalidate();
                return false;
            }
            int parsedChannel = 0;
            try { parsedChannel = std::stoi(ch); } catch (...) { parsedChannel = 0; }
            if (parsedChannel < 1 || parsedChannel > 30) {
                status = "请填写已确认的 RFCOMM 通道号（1–30）。";
                log(status);
                invalidate();
                return false;
            }
            channel = parsedChannel;
        }

        simulation = sim;
        const std::string requestedModel = modelCode;
        const std::string requestedMac = mac;
        const int requestedChannel = channel;
        return startAsync(sim ? "正在连接模拟设备…" : "正在连接真实耳机…",
            [profile, sim, requestedModel, requestedMac, requestedChannel](AsyncResult& result) {
                if (result.controller) {
                    result.controller->disconnectDevice();
                    result.controller.reset();
                }
                std::unique_ptr<sony::IBluetoothConnector> connector;
                if (sim) {
                    connector = std::make_unique<sony::StatefulMdrSimulator>(profile);
                } else {
#if defined(ENABLE_NATIVE_BT) && defined(_WIN32)
                    connector = std::make_unique<sony::WindowsBthConnector>();
#else
                    result.status = "当前构建没有启用 Windows 原生蓝牙。请用 build_sony_ui.cmd 重新编译。";
                    result.messages.push_back(result.status);
                    return;
#endif
                }
                auto next = std::make_unique<sony::HeadphoneController>(profile, std::move(connector));
                if (!next->connectDevice(requestedMac, requestedChannel)) {
                    result.status = "连接失败：" + next->getLastError();
                    result.messages.push_back(result.status);
                    result.controller.reset();
                    result.reloadState = false;
                    return;
                }
                result.controller = std::move(next);
                result.status = sim ? "模拟设备已连接（仅验证本地逻辑）"
                                    : "传输连接和初始状态同步成功；功能命令仍需按机型实测验证";
                result.messages.push_back(result.status + " | 型号 " + requestedModel +
                                          (sim ? "" : (" | RFCOMM 通道 " + std::to_string(requestedChannel))));
                result.reloadState = true;
                // 功能 B：connectDevice 内部的 refreshAllDeviceStates 已含 0x56→0x57，这里把结果写进同步提示
                result.hasSyncNote = true;
                result.syncOk = true;
                result.syncNote = describeEqState(*result.controller);
            });
    }

    // ======================================================================
    // 10.5 功能 A：设备发现 / 功能 B：EQ 同步
    // ======================================================================
    // EQ 预设全表（UTF-8 版，供日志/同步摘要使用，与 presetOptions() 一一对应）
    static const char* presetLabelUtf8(sony::EqPresetId id) {
        switch (id) {
            case sony::EqPresetId::OFF:          return "关闭";
            case sony::EqPresetId::BRIGHT:       return "明快";
            case sony::EqPresetId::RELAXED:      return "舒缓";
            case sony::EqPresetId::VOCAL:        return "人声";
            case sony::EqPresetId::BASS_BOOST:   return "重低音";
            case sony::EqPresetId::ROCK:         return "摇滚";
            case sony::EqPresetId::POP:          return "流行";
            case sony::EqPresetId::JAZZ:         return "爵士";
            case sony::EqPresetId::DANCE:        return "舞曲";
            case sony::EqPresetId::ACOUSTIC:     return "原声";
            case sony::EqPresetId::EXCITED:      return "激昂";
            case sony::EqPresetId::MELLOW:       return "醇厚";
            case sony::EqPresetId::TREBLE_BOOST: return "高音增强";
            case sony::EqPresetId::SPEECH:       return "语音";
            case sony::EqPresetId::CUSTOM: return "自定义";
            case sony::EqPresetId::USER_SETTING1: return "用户设定 1";
            case sony::EqPresetId::USER_SETTING2: return "用户设定 2";
            case sony::EqPresetId::USER_SETTING3: return "用户设定 3";
            case sony::EqPresetId::USER_SETTING4: return "用户设定 4";
            case sony::EqPresetId::USER_SETTING5: return "用户设定 5";
            default:                             return "设备其他预设";
        }
    }

    static std::string describeEqState(const sony::HeadphoneController& c) {
        const auto& eq = c.getEqualizer();
        std::string s;
        // P0：ULT 维度与 EQ 维度是两个独立维度，摘要必须同时给出（不再二选一）
        if (c.getModelProfile().isUltWear) {
            const int mode = c.getUltBassMode();
            s += "ULT ";
            s += (mode <= 0 ? std::string("关闭") : std::to_string(mode));
            s += " · ";
        }
        s += std::string("预设 ") + presetLabelUtf8(eq.preset);
        s += " · Clear Bass ";
        s += (eq.clearBass >= 0 ? "+" : "");
        s += std::to_string(static_cast<int>(eq.clearBass));
        s += " · 频段 ";
        for (int i = 0; i < 5; ++i) {
            const int v = static_cast<int>(eq.bands[static_cast<size_t>(i)]);
            s += (v >= 0 ? "+" : "");
            s += std::to_string(v);
            if (i < 4) s += "/";
        }
        return s;
    }

    // 把扫描到的 MAC 填进自绘输入框（只存 12 位十六进制，显示时补冒号）
    void setMacField(const std::string& mac) {
        std::string digits;
        for (const char c : mac) {
            const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!hex) continue;
            digits.push_back((c >= 'a' && c <= 'f') ? static_cast<char>(c - 'a' + 'A') : c);
        }
        macField.text.assign(digits.begin(), digits.end());
        macField.caret = static_cast<int>(macField.text.size());
    }
    void setChannelField(int channel) {
        chField.text = std::to_wstring(channel);
        chField.caret = static_cast<int>(chField.text.size());
    }

    void scanDevices(bool native) {
        if (busy) return;
        startAsync(native ? "正在枚举本机蓝牙设备…" : "正在读取模拟设备列表…",
            [native](AsyncResult& result) {
                auto discovery = sony::makeDeviceDiscovery(native);
                const sony::DeviceScanResult scan = discovery->scan();
                result.hasScanResult = true;
                result.scanUsesNative = native;
                result.scan = scan;
                result.status = scan.message;
                result.messages.push_back(std::string("[设备发现] ") +
                                          sony::deviceScanStatusLabel(scan.status) + "：" + scan.message);
            });
    }

    // 点选设备：填入 MAC + 建议通道 → （真实设备且未配对时先尝试配对）→ 立即连接
    void pickDevice(int index) {
        if (busy) return;
        if (index < 0 || index >= static_cast<int>(devices.size())) return;
        const sony::BluetoothAudioDevice dev = devices[static_cast<size_t>(index)];
        const bool native = deviceScanNative;
        selectedDevice = index;
        setMacField(dev.macAddress);
        setChannelField(dev.suggestedRfcommChannel);

        sony::DeviceModelProfile profile;
        try {
            profile = sony::getProfileByModelCode(modelCode);
        } catch (const std::exception& e) {
            status = e.what();
            log(status);
            invalidate();
            return;
        }
        simulation = !native;
        const bool needPair = native && !dev.paired;
        const std::string progress = !native ? "正在连接所选模拟设备…"
                                    : needPair ? "正在请求配对并连接所选设备…"
                                               : "正在连接所选真实设备…";
        const std::string requestedMac = dev.macAddress;
        const int requestedChannel = dev.suggestedRfcommChannel;
        startAsync(progress, [profile, dev, native, needPair, requestedMac, requestedChannel](AsyncResult& result) {
            if (result.controller) {
                result.controller->disconnectDevice();
                result.controller.reset();
            }
            // 配对是「尽力尝试」：失败必须停下并给出可读原因，绝不假装已配对
            if (needPair) {
                auto discovery = sony::makeDeviceDiscovery(true);
                std::string outcome;
                if (!discovery->pairDevice(dev, nullptr, outcome)) {
                    result.status = "配对失败，未发起连接：" + outcome;
                    result.messages.push_back(result.status);
                    result.reloadState = false;
                    return;
                }
                result.messages.push_back("[配对] " + outcome);
            }
            std::unique_ptr<sony::IBluetoothConnector> connector;
            if (!native) {
                connector = std::make_unique<sony::StatefulMdrSimulator>(profile);
            } else {
#if defined(ENABLE_NATIVE_BT) && defined(_WIN32)
                connector = std::make_unique<sony::WindowsBthConnector>();
#else
                result.status = "当前构建没有启用 Windows 原生蓝牙。请用 build_sony_ui.cmd 重新编译。";
                result.messages.push_back(result.status);
                return;
            }
#endif
            }
            auto next = std::make_unique<sony::HeadphoneController>(profile, std::move(connector));
            if (!next->connectDevice(requestedMac, requestedChannel)) {
                result.status = "连接失败：" + next->getLastError();
                result.messages.push_back(result.status);
                result.controller.reset();
                result.reloadState = false;
                return;
            }
            result.messages.push_back("[设备发现] 已选用 " + dev.name + " (" + requestedMac + ") 通道 " +
                                      std::to_string(requestedChannel));
            result.controller = std::move(next);
            result.status = native ? "所选真实设备已连接；初始状态（含 EQ）已回读"
                                   : "所选模拟设备已连接（仅验证本地逻辑）";
            result.reloadState = true;
            // 连接成功后自动同步一次 EQ，状态栏直接给出设备侧真实值
            result.hasSyncNote = true;
            result.syncOk = true;
            result.syncNote = describeEqState(*result.controller);
        });
    }

    // 功能 B：主动 GET 0x56 并解析 0x57 回包，刷新界面
    void syncEqFromDevice() {
        if (!ensureConnected()) return;
        startAsync("正在从设备同步 EQ 状态…", [](AsyncResult& result) {
            if (!result.controller) {
                result.status = "设备连接已失效。";
                result.messages.push_back(result.status);
                return;
            }
            (void)result.controller->pollDeviceNotifications();  // 先吃掉可能在队列里的 0x59
            const bool ok = result.controller->syncEqualizerState();
            result.reloadState = ok;
            result.hasSyncNote = true;
            result.syncOk = ok;
            result.syncNote = ok ? describeEqState(*result.controller)
                                 : (result.controller->getLastError().empty()
                                        ? std::string("设备未返回 0x57 回包")
                                        : result.controller->getLastError());
            result.status = ok ? ("已与设备同步：" + result.syncNote) : ("同步 EQ 失败：" + result.syncNote);
            result.messages.push_back(result.status);
        });
    }

    static std::string WtoA(const std::wstring& s) {
        if (s.empty()) return std::string();
        const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                          nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
        return out;
    }

    void doRefresh() {
        if (!ensureConnected()) return;
        startAsync("正在读取耳机状态…", [](AsyncResult& result) {
            if (!result.controller) {
                result.status = "请先连接设备。";
                result.messages.push_back(result.status);
                return;
            }
            const bool ok = result.controller->refreshAllDeviceStates();
            result.reloadState = ok;
            result.status = ok ? "设备状态刷新成功。"
                               : "状态刷新失败：" + result.controller->getLastError();
            result.messages.push_back(result.status);
        });
    }

    void runControllerAction(const std::string& progress,
                             std::function<bool(sony::HeadphoneController&)> action,
                             const std::string& successMessage,
                             const std::string& failurePrefix) {
        if (!ensureConnected()) return;
        const bool updateEqBadge = (page == EQUALIZER);
        startAsync(progress, [action = std::move(action), successMessage, failurePrefix, updateEqBadge](AsyncResult& result) {
            if (!result.controller) {
                result.status = "设备连接已失效。";
                result.messages.push_back(result.status);
                return;
            }
            const bool ok = action(*result.controller);
            result.reloadState = ok;
            if (ok) {
                // Nonblocking read of already-arrived notifications only; do not launch an extra 0x56 GET.
                (void)result.controller->pollDeviceNotifications();
                if (updateEqBadge) {
                    result.hasSyncNote = true;
                    result.syncOk = true; // SET transaction's RET/NTFY updated the controller cache.
                    result.syncNote = describeEqState(*result.controller);
                }
            }
            result.status = ok ? successMessage : failurePrefix + result.controller->getLastError();
            result.messages.push_back(result.status);
        });
    }

    void applySound() {
        if (!ensureConnected()) return;
        // ambientLevel is a raw device byte. Do not convert it to an assumed 1..20 UI scale.
        ambientLevel = std::clamp(ambientLevel, 0, 255);
        const sony::AmbientSoundConfig cfg{ambientMode, static_cast<uint8_t>(ambientLevel), voiceFocus};
        runControllerAction("正在应用声音设置…",
            [cfg](sony::HeadphoneController& c) { return c.setAmbientSound(cfg); },
            "设备已确认声音设置命令；档位显示需要设备能力范围。", "声音设置失败：");
    }

    // ----------------------------------------------------------------------
    // 10.5 「点按即下发」：聆听模式 / 关注语音
    //
    // 用户反馈（m00360）：「聆听模式的切换也有问题」。根因：概览页的聆听模式卡只有
    // 三个分段按钮、没有应用按钮，而旧实现点击只改本地变量。这里把一次点击直接变成
    // 一条完整的 0x68 环境音帧：
    //   乐观更新界面 → 工作线程下发 → 成功后回读（reloadState），失败则把界面与设备
    //   一起退回旧值（hasAmbientRollback）。
    // 关键点：只改 mode（以及关闭降噪时顺带清掉 voice），**绝不改动环境音等级**——
    // 等级是用户在滑杆上选的，切模式时被冲掉同样会被当成「切换有问题」。
    // ----------------------------------------------------------------------
    void commitAmbientTap(sony::NcAsmMode mode) {
        if (!ensureConnected()) return;
        if (busy) {
            log("上一次操作仍在执行，请稍候再切换聆听模式。");
            return;
        }
        const sony::AmbientSoundConfig cfg = sony::ambientTapConfig(mode, ambientLevel, voiceFocus);
        const sony::AmbientSoundConfig rollback{ambientMode, static_cast<uint8_t>(ambientLevel), voiceFocus};
        if (cfg.mode == ambientMode && cfg.voicePassthrough == voiceFocus && cfg.ambientLevel == ambientLevel) {
            return;  // 已是该模式，不重复下发
        }
        // 乐观更新：点击后界面立即反映新选择（按钮不再"点了没反应"）
        ambientMode = cfg.mode;
        voiceFocus = cfg.voicePassthrough;
        ambientLevel = cfg.ambientLevel;
        invalidate();
        startAsync("正在切换聆听模式…", [cfg, rollback](AsyncResult& result) {
            if (!result.controller) {
                result.status = "设备连接已失效，聆听模式未下发。";
                result.messages.push_back(result.status);
                result.hasAmbientRollback = true;
                result.rollbackAmbient = rollback;
                return;
            }
            const bool ok = result.controller->setAmbientSound(cfg);
            if (!ok) {
                result.status = "聆听模式切换失败：" + result.controller->getLastError();
                result.messages.push_back(result.status);
                result.hasAmbientRollback = true;
                result.rollbackAmbient = rollback;
                result.reloadState = false;
                return;
            }
            result.reloadState = true;
            result.status = "设备已确认聆听模式命令；状态以设备通知或手动刷新为准。";
            result.messages.push_back(result.status);
        });
    }

    void commitVoiceFocus(bool enabled) {
        if (!ensureConnected()) return;
        if (busy) {
            log("上一次操作仍在执行，请稍候再切换关注语音。");
            return;
        }
        if (ambientMode != sony::NcAsmMode::AMBIENT_SOUND) {
            log("关注语音只适用于环境音 ASM 模式；请先切换到环境音。");
            return;
        }
        const sony::AmbientSoundConfig cfg = sony::ambientTapConfig(ambientMode, ambientLevel, enabled);
        const sony::AmbientSoundConfig rollback{ambientMode, static_cast<uint8_t>(ambientLevel), voiceFocus};
        if (cfg.voicePassthrough == voiceFocus) {
            log("当前聆听模式为「关闭」，关注语音不生效；请先选择降噪或环境音。");
            return;
        }
        voiceFocus = cfg.voicePassthrough;
        invalidate();
        startAsync("正在切换关注语音…", [cfg, rollback](AsyncResult& result) {
            if (!result.controller) {
                result.status = "设备连接已失效，关注语音未下发。";
                result.messages.push_back(result.status);
                result.hasAmbientRollback = true;
                result.rollbackAmbient = rollback;
                return;
            }
            const bool ok = result.controller->setAmbientSound(cfg);
            if (!ok) {
                result.status = "关注语音切换失败：" + result.controller->getLastError();
                result.messages.push_back(result.status);
                result.hasAmbientRollback = true;
                result.rollbackAmbient = rollback;
                result.reloadState = false;
                return;
            }
            result.reloadState = true;
            result.status = "设备已确认关注语音命令；状态以设备通知或手动刷新为准。";
            result.messages.push_back(result.status);
        });
    }

    // 概览页「情景调节」卡：一次点击 = 后端 applySceneProfile 的三维组合
    // （降噪/环境音 + ULT Bass 模式 + EQ 预设），各维正交，失败步骤逐条进日志。
    void applySceneCard(int card) {
        if (!ensureConnected()) return;
        if (busy) {
            log("上一次操作仍在执行，请稍候再套用情景。");
            return;
        }
        const int idx = std::min(std::max(card, 0), sony::kSceneCardCount - 1);
        const sony::SceneProfile profile = sony::sceneProfileAtCard(idx);
        const std::string label = sony::sceneCardLabelUtf8(idx);
        startAsync("正在套用情景：" + label + "…", [profile, idx, label](AsyncResult& result) {
            if (!result.controller) {
                result.status = "设备连接已失效。";
                result.messages.push_back(result.status);
                return;
            }
            const sony::SceneApplyReport report = result.controller->applySceneProfile(profile);
            for (const auto& line : report.stepLogs) result.messages.push_back("[情景] " + line);
            result.sceneCard = idx;
            result.sceneOk = report.allSucceeded();
            // 无论成败都回读：失败时界面必须显示设备的真实状态，而不是"看起来套用了"
            result.reloadState = true;
            result.status = result.sceneOk
                ? ("情景「" + label + "」已套用（" + std::to_string(report.succeededSteps) + "/" +
                   std::to_string(report.totalSteps) + " 步，" + std::to_string(report.totalDurationMs) +
                   " ms）并回读。")
                : ("情景「" + label + "」部分失败（" + std::to_string(report.succeededSteps) + "/" +
                   std::to_string(report.totalSteps) + " 步，" + std::to_string(report.totalDurationMs) +
                   " ms），已按设备实际状态刷新。");
            result.messages.push_back(result.status);
        });
    }

    void applyEq() {
        if (!ensureConnected()) return;
        // Start from the device's last valid GET/RET so a preset hidden from the Chinese-only
        // chip list is never silently changed to OFF. User selection is explicit; manual band/Clear
        // Bass edits are independent and stay available on real ULT WEAR.
        sony::EqualizerConfig eq = controller->getEqualizer();
        if (presetSelectionTouched && selectedPreset >= 0 &&
            selectedPreset < static_cast<int>(presetOptions().size())) {
            eq.preset = presetIdAt(selectedPreset);
        }
        eq.clearBass = static_cast<int8_t>(std::clamp(clearBass, -10, 10));
        for (int i = 0; i < 5; ++i) {
            eq.bands[i] = static_cast<int8_t>(std::clamp(bands[i], -10, 10));
        }
        const bool presetChanged = presetSelectionTouched;
        runControllerAction("正在应用均衡器…",
            [eq](sony::HeadphoneController& c) { return c.setEqualizer(eq); },
            presetChanged ? "已下发所选中文 EQ 预设及独立增益，并以设备回读确认。"
                          : "已下发手动五段 / Clear Bass 调整，并以设备回读确认。",
            "均衡器设置失败：");
    }

    bool ensureConnected() {
        if (!controller || !controller->isConnected()) {
            log("设备尚未连接，请先到「设备设置」建立连接。");
            invalidate();
            return false;
        }
        return true;
    }
};

// ============================================================================
// 12. 窗口过程
// ============================================================================
SoundConnectApp* g_app = nullptr;
HDC g_backBufferDC = nullptr;
HBITMAP g_backBufferBitmap = nullptr;
HBITMAP g_backBufferOldBitmap = nullptr;
int g_backBufferW = 0, g_backBufferH = 0;

void resetBackBuffer() {
    if (g_backBufferDC && g_backBufferOldBitmap) SelectObject(g_backBufferDC, g_backBufferOldBitmap);
    if (g_backBufferBitmap) DeleteObject(g_backBufferBitmap);
    if (g_backBufferDC) DeleteDC(g_backBufferDC);
    g_backBufferDC = nullptr; g_backBufferBitmap = nullptr; g_backBufferOldBitmap = nullptr;
    g_backBufferW = 0; g_backBufferH = 0;
}

LRESULT CALLBACK SoundConnectWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SoundConnectApp* app = g_app;
    switch (msg) {
        case WM_APP_ASYNC_DONE: {
            auto* posted = reinterpret_cast<std::shared_ptr<SoundConnectApp::AsyncResult>*>(lp);
            if (posted) {
                if (app) app->finishAsync(*posted);
                delete posted;
            }
            return 0;
        }
        case WM_CREATE: {
            if (!app) return -1;
            app->hwnd = hwnd;
            g_dpi = queryWindowDpi(hwnd);
            RECT c{};
            GetClientRect(hwnd, &c);
            app->updateMetrics(std::max(1L, c.right), std::max(1L, c.bottom));
            app->initFontsPublic();
            applyDwmWindowLook(hwnd);
            SetTimer(hwnd, 1, 500, nullptr);
            app->connectTo(true);
            return 0;
        }
        case WM_NCCALCSIZE:
            // 去掉整个非客户区（标题栏 + 边框），客户区覆盖整个窗口。
            if (wp == TRUE) return 0;
            break;
        case WM_NCHITTEST: {
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            const int bw = std::max(4, P(6));
            const bool left = p.x < wr.left + bw;
            const bool right = p.x >= wr.right - bw;
            const bool top = p.y < wr.top + bw;
            const bool bottom = p.y >= wr.bottom - bw;
            if (!IsZoomed(hwnd)) {
                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (top) return HTTOP;
                if (bottom) return HTBOTTOM;
            }
            POINT c = p;
            ScreenToClient(hwnd, &c);
            if (app && c.y < app->titleH) {
                // 窗口按钮保持客户区行为，其余部分作为拖动区
                SoundConnectApp::Hit* h = app->hitAt(c.x, c.y);
                if (h && (h->action == SoundConnectApp::A_TITLE_MIN ||
                          h->action == SoundConnectApp::A_TITLE_MAX ||
                          h->action == SoundConnectApp::A_TITLE_CLOSE)) {
                    return HTCLIENT;
                }
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (GetMonitorInfoW(mon, &mi)) {
                mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
            }
            // 最小尺寸只挡住"完全不可用"的极小窗口：以 680×400 设计基准换算，
            // 保证 1280×720 / 1366×768 这类小屏（含高 DPI 缩放）都不会被系统拒绝缩小。
            mmi->ptMinTrackSize.x = std::max(420L, static_cast<LONG>(P(680)));
            mmi->ptMinTrackSize.y = std::max(300L, static_cast<LONG>(P(400)));
            return 0;
        }
        case WM_DPICHANGED: {
            g_dpi = HIWORD(wp);
            if (g_dpi < 48) g_dpi = 96;
            const RECT* sug = reinterpret_cast<const RECT*>(lp);
            if (sug) {
                SetWindowPos(hwnd, nullptr, sug->left, sug->top,
                             sug->right - sug->left, sug->bottom - sug->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            if (app) {
                app->lastScaleKey = -1;
                RECT c{};
                GetClientRect(hwnd, &c);
                app->updateMetrics(c.right, c.bottom);
                app->initFontsPublic();
                app->invalidate();
            }
            return 0;
        }
        case WM_SIZE: {
            if (app) {
                RECT c{};
                GetClientRect(hwnd, &c);
                app->updateMetrics(c.right, c.bottom);
                app->maximized = IsZoomed(hwnd) != FALSE;
                app->invalidate();
            }
            return 0;
        }
        case WM_ACTIVATE:
        case WM_NCACTIVATE:
            if (app) {
                app->windowActive = (msg == WM_NCACTIVATE) ? (wp != FALSE) : (LOWORD(wp) != WA_INACTIVE);
                app->invalidate();
            }
            return TRUE;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int w = rc.right - rc.left, h = rc.bottom - rc.top;
            if (!g_backBufferDC || w != g_backBufferW || h != g_backBufferH) {
                resetBackBuffer();
                g_backBufferDC = CreateCompatibleDC(hdc);
                g_backBufferBitmap = CreateCompatibleBitmap(hdc, std::max(1, w), std::max(1, h));
                g_backBufferOldBitmap = static_cast<HBITMAP>(SelectObject(g_backBufferDC, g_backBufferBitmap));
                g_backBufferW = w; g_backBufferH = h;
            }
            HDC mem = g_backBufferDC;
            if (app) app->draw(mem, w, h);
            else fillRect(mem, Rect{0, 0, w, h}, T_SURFACE);
#ifdef UI_PAINT_TRACE
            if (FILE* f = std::fopen("paint-trace.log", "a")) {
                std::fprintf(f, "paint w=%d h=%d dpi=%u scale=%.3f titleH=%d sideW=%d statusH=%d\n",
                             w, h, g_dpi, g_scale, app ? app->titleH : 0,
                             app ? app->sidebarW : 0, app ? app->statusH : 0);
                std::fclose(f);
            }
#endif
            BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (app && !app->busy) {
                SetCapture(hwnd);
                SetFocus(hwnd);
                app->onMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            }
            return 0;
        case WM_MOUSEMOVE:
            if (app) {
                const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
                if (GetCapture() == hwnd && app->activeSlider != SoundConnectApp::A_NONE) {
                    app->updateSlider(app->activeSlider, app->activeSliderIndex, mx, my);
                } else {
                    app->onHover(mx, my);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (app) {
                ReleaseCapture();
                if (app->activeSlider != SoundConnectApp::A_NONE) {
                    const auto a = app->activeSlider;
                    app->activeSlider = SoundConnectApp::A_NONE;
                    if (a == SoundConnectApp::A_SLIDER_AMBIENT) app->applySound();
                    else app->applyEq();
                }
            }
            return 0;
        case WM_MOUSEWHEEL:
            if (app) app->onWheel(GET_WHEEL_DELTA_WPARAM(wp));
            return 0;
        case WM_CHAR:
            if (app) app->onChar(static_cast<wchar_t>(wp));
            return 0;
        case WM_KEYDOWN:
            if (app) app->onKeyDown(static_cast<int>(wp));
            return 0;
        case WM_TIMER:
            if (app && wp == 1) app->tickCaret();
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                POINT p{};
                GetCursorPos(&p);
                ScreenToClient(hwnd, &p);
                if (app) app->onHover(p.x, p.y);
                return TRUE;
            }
            break;
        case WM_KILLFOCUS:
            if (app) {
                if (app->focusField) { app->focusField->focused = false; app->focusField = nullptr; }
                app->invalidate();
            }
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, 1);
            resetBackBuffer();
            if (g_app == app) g_app = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ============================================================================
// 13. 入口
// ============================================================================
std::vector<std::string> splitCommandLine(const std::wstring& line) {
    std::vector<std::string> out;
    std::wstring cur;
    bool inQuote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const wchar_t c = line[i];
        if (c == L'"') { inQuote = !inQuote; continue; }
        if (!inQuote && (c == L' ' || c == L'\t')) {
            if (!cur.empty()) { out.push_back(SoundConnectApp::WtoA(cur)); cur.clear(); }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(SoundConnectApp::WtoA(cur));
    if (!out.empty()) out.erase(out.begin()); // 去掉 exe 路径
    return out;
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCmd) {
    const std::vector<std::string> args = splitCommandLine(GetCommandLineW());
    // 让 UI 可执行文件也能直接跑协议回归（便于在同一产物上取证）
    for (const auto& a : args) {
        if (a == "--test-sim" || a == "--test" || a == "--help" || a == "-h") {
            if (!GetConsoleWindow()) {
                AllocConsole();
                freopen("CONOUT$", "w", stdout);
                freopen("CONOUT$", "w", stderr);
            }
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>("SonySoundConnectUI.exe"));
            for (const auto& a2 : args) argv.push_back(const_cast<char*>(a2.c_str()));
            return sony_soundconnect_console_main(static_cast<int>(argv.size()), argv.data());
        }
    }

    enableDpiAwareness();

    SoundConnectApp app;
    g_app = &app;
    g_dpi = queryWindowDpi(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = SoundConnectWndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // 窗口图标：优先用资源里的原版 Sound Connect 图标（sony-soundconnect-icon.rc, ID 1）；
    // 万一资源没接进 exe（例如手动 g++ 编译未带 .o），回退到系统默认图标，保证不会没有图标。
    HICON appIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                  GetSystemMetrics(SM_CXICON),
                                                  GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    if (!appIcon) appIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = appIcon;
    wc.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                               GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (!wc.hIconSm) wc.hIconSm = appIcon;
    app.bgBrush = CreateSolidBrush(T_SURFACE);
    wc.hbrBackground = app.bgBrush;
    wc.lpszClassName = L"SonySoundConnectPremiumWindow";
    if (!RegisterClassExW(&wc)) {
        MessageBoxW(nullptr, L"窗口类注册失败。", L"SoundConnect", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 无边框：WS_POPUP + WS_THICKFRAME（保留系统缩放/阴影），非客户区由 WM_NCCALCSIZE 归零。
    const DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    const int dpi0 = static_cast<int>(g_dpi < 48 ? 96 : g_dpi);
    const int startW = MulDiv(1440, dpi0, 96);
    const int startH = MulDiv(940, dpi0, 96);
    RECT want{0, 0, startW, startH};
    HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        const int waW = mi.rcWork.right - mi.rcWork.left;
        const int waH = mi.rcWork.bottom - mi.rcWork.top;
        want.right = mi.rcWork.left + std::min(startW, waW - MulDiv(40, dpi0, 96));
        want.bottom = mi.rcWork.top + std::min(startH, waH - MulDiv(40, dpi0, 96));
        want.left = mi.rcWork.left + (waW - (want.right - want.left)) / 2;
        want.top = mi.rcWork.top + (waH - (want.bottom - want.top)) / 2;
    }
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"Sound Connect — Sony Audio Control Center",
                                style, want.left, want.top, want.right - want.left, want.bottom - want.top,
                                nullptr, nullptr, instance, nullptr);
    if (!hwnd) {
        MessageBoxW(nullptr, L"窗口创建失败。", L"SoundConnect", MB_OK | MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, showCmd);
    UpdateWindow(hwnd);
    app.applyStartupArgs(args);  // 取证/演示启动参数；无参数时行为完全不变

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_app = nullptr;
    return static_cast<int>(msg.wParam);
}

#else
int main(int argc, char* argv[]) {
    // 非 Windows 平台保留后端可编译性（CI / 自测走控制台入口）。
    if (argc > 1) return sony_soundconnect_console_main(argc, argv);
    std::cout << "SoundConnect desktop UI is a Windows frontend.\n"
              << "Run --test-sim to validate the portable protocol backend.\n";
    return 0;
}
#endif
