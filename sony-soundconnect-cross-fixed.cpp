/**
 * @file sony-soundconnect-cross.cpp
 * @brief Sony MDR Protocol (V1/V2) 跨平台命令行控制器与协议验证套件 (C++17)
 * @details
 *   - 架构定位：跨平台命令行交互式控制器 & 字节流协议栈验证工具（非 GUI 程序）
 *   - 核心特性：
 *     1. 持久流式拆帧器 (MdrStreamFrameParser)：支持 RFCOMM 半包拼接、多帧粘包拆分与坏帧隔离
 *     2. 0/1 交替序列号状态机：严格验证 ACK 期望序号，并在收到设备 DATA_MDR 帧时主动回发 ACK
 *     3. 机型能力配置表 (DeviceModelProfile)：区分 WH-1000XM5 / WH-1000XM4 / WF-1000XM5 指令与载荷差异
 *     4. 写后回读确认 (Write-then-Readback)：本地仪表盘状态仅由设备返回的 RET/NOTIFY 帧更新
 *     5. 显式运行模式：严格区分 --sim (有状态协议仿真) 与 --real <MAC> (真实蓝牙 RFCOMM 连接)
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// ==================== 跨平台原生蓝牙 Socket 头文件（通过 ENABLE_NATIVE_BT 启用） ====================
#if defined(ENABLE_NATIVE_BT)
  #if defined(_WIN32)
    #ifndef WIN32_LEAN_AND_MEAN
      #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
      #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2bth.h>
  #elif defined(__linux__)
    #include <cerrno>
    #include <fcntl.h>
    #include <poll.h>
    #include <sys/socket.h>
    #include <unistd.h>
    #ifndef AF_BLUETOOTH
      #define AF_BLUETOOTH 31
    #endif
    #define BTPROTO_RFCOMM 3
    struct bdaddr_compat {
        uint8_t b[6];
    } __attribute__((packed));
    struct sockaddr_rc_compat {
        sa_family_t rc_family;
        bdaddr_compat rc_bdaddr;
        uint8_t rc_channel;
    };
  #endif
#endif

// ==================== 功能 A：Windows 蓝牙设备发现/配对所需头文件 ====================
// 设备枚举 (BluetoothFindFirstRadio / BluetoothFindFirstDevice / BluetoothGetDeviceInfo) 与
// 配对 (BluetoothAuthenticateDeviceEx) 都声明在 bluetoothapis.h，由系统库 bthprops 提供导出。
// 因此链接阶段（无论 console 目标还是 UI 目标）都必须追加 -lbthprops。
// 注意：该能力与 ENABLE_NATIVE_BT 无关——即使只跑模拟器回归，也要能枚举真实适配器做诊断。
#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <bluetoothapis.h>
#endif

namespace sony {

// ============================================================================
// 第一部分：机型配置表与 Sony MDR 协议常量定义
// ============================================================================

constexpr uint8_t FRAME_START  = 0x3E; // '>'
constexpr uint8_t FRAME_END    = 0x3C; // '<'
constexpr uint8_t FRAME_ESCAPE = 0x3D; // '='
constexpr uint8_t ESCAPE_MASK  = 0x10;

enum class DataType : uint8_t {
    DATA       = 0x00,
    ACK        = 0x01,
    DATA_MDR   = 0x0C,
    DATA_MDR_2 = 0x0E
};

// Sony MDR V1/V2 经核实的指令集
enum class Command : uint8_t {
    CONNECT_GET_PROTOCOL_INFO = 0x00,
    CONNECT_RET_PROTOCOL_INFO = 0x01,

    // V1 电量指令 (0x10/0x11) 与 V2 电量指令 (0x22/0x23) 在不同代际存在复用，由 DeviceModelProfile 路由
    V1_BATTERY_GET_STATUS     = 0x10,
    V1_BATTERY_RET_STATUS     = 0x11,
    V2_BATTERY_GET_STATUS     = 0x22,
    V2_BATTERY_RET_STATUS     = 0x23,

    POWER_OFF_GET_CAPABILITY  = 0x24,
    // TODO(protocol): 0x25 当前未由目标机型实机抓包确认；仅作实验性候选值，不能视为通用 Sony 关机命令。
    POWER_OFF_SET             = 0x25,

    EQEBB_GET_PARAM           = 0x56,
    EQEBB_RET_PARAM           = 0x57,
    EQEBB_SET_PARAM           = 0x58,
    EQEBB_NTFY_PARAM          = 0x59,

    NCASM_GET_PARAM           = 0x66,
    NCASM_RET_PARAM           = 0x67,
    NCASM_SET_PARAM           = 0x68,
    NCASM_NTFY_PARAM          = 0x69,

    AUDIO_CODEC_GET_PARAM     = 0xA6,
    AUDIO_CODEC_RET_PARAM     = 0xA7,
    AUDIO_CODEC_SET_PARAM     = 0xA8,

    SYSTEM_WEAR_GET_PARAM     = 0xB6,
    SYSTEM_WEAR_RET_PARAM     = 0xB7,
    SYSTEM_WEAR_SET_PARAM     = 0xB8,

    // 修正后的 DSEE Extreme / HX 指令字 (0xE6 / 0xE7 / 0xE8)
    UPSCALING_GET_PARAM       = 0xE6,
    UPSCALING_RET_PARAM       = 0xE7,
    UPSCALING_SET_PARAM       = 0xE8,

    SMART_TALKING_GET_PARAM   = 0xF6,
    SMART_TALKING_RET_PARAM   = 0xF7,
    SMART_TALKING_SET_PARAM   = 0xF8
};

enum class ProtocolGeneration : uint8_t {
    MDR_V1 = 1,
    MDR_V2 = 2
};

enum class BatteryLayout : uint8_t {
    SINGLE_HEADPHONE = 0, // 头戴式耳机：单一电池百分比 + 充电状态
    DUAL_EARBUDS_CASE = 1 // 真无线耳塞：左耳 + 右耳 + 充电盒
};

// 目标型号的硬件协议能力描述表
struct DeviceModelProfile {
    std::string modelCode;
    std::string displayName;
    ProtocolGeneration generation;
    int defaultRfcommChannel;
    BatteryLayout batteryLayout;
    uint8_t ncAsmInquiredType;   // 如 0x17 (NC/ASM 组合带等级)
    uint8_t eqInquiredType;      // 如 0x00 (标准可调 EQ) 或 0x01
    uint8_t dseeInquiredType;    // 如 0x01 (DSEE Extreme)
    bool isUltWear = false;
    uint8_t handshakeVersion = 0; // 0 表示使用 generation 对应的 1/2；ULT WEAR 的已知握手版本字段为 3
    bool supportsSpeakToChat = true;
    bool supportsDseeControl = true;
    bool supportsCodecQualityControl = true;
    bool supportsWearDetectionControl = true;
    bool supportsAmbientLevel = true;
};

inline DeviceModelProfile getProfileByModelCode(const std::string& code) {
    if (code == "ULT" || code == "ULT-WEAR" || code == "ULT WEAR" || code == "WH-ULT900N") {
        // ULT WEAR (WH-ULT900N): third-party reverse-engineering reports V2 RFCOMM on channel 18,
        // binary NC/ambient control and ULT bass mode through EQ inquiry type 0x03.
        // Unverified features are intentionally gated off instead of borrowing XM5 payloads.
        return {"ULT", "Sony ULT WEAR (WH-ULT900N)", ProtocolGeneration::MDR_V2, 18,
                BatteryLayout::SINGLE_HEADPHONE, 0x17, 0x03, 0x01,
                true, 3, false, false, false, false, false};
    }
    if (code == "XM4" || code == "WH-1000XM4") {
        return {"WH-1000XM4", "Sony WH-1000XM4 (头戴式)", ProtocolGeneration::MDR_V1, 1,
                BatteryLayout::SINGLE_HEADPHONE, 0x17, 0x01, 0x01};
    }
    if (code == "WF-XM5" || code == "WF-1000XM5") {
        return {"WF-1000XM5", "Sony WF-1000XM5 (真无线耳塞)", ProtocolGeneration::MDR_V2, 2,
                BatteryLayout::DUAL_EARBUDS_CASE, 0x17, 0x00, 0x01};
    }
    if (code == "XM5" || code == "WH-1000XM5") {
        return {"WH-1000XM5", "Sony WH-1000XM5 (头戴式)", ProtocolGeneration::MDR_V2, 1,
                BatteryLayout::SINGLE_HEADPHONE, 0x17, 0x00, 0x01};
    }
    throw std::invalid_argument("不支持的机型代码: " + code +
                                "。可用值为 ULT、XM5、XM4、WF-XM5");
}

// 1. 环境音与降噪配置
enum class NcAsmMode : uint8_t {
    OFF           = 0x00,
    NOISE_CANCEL  = 0x01,
    AMBIENT_SOUND = 0x02
};

struct AmbientSoundConfig {
    NcAsmMode mode = NcAsmMode::NOISE_CANCEL;
    uint8_t ambientLevel = 10;     // 仅在 AMBIENT_SOUND 模式下有效 (1 ~ 20)
    bool voicePassthrough = false; // 关注语音
};

// 2. EQ 均衡器预设与配置 (补齐 0xA2 USER_DEFINED)
enum class EqPresetId : uint8_t {
    OFF          = 0x00,
    ROCK         = 0x01,
    POP          = 0x02,
    JAZZ         = 0x03,
    BRIGHT       = 0x10,
    EXCITED      = 0x11,
    MELLOW       = 0x12,
    RELAXED      = 0x13,
    VOCAL        = 0x14,
    TREBLE_BOOST = 0x15,
    BASS_BOOST   = 0x16,
    SPEECH       = 0x17,
    CUSTOM_1     = 0xA0,
    CUSTOM_2     = 0xA1,
    USER_DEFINED = 0xA2
};

struct EqualizerConfig {
    EqPresetId preset = EqPresetId::OFF;
    int8_t clearBass = 0; // -10 ~ +10
    std::array<int8_t, 5> bands = {0, 0, 0, 0, 0}; // 400Hz, 1kHz, 2.5kHz, 6.3kHz, 16kHz
};

// 3. 智能免摘对话配置
enum class SpeakToChatSensitivity : uint8_t {
    AUTO = 0x00,
    HIGH = 0x01,
    LOW  = 0x02
};

enum class SpeakToChatTimeout : uint8_t {
    SHORT    = 0x00, // 5s
    STANDARD = 0x01, // 15s
    LONG     = 0x02, // 30s
    NEVER    = 0x03
};

struct SpeakToChatConfig {
    bool enabled = false;
    SpeakToChatSensitivity sensitivity = SpeakToChatSensitivity::AUTO;
    SpeakToChatTimeout timeout = SpeakToChatTimeout::STANDARD;
};

// 4. 硬件与音频配置
enum class BluetoothConnectionQuality : uint8_t {
    PRIORITIZE_SOUND_QUALITY     = 0x00, // LDAC
    PRIORITIZE_STABLE_CONNECTION = 0x01  // AAC/SBC
};

struct HardwareSystemConfig {
    BluetoothConnectionQuality connectionQuality = BluetoothConnectionQuality::PRIORITIZE_SOUND_QUALITY;
    bool dseeExtremeEnabled = false;
    bool pauseWhenTakenOff = true;
};

// 5. 区分头戴式与真无线耳塞的真实电量结构（不支持的字段保持 std::nullopt）
struct BatteryState {
    std::optional<uint8_t> singleHeadphonePercent; // 头戴式主电量
    bool isCharging = false;
    std::optional<uint8_t> leftEarbudPercent;      // 真无线左耳
    std::optional<uint8_t> rightEarbudPercent;     // 真无线右耳
    std::optional<uint8_t> casePercent;            // 真无线充电盒
};

// 6. 场景预设执行报告（杜绝无条件返回 true）
enum class SceneProfile : uint8_t {
    DEEP_FOCUS      = 0,
    OFFICE_COLLAB   = 1,
    STREET_COMMUTE  = 2,
    HIFI_AUDIOPHILE = 3
};

struct SceneApplyReport {
    size_t totalSteps = 0;
    size_t succeededSteps = 0;
    std::vector<std::string> stepLogs;
    bool allSucceeded() const { return totalSteps > 0 && totalSteps == succeededSteps; }
};

// ---------------------------------------------------------------------------
// 6.1 概览页两块交互的**纯逻辑**（UI 与 --test-sim 共用同一份实现，避免"界面行为无法被断言"）
//
//  背景（用户反馈 m00360）：「聆听模式的切换无效」。根因不是协议层，而是概览页的
//  「聆听模式」卡只有三个分段按钮、没有「应用」按钮，点击只改本地变量。因此这里把
//  「一次点击」显式定义为一条完整的环境音帧：模式改变，当前等级与关注语音原样带过去。
//  概览页的「情景调节」卡同理：卡片序号 → 场景预设 → 三维（降噪/环境音 + ULT + EQ）。
// ---------------------------------------------------------------------------
constexpr int kSceneCardCount = 4;

inline SceneProfile sceneProfileAtCard(int index) {
    const int clamped = index < 0 ? 0 : (index >= kSceneCardCount ? kSceneCardCount - 1 : index);
    return static_cast<SceneProfile>(clamped);
}

inline const char* sceneCardLabelUtf8(int index) {
    switch (sceneProfileAtCard(index)) {
        case SceneProfile::DEEP_FOCUS:      return "深度专注";
        case SceneProfile::OFFICE_COLLAB:   return "办公协作";
        case SceneProfile::STREET_COMMUTE:  return "街头通勤";
        case SceneProfile::HIFI_AUDIOPHILE: return "Hi-Fi 鉴赏";
    }
    return "情景";
}

// 一次「聆听模式」点击 → 待下发的环境音配置。
//  - 等级夹到协议有效区间 [1,20]（0 会被 0x68 帧拒收，见 NCASM_SET_PARAM 校验）；
//  - 关闭降噪时关注语音无意义，统一携带 false，避免下发自相矛盾的帧。
inline AmbientSoundConfig ambientTapConfig(NcAsmMode mode, int ambientLevel, bool voicePassthrough) {
    AmbientSoundConfig cfg;
    cfg.mode = mode;
    cfg.ambientLevel = static_cast<uint8_t>(std::clamp(ambientLevel, 1, 20));
    cfg.voicePassthrough = (mode == NcAsmMode::OFF) ? false : voicePassthrough;
    return cfg;
}

// =====================================================================
// 6.5 P1 离线功能：本地设置持久化（纯本地文件，不涉及任何蓝牙报文）
// 依据：work/xapk/offline-feature-inventory.md「设置与状态持久化」一行；官方 App 同样把
//       上次选择保存在本地（app_outline.txt 中 settings 布局与 strings.txt 的
//       Msg_Save_Settings 一类文案），但其内部存储键名无法从资源确认，故这里自定义
//       纯文本格式：key=value 每行一项、行序无关、未知键忽略、缺失键回落默认值。
// 约束：只用标准库（<fstream>），不引入第三方依赖；解析必须显式失败，绝不"猜"。
// =====================================================================
struct PersistedState {
    std::string modelCode = "ULT";
    std::string macAddress;
    int ultBassMode = 0;
    EqualizerConfig equalizer{};
    AmbientSoundConfig ambient{};
    bool hasAmbient = false;
    bool valid = false;
};

namespace persisted_detail {
inline std::string encodePreset(EqPresetId id) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
        << static_cast<int>(id);
    return out.str();
}

inline bool decodeInt(const std::string& text, int& out) {
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last = first + text.size();
    int value = 0;
    const auto result = std::from_chars(first, last, value, 10);
    if (result.ec != std::errc() || result.ptr != last) return false;
    out = value;
    return true;
}

inline bool decodeHexByte(const std::string& text, int& out) {
    if (text.size() < 3 || text[0] != '0' || (text[1] != 'x' && text[1] != 'X')) return false;
    const char* first = text.data() + 2;
    const char* last = text.data() + text.size();
    int value = 0;
    const auto result = std::from_chars(first, last, value, 16);
    if (result.ec != std::errc() || result.ptr != last || value < 0 || value > 0xFF) return false;
    out = value;
    return true;
}
}  // namespace persisted_detail

inline std::string serializePersistedState(const PersistedState& state) {
    std::ostringstream out;
    out << "version=1\n";
    out << "model=" << state.modelCode << "\n";
    if (!state.macAddress.empty()) out << "mac=" << state.macAddress << "\n";
    const int ult = state.ultBassMode < 0 ? 0 : (state.ultBassMode > 2 ? 2 : state.ultBassMode);
    out << "ult=" << ult << "\n";
    out << "preset=" << persisted_detail::encodePreset(state.equalizer.preset) << "\n";
    out << "clearBass=" << static_cast<int>(state.equalizer.clearBass) << "\n";
    out << "bands=";
    for (int i = 0; i < 5; ++i) {
        out << static_cast<int>(state.equalizer.bands[static_cast<size_t>(i)]);
        if (i < 4) out << ',';
    }
    out << "\n";
    if (state.hasAmbient) {
        out << "ambientMode=" << static_cast<int>(state.ambient.mode) << "\n";
        out << "ambientLevel=" << static_cast<int>(state.ambient.ambientLevel) << "\n";
        out << "voice=" << (state.ambient.voicePassthrough ? 1 : 0) << "\n";
    }
    return out.str();
}

inline bool parsePersistedState(const std::string& text, PersistedState& out) {
    PersistedState state{};
    bool seenVersion = false;
    bool seenPreset = false;
    bool seenBands = false;
    int bandValues[5] = {0, 0, 0, 0, 0};
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t sep = line.find('=');
        if (sep == std::string::npos) return false;
        const std::string key = line.substr(0, sep);
        const std::string value = line.substr(sep + 1);
        int iv = 0;
        if (key == "version") {
            if (!persisted_detail::decodeInt(value, iv) || iv != 1) return false;
            seenVersion = true;
        } else if (key == "model") {
            if (value.empty()) return false;
            try {
                (void)getProfileByModelCode(value);
            } catch (const std::exception&) {
                return false;  // 未知机型码：文件被视为无效，不静默改成默认机型
            }
            state.modelCode = value;
        } else if (key == "mac") {
            state.macAddress = value;
        } else if (key == "ult") {
            if (!persisted_detail::decodeInt(value, iv) || iv < 0 || iv > 2) return false;
            state.ultBassMode = iv;
        } else if (key == "preset") {
            if (!persisted_detail::decodeHexByte(value, iv)) return false;
            state.equalizer.preset = static_cast<EqPresetId>(iv);
            seenPreset = true;
        } else if (key == "clearBass") {
            if (!persisted_detail::decodeInt(value, iv) || iv < -10 || iv > 10) return false;
            state.equalizer.clearBass = static_cast<int8_t>(iv);
        } else if (key == "bands") {
            int filled = 0;
            size_t start = 0;
            while (start <= value.size()) {
                const size_t comma = value.find(',', start);
                const std::string piece = (comma == std::string::npos)
                                              ? value.substr(start)
                                              : value.substr(start, comma - start);
                if (filled >= 5) return false;
                if (!persisted_detail::decodeInt(piece, iv) || iv < -10 || iv > 10) return false;
                bandValues[filled++] = iv;
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            if (filled != 5) return false;  // 必须正好 5 段，残缺不猜
            seenBands = true;
        } else if (key == "ambientMode") {
            if (!persisted_detail::decodeInt(value, iv) || iv < 0 || iv > 2) return false;
            state.ambient.mode = static_cast<NcAsmMode>(iv);
            state.hasAmbient = true;
        } else if (key == "ambientLevel") {
            if (!persisted_detail::decodeInt(value, iv) || iv < 0 || iv > 20) return false;
            state.ambient.ambientLevel = iv;
        } else if (key == "voice") {
            if (!persisted_detail::decodeInt(value, iv) || (iv != 0 && iv != 1)) return false;
            state.ambient.voicePassthrough = (iv == 1);
        }
        // 未知键忽略：保持向前/向后兼容（旧版本写的新键不会让文件整体失效）
    }
    if (!seenVersion || !seenPreset || !seenBands) return false;
    for (int i = 0; i < 5; ++i) {
        state.equalizer.bands[static_cast<size_t>(i)] = static_cast<int8_t>(bandValues[i]);
    }
    state.valid = true;
    out = state;
    return true;
}

// 原子写：先写 .tmp 再替换，避免掉电/崩溃留下半个文件
inline bool savePersistedState(const std::string& path, const PersistedState& state) {
    if (path.empty()) return false;
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << serializePersistedState(state);
        if (!out.good()) return false;
    }
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

inline bool loadPersistedState(const std::string& path, PersistedState& out) {
    if (path.empty()) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return parsePersistedState(buffer.str(), out);
}

// 从控制器当前状态构建一份可持久化快照（ULT 与 EQ 两维同时保存，互不丢失）
// —— 定义在 HeadphoneController 之后（见文件下方 6.5.1）。

struct DecodedFrame {
    DataType dataType;
    uint8_t seqNumber;
    std::vector<uint8_t> payload;
};

// ============================================================================
// 第二部分：Sony MDR 协议帧编解码与流式拆帧器 (MdrStreamFrameParser)
// ============================================================================

class MdrProtocol {
public:
    // 翻转 0/1 序列号 (Sony MDR 标准交替序号)
    static constexpr uint8_t nextSequenceNumber(uint8_t currentSeq) noexcept {
        return static_cast<uint8_t>((currentSeq == 0) ? 1 : 0);
    }

    static std::vector<uint8_t> encodeFrame(DataType type, uint8_t seqNumber, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> unescaped;
        unescaped.reserve(7 + payload.size());

        unescaped.push_back(static_cast<uint8_t>(type));
        unescaped.push_back(seqNumber);

        uint32_t len = static_cast<uint32_t>(payload.size());
        unescaped.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
        unescaped.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
        unescaped.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        unescaped.push_back(static_cast<uint8_t>(len & 0xFF));

        unescaped.insert(unescaped.end(), payload.begin(), payload.end());

        uint8_t checksum = 0;
        for (uint8_t b : unescaped) {
            checksum = static_cast<uint8_t>(checksum + b);
        }
        unescaped.push_back(checksum);

        std::vector<uint8_t> frame;
        frame.reserve(unescaped.size() + 8);
        frame.push_back(FRAME_START);
        for (uint8_t b : unescaped) {
            if (b == FRAME_START || b == FRAME_END || b == FRAME_ESCAPE) {
                frame.push_back(FRAME_ESCAPE);
                frame.push_back(b & static_cast<uint8_t>(~ESCAPE_MASK));
            } else {
                frame.push_back(b);
            }
        }
        frame.push_back(FRAME_END);
        return frame;
    }

    static std::optional<DecodedFrame> decodeSingleCompleteFrame(const std::vector<uint8_t>& rawFrame) {
        if (rawFrame.size() < 9 || rawFrame.front() != FRAME_START || rawFrame.back() != FRAME_END) {
            return std::nullopt;
        }

        std::vector<uint8_t> unescaped;
        unescaped.reserve(rawFrame.size());

        for (size_t i = 1; i + 1 < rawFrame.size(); ++i) {
            if (rawFrame[i] == FRAME_ESCAPE) {
                if (i + 2 >= rawFrame.size()) {
                    return std::nullopt;
                }
                ++i;
                unescaped.push_back(rawFrame[i] | ESCAPE_MASK);
            } else {
                unescaped.push_back(rawFrame[i]);
            }
        }

        if (unescaped.size() < 7) {
            return std::nullopt;
        }

        uint8_t expectedChecksum = unescaped.back();
        uint8_t actualChecksum = 0;
        for (size_t i = 0; i + 1 < unescaped.size(); ++i) {
            actualChecksum = static_cast<uint8_t>(actualChecksum + unescaped[i]);
        }
        if (expectedChecksum != actualChecksum) {
            return std::nullopt;
        }

        uint32_t payloadLen = (static_cast<uint32_t>(unescaped[2]) << 24) |
                              (static_cast<uint32_t>(unescaped[3]) << 16) |
                              (static_cast<uint32_t>(unescaped[4]) << 8)  |
                              static_cast<uint32_t>(unescaped[5]);

        if (unescaped.size() != 7 + payloadLen) {
            return std::nullopt;
        }

        DecodedFrame result;
        result.dataType = static_cast<DataType>(unescaped[0]);
        result.seqNumber = unescaped[1];
        result.payload.assign(unescaped.begin() + 6, unescaped.end() - 1);
        return result;
    }

    // ==================== 经修正的 Sony MDR 协议载荷构造器 ====================

    static std::vector<uint8_t> buildHandshakePayload() {
        return {static_cast<uint8_t>(Command::CONNECT_GET_PROTOCOL_INFO), 0x00};
    }

    static std::vector<uint8_t> buildBatteryGetPayload(const DeviceModelProfile& profile) {
        if (profile.generation == ProtocolGeneration::MDR_V2) {
            uint8_t inquiryType = (profile.batteryLayout == BatteryLayout::DUAL_EARBUDS_CASE) ? 0x01 : 0x00;
            return {static_cast<uint8_t>(Command::V2_BATTERY_GET_STATUS), inquiryType};
        }
        return {static_cast<uint8_t>(Command::V1_BATTERY_GET_STATUS), 0x00};
    }

    // 环境音与降噪：修正为完整的 6 参数字段结构
    // [Cmd(0x68), InquiredType(0x17), TotalEffect(0/1), NcAsmSettingType(0=DualNC/1=ASM),
    //  NcValue(0/1), AsmMode(0=Normal/1=Voice), AsmLevel(1..20)]
    static std::vector<uint8_t> buildNcAsmSetPayload(const DeviceModelProfile& profile, const AmbientSoundConfig& cfg) {
        if (profile.isUltWear) {
            // ULT WEAR protocol reported by sony-ult-ctl:
            // [0x68, 0x17, 0x01, enabled, ambient, 0x02, voice_focus, 0x00].
            const uint8_t enabled = (cfg.mode == NcAsmMode::OFF) ? 0x00 : 0x01;
            const uint8_t ambient = (cfg.mode == NcAsmMode::AMBIENT_SOUND) ? 0x01 : 0x00;
            const uint8_t voice = (ambient && cfg.voicePassthrough) ? 0x01 : 0x00;
            return {static_cast<uint8_t>(Command::NCASM_SET_PARAM), 0x17, 0x01,
                    enabled, ambient, 0x02, voice, 0x00};
        }
        uint8_t totalEffect = (cfg.mode == NcAsmMode::OFF) ? 0x00 : 0x01;
        uint8_t settingType = (cfg.mode == NcAsmMode::AMBIENT_SOUND) ? 0x02 : 0x01;
        uint8_t ncDualSingle = (cfg.mode == NcAsmMode::NOISE_CANCEL) ? 0x01 : 0x00;
        uint8_t asmVoiceMode = (cfg.voicePassthrough) ? 0x01 : 0x00;
        uint8_t asmLevel = (cfg.mode == NcAsmMode::AMBIENT_SOUND)
                           ? std::clamp<uint8_t>(cfg.ambientLevel, 1, 20)
                           : 0x00;

        return {
            static_cast<uint8_t>(Command::NCASM_SET_PARAM),
            profile.ncAsmInquiredType, // 0x17
            totalEffect,
            settingType,
            ncDualSingle,
            asmVoiceMode,
            asmLevel
        };
    }

    static std::vector<uint8_t> buildNcAsmGetPayload(const DeviceModelProfile& profile) {
        return {static_cast<uint8_t>(Command::NCASM_GET_PARAM), profile.ncAsmInquiredType};
    }

    // EQ 增益编码：-10..+10 dB → 0x00..0x14（EQ 与 ULT 两条载荷共用同一个编码器，避免两处漂移）
    static uint8_t encodeEqGainByte(int8_t gain) {
        return static_cast<uint8_t>(std::clamp<int8_t>(gain, -10, 10) + 10);
    }

    // EQ 均衡器：使用机型对应的 eqInquiredType (如 WH-1000XM5/XB910N 使用 0x00，部分 V1 机型使用 0x01)
    static std::vector<uint8_t> buildEqualizerSetPayload(const DeviceModelProfile& profile, const EqualizerConfig& cfg) {
        std::vector<uint8_t> payload = {
            static_cast<uint8_t>(Command::EQEBB_SET_PARAM),
            profile.eqInquiredType,
            static_cast<uint8_t>(cfg.preset),
            0x06, // 1 ClearBass + 5 Bands
            encodeEqGainByte(cfg.clearBass)
        };
        for (int8_t b : cfg.bands) {
            payload.push_back(encodeEqGainByte(b));
        }
        return payload;
    }

    static std::vector<uint8_t> buildEqualizerGetPayload(const DeviceModelProfile& profile) {
        return {static_cast<uint8_t>(Command::EQEBB_GET_PARAM), profile.eqInquiredType};
    }

    // ULT WEAR（eqInquiredType 0x03）的 11 字节 SET 帧：同一帧同时承载「EQ 维度」与「ULT 维度」。
    //   [0]=0x58 EQEBB_SET_PARAM
    //   [1]=0x03 ULT WEAR 的 eqInquiredType
    //   [2]=preset（EQ 预设 id，原实现恒写 0x00，等价于「预设＝关闭」，掩盖了 EQ 维度）
    //   [3]=ult_mode 0/1/2
    //   [4]=0x06 「1 Clear Bass + 5 Bands」计数（与非 ULT 帧 [3] 语义相同，只是整体后移一格）
    //   [5]=clearBass+10
    //   [6..10]=五段增益 +10
    // 布局依据与不确定性见 CHANGES.md 第 7.2 节。
    static std::vector<uint8_t> buildUltBassSetPayload(const DeviceModelProfile& profile,
                                                       const EqualizerConfig& eq,
                                                       uint8_t mode) {
        const uint8_t safeMode = std::min<uint8_t>(mode, 2);
        std::vector<uint8_t> payload = {
            static_cast<uint8_t>(Command::EQEBB_SET_PARAM),
            profile.eqInquiredType,
            static_cast<uint8_t>(eq.preset),
            safeMode,
            0x06, // 1 ClearBass + 5 Bands
            encodeEqGainByte(eq.clearBass)
        };
        for (int8_t b : eq.bands) {
            payload.push_back(encodeEqGainByte(b));
        }
        return payload;
    }

    static std::vector<uint8_t> buildSpeakToChatSetPayload(const SpeakToChatConfig& cfg) {
        return {
            static_cast<uint8_t>(Command::SMART_TALKING_SET_PARAM),
            0x01,
            static_cast<uint8_t>(cfg.enabled ? 0x01 : 0x00),
            static_cast<uint8_t>(cfg.sensitivity),
            static_cast<uint8_t>(cfg.timeout)
        };
    }

    static std::vector<uint8_t> buildSpeakToChatGetPayload() {
        return {static_cast<uint8_t>(Command::SMART_TALKING_GET_PARAM), 0x01};
    }

    // 修正后的 DSEE Extreme 命令 (0xE8 SET / 0xE6 GET)
    static std::vector<uint8_t> buildDseeSetPayload(const DeviceModelProfile& profile, bool enabled) {
        return {
            static_cast<uint8_t>(Command::UPSCALING_SET_PARAM),
            profile.dseeInquiredType,
            static_cast<uint8_t>(enabled ? 0x01 : 0x00)
        };
    }

    static std::vector<uint8_t> buildDseeGetPayload(const DeviceModelProfile& profile) {
        return {static_cast<uint8_t>(Command::UPSCALING_GET_PARAM), profile.dseeInquiredType};
    }

    static std::vector<uint8_t> buildCodecSetPayload(BluetoothConnectionQuality quality) {
        return {
            static_cast<uint8_t>(Command::AUDIO_CODEC_SET_PARAM),
            0x01,
            static_cast<uint8_t>(quality)
        };
    }

    static std::vector<uint8_t> buildCodecGetPayload() {
        return {static_cast<uint8_t>(Command::AUDIO_CODEC_GET_PARAM), 0x01};
    }

    static std::vector<uint8_t> buildWearDetectionSetPayload(bool pauseWhenTakenOff) {
        return {
            static_cast<uint8_t>(Command::SYSTEM_WEAR_SET_PARAM),
            0x01,
            static_cast<uint8_t>(pauseWhenTakenOff ? 0x01 : 0x00)
        };
    }

    static std::vector<uint8_t> buildWearDetectionGetPayload() {
        return {static_cast<uint8_t>(Command::SYSTEM_WEAR_GET_PARAM), 0x01};
    }

    static std::vector<uint8_t> buildPowerOffPayload() {
        return {static_cast<uint8_t>(Command::POWER_OFF_SET), 0x01, 0x01};
    }

    static std::string getPresetName(EqPresetId preset) {
        switch (preset) {
            case EqPresetId::OFF:          return "Off (关闭)";
            case EqPresetId::ROCK:         return "Rock (摇滚)";
            case EqPresetId::POP:          return "Pop (流行)";
            case EqPresetId::JAZZ:         return "Jazz (爵士)";
            case EqPresetId::BRIGHT:       return "Bright (明快)";
            case EqPresetId::EXCITED:      return "Excited (激昂)";
            case EqPresetId::MELLOW:       return "Mellow (醇厚)";
            case EqPresetId::RELAXED:      return "Relaxed (舒缓)";
            case EqPresetId::VOCAL:        return "Vocal (人声)";
            case EqPresetId::TREBLE_BOOST: return "Treble Boost (高音增强)";
            case EqPresetId::BASS_BOOST:   return "Bass Boost (重低音增强)";
            case EqPresetId::SPEECH:       return "Speech (演讲)";
            case EqPresetId::CUSTOM_1:     return "Custom 1 (自定义 1)";
            case EqPresetId::CUSTOM_2:     return "Custom 2 (自定义 2)";
            case EqPresetId::USER_DEFINED: return "User Defined (用户自定义 0xA2)";
            default:                       return "Unknown";
        }
    }
};

/**
 * @brief 持久化 RFCOMM 字节流拆帧器
 * @details 解决单次 recv() 返回半包、粘包（多帧合并）或夹杂前导噪声的问题
 */
class MdrStreamFrameParser {
public:
    void feedBytes(const uint8_t* data, size_t len) {
        if (data && len > 0) {
            buffer_.insert(buffer_.end(), data, data + len);
        }
    }

    void reset() {
        buffer_.clear();
        corruptedFrameCount_ = 0;
    }

    // 从持久缓冲区中提取所有已到达的完整帧，保留尾部未接收完整的半包
    std::vector<DecodedFrame> extractFrames() {
        std::vector<DecodedFrame> output;

        while (!buffer_.empty()) {
            // 1. 寻找帧起始符 FRAME_START (0x3E)，丢弃起始符之前的无效字节
            auto startIt = std::find(buffer_.begin(), buffer_.end(), FRAME_START);
            if (startIt == buffer_.end()) {
                buffer_.clear();
                break;
            }
            if (startIt != buffer_.begin()) {
                buffer_.erase(buffer_.begin(), startIt);
            }

            // 2. 在起始符之后寻找帧结束符 FRAME_END (0x3C)
            //    注意：如果中间再次出现未转义的 FRAME_START，说明前一个起始符是残帧，重对齐到新起始符
            auto endIt = std::find(buffer_.begin() + 1, buffer_.end(), FRAME_END);
            if (endIt == buffer_.end()) {
                // 所有 MDR 命令均为小型控制帧；异常对端不能无限堆积未结束帧。
                constexpr size_t MAX_PENDING_FRAME_SIZE = 64 * 1024;
                if (buffer_.size() > MAX_PENDING_FRAME_SIZE) {
                    auto nextStart = std::find(buffer_.begin() + 1, buffer_.end(), FRAME_START);
                    if (nextStart != buffer_.end()) {
                        buffer_.erase(buffer_.begin(), nextStart);
                    } else {
                        buffer_.clear();
                    }
                    ++corruptedFrameCount_;
                    continue;
                }
                // 尚未收到完整帧，保留在 buffer_ 中等待后续 recv()
                break;
            }

            auto nestedStartIt = std::find(buffer_.begin() + 1, endIt, FRAME_START);
            if (nestedStartIt != endIt) {
                // 发现残缺前半帧，丢弃到新的 FRAME_START 处重新解析
                buffer_.erase(buffer_.begin(), nestedStartIt);
                ++corruptedFrameCount_;
                continue;
            }

            // 3. 切出单个候选帧 [FRAME_START ... FRAME_END]
            std::vector<uint8_t> candidate(buffer_.begin(), endIt + 1);
            buffer_.erase(buffer_.begin(), endIt + 1);

            auto decoded = MdrProtocol::decodeSingleCompleteFrame(candidate);
            if (decoded.has_value()) {
                output.push_back(std::move(*decoded));
            } else {
                ++corruptedFrameCount_;
            }
        }
        return output;
    }

    size_t getCorruptedFrameCount() const { return corruptedFrameCount_; }
    size_t getPendingBufferSize() const { return buffer_.size(); }

private:
    std::vector<uint8_t> buffer_;
    size_t corruptedFrameCount_ = 0;
};

// ============================================================================
// 第三部分：MAC 地址严格校验器与跨平台传输接口 (IBluetoothConnector)
// ============================================================================

inline bool validateBluetoothMacAddress(const std::string& mac, std::array<uint8_t, 6>& outBytes) {
    // 严格要求格式为 XX:XX:XX:XX:XX:XX (长度恰好为 17)
    if (mac.size() != 17) {
        return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        size_t pos = i * 3;
        if (i < 5 && mac[pos + 2] != ':') {
            return false;
        }
        char c1 = mac[pos];
        char c2 = mac[pos + 1];
        if (!std::isxdigit(static_cast<unsigned char>(c1)) ||
            !std::isxdigit(static_cast<unsigned char>(c2))) {
            return false;
        }
        unsigned int val = 0;
        std::sscanf(mac.c_str() + pos, "%2x", &val);
        if (val > 0xFF) {
            return false;
        }
        outBytes[i] = static_cast<uint8_t>(val);
    }
    return true;
}

enum class RecvStatus {
    OK,
    TIMEOUT,
    DISCONNECTED,
    ERROR_STATE
};

struct RecvResult {
    RecvStatus status = RecvStatus::TIMEOUT;
    std::vector<uint8_t> bytes;
    std::string errorMessage;
};

class IBluetoothConnector {
public:
    virtual ~IBluetoothConnector() = default;
    virtual bool isSimulation() const = 0;
    virtual bool connect(const std::string& macAddress, int rfcommChannel) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual bool sendAllBytes(const std::vector<uint8_t>& rawFrame) = 0;
    virtual RecvResult receiveRawChunk(int timeoutMs) = 0;
    virtual std::string getLastError() const = 0;
};

// ============================================================================
// 第四部分：有状态 Sony MDR 硬件协议仿真器 (StatefulMdrSimulator)
// ============================================================================

/**
 * @brief 有状态硬件协议仿真器
 * @details 绝不无条件确认命令！严格校验指令字、子类型、载荷长度、参数合法性与序列号交替。
 *          支持 GET 查询与 SET 状态修改，模拟真实耳机的雙向帧交互。
 */
class StatefulMdrSimulator : public IBluetoothConnector {
public:
    explicit StatefulMdrSimulator(DeviceModelProfile profile)
        : profile_(std::move(profile)) {}

    bool isSimulation() const override { return true; }

    bool connect(const std::string& macAddress, int rfcommChannel) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::array<uint8_t, 6> parsedMac{};
        if (!validateBluetoothMacAddress(macAddress, parsedMac)) {
            lastError_ = "无效的蓝牙 MAC 地址格式: " + macAddress;
            return false;
        }
        if (rfcommChannel <= 0 || rfcommChannel > 30) {
            lastError_ = "无效的 RFCOMM 通道号: " + std::to_string(rfcommChannel);
            return false;
        }
        connected_ = true;
        rxStreamBuffer_.clear();
        txParser_.reset();
        expectedHostSeq_ = 0;
        deviceTxSeq_ = 0;
        outstandingDeviceSeq_.reset();
        lastAcknowledgedDeviceSeq_.reset();
        deviceFramesSent_ = 0;
        deviceFramesAcked_ = 0;
        return true;
    }

    void disconnect() override {
        std::lock_guard<std::mutex> lock(mutex_);
        connected_ = false;
        rxStreamBuffer_.clear();
        txParser_.reset();
    }

    bool isConnected() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return connected_;
    }

    bool sendAllBytes(const std::vector<uint8_t>& rawFrame) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!connected_) {
            lastError_ = "仿真器未处于连接状态";
            return false;
        }

        txParser_.feedBytes(rawFrame.data(), rawFrame.size());
        auto frames = txParser_.extractFrames();
        if (frames.empty()) {
            lastError_ = "发送的数据未能解析出有效 MDR 帧";
            return false;
        }

        for (const auto& frame : frames) {
            if (!processIncomingHostFrame(frame)) {
                return false;
            }
        }
        return true;
    }

    RecvResult receiveRawChunk(int /*timeoutMs*/) override {
        std::lock_guard<std::mutex> lock(mutex_);
        // 先交付关机前已排队的 ACK/数据，再报告连接关闭；否则会丢失有效 ACK。
        if (!rxStreamBuffer_.empty()) {
            size_t chunkLen = rxStreamBuffer_.size();
            if (forceFragmentedOutput_ && chunkLen > 5) {
                chunkLen = 5;
            }
            const auto chunkEnd = rxStreamBuffer_.begin() +
                static_cast<std::vector<uint8_t>::difference_type>(chunkLen);
            std::vector<uint8_t> chunk(rxStreamBuffer_.begin(), chunkEnd);
            rxStreamBuffer_.erase(rxStreamBuffer_.begin(), chunkEnd);
            return {RecvStatus::OK, std::move(chunk), ""};
        }
        if (!connected_) {
            return {RecvStatus::DISCONNECTED, {}, "连接已断开"};
        }
        return {RecvStatus::TIMEOUT, {}, ""};
    }

    std::string getLastError() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastError_;
    }

    void setForceFragmentedOutput(bool enable) {
        std::lock_guard<std::mutex> lock(mutex_);
        forceFragmentedOutput_ = enable;
    }

    // 测试钩子（功能 B）：把一帧"设备主动推送"的 NOTIFY 载荷排入设备→主机队列，
    // 用于验证 0x59 在真实设备主动上报时的解析路径（不经过任何 GET/SET 事务）。
    bool pushUnsolicitedNotificationForTest(const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (outstandingDeviceSeq_.has_value()) {
            return false; // 仿真器一次只排一帧待确认数据，避免污染序号状态
        }
        sendDeviceDataFrame(payload);
        return true;
    }

    bool didHostAcknowledgeAllDeviceFrames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deviceFramesSent_ == deviceFramesAcked_ && !outstandingDeviceSeq_.has_value();
    }

    size_t getDeviceFramesSentCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deviceFramesSent_;
    }

    size_t getDeviceFramesAckedCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return deviceFramesAcked_;
    }

private:
    void enqueueFrameToHost(DataType type, uint8_t seq, const std::vector<uint8_t>& payload) {
        auto encoded = MdrProtocol::encodeFrame(type, seq, payload);
        rxStreamBuffer_.insert(rxStreamBuffer_.end(), encoded.begin(), encoded.end());
    }

    // 当前 EQ/ULT 两维真实状态编码成 0x57 帧。
    // 实机上 GET(0x56) 与 SET(0x58) 都会得到这样一帧 0x57；仿真器必须同样如实回复，
    // 否则"SET 回包被下一次 GET 吃掉"这条回归根本无法在模拟环境复现。
    std::vector<uint8_t> buildEqStateReplyPayload() const {
        if (profile_.isUltWear) {
            // ULT WEAR：一条 0x03 帧同时给出 EQ 与 ULT 两维
            std::vector<uint8_t> reply = {
                static_cast<uint8_t>(Command::EQEBB_RET_PARAM),
                profile_.eqInquiredType,
                static_cast<uint8_t>(eqState_.preset),
                ultBassMode_,
                0x06,
                MdrProtocol::encodeEqGainByte(eqState_.clearBass)};
            for (int8_t b : eqState_.bands) reply.push_back(MdrProtocol::encodeEqGainByte(b));
            return reply;
        }
        std::vector<uint8_t> reply = {
            static_cast<uint8_t>(Command::EQEBB_RET_PARAM),
            profile_.eqInquiredType,
            static_cast<uint8_t>(eqState_.preset),
            0x06,
            static_cast<uint8_t>(eqState_.clearBass + 10)};
        for (int8_t b : eqState_.bands) {
            reply.push_back(static_cast<uint8_t>(b + 10));
        }
        return reply;
    }

    void sendDeviceDataFrame(const std::vector<uint8_t>& payload) {
        // 这个简化仿真器一次只允许一条设备数据帧等待 ACK。
        if (outstandingDeviceSeq_.has_value()) {
            lastError_ = "仿真器仍有未确认的设备数据帧";
            return;
        }
        const uint8_t seq = deviceTxSeq_;
        outstandingDeviceSeq_ = seq;
        deviceTxSeq_ = MdrProtocol::nextSequenceNumber(seq);
        ++deviceFramesSent_;
        enqueueFrameToHost(DataType::DATA_MDR, seq, payload);
    }

    bool processIncomingHostFrame(const DecodedFrame& frame) {
        // 1. 处理主机发给设备的 ACK 帧
        if (frame.dataType == DataType::ACK) {
            if (outstandingDeviceSeq_.has_value() &&
                frame.seqNumber == MdrProtocol::nextSequenceNumber(*outstandingDeviceSeq_)) {
                lastAcknowledgedDeviceSeq_ = frame.seqNumber;
                outstandingDeviceSeq_.reset();
                ++deviceFramesAcked_;
                return true;
            }
            // 重复 ACK 可幂等接受，但不能重复增加确认计数。
            if (!outstandingDeviceSeq_.has_value() && lastAcknowledgedDeviceSeq_.has_value() &&
                frame.seqNumber == *lastAcknowledgedDeviceSeq_) {
                return true;
            }
            lastError_ = "主机返回的 ACK 序号与待确认设备帧不匹配";
            return false;
        }

        if (frame.dataType != DataType::DATA_MDR && frame.dataType != DataType::DATA_MDR_2) {
            lastError_ = "不支持的 DataType";
            return false;
        }

        // 2. 严格校验主机发送的 0/1 交替序列号
        if (frame.seqNumber != expectedHostSeq_) {
            lastError_ = "主机序列号错误：期望 " + std::to_string(expectedHostSeq_) +
                         "，实际收到 " + std::to_string(frame.seqNumber);
            return false;
        }

        if (frame.payload.empty()) {
            lastError_ = "空的 DATA_MDR 载荷";
            return false;
        }

        // 3. 严格校验具体命令字、子类型与参数结构（拒绝未知或格式错误的命令）
        auto cmd = static_cast<Command>(frame.payload[0]);
        std::vector<uint8_t> replyPayload;

        switch (cmd) {
            case Command::CONNECT_GET_PROTOCOL_INFO: {
                if (frame.payload.size() != 2 || frame.payload[1] != 0x00) {
                    lastError_ = "CONNECT_GET_PROTOCOL_INFO 载荷格式错误";
                    return false;
                }
                uint8_t majorVer = profile_.handshakeVersion != 0 ? profile_.handshakeVersion
                    : static_cast<uint8_t>(profile_.generation);
                // 返回包含协议版本与能力标志的真实格式帧
                replyPayload = {
                    static_cast<uint8_t>(Command::CONNECT_RET_PROTOCOL_INFO),
                    0x00, majorVer, 0x00, 0x01, 0x01
                };
                break;
            }

            case Command::V1_BATTERY_GET_STATUS: {
                if (profile_.generation != ProtocolGeneration::MDR_V1 || frame.payload.size() != 2) {
                    lastError_ = "当前机型不支持 V1 (0x10) 电量查询或长度错误";
                    return false;
                }
                replyPayload = {
                    static_cast<uint8_t>(Command::V1_BATTERY_RET_STATUS),
                    0x00, singleBatteryPercent_, static_cast<uint8_t>(isCharging_ ? 0x01 : 0x00)
                };
                break;
            }

            case Command::V2_BATTERY_GET_STATUS: {
                if (profile_.generation != ProtocolGeneration::MDR_V2 || frame.payload.size() != 2) {
                    lastError_ = "当前机型不支持 V2 (0x22) 电量查询或长度错误";
                    return false;
                }
                uint8_t inq = frame.payload[1];
                if (profile_.batteryLayout == BatteryLayout::SINGLE_HEADPHONE && inq == 0x00) {
                    replyPayload = {
                        static_cast<uint8_t>(Command::V2_BATTERY_RET_STATUS),
                        0x00, singleBatteryPercent_, static_cast<uint8_t>(isCharging_ ? 0x01 : 0x00)
                    };
                } else if (profile_.batteryLayout == BatteryLayout::DUAL_EARBUDS_CASE && inq == 0x01) {
                    replyPayload = {
                        static_cast<uint8_t>(Command::V2_BATTERY_RET_STATUS),
                        0x01, leftBatteryPercent_, rightBatteryPercent_, caseBatteryPercent_
                    };
                } else {
                    lastError_ = "电量查询子类型与机型电池布局不匹配";
                    return false;
                }
                break;
            }

            case Command::NCASM_SET_PARAM: {
                if (profile_.isUltWear) {
                    // ULT WEAR layout: 68 17 01 enabled ambient 02 voiceFocus 00.
                    if (frame.payload.size() != 8 || frame.payload[1] != 0x17 ||
                        frame.payload[2] != 0x01 || frame.payload[3] > 1 ||
                        frame.payload[4] > 1 || frame.payload[5] != 0x02 ||
                        frame.payload[6] > 1 || frame.payload[7] != 0x00) {
                        lastError_ = "ULT WEAR NC/ASM SET 参数格式错误";
                        return false;
                    }
                    const bool enabled = frame.payload[3] != 0;
                    const bool ambient = frame.payload[4] != 0;
                    ambientState_.mode = !enabled ? NcAsmMode::OFF
                        : (ambient ? NcAsmMode::AMBIENT_SOUND : NcAsmMode::NOISE_CANCEL);
                    ambientState_.voicePassthrough = enabled && ambient && frame.payload[6] != 0;
                    ambientState_.ambientLevel = 10;
                    break;
                }
                // 严格要求 7 字节完整结构: [0x68, 0x17, totalEffect, settingType, ncDual, voiceFocus, level]
                if (frame.payload.size() != 7 || frame.payload[1] != profile_.ncAsmInquiredType) {
                    lastError_ = "NCASM_SET_PARAM 载荷长度或子类型错误（期望 7 字节完整参数）";
                    return false;
                }
                uint8_t totalEffect = frame.payload[2];
                uint8_t settingType = frame.payload[3];
                uint8_t voiceFocus  = frame.payload[5];
                uint8_t asmLevel    = frame.payload[6];

                if (totalEffect == 0x00) {
                    ambientState_.mode = NcAsmMode::OFF;
                } else if (settingType == 0x02) {
                    if (asmLevel < 1 || asmLevel > 20) {
                        lastError_ = "环境音等级超出 [1, 20] 有效范围";
                        return false;
                    }
                    ambientState_.mode = NcAsmMode::AMBIENT_SOUND;
                    ambientState_.ambientLevel = asmLevel;
                    ambientState_.voicePassthrough = (voiceFocus == 0x01);
                } else {
                    ambientState_.mode = NcAsmMode::NOISE_CANCEL;
                    ambientState_.voicePassthrough = false;
                }
                break;
            }

            case Command::NCASM_GET_PARAM: {
                if (frame.payload.size() != 2 || frame.payload[1] != profile_.ncAsmInquiredType) {
                    lastError_ = "NCASM_GET_PARAM 格式错误";
                    return false;
                }
                if (profile_.isUltWear) {
                    const uint8_t enabled = (ambientState_.mode == NcAsmMode::OFF) ? 0x00 : 0x01;
                    const uint8_t ambient = (ambientState_.mode == NcAsmMode::AMBIENT_SOUND) ? 0x01 : 0x00;
                    const uint8_t voice = ambientState_.voicePassthrough ? 0x01 : 0x00;
                    replyPayload = {static_cast<uint8_t>(Command::NCASM_RET_PARAM), 0x17,
                                    0x01, enabled, ambient, 0x00, voice};
                    break;
                }
                uint8_t totalEffect = (ambientState_.mode == NcAsmMode::OFF) ? 0x00 : 0x01;
                uint8_t settingType = (ambientState_.mode == NcAsmMode::AMBIENT_SOUND) ? 0x02 : 0x01;
                uint8_t ncDual      = (ambientState_.mode == NcAsmMode::NOISE_CANCEL) ? 0x01 : 0x00;
                uint8_t voiceFocus  = ambientState_.voicePassthrough ? 0x01 : 0x00;
                replyPayload = {
                    static_cast<uint8_t>(Command::NCASM_RET_PARAM),
                    profile_.ncAsmInquiredType,
                    totalEffect, settingType, ncDual, voiceFocus, ambientState_.ambientLevel
                };
                break;
            }

            case Command::EQEBB_SET_PARAM: {
                if (profile_.isUltWear) {
                    // ULT WEAR 11 字节帧同时携带 EQ 维度与 ULT 维度：
                    // [0x58, 0x03, preset, ult_mode, 0x06, clearBass+10, b0..b4+10]
                    if (frame.payload.size() != 11 ||
                        frame.payload[1] != profile_.eqInquiredType ||
                        frame.payload[3] > 2 || frame.payload[4] != 0x06) {
                        lastError_ = "ULT WEAR EQ/ULT 组合 SET 参数格式错误";
                        return false;
                    }
                    for (size_t i = 5; i < 11; ++i) {
                        if (frame.payload[i] > 20) {
                            lastError_ = "ULT WEAR EQ 增益编码超出范围";
                            return false;
                        }
                    }
                    // 两维各自独立落库：写 EQ 不会清掉 ULT 模式，反之亦然
                    eqState_.preset = static_cast<EqPresetId>(frame.payload[2]);
                    ultBassMode_ = frame.payload[3];
                    eqState_.clearBass = static_cast<int8_t>(static_cast<int>(frame.payload[5]) - 10);
                    for (size_t i = 0; i < 5; ++i) {
                        eqState_.bands[i] = static_cast<int8_t>(static_cast<int>(frame.payload[6 + i]) - 10);
                    }
                    // 实机特性：SET 之后设备同样补一帧 0x57（写入后的最新状态）。
                    replyPayload = buildEqStateReplyPayload();
                    break;
                }
                // [0x58, eqInquiredType, presetId, 0x06, clearBass, b0, b1, b2, b3, b4] -> 共 10 字节
                if (frame.payload.size() != 10 ||
                    frame.payload[1] != profile_.eqInquiredType ||
                    frame.payload[3] != 0x06) {
                    lastError_ = "EQEBB_SET_PARAM 子类型或频段数量错误";
                    return false;
                }
                for (size_t i = 4; i < 10; ++i) {
                    if (frame.payload[i] > 20) {
                        lastError_ = "EQ 增益编码值超出 [0, 20] 范围";
                        return false;
                    }
                }
                eqState_.preset = static_cast<EqPresetId>(frame.payload[2]);
                eqState_.clearBass = static_cast<int8_t>(static_cast<int>(frame.payload[4]) - 10);
                for (size_t i = 0; i < 5; ++i) {
                    eqState_.bands[i] = static_cast<int8_t>(static_cast<int>(frame.payload[5 + i]) - 10);
                }
                // 实机特性：SET 之后设备同样补一帧 0x57（写入后的最新状态）。
                replyPayload = buildEqStateReplyPayload();
                break;
            }

            case Command::EQEBB_GET_PARAM: {
                if (frame.payload.size() != 2 || frame.payload[1] != profile_.eqInquiredType) {
                    lastError_ = "EQEBB_GET_PARAM 子类型与当前机型不匹配";
                    return false;
                }
                replyPayload = buildEqStateReplyPayload();
                break;
            }

            case Command::SMART_TALKING_SET_PARAM: {
                if (frame.payload.size() != 5 || frame.payload[1] != 0x01 ||
                    frame.payload[3] > 0x02 || frame.payload[4] > 0x03) {
                    lastError_ = "SMART_TALKING_SET_PARAM 参数非法";
                    return false;
                }
                stcState_.enabled = (frame.payload[2] != 0x00);
                stcState_.sensitivity = static_cast<SpeakToChatSensitivity>(frame.payload[3]);
                stcState_.timeout = static_cast<SpeakToChatTimeout>(frame.payload[4]);
                break;
            }

            case Command::SMART_TALKING_GET_PARAM: {
                if (frame.payload.size() != 2 || frame.payload[1] != 0x01) {
                    return false;
                }
                replyPayload = {
                    static_cast<uint8_t>(Command::SMART_TALKING_RET_PARAM),
                    0x01,
                    static_cast<uint8_t>(stcState_.enabled ? 0x01 : 0x00),
                    static_cast<uint8_t>(stcState_.sensitivity),
                    static_cast<uint8_t>(stcState_.timeout)
                };
                break;
            }

            case Command::UPSCALING_SET_PARAM: {
                // 仅接受 0xE8，拒绝旧代码中错误的 0x88
                if (frame.payload.size() != 3 || frame.payload[1] != profile_.dseeInquiredType) {
                    lastError_ = "UPSCALING_SET_PARAM (0xE8) 格式错误";
                    return false;
                }
                hwState_.dseeExtremeEnabled = (frame.payload[2] != 0x00);
                break;
            }

            case Command::UPSCALING_GET_PARAM: {
                if (frame.payload.size() != 2 || frame.payload[1] != profile_.dseeInquiredType) {
                    return false;
                }
                replyPayload = {
                    static_cast<uint8_t>(Command::UPSCALING_RET_PARAM),
                    profile_.dseeInquiredType,
                    static_cast<uint8_t>(hwState_.dseeExtremeEnabled ? 0x01 : 0x00)
                };
                break;
            }

            case Command::AUDIO_CODEC_SET_PARAM: {
                if (frame.payload.size() != 3 || frame.payload[1] != 0x01 || frame.payload[2] > 0x01) {
                    return false;
                }
                hwState_.connectionQuality = static_cast<BluetoothConnectionQuality>(frame.payload[2]);
                break;
            }

            case Command::AUDIO_CODEC_GET_PARAM: {
                if (frame.payload.size() != 2 || frame.payload[1] != 0x01) {
                    return false;
                }
                replyPayload = {
                    static_cast<uint8_t>(Command::AUDIO_CODEC_RET_PARAM),
                    0x01,
                    static_cast<uint8_t>(hwState_.connectionQuality)
                };
                break;
            }

            case Command::SYSTEM_WEAR_SET_PARAM: {
                if (frame.payload.size() != 3 || frame.payload[1] != 0x01) {
                    return false;
                }
                hwState_.pauseWhenTakenOff = (frame.payload[2] != 0x00);
                break;
            }

            case Command::SYSTEM_WEAR_GET_PARAM: {
                if (frame.payload.size() != 2 || frame.payload[1] != 0x01) {
                    return false;
                }
                replyPayload = {
                    static_cast<uint8_t>(Command::SYSTEM_WEAR_RET_PARAM),
                    0x01,
                    static_cast<uint8_t>(hwState_.pauseWhenTakenOff ? 0x01 : 0x00)
                };
                break;
            }

            case Command::POWER_OFF_SET: {
                if (frame.payload.size() != 3 || frame.payload[1] != 0x01) {
                    return false;
                }
                expectedHostSeq_ = MdrProtocol::nextSequenceNumber(expectedHostSeq_);
                enqueueFrameToHost(DataType::ACK, expectedHostSeq_, {});
                connected_ = false;
                return true;
            }

            default:
                lastError_ = "设备不支持或未识别的 MDR 命令字: 0x" + std::to_string(frame.payload[0]);
                return false;
        }

        // 4. 命令校验通过：翻转期望的主机序列号 (0 <-> 1)，并回复 ACK 帧
        expectedHostSeq_ = MdrProtocol::nextSequenceNumber(expectedHostSeq_);
        enqueueFrameToHost(DataType::ACK, expectedHostSeq_, {});

        // 5. 若有查询回复数据帧，紧接着发送 DATA_MDR 帧
        if (!replyPayload.empty()) {
            sendDeviceDataFrame(replyPayload);
        }
        return true;
    }

    mutable std::mutex mutex_;
    DeviceModelProfile profile_;
    bool connected_ = false;
    bool forceFragmentedOutput_ = false;
    std::string lastError_;

    uint8_t expectedHostSeq_ = 0;
    uint8_t deviceTxSeq_ = 0;
    std::optional<uint8_t> outstandingDeviceSeq_;
    std::optional<uint8_t> lastAcknowledgedDeviceSeq_;
    size_t deviceFramesSent_ = 0;
    size_t deviceFramesAcked_ = 0;

    MdrStreamFrameParser txParser_;
    std::vector<uint8_t> rxStreamBuffer_;

    // 仿真器内部真实维护的硬件状态
    uint8_t singleBatteryPercent_ = 86;
    uint8_t ultBassMode_ = 0;
    bool isCharging_ = false;
    uint8_t leftBatteryPercent_ = 90;
    uint8_t rightBatteryPercent_ = 88;
    uint8_t caseBatteryPercent_ = 95;

    AmbientSoundConfig ambientState_{NcAsmMode::NOISE_CANCEL, 10, false};
    EqualizerConfig eqState_{};
    SpeakToChatConfig stcState_{};
    HardwareSystemConfig hwState_{};
};

// ============================================================================
// 第四部分附：本机蓝牙音频设备发现与配对（功能 A：自行获取 MAC + 自动配对/连接）
// ----------------------------------------------------------------------------
// 设计约束（实现前已逐条确认）：
//   1. 发现逻辑放在后端：命令行 --scan 与 --test-sim 都能覆盖；模拟模式提供固定的假设备
//      列表，保证没有蓝牙硬件的机器上回归仍然可跑。
//   2. 不做 SDP/SPP 服务发现：Windows 侧 SDP 查询需要额外协议栈交互且易被安全软件拦截。
//      RFCOMM 通道只能按机型名称启发式推测，界面必须标注"推测值、可手动修改"，不能把
//      推测值伪装成协商结果。
//   3. 配对必须闭环验证：BluetoothAuthenticateDeviceEx 返回 S_OK 后仍要用
//      BluetoothGetDeviceInfo 复查 fAuthenticated，否则一律报"未确认配对成功"。
//   4. 三种失败分支（无适配器 / 无设备 / 无权限）各自有确定文案，不允许静默返回空列表。
// ============================================================================

enum class DeviceScanStatus : uint8_t {
    OK = 0,        // 原生扫描成功
    NO_ADAPTER,    // 没有蓝牙适配器，或蓝牙被关闭/驱动缺失
    NO_DEVICES,    // 适配器正常，但没有已配对/已知设备
    ACCESS_DENIED, // 蓝牙服务未运行，或策略/权限拒绝
    UNSUPPORTED,   // 平台不支持原生枚举
    SIMULATED      // 模拟器固定假列表
};

struct BluetoothAudioDevice {
    std::string name;                     // UTF-8 设备名
    std::string macAddress;               // 大写 XX:XX:XX:XX:XX:XX
    bool connected = false;               // 当前是否已连接
    bool paired = false;                  // 是否已完成配对 (fAuthenticated)
    bool remembered = false;              // 是否在系统"已知设备"里
    bool audioCandidate = false;          // CoD 或名称启发式判定为音频设备
    int suggestedRfcommChannel = 8;       // 启发式推测值，非 SDP 结果
    bool simulated = false;               // 来自模拟器假列表
};

struct DeviceScanResult {
    DeviceScanStatus status = DeviceScanStatus::UNSUPPORTED;
    std::string message;                  // 面向用户的确定性文案
    unsigned long systemError = 0;        // 系统错误码（0 表示未取到）
    int radioCount = 0;                   // 枚举到的蓝牙适配器数量
    std::vector<BluetoothAudioDevice> devices;

    bool succeeded() const {
        return status == DeviceScanStatus::OK || status == DeviceScanStatus::SIMULATED;
    }
};

// BLUETOOTH_ADDRESS.ullLong 的 12 位十六进制形式与系统显示顺序（XX:XX:…）一致：
// 实测 HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices 下的子键名
// 与该值逐字节相同，因此这里直接按高字节在前拆分。
std::string formatMacFromAddress(uint64_t bthAddress) {
    constexpr char hexDigits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(17);
    for (int i = 5; i >= 0; --i) {
        const uint8_t byte = static_cast<uint8_t>((bthAddress >> (8 * i)) & 0xFF);
        out.push_back(hexDigits[(byte >> 4) & 0x0F]);
        out.push_back(hexDigits[byte & 0x0F]);
        if (i != 0) out.push_back(':');
    }
    return out;
}

uint64_t addressFromMacBytes(const std::array<uint8_t, 6>& macBytes) {
    uint64_t address = 0;
    for (uint8_t byte : macBytes) {
        address = (address << 8) | static_cast<uint64_t>(byte);
    }
    return address;
}

// 把用户/系统给出的各种写法归一化为 "XX:XX:XX:XX:XX:XX"；非法输入返回空串。
// 支持 "ac-80-0a-12-34-56"、"AABBCCDDEEFF"、"ac:80:0a:12:34:56" 三种常见写法。
std::string normalizeBluetoothMacText(const std::string& raw) {
    std::string digits;
    digits.reserve(12);
    for (char ch : raw) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isxdigit(uch)) {
            digits.push_back(static_cast<char>(std::toupper(uch)));
        } else if (ch == ':' || ch == '-' || ch == '.' || ch == ' ') {
            continue; // 允许分隔符；其它字符直接判非法
        } else {
            return std::string();
        }
    }
    if (digits.size() != 12) return std::string();
    std::string out;
    out.reserve(17);
    for (size_t i = 0; i < 12; i += 2) {
        if (i != 0) out.push_back(':');
        out.push_back(digits[i]);
        out.push_back(digits[i + 1]);
    }
    return out;
}

// 蓝牙 Class of Device：bit8..12 为 major device class，0x04 = 音频/视频类。
bool classOfDeviceIsAudio(unsigned long classOfDevice) {
    return ((classOfDevice >> 8) & 0x1F) == 0x04;
}

bool nameLooksLikeAudioDevice(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char ch : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    static const char* const kKeywords[] = {
        "sony",  "wh-",     "wf-",    "ult",     "1000x",  "headphone",
        "headset", "earbud", "buds",  "speaker", "airpods", "audio"
    };
    for (const char* keyword : kKeywords) {
        if (lower.find(keyword) != std::string::npos) return true;
    }
    return false;
}

// 依据机型名称推测 RFCOMM 通道。来源：本仓库机型配置表 + 公开逆向资料，
// 不是 SDP 协商结果；界面必须提示用户可手动修改。
int suggestRfcommChannelForDeviceName(const std::string& name) {
    std::string lower;
    lower.reserve(name.size());
    for (char ch : name) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (lower.find("ult") != std::string::npos) return 18;               // ULT WEAR 配置表默认通道
    if (lower.find("wf-1000") != std::string::npos || lower.find("wf1000") != std::string::npos) return 2;
    if (lower.find("1000x") != std::string::npos) return 1;              // WH-1000XM4 / XM5 默认通道
    return 8;                                                            // Sony MDR 系列 SPP 常见通道
}

const char* deviceScanStatusLabel(DeviceScanStatus status) {
    switch (status) {
        case DeviceScanStatus::OK: return "扫描成功";
        case DeviceScanStatus::NO_ADAPTER: return "未检测到蓝牙适配器";
        case DeviceScanStatus::NO_DEVICES: return "没有已知蓝牙设备";
        case DeviceScanStatus::ACCESS_DENIED: return "蓝牙访问被拒绝";
        case DeviceScanStatus::UNSUPPORTED: return "当前平台不支持原生枚举";
        case DeviceScanStatus::SIMULATED: return "模拟设备列表";
    }
    return "未知状态";
}

class IDeviceDiscovery {
public:
    virtual ~IDeviceDiscovery() = default;
    // 列出本机已配对/已知的蓝牙设备；失败必须给出可读原因。
    virtual DeviceScanResult scan() = 0;
    // 尝试配对（可能弹出系统配对对话框）。只有在系统确认后返回 true。
    virtual bool pairDevice(const BluetoothAudioDevice& device, void* ownerWindow, std::string& outcome) = 0;
    virtual bool isSimulated() const = 0;
};

// ---------- 模拟设备发现：固定的假设备列表，保证 --sim 与传统回归可测 ----------
class SimulatedDeviceDiscovery : public IDeviceDiscovery {
public:
    bool isSimulated() const override { return true; }

    DeviceScanResult scan() override {
        DeviceScanResult result;
        result.status = DeviceScanStatus::SIMULATED;
        result.radioCount = 1;
        result.message =
            "模拟扫描：返回 4 个固定的假蓝牙音频设备（仅用于界面演示与回归验证，未访问真实蓝牙硬件）。"
            "RFCOMM 通道是按机型名称推测的建议值，可在设备页手动修改。";
        result.devices = {
            makeSimulatedDevice("Sony ULT WEAR (WH-ULT900N)", "AC:80:0A:12:34:56", true, true),
            makeSimulatedDevice("Sony WH-1000XM5", "AC:80:0A:65:43:21", false, true),
            makeSimulatedDevice("Sony WF-1000XM5", "AC:80:0A:AA:BB:CC", false, true),
            makeSimulatedDevice("未知蓝牙音频设备", "AC:80:0A:DE:AD:BE", false, false)
        };
        return result;
    }

    // 模拟配对：只验证"调用链与文案"，不产生任何真实配对效果，因此结果必须带（模拟）标记。
    bool pairDevice(const BluetoothAudioDevice& device, void* ownerWindow, std::string& outcome) override {
        (void)ownerWindow;
        std::array<uint8_t, 6> macBytes{};
        if (!validateBluetoothMacAddress(device.macAddress, macBytes)) {
            outcome = "配对失败：设备 MAC 地址非法（" + device.macAddress + "），无法发起配对。";
            return false;
        }
        if (!device.simulated) {
            outcome = "配对失败：模拟发现器只能处理假列表中的设备，真实设备请使用原生扫描。";
            return false;
        }
        outcome = device.paired
            ? "（模拟）该假设备已标记为已配对，跳过配对对话框直接继续连接流程。"
            : "（模拟）已模拟配对成功；真实配对需要 --real 模式下调用系统配对对话框。";
        return true;
    }

private:
    static BluetoothAudioDevice makeSimulatedDevice(const std::string& name, const std::string& mac,
                                                    bool connected, bool paired) {
        BluetoothAudioDevice device;
        device.name = name;
        device.macAddress = mac;
        device.connected = connected;
        device.paired = paired;
        device.remembered = true;
        device.audioCandidate = true;
        device.suggestedRfcommChannel = suggestRfcommChannelForDeviceName(name);
        device.simulated = true;
        return device;
    }
};

// ---------- 原生设备发现（Windows / bthprops） ----------
#if defined(_WIN32)
namespace btdiscovery_detail {

inline std::string utf8FromWide(const wchar_t* text) {
    if (text == nullptr || text[0] == L'\0') return std::string();
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], needed, nullptr, nullptr);
    out.resize(static_cast<size_t>(needed - 1));
    return out;
}

inline std::string hexCode(unsigned long value, int width) {
    std::ostringstream oss;
    oss << std::hex << std::uppercase << std::setw(width) << std::setfill('0') << value;
    return oss.str();
}

// 三种失败分支的确定性文案（无适配器 / 无权限 归入适配器级失败；无设备由调用点给出）
inline void fillRadioFailure(DeviceScanResult& result, unsigned long err) {
    result.systemError = err;
    const std::string code = "（系统错误码 " + std::to_string(err) + "）";
    if (err == ERROR_ACCESS_DENIED || err == ERROR_PRIVILEGE_NOT_HELD ||
        err == ERROR_SERVICE_NOT_ACTIVE || err == ERROR_SERVICE_DISABLED) {
        result.status = DeviceScanStatus::ACCESS_DENIED;
        result.message =
            "无权访问蓝牙设备列表" + code +
            "：蓝牙支持服务可能已停止，或当前用户/组策略禁止访问蓝牙。"
            "请检查「服务 → 蓝牙支持服务 (bthserv)」是否正在运行，然后重新扫描。";
        return;
    }
    if (err == ERROR_NO_MORE_ITEMS || err == ERROR_NOT_FOUND || err == ERROR_DEVICE_NOT_CONNECTED) {
        result.status = DeviceScanStatus::NO_ADAPTER;
        result.message =
            "未检测到蓝牙适配器" + code +
            "：本机没有可用的蓝牙硬件，或蓝牙已关闭/驱动未安装。"
            "请先在「设置 → 蓝牙和其他设备」中启用蓝牙，然后重新扫描。";
        return;
    }
    result.status = DeviceScanStatus::NO_ADAPTER;
    result.message =
        "蓝牙适配器枚举失败" + code +
        "：蓝牙可能被硬件开关关闭，或驱动异常。请在系统设置中确认蓝牙可用后重试；"
        "也可以绕过扫描，直接手动填写 MAC 与 RFCOMM 通道。";
}

inline std::string describePairResult(HRESULT hr) {
    switch (static_cast<unsigned long>(hr)) {
        case static_cast<unsigned long>(E_ACCESSDENIED):
            return "系统拒绝了配对请求：当前账户权限不足，或蓝牙配对被组策略/安全软件阻止。";
        case 0x800704C7UL: // HRESULT_FROM_WIN32(ERROR_CANCELLED)
            return "配对已被取消：系统配对提示被关闭或在提示中选择了取消。";
        case 0x80070483UL: // HRESULT_FROM_WIN32(ERROR_SERVICE_NOT_ACTIVE)
            return "蓝牙支持服务未运行，无法发起配对。请启动 bthserv 服务后重试。";
        case 0x80070422UL: // HRESULT_FROM_WIN32(ERROR_SERVICE_DISABLED)
            return "蓝牙支持服务已被禁用，无法发起配对。请在服务管理器中启用 bthserv。";
        case 0x8007001FUL: // HRESULT_FROM_WIN32(ERROR_GEN_FAILURE)
            return "系统报告通用失败：设备可能不在范围内、已被其它主机占用，或固件拒绝配对。";
        default:
            break;
    }
    return "系统返回配对失败，HRESULT=0x" + hexCode(static_cast<unsigned long>(hr), 8) +
           "。请确认耳机处于配对模式且在本机附近，或改用「设置 → 蓝牙和其他设备」手动配对。";
}

} // namespace btdiscovery_detail

class WindowsBluetoothDiscovery : public IDeviceDiscovery {
public:
    bool isSimulated() const override { return false; }

    DeviceScanResult scan() override {
        DeviceScanResult result;
        std::vector<HANDLE> radios;

        BLUETOOTH_FIND_RADIO_PARAMS radioParams{};
        radioParams.dwSize = sizeof(radioParams);
        HANDLE firstRadio = nullptr;
        HBLUETOOTH_RADIO_FIND radioFind = ::BluetoothFindFirstRadio(&radioParams, &firstRadio);
        if (radioFind == nullptr) {
            btdiscovery_detail::fillRadioFailure(result, static_cast<unsigned long>(::GetLastError()));
            return result;
        }
        HANDLE currentRadio = firstRadio;
        for (;;) {
            radios.push_back(currentRadio);
            HANDLE nextRadio = nullptr;
            if (::BluetoothFindNextRadio(radioFind, &nextRadio) == FALSE) break;
            currentRadio = nextRadio;
        }
        ::BluetoothFindRadioClose(radioFind);
        result.radioCount = static_cast<int>(radios.size());

        BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
        search.dwSize = sizeof(search);
        search.fReturnAuthenticated = TRUE;
        search.fReturnRemembered = TRUE;
        search.fReturnUnknown = FALSE; // 不做 inquiry：避免数十秒扫描与"必须可发现"的前置条件
        search.fReturnConnected = TRUE;
        search.fIssueInquiry = FALSE;
        search.cTimeoutMultiplier = 2;
        search.hRadio = radios.front();

        BLUETOOTH_DEVICE_INFO info{};
        info.dwSize = sizeof(info);
        HBLUETOOTH_DEVICE_FIND deviceFind = ::BluetoothFindFirstDevice(&search, &info);
        if (deviceFind == nullptr) {
            const unsigned long err = static_cast<unsigned long>(::GetLastError());
            closeRadios(radios);
            if (err == ERROR_NO_MORE_ITEMS || err == ERROR_NOT_FOUND) {
                result.status = DeviceScanStatus::NO_DEVICES;
                result.systemError = err;
                result.message =
                    "蓝牙适配器可用，但没有找到任何已配对或已知的蓝牙设备。"
                    "请先在 Windows「设置 → 蓝牙和其他设备」中完成耳机配对，然后重新扫描。";
            } else {
                btdiscovery_detail::fillRadioFailure(result, err);
            }
            return result;
        }

        do {
            result.devices.push_back(convert(info));
        } while (::BluetoothFindNextDevice(deviceFind, &info) != FALSE);
        ::BluetoothFindDeviceClose(deviceFind);
        closeRadios(radios);

        if (result.devices.empty()) {
            result.status = DeviceScanStatus::NO_DEVICES;
            result.message =
                "蓝牙适配器可用，但没有找到任何已配对或已知的蓝牙设备。"
                "请先在 Windows「设置 → 蓝牙和其他设备」中完成耳机配对，然后重新扫描。";
            return result;
        }

        // 排序：音频类优先 → 已连接优先 → 名称字典序（稳定结构，便于界面定位与截图取证）
        std::stable_sort(result.devices.begin(), result.devices.end(),
                         [](const BluetoothAudioDevice& a, const BluetoothAudioDevice& b) {
                             if (a.audioCandidate != b.audioCandidate) return a.audioCandidate;
                             if (a.connected != b.connected) return a.connected;
                             return a.name < b.name;
                         });

        size_t audioCount = 0;
        for (const auto& device : result.devices) {
            if (device.audioCandidate) ++audioCount;
        }
        result.status = DeviceScanStatus::OK;
        std::ostringstream oss;
        oss << "扫描完成：适配器 " << result.radioCount << " 个，已知设备 " << result.devices.size()
            << " 个，其中判定为音频类 " << audioCount << " 个。";
        if (audioCount == 0) {
            oss << " 未从 Class of Device 或名称识别到音频设备，仍可手动填写 MAC 与通道。";
        }
        oss << " RFCOMM 通道为按机型名称推测的建议值，可在设备页上方手改。";
        result.message = oss.str();
        return result;
    }

    // 发起配对：先查当前认证状态，再调用系统配对对话框，最后复查 fAuthenticated。
    bool pairDevice(const BluetoothAudioDevice& device, void* ownerWindow, std::string& outcome) override {
        outcome.clear();
        std::array<uint8_t, 6> macBytes{};
        if (!validateBluetoothMacAddress(device.macAddress, macBytes)) {
            outcome = "配对失败：设备 MAC 地址非法（" + device.macAddress + "），无法发起配对。";
            return false;
        }

        BLUETOOTH_FIND_RADIO_PARAMS radioParams{};
        radioParams.dwSize = sizeof(radioParams);
        HANDLE radio = nullptr;
        HBLUETOOTH_RADIO_FIND radioFind = ::BluetoothFindFirstRadio(&radioParams, &radio);
        if (radioFind == nullptr) {
            DeviceScanResult failure;
            btdiscovery_detail::fillRadioFailure(failure, static_cast<unsigned long>(::GetLastError()));
            outcome = "配对失败：" + failure.message;
            return false;
        }
        ::BluetoothFindRadioClose(radioFind);

        BLUETOOTH_DEVICE_INFO info{};
        info.dwSize = sizeof(info);
        info.Address.ullLong = addressFromMacBytes(macBytes);
        if (::BluetoothGetDeviceInfo(radio, &info) != S_OK) {
            ::CloseHandle(radio);
            outcome = "配对失败：系统查不到该设备的记录（" + device.macAddress +
                      "）。请先让耳机进入配对模式，或改用「设置 → 蓝牙和其他设备」手动配对。";
            return false;
        }
        if (info.fAuthenticated != FALSE) {
            ::CloseHandle(radio);
            outcome = "该设备在系统中已完成配对，无需重复配对，可以直接连接。";
            return true;
        }

        const HRESULT hr = ::BluetoothAuthenticateDeviceEx(
            static_cast<HWND>(ownerWindow), radio, &info, nullptr, MITMProtectionNotRequired);
        if (hr != S_OK) {
            ::CloseHandle(radio);
            outcome = "配对失败：" + btdiscovery_detail::describePairResult(hr);
            return false;
        }

        // 闭环复查：只有系统把 fAuthenticated 置位才算配对成功，绝不凭返回值假装成功。
        BLUETOOTH_DEVICE_INFO verify{};
        verify.dwSize = sizeof(verify);
        verify.Address.ullLong = info.Address.ullLong;
        const HRESULT verifyResult = ::BluetoothGetDeviceInfo(radio, &verify);
        ::CloseHandle(radio);
        if (verifyResult == S_OK && verify.fAuthenticated != FALSE) {
            outcome = "配对成功：系统已确认设备完成认证（fAuthenticated=1）。";
            return true;
        }
        outcome = "配对未确认：系统接受了配对请求，但复查显示设备仍未通过认证（fAuthenticated=0）。"
                  "配对可能仍在进行中，请在列表上重新扫描确认，或改用系统设置手动配对。";
        return false;
    }

private:
    static void closeRadios(std::vector<HANDLE>& radios) {
        for (HANDLE handle : radios) {
            if (handle != nullptr) ::CloseHandle(handle);
        }
        radios.clear();
    }

    static BluetoothAudioDevice convert(const BLUETOOTH_DEVICE_INFO& info) {
        BluetoothAudioDevice device;
        device.name = btdiscovery_detail::utf8FromWide(info.szName);
        if (device.name.empty()) device.name = "(未命名蓝牙设备)";
        device.macAddress = formatMacFromAddress(static_cast<uint64_t>(info.Address.ullLong));
        device.connected = info.fConnected != FALSE;
        device.paired = info.fAuthenticated != FALSE;
        device.remembered = info.fRemembered != FALSE;
        device.audioCandidate =
            classOfDeviceIsAudio(static_cast<unsigned long>(info.ulClassofDevice)) ||
            nameLooksLikeAudioDevice(device.name);
        device.suggestedRfcommChannel = suggestRfcommChannelForDeviceName(device.name);
        device.simulated = false;
        return device;
    }
};
#else
// 非 Windows 平台：保留明确失败而不是空实现，避免"看起来扫描成功但其实没扫"
class WindowsBluetoothDiscovery : public IDeviceDiscovery {
public:
    bool isSimulated() const override { return false; }

    DeviceScanResult scan() override {
        DeviceScanResult result;
        result.status = DeviceScanStatus::UNSUPPORTED;
        result.message = "当前平台不支持通过 bthprops 枚举蓝牙设备；请手动填写 MAC 地址与 RFCOMM 通道。";
        return result;
    }

    bool pairDevice(const BluetoothAudioDevice& device, void* ownerWindow, std::string& outcome) override {
        (void)device;
        (void)ownerWindow;
        outcome = "配对失败：当前平台没有实现蓝牙配对；请使用系统蓝牙设置完成配对。";
        return false;
    }
};
#endif

std::unique_ptr<IDeviceDiscovery> makeDeviceDiscovery(bool useNative) {
    if (useNative) {
        return std::unique_ptr<IDeviceDiscovery>(new WindowsBluetoothDiscovery());
    }
    return std::unique_ptr<IDeviceDiscovery>(new SimulatedDeviceDiscovery());
}

void printDeviceScanResult(const DeviceScanResult& result, std::ostream& os) {
    os << "\n============== [ 蓝牙设备扫描结果 ] ==============\n"
       << " 状态     : " << deviceScanStatusLabel(result.status)
       << " (" << static_cast<int>(result.status) << ")"
       << (result.systemError != 0 ? " | 系统错误码 " + std::to_string(result.systemError) : "") << "\n"
       << " 适配器数 : " << result.radioCount << " | 设备数: " << result.devices.size() << "\n"
       << " 说明     : " << result.message << "\n";
    if (!result.devices.empty()) {
        os << " -------------------------------------------------------------------------------\n"
           << "  #  名称                              MAC                 通道  连接  配对  音频\n"
           << " -------------------------------------------------------------------------------\n";
        for (size_t i = 0; i < result.devices.size(); ++i) {
            const auto& device = result.devices[i];
            os << "  " << i << "  " << device.name << std::string(device.name.size() < 32 ? 32 - device.name.size() : 1, ' ')
               << device.macAddress << "  " << std::setw(4) << device.suggestedRfcommChannel << "  "
               << (device.connected ? " 是 " : " 否 ") << "  " << (device.paired ? " 是 " : " 否 ")
               << "  " << (device.audioCandidate ? " 是" : " 否") << "\n";
        }
        os << " -------------------------------------------------------------------------------\n";
    }
    os << "================================================\n";
}

// ============================================================================
// 第五部分：Windows 与 Linux 原生蓝牙 RFCOMM 连接器（带超时、循环发送与安全重连）
// ============================================================================

#if defined(ENABLE_NATIVE_BT) && defined(_WIN32)
class WindowsBthConnector : public IBluetoothConnector {
public:
    WindowsBthConnector() {
        WSADATA wsaData{};
        wsaInitialized_ = (::WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
        if (!wsaInitialized_) {
            lastError_ = "WSAStartup 初始化失败";
        }
    }

    ~WindowsBthConnector() override {
        disconnect();
        if (wsaInitialized_) {
            ::WSACleanup();
        }
    }

    bool isSimulation() const override { return false; }

    bool connect(const std::string& macAddress, int rfcommChannel) override {
        disconnect(); // 防止重连时句柄泄漏
        if (!wsaInitialized_) {
            lastError_ = "Winsock 未初始化";
            return false;
        }

        std::array<uint8_t, 6> macBytes{};
        if (!validateBluetoothMacAddress(macAddress, macBytes)) {
            lastError_ = "非法蓝牙 MAC 地址: " + macAddress;
            return false;
        }
        if (rfcommChannel <= 0 || rfcommChannel > 30) {
            lastError_ = "非法 RFCOMM 通道: " + std::to_string(rfcommChannel);
            return false;
        }

        BTH_ADDR btAddr = 0;
        for (uint8_t b : macBytes) {
            btAddr = (btAddr << 8) | static_cast<BTH_ADDR>(b);
        }

        sock_ = ::socket(AF_BTH, SOCK_STREAM, BTHPROTO_RFCOMM);
        if (sock_ == INVALID_SOCKET) {
            lastError_ = "创建 AF_BTH Socket 失败，错误码: " + std::to_string(::WSAGetLastError());
            return false;
        }

        SOCKADDR_BTH addr{};
        addr.addressFamily = AF_BTH;
        addr.btAddr = btAddr;
        addr.port = static_cast<ULONG>(rfcommChannel);

        // Non-blocking connect + select timeout: the UI must never wait indefinitely
        // for a radio/device that is off, out of range, or advertising another service.
        u_long nonBlocking = 1;
        if (::ioctlsocket(sock_, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
            lastError_ = "无法启用蓝牙连接超时控制，错误码: " + std::to_string(::WSAGetLastError());
            disconnect();
            return false;
        }

        int connectResult = ::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (connectResult == SOCKET_ERROR) {
            const int connectError = ::WSAGetLastError();
            if (connectError != WSAEWOULDBLOCK && connectError != WSAEINPROGRESS && connectError != WSAEALREADY) {
                lastError_ = "连接蓝牙设备失败，错误码: " + std::to_string(connectError);
                disconnect();
                return false;
            }

            fd_set writeSet, errorSet;
            FD_ZERO(&writeSet); FD_ZERO(&errorSet);
            FD_SET(sock_, &writeSet); FD_SET(sock_, &errorSet);
            timeval timeout{}; timeout.tv_sec = 8; timeout.tv_usec = 0;
            const int ready = ::select(0, nullptr, &writeSet, &errorSet, &timeout);
            if (ready == 0) {
                lastError_ = "连接蓝牙设备超时（8 秒）。请确认耳机已开机、已配对且 RFCOMM 通道正确。";
                disconnect();
                return false;
            }
            if (ready == SOCKET_ERROR) {
                lastError_ = "等待蓝牙连接完成失败，错误码: " + std::to_string(::WSAGetLastError());
                disconnect();
                return false;
            }

            int socketError = 0;
            int errorSize = sizeof(socketError);
            if (::getsockopt(sock_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &errorSize) == SOCKET_ERROR || socketError != 0) {
                if (socketError == 0) socketError = ::WSAGetLastError();
                lastError_ = "蓝牙 RFCOMM 连接未建立，错误码: " + std::to_string(socketError);
                disconnect();
                return false;
            }
        }

        u_long blocking = 0;
        if (::ioctlsocket(sock_, FIONBIO, &blocking) == SOCKET_ERROR) {
            lastError_ = "无法恢复蓝牙 Socket 阻塞模式，错误码: " + std::to_string(::WSAGetLastError());
            disconnect();
            return false;
        }
        // Some Sony RFCOMM services become writable shortly after connect() returns.
        // The caller runs this connector on a worker thread, so this settle delay cannot freeze the UI.
        ::Sleep(350);
        return true;
    }

    void disconnect() override {
        if (sock_ != INVALID_SOCKET) {
            ::closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
    }

    bool isConnected() const override { return sock_ != INVALID_SOCKET; }

    bool sendAllBytes(const std::vector<uint8_t>& rawFrame) override {
        if (sock_ == INVALID_SOCKET) {
            lastError_ = "Socket 未连接";
            return false;
        }
        size_t totalSent = 0;
        while (totalSent < rawFrame.size()) {
            int sent = ::send(sock_,
                              reinterpret_cast<const char*>(rawFrame.data() + totalSent),
                              static_cast<int>(rawFrame.size() - totalSent), 0);
            if (sent <= 0) {
                lastError_ = "send() 失败，错误码: " + std::to_string(::WSAGetLastError());
                disconnect();
                return false;
            }
            totalSent += static_cast<size_t>(sent);
        }
        return true;
    }

    RecvResult receiveRawChunk(int timeoutMs) override {
        if (sock_ == INVALID_SOCKET) {
            return {RecvStatus::DISCONNECTED, {}, "Socket 未连接"};
        }
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(sock_, &readSet);

        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int sel = ::select(0, &readSet, nullptr, nullptr, &tv);
        if (sel == 0) {
            return {RecvStatus::TIMEOUT, {}, ""};
        }
        if (sel < 0) {
            lastError_ = "select() 异常";
            disconnect();
            return {RecvStatus::ERROR_STATE, {}, lastError_};
        }

        std::vector<uint8_t> buf(512);
        int n = ::recv(sock_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0);
        if (n == 0) {
            disconnect();
            return {RecvStatus::DISCONNECTED, {}, "远端蓝牙设备主动关闭了连接"};
        }
        if (n < 0) {
            lastError_ = "recv() 错误码: " + std::to_string(::WSAGetLastError());
            disconnect();
            return {RecvStatus::ERROR_STATE, {}, lastError_};
        }
        buf.resize(static_cast<size_t>(n));
        return {RecvStatus::OK, std::move(buf), ""};
    }

    std::string getLastError() const override { return lastError_; }

private:
    bool wsaInitialized_ = false;
    SOCKET sock_ = INVALID_SOCKET;
    std::string lastError_;
};

#elif defined(ENABLE_NATIVE_BT) && defined(__linux__)

class LinuxBlueZConnector : public IBluetoothConnector {
public:
    ~LinuxBlueZConnector() override { disconnect(); }

    bool isSimulation() const override { return false; }

    bool connect(const std::string& macAddress, int rfcommChannel) override {
        disconnect(); // 确保关闭旧连接，杜绝句柄泄漏

        std::array<uint8_t, 6> macBytes{};
        if (!validateBluetoothMacAddress(macAddress, macBytes)) {
            lastError_ = "非法蓝牙 MAC 地址: " + macAddress;
            return false;
        }
        if (rfcommChannel <= 0 || rfcommChannel > 30) {
            lastError_ = "非法 RFCOMM 通道: " + std::to_string(rfcommChannel);
            return false;
        }

        sock_ = ::socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
        if (sock_ < 0) {
            lastError_ = "创建 BlueZ RFCOMM Socket 失败 (errno=" + std::to_string(errno) + ")";
            return false;
        }

        sockaddr_rc_compat addr{};
        addr.rc_family = AF_BLUETOOTH;
        addr.rc_channel = static_cast<uint8_t>(rfcommChannel);
        // BlueZ bdaddr 采用小端字节序存储
        for (int i = 0; i < 6; ++i) {
            addr.rc_bdaddr.b[i] = macBytes[5 - i];
        }

        if (::connect(sock_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            lastError_ = "连接蓝牙设备失败 (errno=" + std::to_string(errno) + ")";
            disconnect();
            return false;
        }
        return true;
    }

    void disconnect() override {
        if (sock_ >= 0) {
            ::close(sock_);
            sock_ = -1;
        }
    }

    bool isConnected() const override { return sock_ >= 0; }

    bool sendAllBytes(const std::vector<uint8_t>& rawFrame) override {
        if (sock_ < 0) {
            lastError_ = "Socket 未连接";
            return false;
        }
        size_t totalSent = 0;
        while (totalSent < rawFrame.size()) {
#ifdef MSG_NOSIGNAL
            ssize_t sent = ::send(sock_, rawFrame.data() + totalSent,
                                  rawFrame.size() - totalSent, MSG_NOSIGNAL);
#else
            ssize_t sent = ::send(sock_, rawFrame.data() + totalSent,
                                  rawFrame.size() - totalSent, 0);
#endif
            if (sent < 0) {
                if (errno == EINTR) continue;
                lastError_ = "send() 失败 (errno=" + std::to_string(errno) + ")";
                disconnect();
                return false;
            }
            if (sent == 0) {
                lastError_ = "send() 返回 0，连接已关闭";
                disconnect();
                return false;
            }
            totalSent += static_cast<size_t>(sent);
        }
        return true;
    }

    RecvResult receiveRawChunk(int timeoutMs) override {
        if (sock_ < 0) {
            return {RecvStatus::DISCONNECTED, {}, "Socket 未连接"};
        }

        pollfd pfd{};
        pfd.fd = sock_;
        pfd.events = POLLIN;

        int ret = ::poll(&pfd, 1, timeoutMs);
        if (ret == 0) {
            return {RecvStatus::TIMEOUT, {}, ""};
        }
        if (ret < 0) {
            if (errno == EINTR) return {RecvStatus::TIMEOUT, {}, ""};
            lastError_ = "poll() 失败 (errno=" + std::to_string(errno) + ")";
            disconnect();
            return {RecvStatus::ERROR_STATE, {}, lastError_};
        }

        // 若对端在关闭前仍发送了最后一批数据，优先读取可读数据，避免丢掉 ACK。
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) &&
            !(pfd.revents & POLLIN)) {
            disconnect();
            return {RecvStatus::DISCONNECTED, {}, "蓝牙对端挂断或发生通道错误"};
        }

        std::vector<uint8_t> buf(512);
        ssize_t n = ::recv(sock_, buf.data(), buf.size(), 0);
        if (n == 0) {
            disconnect();
            return {RecvStatus::DISCONNECTED, {}, "蓝牙设备关闭了 RFCOMM 连接"};
        }
        if (n < 0) {
            lastError_ = "recv() 错误 (errno=" + std::to_string(errno) + ")";
            disconnect();
            return {RecvStatus::ERROR_STATE, {}, lastError_};
        }
        buf.resize(static_cast<size_t>(n));
        return {RecvStatus::OK, std::move(buf), ""};
    }

    std::string getLastError() const override { return lastError_; }

private:
    int sock_ = -1;
    std::string lastError_;
};
#endif

// ============================================================================
// 第六部分：单设备协议状态机与控制器 (HeadphoneController)
// ============================================================================

class HeadphoneController {
public:
    HeadphoneController(DeviceModelProfile profile, std::unique_ptr<IBluetoothConnector> connector)
        : profile_(std::move(profile)), connector_(std::move(connector)) {
        if (!connector_) {
            throw std::invalid_argument("HeadphoneController: connector 指针不能为空");
        }
    }

    bool connectDevice(const std::string& macAddress, int rfcommChannel = -1) {
        int channel = (rfcommChannel > 0) ? rfcommChannel : profile_.defaultRfcommChannel;
        macAddress_ = macAddress;
        rfcommChannel_ = channel;
        rxParser_.reset();
        hostTxSeq_ = 0;
        deviceRxExpectedSeq_ = 0;
        lastDeviceRxSeq_.reset();
        negotiatedProtocolMajor_ = 0;
        negotiatedProtocolText_ = "未协商";

        if (!connector_->connect(macAddress_, rfcommChannel_)) {
            lastError_ = connector_->getLastError();
            return false;
        }

        // 1. 发送协议握手指令并校验真实回复
        if (!executeCommandTransaction(MdrProtocol::buildHandshakePayload(),
                                       Command::CONNECT_RET_PROTOCOL_INFO)) {
            lastError_ = "协议握手失败: " + lastError_;
            connector_->disconnect();
            return false;
        }

        // 不仅显示握手版本；还必须确认它与用户选择的机型配置一致。
        const auto expectedMajor = profile_.handshakeVersion != 0
            ? profile_.handshakeVersion : static_cast<uint8_t>(profile_.generation);
        if (negotiatedProtocolMajor_ != expectedMajor) {
            lastError_ = "握手协议版本与所选机型不匹配：配置期望 V" +
                         std::to_string(expectedMajor) + "，设备报告 V" +
                         std::to_string(negotiatedProtocolMajor_);
            connector_->disconnect();
            return false;
        }

        // 2. 同步初始电量与各项硬件状态（写后回读基准）
        if (!refreshAllDeviceStates()) {
            lastError_ = "同步初始设备状态失败: " + lastError_;
            connector_->disconnect();
            return false;
        }
        return true;
    }

    void disconnectDevice() {
        connector_->disconnect();
    }

    bool isConnected() const {
        return connector_->isConnected();
    }

    bool isSimulationMode() const {
        return connector_->isSimulation();
    }

    // 所有 SET 操作：发送 SET 等待 ACK -> 主动发送 GET 回读真实设备状态
    bool setAmbientSound(const AmbientSoundConfig& config) {
        if (!executeCommandTransaction(MdrProtocol::buildNcAsmSetPayload(profile_, config), std::nullopt)) {
            return false;
        }
        return executeCommandTransaction(MdrProtocol::buildNcAsmGetPayload(profile_), Command::NCASM_RET_PARAM);
    }

    // EQ 维度写入。ULT WEAR 上 EQ 与 ULT 是正交两维且共用同一条 11 字节帧，
    // 所以这里必须把「当前 ULT 模式」一起写下去；否则写 EQ 会把 ULT 模式重置为 0（互斥的根因）。
    // 实机对 SET 也回 0x57，所以写入事务本身就等到回包（不再另起一条 GET）；
    // SET 拿不到回包时才退回一次 0x56 GET（见 writeEqUltFrame）。
    bool setEqualizer(const EqualizerConfig& config) {
        return writeEqUltFrame(config, ultBassMode_);
    }

    bool setSpeakToChat(const SpeakToChatConfig& config) {
        if (!profile_.supportsSpeakToChat) { lastError_ = "该机型配置未启用 Speak-to-Chat 控制。"; return false; }
        if (!executeCommandTransaction(MdrProtocol::buildSpeakToChatSetPayload(config), std::nullopt)) {
            return false;
        }
        return executeCommandTransaction(MdrProtocol::buildSpeakToChatGetPayload(), Command::SMART_TALKING_RET_PARAM);
    }

    bool setDseeExtreme(bool enabled) {
        if (!profile_.supportsDseeControl) { lastError_ = "该机型的 DSEE 控制命令尚未核实，已禁用。"; return false; }
        if (!executeCommandTransaction(MdrProtocol::buildDseeSetPayload(profile_, enabled), std::nullopt)) {
            return false;
        }
        return executeCommandTransaction(MdrProtocol::buildDseeGetPayload(profile_), Command::UPSCALING_RET_PARAM);
    }

    bool setBluetoothConnectionQuality(BluetoothConnectionQuality quality) {
        if (!profile_.supportsCodecQualityControl) { lastError_ = "该机型的音质优先控制命令尚未核实，已禁用。"; return false; }
        if (!executeCommandTransaction(MdrProtocol::buildCodecSetPayload(quality), std::nullopt)) {
            return false;
        }
        return executeCommandTransaction(MdrProtocol::buildCodecGetPayload(), Command::AUDIO_CODEC_RET_PARAM);
    }

    bool setWearDetection(bool pauseWhenTakenOff) {
        if (!profile_.supportsWearDetectionControl) { lastError_ = "该机型的佩戴感应控制命令尚未核实，已禁用。"; return false; }
        if (!executeCommandTransaction(MdrProtocol::buildWearDetectionSetPayload(pauseWhenTakenOff), std::nullopt)) {
            return false;
        }
        return executeCommandTransaction(MdrProtocol::buildWearDetectionGetPayload(), Command::SYSTEM_WEAR_RET_PARAM);
    }

    bool setUltBassMode(uint8_t mode) {
        if (!profile_.isUltWear) {
            lastError_ = "ULT Bass 模式仅针对 ULT WEAR 配置开放。";
            return false;
        }
        if (mode > 2) {
            lastError_ = "ULT Bass 模式值必须为 0、1 或 2。";
            return false;
        }
        // ULT 维度写入同样把当前 EQ 维度（预设 / Clear Bass / 五段）一起带下去；
        // 写入后回读闭环与 setEqualizer 完全相同（SET 等 0x57，失败退回一次 GET）。
        return writeEqUltFrame(eqConfig_, mode);
    }

    // ------------------------------------------------------------------
    // 功能 B：均衡器状态同步（主动向设备回读当前 EQ）
    //   ULT WEAR（eqInquiredType 0x03）→ 0x56 0x03 → 0x57 0x03 <preset> <ult_mode> 0x06 <cb+10> <b0+10>…<b4+10>
    //   （两维都进同一条 0x03 帧：ULT 模式在 [3]，EQ 预设/增益在 [2]/[5..10]；旧实现在 [2] 与 [5..10] 写死常量）
    //   非 ULT（eqInquiredType 0x00/0x01）→ 0x56 <type> → 0x57 <type> <preset> 0x06 <cb+10> <b0+10>…<b4+10>
    //   状态只由设备回包更新（parseDeviceStateFrame），因此能反映"用户在耳机上手动改过 EQ"的情况。
    // ------------------------------------------------------------------
    bool syncEqualizerState() {
        lastError_.clear();
        if (!isConnected()) {
            lastError_ = "设备未连接，无法同步均衡器状态。";
            return false;
        }
        return executeCommandTransaction(MdrProtocol::buildEqualizerGetPayload(profile_),
                                         Command::EQEBB_RET_PARAM);
    }

    // 非阻塞处理设备主动推送的 NOTIFY 帧（0x59 EQ / 0x69 NCASM 等）：先做一次 select 超时为 0 的
    // 传输层读取，把已到达字节喂给拆帧器，然后只解析完整帧，不发送任何指令。
    // 返回被解析并应用的状态帧数量。
    int pollDeviceNotifications() {
        if (!isConnected()) return 0;
        const RecvResult recvRes = connector_->receiveRawChunk(0);
        if (recvRes.status == RecvStatus::DISCONNECTED) {
            lastError_ = "读取设备主动通知时连接断开: " + recvRes.errorMessage;
            return 0;
        }
        if (recvRes.status == RecvStatus::ERROR_STATE) {
            lastError_ = "读取设备主动通知失败: " + recvRes.errorMessage;
            return 0;
        }
        if (recvRes.status == RecvStatus::OK && !recvRes.bytes.empty()) {
            rxParser_.feedBytes(recvRes.bytes.data(), recvRes.bytes.size());
        }

        int applied = 0;
        const auto frames = rxParser_.extractFrames();
        for (const auto& decoded : frames) {
            if (decoded.dataType != DataType::DATA_MDR && decoded.dataType != DataType::DATA_MDR_2) continue;
            const auto ackFrame = MdrProtocol::encodeFrame(
                DataType::ACK, MdrProtocol::nextSequenceNumber(decoded.seqNumber), {});
            (void)connector_->sendAllBytes(ackFrame);
            if (decoded.seqNumber != deviceRxExpectedSeq_) continue; // 只应用下一个期望序号
            lastDeviceRxSeq_ = decoded.seqNumber;
            deviceRxExpectedSeq_ = MdrProtocol::nextSequenceNumber(deviceRxExpectedSeq_);
            if (decoded.payload.empty()) continue;
            if (parseDeviceStateFrame(static_cast<Command>(decoded.payload[0]), decoded.payload)) {
                ++applied;
            }
        }
        return applied;
    }

    bool powerOffHeadphone() {
        lastError_.clear();
        // 真实设备上的关机操作码尚未按目标型号实测。仿真器可用于回归，真实设备一律拒绝发送。
        if (!connector_->isSimulation()) {
            lastError_ = "真实耳机远程关机命令尚未验证，已阻止发送。";
            return false;
        }
        // 事务成功只证明模拟回归流程完成，不表示真实耳机已验证。
        const bool acknowledged = executeCommandTransaction(
            MdrProtocol::buildPowerOffPayload(), std::nullopt);
        if (acknowledged) {
            connector_->disconnect();
        }
        return acknowledged;
    }

    bool refreshBatteryStatus() {
        Command expectedRet = (profile_.generation == ProtocolGeneration::MDR_V2)
                              ? Command::V2_BATTERY_RET_STATUS
                              : Command::V1_BATTERY_RET_STATUS;
        return executeCommandTransaction(MdrProtocol::buildBatteryGetPayload(profile_), expectedRet);
    }

    bool refreshAllDeviceStates() {
        if (!refreshBatteryStatus() ||
            !executeCommandTransaction(MdrProtocol::buildNcAsmGetPayload(profile_), Command::NCASM_RET_PARAM) ||
            !executeCommandTransaction(MdrProtocol::buildEqualizerGetPayload(profile_), Command::EQEBB_RET_PARAM)) {
            return false;
        }
        // ULT WEAR's verified profile covers battery, binary NC/ambient, and ULT Bass only.
        // Do not make startup depend on unverified XM-family commands.
        if (profile_.isUltWear) return true;
        if (profile_.supportsSpeakToChat &&
            !executeCommandTransaction(MdrProtocol::buildSpeakToChatGetPayload(), Command::SMART_TALKING_RET_PARAM)) return false;
        if (profile_.supportsDseeControl &&
            !executeCommandTransaction(MdrProtocol::buildDseeGetPayload(profile_), Command::UPSCALING_RET_PARAM)) return false;
        if (profile_.supportsCodecQualityControl &&
            !executeCommandTransaction(MdrProtocol::buildCodecGetPayload(), Command::AUDIO_CODEC_RET_PARAM)) return false;
        if (profile_.supportsWearDetectionControl &&
            !executeCommandTransaction(MdrProtocol::buildWearDetectionGetPayload(), Command::SYSTEM_WEAR_RET_PARAM)) return false;
        return true;
    }

    // 场景预设：逐项检查并记录每个子步骤的真实执行结果
    SceneApplyReport applySceneProfile(SceneProfile profile) {
        SceneApplyReport report;
        auto runStep = [&](const std::string& name, bool ok) {
            ++report.totalSteps;
            if (ok) {
                ++report.succeededSteps;
                report.stepLogs.push_back("[成功] " + name);
            } else {
                report.stepLogs.push_back("[失败] " + name + " (" + lastError_ + ")");
            }
        };

        if (profile_.isUltWear) {
            // ULT WEAR 场景同样双维下发：聆听模式 + ULT 模式 + EQ 预设（三者互不覆盖）
            switch (profile) {
                case SceneProfile::DEEP_FOCUS:
                    runStep("ULT WEAR 主动降噪", setAmbientSound({NcAsmMode::NOISE_CANCEL, 10, false}));
                    runStep("关闭 ULT Bass", setUltBassMode(0));
                    runStep("切换舒缓 EQ (Relaxed)", setEqualizer({EqPresetId::RELAXED, 2, {0, 1, 2, 1, 0}}));
                    break;
                case SceneProfile::OFFICE_COLLAB:
                    runStep("ULT WEAR 环境音 + 关注语音", setAmbientSound({NcAsmMode::AMBIENT_SOUND, 10, true}));
                    runStep("关闭 ULT Bass", setUltBassMode(0));
                    runStep("切换人声 EQ (Vocal)", setEqualizer({EqPresetId::VOCAL, 0, {-1, 2, 4, 3, 1}}));
                    break;
                case SceneProfile::STREET_COMMUTE:
                    runStep("ULT WEAR 环境音", setAmbientSound({NcAsmMode::AMBIENT_SOUND, 10, false}));
                    runStep("启用 ULT 1", setUltBassMode(1));
                    runStep("切换重低音增强 EQ", setEqualizer({EqPresetId::BASS_BOOST, 5, {3, 1, 0, 1, 2}}));
                    break;
                case SceneProfile::HIFI_AUDIOPHILE:
                    runStep("ULT WEAR 主动降噪", setAmbientSound({NcAsmMode::NOISE_CANCEL, 10, false}));
                    runStep("启用 ULT 2", setUltBassMode(2));
                    runStep("切换明快 EQ (Bright)", setEqualizer({EqPresetId::BRIGHT, 4, {2, 1, 1, 3, 5}}));
                    break;
            }
            return report;
        }

        switch (profile) {
            case SceneProfile::DEEP_FOCUS:
                runStep("开启主动降噪 (ANC)", setAmbientSound({NcAsmMode::NOISE_CANCEL, 10, false}));
                runStep("切换舒缓 EQ (Relaxed)", setEqualizer({EqPresetId::RELAXED, 2, {0, 1, 2, 1, 0}}));
                runStep("关闭智能免摘对话", setSpeakToChat({false, SpeakToChatSensitivity::AUTO, SpeakToChatTimeout::STANDARD}));
                break;
            case SceneProfile::OFFICE_COLLAB:
                runStep("环境音 14 级 + 关注语音", setAmbientSound({NcAsmMode::AMBIENT_SOUND, 14, true}));
                runStep("切换人声 EQ (Vocal)", setEqualizer({EqPresetId::VOCAL, 0, {-1, 2, 4, 3, 1}}));
                runStep("开启高灵敏度智能免摘", setSpeakToChat({true, SpeakToChatSensitivity::HIGH, SpeakToChatTimeout::SHORT}));
                break;
            case SceneProfile::STREET_COMMUTE:
                runStep("环境音 8 级", setAmbientSound({NcAsmMode::AMBIENT_SOUND, 8, false}));
                runStep("切换重低音增强 EQ", setEqualizer({EqPresetId::BASS_BOOST, 6, {3, 1, 0, 1, 2}}));
                runStep("切换稳定连接优先", setBluetoothConnectionQuality(BluetoothConnectionQuality::PRIORITIZE_STABLE_CONNECTION));
                break;
            case SceneProfile::HIFI_AUDIOPHILE:
                runStep("开启主动降噪 (ANC)", setAmbientSound({NcAsmMode::NOISE_CANCEL, 10, false}));
                runStep("切换明快 EQ (Bright)", setEqualizer({EqPresetId::BRIGHT, 4, {2, 1, 1, 3, 5}}));
                runStep("切换 LDAC 音质优先", setBluetoothConnectionQuality(BluetoothConnectionQuality::PRIORITIZE_SOUND_QUALITY));
                runStep("开启 DSEE Extreme", setDseeExtreme(true));
                break;
        }
        return report;
    }

    // 仅供协议单元测试注入原始帧验证错误处理
    bool sendRawPayloadForTest(const std::vector<uint8_t>& payload, std::optional<Command> expectedRet) {
        return executeCommandTransaction(payload, expectedRet);
    }

    const DeviceModelProfile& getModelProfile() const { return profile_; }
    const std::string& getMacAddress() const { return macAddress_; }
    int getRfcommChannel() const { return rfcommChannel_; }
    const std::string& getNegotiatedProtocolText() const { return negotiatedProtocolText_; }
    const AmbientSoundConfig& getAmbientSound() const { return ambientConfig_; }
    const EqualizerConfig& getEqualizer() const { return eqConfig_; }
    uint8_t getUltBassMode() const { return ultBassMode_; }
    const SpeakToChatConfig& getSpeakToChat() const { return speakToChatConfig_; }
    const HardwareSystemConfig& getHardwareConfig() const { return hwConfig_; }
    const BatteryState& getBatteryState() const { return battery_; }
    uint32_t getAckCount() const { return ackCount_; }
    const std::string& getLastError() const { return lastError_; }

private:
    // 发送任何新命令之前，先把"上一轮遗留"的接收缓冲消费干净。
    // 根因（实机复现）：设备对 EQ 的 0x58 SET 也会回一帧 0x57；当这帧在上一次事务
    // 已返回（只等 ACK）之后才到达，它就残留在 rxParser_ 里，紧接着的 0x56 GET 事务会
    // 把这帧旧回包当成自己的应答，于是真回读被推迟到下一次命令——UI 每次应用后都显示
    // 上一轮的值（"回读慢一拍"）。
    // 处理原则：逐帧消费（回 ACK、推进序号、解析并应用状态），绝不静默丢弃设备通知；
    // 但绝不把这些遗留帧当作本次事务的应答。
    int drainBufferedFrames() {
        // 先把"已到达但还没喂给拆帧器"的字节全部收干（0 超时读，读到没有为止）。
        // 必须循环：分片传输（实机 MTU、仿真器 5 字节极端分片）下每次只回来一小段，
        // 只读一次就会让残留帧跨到下一次事务里，等于没修。
        std::vector<DecodedFrame> frames;
        for (int round = 0; round < 64; ++round) {
            const RecvResult pending = connector_->receiveRawChunk(0);
            if (pending.status != RecvStatus::OK || pending.bytes.empty()) break;
            rxParser_.feedBytes(pending.bytes.data(), pending.bytes.size());
            auto justFrames = rxParser_.extractFrames();
            for (auto& f : justFrames) frames.push_back(std::move(f));
        }

        int drained = 0;
        for (const auto& decoded : frames) {
            if (decoded.dataType != DataType::DATA_MDR && decoded.dataType != DataType::DATA_MDR_2) {
                // 遗留 ACK 没有对应事务可归属，直接作废，也不计入 ackCount_。
                continue;
            }
            ++drained;
            // 无论序号是否可应用，都要回 ACK：否则设备侧会一直卡在"未确认帧"上，
            // 后续 SET/GET 的回包就发不出来（仿真器与实机都是这个语义）。
            const auto ackFrame = MdrProtocol::encodeFrame(
                DataType::ACK, MdrProtocol::nextSequenceNumber(decoded.seqNumber), {});
            (void)connector_->sendAllBytes(ackFrame);

            const bool isDuplicate = lastDeviceRxSeq_.has_value() &&
                                     decoded.seqNumber == *lastDeviceRxSeq_;
            if (isDuplicate) continue;                       // 重复帧只重发 ACK，不重复应用
            if (decoded.seqNumber != deviceRxExpectedSeq_) continue;  // 序号断裂：只 ACK，不应用
            lastDeviceRxSeq_ = decoded.seqNumber;
            deviceRxExpectedSeq_ = MdrProtocol::nextSequenceNumber(deviceRxExpectedSeq_);
            if (!decoded.payload.empty()) {
                (void)parseDeviceStateFrame(static_cast<Command>(decoded.payload[0]), decoded.payload);
            }
        }
        return drained;
    }

    // 把「EQ 维度 + ULT 维度」合并成唯一一条 SET 帧下发（互斥修复的核心）：
    //   ULT WEAR → 11 字节组合帧（子类型 0x03：preset 与 ult_mode 同帧，增益为真实值）
    //   其他机型 → 10 字节 EQ 帧（这些机型只有 EQ 维度，没有 ULT 入口）
    // 并且把 0x57 直接作为 SET 的期望回包：实机对 SET 也回 0x57，写入与回读在同一次
    // 事务里闭环，不再依赖"紧跟着再发一条 GET"（那正是慢一拍的放大器）。
    bool writeEqUltFrame(const EqualizerConfig& eq, uint8_t ultMode) {
        std::vector<uint8_t> setPayload;
        if (profile_.isUltWear) {
            if (ultMode > 2) {
                lastError_ = "ULT Bass 模式值必须为 0、1 或 2。";
                return false;
            }
            setPayload = MdrProtocol::buildUltBassSetPayload(profile_, eq, ultMode);
        } else {
            setPayload = MdrProtocol::buildEqualizerSetPayload(profile_, eq);
        }

        if (executeCommandTransaction(setPayload, Command::EQEBB_RET_PARAM)) {
            return true;
        }

        // 保险（与第 1 道修复互为备份）：个别固件只在 GET 上回 0x57。SET 没等到回包就
        // 退回发一次 0x56 GET。此时 SET 的残留回包已由 drainBufferedFrames() 清掉，
        // GET 只可能读到自己的应答。
        const std::string setError = lastError_;
        if (executeCommandTransaction(MdrProtocol::buildEqualizerGetPayload(profile_),
                                      Command::EQEBB_RET_PARAM)) {
            return true;
        }
        lastError_ = "EQ 写入后回读失败：SET 未拿到 0x57（" + setError +
                     "），退回 0x56 GET 兜底也失败（" + lastError_ + "）";
        return false;
    }

    bool executeCommandTransaction(const std::vector<uint8_t>& payload,
                                   std::optional<Command> expectedReplyCommand,
                                   int timeoutMs = 1500) {
        lastError_.clear();
        if (!isConnected()) {
            lastError_ = "设备未连接";
            return false;
        }

        // 关键顺序：先清掉上一轮遗留帧，再发新命令。
        // 这样本次事务读到的任何帧都只可能来自本次发出的命令。
        (void)drainBufferedFrames();

        uint8_t currentSeq = hostTxSeq_;
        uint8_t expectedAckSeq = MdrProtocol::nextSequenceNumber(currentSeq);
        auto rawFrame = MdrProtocol::encodeFrame(DataType::DATA_MDR, currentSeq, payload);

        if (!connector_->sendAllBytes(rawFrame)) {
            lastError_ = "发送指令失败: " + connector_->getLastError();
            return false;
        }

        bool gotValidAck = false;
        bool gotExpectedReply = !expectedReplyCommand.has_value();

        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

        while (!gotValidAck || !gotExpectedReply) {
            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                lastError_ = "等待设备响应超时 (" + std::to_string(timeoutMs) + " ms)";
                return false;
            }
            int remainingMs = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

            // 若拆帧器缓冲区中尚无待处理帧，则从传输层读取新字节块
            auto frames = rxParser_.extractFrames();
            if (frames.empty()) {
                auto recvRes = connector_->receiveRawChunk(std::max(1, remainingMs));
                if (recvRes.status == RecvStatus::DISCONNECTED) {
                    // 如果是发送关机指令引起的断连且已收到 ACK，视为正常完成
                    if (!payload.empty() &&
                        static_cast<Command>(payload[0]) == Command::POWER_OFF_SET &&
                        gotValidAck) {
                        return true;
                    }
                    lastError_ = "等待响应期间连接断开: " + recvRes.errorMessage;
                    return false;
                }
                if (recvRes.status == RecvStatus::ERROR_STATE) {
                    lastError_ = "接收错误: " + recvRes.errorMessage;
                    return false;
                }
                if (recvRes.status == RecvStatus::TIMEOUT) {
                    continue;
                }
                rxParser_.feedBytes(recvRes.bytes.data(), recvRes.bytes.size());
                frames = rxParser_.extractFrames();
            }

            for (const auto& decoded : frames) {
                if (decoded.dataType == DataType::ACK) {
                    if (decoded.seqNumber == expectedAckSeq) {
                        gotValidAck = true;
                        hostTxSeq_ = expectedAckSeq; // 翻转主机发送序列号 (0 <-> 1)
                        ++ackCount_;
                    } else {
                        lastError_ = "收到序号不匹配的 ACK 帧";
                        return false;
                    }
                } else if (decoded.dataType == DataType::DATA_MDR ||
                           decoded.dataType == DataType::DATA_MDR_2) {
                    // 严格验证设备数据帧的交替序号。重复帧只重发 ACK，不重复应用其状态。
                    const bool isExpected = decoded.seqNumber == deviceRxExpectedSeq_;
                    const bool isDuplicate = lastDeviceRxSeq_.has_value() &&
                                             decoded.seqNumber == *lastDeviceRxSeq_;
                    if (!isExpected && !isDuplicate) {
                        lastError_ = "设备 DATA_MDR 序号错误：期望 " +
                                     std::to_string(deviceRxExpectedSeq_) + "，实际收到 " +
                                     std::to_string(decoded.seqNumber);
                        return false;
                    }

                    const uint8_t ackToDeviceSeq =
                        MdrProtocol::nextSequenceNumber(decoded.seqNumber);
                    const auto ackFrame = MdrProtocol::encodeFrame(DataType::ACK, ackToDeviceSeq, {});
                    if (!connector_->sendAllBytes(ackFrame)) {
                        lastError_ = "向设备回发 ACK 失败: " + connector_->getLastError();
                        return false;
                    }

                    if (isDuplicate) {
                        continue;
                    }

                    lastDeviceRxSeq_ = decoded.seqNumber;
                    deviceRxExpectedSeq_ = MdrProtocol::nextSequenceNumber(deviceRxExpectedSeq_);
                    if (!decoded.payload.empty()) {
                        const auto replyCmd = static_cast<Command>(decoded.payload[0]);
                        const bool stateFrameValid = parseDeviceStateFrame(replyCmd, decoded.payload);
                        if (expectedReplyCommand.has_value() && *expectedReplyCommand == replyCmd) {
                            if (!stateFrameValid) {
                                lastError_ = "设备返回了预期命令字，但载荷格式/字段校验失败 (0x";
                                constexpr char hexDigits[] = "0123456789ABCDEF";
                                const uint8_t commandByte = decoded.payload[0];
                                lastError_.push_back(hexDigits[(commandByte >> 4) & 0x0F]);
                                lastError_.push_back(hexDigits[commandByte & 0x0F]);
                                lastError_ += ")；原始载荷=";
                                for (size_t i = 0; i < decoded.payload.size(); ++i) {
                                    if (i != 0) lastError_ += ' ';
                                    lastError_.push_back(hexDigits[(decoded.payload[i] >> 4) & 0x0F]);
                                    lastError_.push_back(hexDigits[decoded.payload[i] & 0x0F]);
                                }
                                return false;
                            }
                            gotExpectedReply = true;
                        }
                    }
                }
            }
        }
        return true;
    }

    // 仅根据设备真实返回的 RET/NOTIFY 数据帧更新控制器状态
    bool parseDeviceStateFrame(Command cmd, const std::vector<uint8_t>& p) {
        switch (cmd) {
            case Command::CONNECT_RET_PROTOCOL_INFO: {
                if (p.size() < 4 || p[2] < 1 || p[2] > 3) return false;
                negotiatedProtocolMajor_ = p[2];
                if (profile_.isUltWear && p[2] == 3) {
                    negotiatedProtocolText_ = "Sony MDR V2（ULT 握手字段 0x03）";
                } else {
                    negotiatedProtocolText_ = "Sony MDR v" + std::to_string(p[2]) + "." + std::to_string(p[3]);
                }
                return true;
            }
            case Command::V1_BATTERY_RET_STATUS:
            case Command::V2_BATTERY_RET_STATUS: {
                if ((profile_.generation == ProtocolGeneration::MDR_V1 &&
                     cmd != Command::V1_BATTERY_RET_STATUS) ||
                    (profile_.generation == ProtocolGeneration::MDR_V2 &&
                     cmd != Command::V2_BATTERY_RET_STATUS) || p.size() < 3) {
                    return false;
                }
                const uint8_t subType = p[1];
                if (subType == 0x00 && p.size() == 4 &&
                    profile_.batteryLayout == BatteryLayout::SINGLE_HEADPHONE &&
                    p[2] <= 100 && p[3] <= 1) {
                    // 头戴式单电池布局：不伪造左右耳与充电盒电量
                    battery_.singleHeadphonePercent = p[2];
                    battery_.isCharging = (p[3] != 0x00);
                    battery_.leftEarbudPercent = std::nullopt;
                    battery_.rightEarbudPercent = std::nullopt;
                    battery_.casePercent = std::nullopt;
                    return true;
                }
                if (subType == 0x01 && p.size() == 5 &&
                    profile_.batteryLayout == BatteryLayout::DUAL_EARBUDS_CASE &&
                    p[2] <= 100 && p[3] <= 100 && p[4] <= 100) {
                    // 真无线耳塞三电量布局
                    battery_.singleHeadphonePercent = std::nullopt;
                    battery_.leftEarbudPercent = p[2];
                    battery_.rightEarbudPercent = p[3];
                    battery_.casePercent = p[4];
                    return true;
                }
                return false;
            }
            case Command::NCASM_RET_PARAM:
            case Command::NCASM_NTFY_PARAM: {
                if (profile_.isUltWear) {
                    // ULT WEAR third-party reverse-engineered implementations accept RET payloads
                    // with at least 7 bytes and read enabled/ambient/voice at indexes 3/4/6.
                    // Firmware variants may append fields or encode flags as non-zero values, so do
                    // not reject a valid 0x67/0x17 response solely because of trailing bytes or p[2].
                    if (p.size() < 7 || p[1] != 0x17) return false;
                    const bool enabled = p[3] != 0;
                    const bool ambient = p[4] != 0;
                    const bool voiceFocus = p[6] != 0;
                    if (!enabled) ambientConfig_.mode = NcAsmMode::OFF;
                    else ambientConfig_.mode = ambient ? NcAsmMode::AMBIENT_SOUND : NcAsmMode::NOISE_CANCEL;
                    ambientConfig_.ambientLevel = 10; // ULT WEAR uses binary NC/Ambient, not XM5's 1-20 level.
                    ambientConfig_.voicePassthrough = enabled && ambient && voiceFocus;
                    return true;
                }
                if (p.size() != 7 || p[1] != profile_.ncAsmInquiredType ||
                    p[2] > 1 || (p[3] != 0x01 && p[3] != 0x02) ||
                    p[4] > 1 || p[5] > 1) return false;
                const uint8_t totalEffect = p[2];
                const uint8_t settingType = p[3];
                const uint8_t voiceFocus  = p[5];
                const uint8_t asmLevel    = p[6];
                if (totalEffect == 0x00) {
                    ambientConfig_.mode = NcAsmMode::OFF;
                } else if (settingType == 0x02) {
                    if (asmLevel < 1 || asmLevel > 20) return false;
                    ambientConfig_.mode = NcAsmMode::AMBIENT_SOUND;
                    ambientConfig_.ambientLevel = asmLevel;
                    ambientConfig_.voicePassthrough = (voiceFocus != 0x00);
                } else {
                    ambientConfig_.mode = NcAsmMode::NOISE_CANCEL;
                    ambientConfig_.voicePassthrough = false;
                }
                return true;
            }
            case Command::EQEBB_RET_PARAM:
            case Command::EQEBB_NTFY_PARAM: {
                if (profile_.isUltWear) {
                    // ULT WEAR 回包同时携带两维：[0x57,0x03,preset,ult_mode,0x06,clearBass+10,b0..b4+10]
                    if (p.size() != 11 || p[1] != profile_.eqInquiredType ||
                        p[3] > 2 || p[4] != 0x06) {
                        return false;
                    }
                    for (size_t i = 5; i < 11; ++i) {
                        if (p[i] > 20) return false;
                    }
                    // 先整体校验后统一落库：任一字段非法都不污染已有状态
                    const EqPresetId preset = static_cast<EqPresetId>(p[2]);
                    const int8_t clearBass = static_cast<int8_t>(static_cast<int>(p[5]) - 10);
                    std::array<int8_t, 5> bands{};
                    for (size_t i = 0; i < 5; ++i) {
                        bands[i] = static_cast<int8_t>(static_cast<int>(p[6 + i]) - 10);
                    }
                    eqConfig_.preset = preset;
                    eqConfig_.clearBass = clearBass;
                    eqConfig_.bands = bands;
                    ultBassMode_ = p[3];
                    return true;
                }
                if (p.size() != 10 || p[1] != profile_.eqInquiredType || p[3] != 0x06) return false;
                for (size_t i = 4; i < 10; ++i) {
                    if (p[i] > 20) return false;
                }
                eqConfig_.preset = static_cast<EqPresetId>(p[2]);
                eqConfig_.clearBass = static_cast<int8_t>(static_cast<int>(p[4]) - 10);
                for (size_t i = 0; i < 5; ++i) {
                    eqConfig_.bands[i] = static_cast<int8_t>(static_cast<int>(p[5 + i]) - 10);
                }
                return true;
            }
            case Command::SMART_TALKING_RET_PARAM: {
                if (p.size() != 5 || p[1] != 0x01 || p[2] > 1 || p[3] > 2 || p[4] > 3) return false;
                speakToChatConfig_.enabled = (p[2] != 0x00);
                speakToChatConfig_.sensitivity = static_cast<SpeakToChatSensitivity>(p[3]);
                speakToChatConfig_.timeout = static_cast<SpeakToChatTimeout>(p[4]);
                return true;
            }
            case Command::UPSCALING_RET_PARAM: {
                if (p.size() != 3 || p[1] != profile_.dseeInquiredType || p[2] > 1) return false;
                hwConfig_.dseeExtremeEnabled = (p[2] != 0x00);
                return true;
            }
            case Command::AUDIO_CODEC_RET_PARAM: {
                if (p.size() != 3 || p[1] != 0x01 || p[2] > 1) return false;
                hwConfig_.connectionQuality = static_cast<BluetoothConnectionQuality>(p[2]);
                return true;
            }
            case Command::SYSTEM_WEAR_RET_PARAM: {
                if (p.size() != 3 || p[1] != 0x01 || p[2] > 1) return false;
                hwConfig_.pauseWhenTakenOff = (p[2] != 0x00);
                return true;
            }
            default:
                return false;
        }
    }

    DeviceModelProfile profile_;
    std::unique_ptr<IBluetoothConnector> connector_;
    MdrStreamFrameParser rxParser_;

    std::string macAddress_;
    int rfcommChannel_ = 1;
    std::string negotiatedProtocolText_ = "未协商";

    uint8_t hostTxSeq_ = 0;
    uint8_t deviceRxExpectedSeq_ = 0;
    std::optional<uint8_t> lastDeviceRxSeq_;
    uint8_t negotiatedProtocolMajor_ = 0;
    uint8_t ultBassMode_ = 0;
    uint32_t ackCount_ = 0;
    std::string lastError_;

    AmbientSoundConfig ambientConfig_{};
    EqualizerConfig eqConfig_{};
    SpeakToChatConfig speakToChatConfig_{};
    HardwareSystemConfig hwConfig_{};
    BatteryState battery_{};
};

// =====================================================================
// 6.5.1 本地快照构建（定义在 HeadphoneController 之后，故放在命名空间尾部）
// 两维同时保存：ULT 模式与 EQ（预设 / Clear Bass / 五段）都写进同一个文件，
// 恢复时不会只还原其中一维。
// =====================================================================
inline PersistedState makePersistedState(const HeadphoneController& controller,
                                         const std::string& modelCode,
                                         const std::string& macAddress) {
    PersistedState state{};
    state.modelCode = modelCode;
    state.macAddress = macAddress;
    state.ultBassMode = controller.getUltBassMode();
    state.equalizer = controller.getEqualizer();
    state.ambient = controller.getAmbientSound();
    state.hasAmbient = true;
    state.valid = true;
    return state;
}

// =====================================================================
// 6.6 取证/演示用启动参数解析（纯函数，可被 --test-sim 直接断言）
//     只决定「打开时把哪个页面、哪台机型、哪两维本地状态摆到眼前」，
//     不参与任何协议收发，也不会伪造"已同步"。非法值一律记进 rejectNote，
//     不做静默回落（与 P1 配置文件解析的严格性保持一致）。
// =====================================================================
constexpr int kEqPresetOptionCount = 15;  // 必须与 UI 的 presetOptions() 条目数一致

struct ShotStartupSpec {
    int page = -1;          // -1 未指定；0..3 = 概览/声音/均衡器/设备（场景预设页已下线）
    std::string modelCode;  // 空 = 未指定
    int ultBassMode = -1;   // -1 未指定；0..2
    int presetIndex = -1;   // -1 未指定；0..kEqPresetOptionCount-1
    bool connectSim = false;
    std::string rejectNote; // 被忽略的参数（逗号分隔）
};

inline ShotStartupSpec parseShotStartupArgs(const std::vector<std::string>& args) {
    ShotStartupSpec spec{};
    auto reject = [&spec](const std::string& token) {
        if (!spec.rejectNote.empty()) spec.rejectNote += ", ";
        spec.rejectNote += token;
    };
    auto intOf = [](const std::string& text, int& out) {
        if (text.empty()) return false;
        int value = 0;
        const char* first = text.data();
        const char* last = text.data() + text.size();
        const auto parsed = std::from_chars(first, last, value);
        if (parsed.ec != std::errc() || parsed.ptr != last) return false;
        out = value;
        return true;
    };

    for (const auto& arg : args) {
        auto take = [&arg](const char* key, std::string& value) {
            const std::string prefix(key);
            if (arg.compare(0, prefix.size(), prefix) != 0) return false;
            value = arg.substr(prefix.size());
            return true;
        };
        std::string value;
        if (take("--page=", value)) {
            int page = 0;
            if (intOf(value, page) && page >= 0 && page <= 3) spec.page = page;
            else reject(arg);
        } else if (take("--model=", value)) {
            try {
                getProfileByModelCode(value);  // 未知机型码直接抛异常 → 拒绝该参数
                spec.modelCode = value;
            } catch (...) {
                reject(arg);
            }
        } else if (take("--ult=", value)) {
            int mode = 0;
            if (intOf(value, mode) && mode >= 0 && mode <= 2) spec.ultBassMode = mode;
            else reject(arg);
        } else if (take("--preset=", value)) {
            int index = 0;
            if (intOf(value, index) && index >= 0 && index < kEqPresetOptionCount) spec.presetIndex = index;
            else reject(arg);
        } else if (arg == "--connect-sim") {
            spec.connectSim = true;
        }
    }
    return spec;
}

} // namespace sony

// ============================================================================
// 第七部分：安全输入辅助、状态仪表盘与带严格断言的自检套件
// ============================================================================

using namespace sony;

namespace {

// 安全读取整数：处理非数字输入导致的 cin.fail()，并在 int 范围内校验后返回
int readCheckedInt(const std::string& prompt, int minVal, int maxVal) {
    while (true) {
        std::cout << prompt;
        int value = 0;
        if (std::cin >> value) {
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            if (value >= minVal && value <= maxVal) {
                return value;
            }
            std::cout << "  [输入越界] 请输入 " << minVal << " 到 " << maxVal << " 之间的整数。\n";
        } else {
            if (std::cin.eof()) {
                return minVal;
            }
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << "  [格式错误] 输入包含非数字字符，请重新输入。\n";
        }
    }
}

void printDeviceDashboard(const HeadphoneController& controller) {
    const auto& prof = controller.getModelProfile();
    const auto& amb  = controller.getAmbientSound();
    const auto& eq   = controller.getEqualizer();
    const auto& stc  = controller.getSpeakToChat();
    const auto& hw   = controller.getHardwareConfig();
    const auto& bat  = controller.getBatteryState();

    std::cout << "\n==================== [ 索尼耳机状态仪表盘 ] ====================\n"
              << " 运行后端 : " << (controller.isSimulationMode() ? "[模拟验证模式 - StatefulMdrSimulator]" : "[真实蓝牙硬件模式 - Native RFCOMM]") << "\n"
              << " 目标型号 : " << prof.displayName << " | MAC: " << controller.getMacAddress()
              << " (RFCOMM Ch=" << controller.getRfcommChannel() << ")\n"
              << " 连接状态 : " << (controller.isConnected() ? "已连接" : "已断开")
              << " | 协商协议: " << controller.getNegotiatedProtocolText()
              << " | 有效 ACK 计数: " << controller.getAckCount() << "\n";

    // 按设备型号真实电池布局打印，不支持的字段明确标注为 N/A
    std::cout << " 电池电量 : ";
    if (bat.singleHeadphonePercent.has_value()) {
        std::cout << "主机电量 " << static_cast<int>(*bat.singleHeadphonePercent) << "% ("
                  << (bat.isCharging ? "充电中" : "放电中") << ") | 左右耳独立电量: 不适用(头戴式单电池)\n";
    } else if (bat.leftEarbudPercent.has_value()) {
        std::cout << "左耳 " << static_cast<int>(*bat.leftEarbudPercent) << "% | 右耳 "
                  << static_cast<int>(*bat.rightEarbudPercent) << "% | 充电盒 "
                  << static_cast<int>(*bat.casePercent) << "%\n";
    } else {
        std::cout << "未知 (尚未读取)\n";
    }

    if (prof.isUltWear) {
        const char* modeName = controller.getUltBassMode() == 0 ? "关闭" :
                               controller.getUltBassMode() == 1 ? "ULT 1" : "ULT 2";
        std::cout << " ULT Bass : " << modeName << "（EQ 子类型 0x03 的 [3] 位）\n"
                  << " 均衡器 EQ: 预设=" << MdrProtocol::getPresetName(eq.preset)
                  << " | Clear Bass=" << std::showpos << static_cast<int>(eq.clearBass)
                  << " | 频段 400Hz(" << static_cast<int>(eq.bands[0]) << ") "
                  << "1kHz(" << static_cast<int>(eq.bands[1]) << ") "
                  << "2.5kHz(" << static_cast<int>(eq.bands[2]) << ") "
                  << "6.3kHz(" << static_cast<int>(eq.bands[3]) << ") "
                  << "16kHz(" << static_cast<int>(eq.bands[4]) << ")"
                  << std::noshowpos << "（0x03 帧的 [2]/[5..10]）\n"
                  << "----------------------------------------------------------------\n"
                  << " [1] 降噪/环境音 : " << (amb.mode == NcAsmMode::NOISE_CANCEL ? "主动降噪 ANC" :
                       amb.mode == NcAsmMode::AMBIENT_SOUND ? "环境音 Ambient" : "关闭 Off")
                  << " | 关注语音=" << (amb.voicePassthrough ? "开启" : "关闭") << "\n"
                  << " [2] ULT Bass : 关闭 / ULT 1 / ULT 2（与 EQ 预设、五段增益同帧下发，互不覆盖）\n"
                  << " [3] 均衡器 EQ : 预设 / Clear Bass / 五段推子（ULT WEAR 同样可用）\n"
                  << " [4] 电池 : 头戴式单电池 + 充电状态\n"
                  << " [5] 设备设置 : 其他写命令尚未在 ULT WEAR 配置中核实，当前禁用\n"
                  << "================================================================\n";
        return;
    }

    // 修复降噪模式下显示 0/20 的问题：仅在 AMBIENT_SOUND 模式下展示环境音等级与关注语音
    std::cout << "----------------------------------------------------------------\n"
              << " [1] 降噪/环境音 : ";
    if (amb.mode == NcAsmMode::NOISE_CANCEL) {
        std::cout << "主动降噪开启 (ANC) [环境音等级在降噪模式下不适用]\n";
    } else if (amb.mode == NcAsmMode::AMBIENT_SOUND) {
        std::cout << "环境音模式 (ASM) | 等级: " << static_cast<int>(amb.ambientLevel) << "/20"
                  << " | 关注语音: " << (amb.voicePassthrough ? "开启" : "关闭") << "\n";
    } else {
        std::cout << "关闭 (OFF)\n";
    }

    std::cout << " [2] 均衡器 (EQ) : 预设=" << MdrProtocol::getPresetName(eq.preset)
              << " (InquiredType=0x" << std::hex << std::uppercase << std::setw(2) << std::setfill('0')
              << static_cast<int>(prof.eqInquiredType) << std::dec << std::nouppercase << std::setfill(' ') << ")"
              << " | Clear Bass=" << std::showpos << static_cast<int>(eq.clearBass) << "\n"
              << "     5 频段增益  : 400Hz(" << static_cast<int>(eq.bands[0]) << ") "
              << "1kHz(" << static_cast<int>(eq.bands[1]) << ") "
              << "2.5kHz(" << static_cast<int>(eq.bands[2]) << ") "
              << "6.3kHz(" << static_cast<int>(eq.bands[3]) << ") "
              << "16kHz(" << static_cast<int>(eq.bands[4]) << ")" << std::noshowpos << "\n"
              << " [3] 智能免摘对话: " << (stc.enabled ? "开启" : "关闭")
              << " | 灵敏度: " << (stc.sensitivity == SpeakToChatSensitivity::HIGH ? "高" :
                                   stc.sensitivity == SpeakToChatSensitivity::LOW  ? "低" : "自动") << "\n"
              << " [4] 硬件与音频  : 连接模式="
              << (hw.connectionQuality == BluetoothConnectionQuality::PRIORITIZE_SOUND_QUALITY
                  ? "音质优先 (LDAC)" : "稳定连接优先 (AAC/SBC)")
              << " | DSEE Extreme(0xE8)=" << (hw.dseeExtremeEnabled ? "开启" : "关闭")
              << " | 佩戴感应=" << (hw.pauseWhenTakenOff ? "开启" : "关闭") << "\n"
              << "================================================================\n";
}

// 严格断言自检套件：验证流拆帧（半包/粘包）、0/1 序号交替、非法命令拒收、写后回读一致性
int runStrictSimulationSelfTests() {
    std::cout << "================ [ 开始执行协议栈与有状态仿真严格自检 ] ================\n";
    int failedAssertions = 0;

    auto requireTrue = [&](bool cond, const std::string& testName, const std::string& detail = "") {
        if (cond) {
            std::cout << "  [PASS] " << testName << "\n";
        } else {
            std::cerr << "  [FAIL] " << testName << (detail.empty() ? "" : " -> " + detail) << "\n";
            ++failedAssertions;
        }
    };

    // 测试 1：流式拆帧器半包拼接、粘包提取与特殊字节转义测试
    {
        MdrStreamFrameParser parser;
        std::vector<uint8_t> payload1 = {0x68, 0x17, 0x01, 0x3C, 0x3D, 0x3E, 0x10}; // 含全部需转义特殊字节
        std::vector<uint8_t> payload2 = {0xE8, 0x01, 0x01};
        auto raw1 = MdrProtocol::encodeFrame(DataType::DATA_MDR, 0, payload1);
        auto raw2 = MdrProtocol::encodeFrame(DataType::DATA_MDR, 1, payload2);

        // 构造前导噪声 + 第一帧前半段
        std::vector<uint8_t> partA = {0xFF, 0x00, 0xAA};
        partA.insert(partA.end(), raw1.begin(), raw1.begin() + 4);
        parser.feedBytes(partA.data(), partA.size());
        requireTrue(parser.extractFrames().empty(), "流式拆帧器：收到半包时暂存不误解析");

        // 构造第一帧后半段 + 完整第二帧（模拟粘包）
        std::vector<uint8_t> partB(raw1.begin() + 4, raw1.end());
        partB.insert(partB.end(), raw2.begin(), raw2.end());
        parser.feedBytes(partB.data(), partB.size());

        auto extracted = parser.extractFrames();
        requireTrue(extracted.size() == 2 &&
                    extracted[0].payload == payload1 &&
                    extracted[0].seqNumber == 0 &&
                    extracted[1].payload == payload2 &&
                    extracted[1].seqNumber == 1,
                    "流式拆帧器：跨次半包重组 + 双帧粘包同时提取 + 特殊字节转义还原");
    }

    // 测试 2：型号选择不能把未知型号静默当成 XM5
    {
        bool unknownModelRejected = false;
        try {
            (void)getProfileByModelCode("UNKNOWN-MODEL");
        } catch (const std::invalid_argument&) {
            unknownModelRejected = true;
        }
        requireTrue(unknownModelRejected, "机型配置：拒绝未知型号，不静默回退到 XM5");
    }

    // 测试 3：MAC 地址严格校验测试
    {
        std::array<uint8_t, 6> mac{};
        requireTrue(validateBluetoothMacAddress("AC:80:0A:12:34:56", mac), "MAC 校验：合法地址通过");
        requireTrue(!validateBluetoothMacAddress("AC:80:0A:12:34:56:78", mac) &&
                    !validateBluetoothMacAddress("AC:80:0A:12:34:GG", mac) &&
                    !validateBluetoothMacAddress("AC:80:0A:12:34:56_extra", mac),
                    "MAC 校验：拒绝非法字符与尾随垃圾字符");
    }

    // 测试 4：有状态仿真器 + 分片流式接收 + 0/1 序列号交替 + 写后回读验证
    {
        auto profile = getProfileByModelCode("WH-1000XM5");
        auto sim = std::make_unique<StatefulMdrSimulator>(profile);
        auto* simRawPtr = sim.get();
        // 强制开启 5 字节极小分片接收，验证客户端在极端分段下的完整通信能力
        simRawPtr->setForceFragmentedOutput(true);

        HeadphoneController controller(profile, std::move(sim));
        bool connected = controller.connectDevice("AC:80:0A:12:34:56");
        requireTrue(connected, "连接握手与初始状态全量回读 (5字节极端分片流模式)", controller.getLastError());

        // 验证 WH-1000XM5 头戴式电量布局：仅单电池有值，左右耳必须为 nullopt
        const auto& bat = controller.getBatteryState();
        requireTrue(bat.singleHeadphonePercent.has_value() && *bat.singleHeadphonePercent == 86 &&
                    !bat.leftEarbudPercent.has_value() && !bat.casePercent.has_value(),
                    "机型电量区分：WH-1000XM5 仅解析单电池百分比，不伪造左右耳与充电盒电量");

        // 验证环境音 6 参数设置与回读
        bool ambOk = controller.setAmbientSound({NcAsmMode::AMBIENT_SOUND, 16, true});
        requireTrue(ambOk &&
                    controller.getAmbientSound().mode == NcAsmMode::AMBIENT_SOUND &&
                    controller.getAmbientSound().ambientLevel == 16 &&
                    controller.getAmbientSound().voicePassthrough,
                    "环境音控制：完整 6 参数下发与 GET 回读状态一致", controller.getLastError());

        // 验证 EQ 0xA2 (USER_DEFINED) 预设与 5 频段回读
        EqualizerConfig targetEq{EqPresetId::USER_DEFINED, -4, {-3, 0, 2, 5, 8}};
        bool eqOk = controller.setEqualizer(targetEq);
        requireTrue(eqOk &&
                    controller.getEqualizer().preset == EqPresetId::USER_DEFINED &&
                    controller.getEqualizer().clearBass == -4 &&
                    controller.getEqualizer().bands == targetEq.bands,
                    "均衡器控制：子类型 0x00 + 预设 0xA2 + 频段写后回读一致", controller.getLastError());

        // 验证修正后的 DSEE Extreme (0xE8)
        bool dseeOk = controller.setDseeExtreme(true);
        requireTrue(dseeOk && controller.getHardwareConfig().dseeExtremeEnabled,
                    "DSEE Extreme 控制：使用修正操作码 0xE8/0xE6 写后回读一致", controller.getLastError());

        // 验证负向测试：发送旧版错误操作码 0x88 或残缺的环境音载荷，仿真器必须拒绝且返回失败
        bool badCmdAccepted = controller.sendRawPayloadForTest({0x88, 0x01, 0x01}, std::nullopt);
        requireTrue(!badCmdAccepted, "负向拦截测试：仿真器成功拒绝错误的 0x88 伪 DSEE 指令");

        bool badNcPayloadAccepted = controller.sendRawPayloadForTest({0x68, 0x17, 0x01, 0x02}, std::nullopt);
        requireTrue(!badNcPayloadAccepted, "负向拦截测试：仿真器成功拒绝字段不全的 0x68 环境音指令");

        // 验证主机对设备发来的 DATA_MDR 帧均按规范回了 ACK
        requireTrue(simRawPtr->didHostAcknowledgeAllDeviceFrames() &&
                    simRawPtr->getDeviceFramesSentCount() == simRawPtr->getDeviceFramesAckedCount(),
                    "双向 ACK 验证：设备发送帧计数与主机有效确认计数相等");

        // 验证场景预设报告及降噪模式下不显示无效的 0/20 等级
        auto sceneReport = controller.applySceneProfile(SceneProfile::DEEP_FOCUS);
        requireTrue(sceneReport.allSucceeded() &&
                    controller.getAmbientSound().mode == NcAsmMode::NOISE_CANCEL,
                    "场景预设事务：深度专注模式全部子步骤成功并完成回读验证");

        printDeviceDashboard(controller);
    }

    // =====================================================================
    // 概览页交互自检（用户反馈 m00360）：
    //   ① 聆听模式「点按即下发」——概览页只有三个分段按钮、没有应用按钮，
    //      所以一次点击必须自己成帧；② 情景调节卡——一次点击 = applySceneProfile
    //      的三维组合（降噪/环境音 + ULT 模式 + EQ 预设）。
    // 断言走 UI 与 --test-sim 共用的纯函数（ambientTapConfig / sceneProfileAtCard），
    // 界面行为因此有回归覆盖，不再只能靠手点验证。
    // =====================================================================
    {
        // ① XM5（非 ULT）：环境音 7 级 → 点「降噪」一次即成帧，且等级不被冲掉
        const auto xm5 = getProfileByModelCode("XM5");
        auto tapSim = std::make_unique<StatefulMdrSimulator>(xm5);
        HeadphoneController tapController(xm5, std::move(tapSim));
        const bool tapConnected = tapController.connectDevice("AC:80:0A:12:34:56");
        const bool tapPresetOk = tapConnected &&
                                 tapController.setAmbientSound({NcAsmMode::AMBIENT_SOUND, 7, false});
        const int levelBefore = tapController.getAmbientSound().ambientLevel;
        // 概览页「聆听模式」卡第二段（降噪）被点了一下
        const AmbientSoundConfig tapCfg = ambientTapConfig(NcAsmMode::NOISE_CANCEL, levelBefore, false);
        const bool tapOk = tapPresetOk && tapController.setAmbientSound(tapCfg);
        requireTrue(tapOk && tapCfg.mode == NcAsmMode::NOISE_CANCEL &&
                    tapController.getAmbientSound().mode == NcAsmMode::NOISE_CANCEL,
                    "概览页聆听模式点按即下发：一次点击即写入降噪并回读一致（不依赖应用按钮）",
                    tapController.getLastError());
        requireTrue(tapPresetOk && levelBefore == 7 && tapController.getAmbientSound().ambientLevel == 7,
                    "概览页聆听模式点按即下发：切换模式不改动用户已选的环境音等级（7 级保留）",
                    "levelBefore=" + std::to_string(levelBefore) +
                        " levelAfter=" + std::to_string(tapController.getAmbientSound().ambientLevel));

        // ② ambientTapConfig 归一：等级夹到 [1,20]，关闭降噪时清掉关注语音
        const AmbientSoundConfig tapLo = ambientTapConfig(NcAsmMode::AMBIENT_SOUND, 0, true);
        const AmbientSoundConfig tapHi = ambientTapConfig(NcAsmMode::AMBIENT_SOUND, 99, true);
        const AmbientSoundConfig tapOff = ambientTapConfig(NcAsmMode::OFF, 12, true);
        requireTrue(tapLo.ambientLevel == 1 && tapHi.ambientLevel == 20 && tapHi.voicePassthrough &&
                    !tapOff.voicePassthrough,
                    "聆听模式点击载荷归一：等级夹到 1..20，关闭降噪的帧不携带关注语音",
                    "lo=" + std::to_string(tapLo.ambientLevel) + " hi=" + std::to_string(tapHi.ambientLevel));

        // ③ 情景卡序号 → 场景预设：4 张卡与标签一一对应，越界自动夹取
        bool cardsMatch = kSceneCardCount == 4 &&
                          sceneProfileAtCard(0) == SceneProfile::DEEP_FOCUS &&
                          sceneProfileAtCard(1) == SceneProfile::OFFICE_COLLAB &&
                          sceneProfileAtCard(2) == SceneProfile::STREET_COMMUTE &&
                          sceneProfileAtCard(3) == SceneProfile::HIFI_AUDIOPHILE &&
                          sceneProfileAtCard(-3) == SceneProfile::DEEP_FOCUS &&
                          sceneProfileAtCard(99) == SceneProfile::HIFI_AUDIOPHILE &&
                          std::string(sceneCardLabelUtf8(0)) == "深度专注" &&
                          std::string(sceneCardLabelUtf8(1)) == "办公协作" &&
                          std::string(sceneCardLabelUtf8(2)) == "街头通勤" &&
                          std::string(sceneCardLabelUtf8(3)) == "Hi-Fi 鉴赏";
        requireTrue(cardsMatch,
                    "概览页情景调节卡：4 张卡序号/标签与场景预设一一对应，越界索引自动夹取");

        // ④ ULT 机型：4 张情景卡各点一次，每次点击都写满 降噪/环境音 + ULT + EQ 三维
        const auto ultProfile = getProfileByModelCode("WH-ULT900N");
        auto sceneSim = std::make_unique<StatefulMdrSimulator>(ultProfile);
        HeadphoneController sceneController(ultProfile, std::move(sceneSim));
        const bool sceneConnected = sceneController.connectDevice("AA:BB:CC:DD:EE:02");
        bool allSceneOk = sceneConnected;
        bool threeDimsOk = sceneConnected;
        for (int card = 0; card < kSceneCardCount && allSceneOk; ++card) {
            const SceneApplyReport report = sceneController.applySceneProfile(sceneProfileAtCard(card));
            allSceneOk = report.allSucceeded();
            const auto& amb = sceneController.getAmbientSound();
            const int ultMode = sceneController.getUltBassMode();
            const EqPresetId preset = sceneController.getEqualizer().preset;
            switch (sceneProfileAtCard(card)) {
                case SceneProfile::DEEP_FOCUS:
                    threeDimsOk = threeDimsOk && amb.mode == NcAsmMode::NOISE_CANCEL && ultMode == 0 &&
                                  preset == EqPresetId::RELAXED;
                    break;
                case SceneProfile::OFFICE_COLLAB:
                    threeDimsOk = threeDimsOk && amb.mode == NcAsmMode::AMBIENT_SOUND &&
                                  amb.voicePassthrough && ultMode == 0 && preset == EqPresetId::VOCAL;
                    break;
                case SceneProfile::STREET_COMMUTE:
                    threeDimsOk = threeDimsOk && amb.mode == NcAsmMode::AMBIENT_SOUND &&
                                  !amb.voicePassthrough && ultMode == 1 && preset == EqPresetId::BASS_BOOST;
                    break;
                case SceneProfile::HIFI_AUDIOPHILE:
                    threeDimsOk = threeDimsOk && amb.mode == NcAsmMode::NOISE_CANCEL && ultMode == 2 &&
                                  preset == EqPresetId::BRIGHT;
                    break;
            }
        }
        requireTrue(allSceneOk && threeDimsOk,
                    "概览页情景卡点击：4 张卡各一次点击即写满降噪/环境音 + ULT + EQ 三维",
                    sceneController.getLastError());
    }

    // 回归测试：关机 ACK 必须在模拟器断开前交付；成功后连接器应关闭。
    {
        const auto profile = getProfileByModelCode("XM5");
        auto sim = std::make_unique<StatefulMdrSimulator>(profile);
        sim->setForceFragmentedOutput(true);
        HeadphoneController controller(profile, std::move(sim));
        const bool connected = controller.connectDevice("AC:80:0A:12:34:56");
        const bool offOk = connected && controller.powerOffHeadphone();
        requireTrue(offOk && !controller.isConnected(),
                    "关机回归：已排队的 ACK 能被读取，成功后本地连接关闭",
                    controller.getLastError());
    }

    // ULT WEAR regression tests based on published third-party reverse-engineering notes.
    {
        const auto profile = getProfileByModelCode("WH-ULT900N");
        requireTrue(profile.isUltWear && profile.defaultRfcommChannel == 18 &&
                    profile.eqInquiredType == 0x03 && profile.handshakeVersion == 3,
                    "ULT WEAR 型号配置：V2 传输、握手字段 3、通道 18 与 EQ 子类型 0x03");
        const std::vector<uint8_t> expectedBattery{0x22, 0x00};
        requireTrue(MdrProtocol::buildBatteryGetPayload(profile) == expectedBattery,
                    "ULT WEAR 电量查询载荷：0x22 0x00");
        const std::vector<uint8_t> expectedAmbient{0x68, 0x17, 0x01, 0x01, 0x01, 0x02, 0x01, 0x00};
        requireTrue(MdrProtocol::buildNcAsmSetPayload(profile, {NcAsmMode::AMBIENT_SOUND, 14, true}) == expectedAmbient,
                    "ULT WEAR 环境音/Voice Focus 载荷与公开逆向格式逐字节一致");
        const std::vector<uint8_t> expectedUlt2{0x58, 0x03, 0x00, 0x02, 0x06, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A};
        requireTrue(MdrProtocol::buildUltBassSetPayload(profile, EqualizerConfig{}, 2) == expectedUlt2,
                    "ULT WEAR ULT 2 载荷与公开逆向格式逐字节一致（EQ 中性时 [2]/[5..10] 全为 0x00/0x0A）");
        // 同一条 0x03 帧必须能同时承载预设 + Clear Bass + 五段增益（不再写死 0x0A）
        const EqualizerConfig ultEq{EqPresetId::BASS_BOOST, 7, {3, 1, 0, -2, -4}};
        const std::vector<uint8_t> expectedUltEq{0x58, 0x03, 0x16, 0x01, 0x06, 0x11, 0x0D, 0x0B, 0x0A, 0x08, 0x06};
        requireTrue(MdrProtocol::buildUltBassSetPayload(profile, ultEq, 1) == expectedUltEq,
                    "ULT WEAR 组合帧：preset/Clear Bass/五段增益与 ULT 模式落在同一 11 字节帧的既定位置");
        auto sim = std::make_unique<StatefulMdrSimulator>(profile);
        HeadphoneController controller(profile, std::move(sim));
        const bool connected = controller.connectDevice("AA:BB:CC:DD:EE:FF");
        requireTrue(connected, "ULT WEAR 模拟握手与初始状态读取", controller.getLastError());
        const bool ncOk = connected && controller.setAmbientSound({NcAsmMode::AMBIENT_SOUND, 14, true});
        requireTrue(ncOk && controller.getAmbientSound().mode == NcAsmMode::AMBIENT_SOUND &&
                    controller.getAmbientSound().voicePassthrough,
                    "ULT WEAR 二进制环境音与 Voice Focus 写入/回读", controller.getLastError());
        const bool ultOk = connected && controller.setUltBassMode(2);
        requireTrue(ultOk && controller.getUltBassMode() == 2,
                    "ULT WEAR ULT 2 写入/回读", controller.getLastError());

        // ---- P0 互斥解除：两维正交，写任一维都不覆盖另一维 ----
        const bool eqOk = connected && controller.setEqualizer({EqPresetId::VOCAL, -3, {-2, 0, 3, 4, 1}});
        requireTrue(eqOk &&
                    controller.getEqualizer().preset == EqPresetId::VOCAL &&
                    controller.getEqualizer().clearBass == -3 &&
                    controller.getEqualizer().bands[3] == 4,
                    "ULT WEAR 通用 EQ 写入/回读：预设 + Clear Bass + 五段全部落到 0x03 帧",
                    controller.getLastError());
        requireTrue(controller.getUltBassMode() == 2,
                    "P0 正交性：写 EQ 不覆盖 ULT 模式（EQ 写入后 ULT 仍为 2）",
                    "ult=" + std::to_string(static_cast<int>(controller.getUltBassMode())));
        const bool ultAgain = connected && controller.setUltBassMode(0);
        requireTrue(ultAgain && controller.getUltBassMode() == 0 &&
                    controller.getEqualizer().preset == EqPresetId::VOCAL &&
                    controller.getEqualizer().clearBass == -3 &&
                    controller.getEqualizer().bands[3] == 4,
                    "P0 正交性：写 ULT 模式不覆盖 EQ 维度（预设/Clear Bass/五段保持）",
                    controller.getLastError());

        // ---- 0x59 主动上报同样同时携带两维 ----
        auto ultSim = std::make_unique<StatefulMdrSimulator>(profile);
        auto* ultSimRaw = ultSim.get();
        HeadphoneController notifyController(profile, std::move(ultSim));
        const bool notifyConnected = notifyController.connectDevice("AA:BB:CC:DD:EE:01");
        const std::vector<uint8_t> dualNotify{
            static_cast<uint8_t>(Command::EQEBB_NTFY_PARAM), 0x03,
            static_cast<uint8_t>(EqPresetId::JAZZ), 0x02, 0x06, 0x0D, 0x0C, 0x0A, 0x09, 0x08, 0x0B};
        const bool pushed = notifyConnected && ultSimRaw->pushUnsolicitedNotificationForTest(dualNotify);
        const int appliedNotify = pushed ? notifyController.pollDeviceNotifications() : 0;
        requireTrue(pushed && appliedNotify >= 1 &&
                    notifyController.getUltBassMode() == 2 &&
                    notifyController.getEqualizer().preset == EqPresetId::JAZZ &&
                    notifyController.getEqualizer().clearBass == 3 &&
                    notifyController.getEqualizer().bands[0] == 2 &&
                    notifyController.getEqualizer().bands[4] == 1,
                    "ULT WEAR 0x59 主动上报：同一帧内同时刷新 ULT 模式与 EQ 五段",
                    notifyController.getLastError());

        // ---- 负向：0x03 帧增益越界 / ULT 模式越界必须被拒绝且不污染状态 ----
        const bool badGain = notifyConnected &&
            notifyController.sendRawPayloadForTest({0x58, 0x03, 0x01, 0x01, 0x06, 0x15, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A}, std::nullopt);
        const bool badMode = notifyConnected &&
            notifyController.sendRawPayloadForTest({0x58, 0x03, 0x01, 0x03, 0x06, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A}, std::nullopt);
        requireTrue(!badGain && !badMode &&
                    notifyController.getUltBassMode() == 2 &&
                    notifyController.getEqualizer().preset == EqPresetId::JAZZ,
                    "ULT WEAR 负向拦截：增益越界与 ULT 模式=3 均被拒收，已缓存状态不被污染");

        const auto ultScene = connected ? controller.applySceneProfile(SceneProfile::HIFI_AUDIOPHILE) : SceneApplyReport{};
        requireTrue(ultScene.allSucceeded() && controller.getAmbientSound().mode == NcAsmMode::NOISE_CANCEL &&
                    controller.getUltBassMode() == 2 && controller.getEqualizer().preset == EqPresetId::BRIGHT,
                    "ULT WEAR 场景预设：ANC + ULT 2 + Bright EQ 三维同时落地",
                    controller.getLastError());

        // ---- 非 ULT 机型：只有 EQ 维度，ULT 入口必须被拒绝 ----
        {
            const auto xm = getProfileByModelCode("XM5");
            auto xmSim = std::make_unique<StatefulMdrSimulator>(xm);
            HeadphoneController xmController(xm, std::move(xmSim));
            const bool xmConnected = xmController.connectDevice("AC:80:0A:12:34:57");
            const bool ultRejected = xmConnected && !xmController.setUltBassMode(1);
            requireTrue(ultRejected && !xmController.getLastError().empty(),
                        "非 ULT 机型：没有 ULT 维度，setUltBassMode 明确拒绝并给出原因");
            const bool xmEqOk = xmConnected && xmController.setEqualizer({EqPresetId::CUSTOM_1, 6, {1, 2, 3, 4, 5}});
            requireTrue(xmEqOk && xmController.getEqualizer().preset == EqPresetId::CUSTOM_1 &&
                        xmController.getEqualizer().clearBass == 6,
                        "非 ULT 机型：EQ 预设全表（含 Custom 1）走 10 字节帧并正确回读");
            printDeviceDashboard(xmController);
        }
    }

    // =====================================================================
    // 功能 A 自检：模拟设备发现（假设备列表必须可测）、MAC 文本归一化与配对文案分支
    // =====================================================================
    {
        SimulatedDeviceDiscovery discovery;
        const DeviceScanResult scan = discovery.scan();
        requireTrue(scan.status == DeviceScanStatus::SIMULATED && scan.devices.size() == 4,
                    "模拟设备发现：--sim 返回固定的 4 个假蓝牙音频设备（回归可跑）",
                    "status=" + std::to_string(static_cast<int>(scan.status)) +
                        " count=" + std::to_string(scan.devices.size()));

        bool allMacsValid = !scan.devices.empty();
        std::array<uint8_t, 6> macBytes{};
        for (const auto& device : scan.devices) {
            allMacsValid = allMacsValid && validateBluetoothMacAddress(device.macAddress, macBytes);
        }
        requireTrue(allMacsValid, "模拟设备发现：每个假设备 MAC 都通过严格校验，可直接填进连接输入框");

        requireTrue(!scan.devices.empty() && scan.devices[0].simulated &&
                    scan.devices[0].suggestedRfcommChannel == 18 && scan.devices[0].connected,
                    "模拟设备发现：首个假设备为 ULT WEAR，建议通道 18 且标记为已连接");

        requireTrue(normalizeBluetoothMacText("ac-80-0a-12-34-56") == "AC:80:0A:12:34:56" &&
                    normalizeBluetoothMacText("AC:80:0A:12:34:56") == "AC:80:0A:12:34:56" &&
                    normalizeBluetoothMacText("aabbccddeeff") == "AA:BB:CC:DD:EE:FF" &&
                    normalizeBluetoothMacText("AC:80:0A:12:34:5").empty() &&
                    normalizeBluetoothMacText("AC:80:0A:12:34:ZZ").empty(),
                    "MAC 文本归一化：兼容无分隔/短横线/冒号写法，拒绝长度错误与非十六进制字符");

        requireTrue(formatMacFromAddress(0x9C19C21F16ECULL) == "9C:19:C2:1F:16:EC" &&
                    addressFromMacBytes({0x9C, 0x19, 0xC2, 0x1F, 0x16, 0xEC}) == 0x9C19C21F16ECULL,
                    "蓝牙地址编解码：ullLong 的 12 位十六进制即系统显示顺序（与 BTHPORT 注册表子键一致）");

        requireTrue(classOfDeviceIsAudio(0x240418) && classOfDeviceIsAudio(0x240404) &&
                    !classOfDeviceIsAudio(0x00010C),
                    "蓝牙设备类判定：Class of Device major=0x04 判为音频类，鼠标/键盘类不误判");

        std::string pairOutcome;
        const bool simulatedPairOk = discovery.pairDevice(scan.devices[0], nullptr, pairOutcome);
        requireTrue(simulatedPairOk && pairOutcome.find("（模拟）") == 0,
                    "模拟配对：返回带（模拟）前缀的明确文案，不冒充真实配对成功", pairOutcome);

        BluetoothAudioDevice badMacDevice;
        badMacDevice.macAddress = "AC:80:0A:12:34"; // 故意少一段
        std::string badOutcome;
        requireTrue(!discovery.pairDevice(badMacDevice, nullptr, badOutcome) && !badOutcome.empty(),
                    "模拟配对：非法 MAC 直接失败并给出原因，不静默返回成功", badOutcome);
    }

    // =====================================================================
    // 功能 B 自检：0x56 GET → 0x57 RET 回读刷新；0x59 NTFY 主动上报解析
    // =====================================================================
    {
        const auto profile = getProfileByModelCode("XM5");
        auto sim = std::make_unique<StatefulMdrSimulator>(profile);
        auto* simRaw = sim.get();
        HeadphoneController controller(profile, std::move(sim));
        const bool connected = controller.connectDevice("AC:80:0A:12:34:56");
        requireTrue(connected, "EQ 同步前置：XM5 模拟连接成功", controller.getLastError());

        // 实机对 0x58 SET 也会回一帧 0x57（仿真器如实建模）。因此"仅下发 SET"这条路径
        // 的缓存刷新同样来自设备回包，而不是本地猜测——写后缓存必须等于设备回来的新值。
        const EqualizerConfig hiddenTarget{EqPresetId::BRIGHT, 5, {6, 4, 2, 0, -3}};
        const bool rawSetOk = connected &&
            controller.sendRawPayloadForTest(MdrProtocol::buildEqualizerSetPayload(profile, hiddenTarget), std::nullopt);
        requireTrue(rawSetOk && controller.getEqualizer().preset == hiddenTarget.preset &&
                    controller.getEqualizer().clearBass == hiddenTarget.clearBass &&
                    controller.getEqualizer().bands == hiddenTarget.bands,
                    "EQ 写入前置：仅下发 0x58（不自带 GET）时，写入后的缓存即为设备 0x57 回包里的新值",
                    controller.getLastError());

        const bool syncOk = connected && controller.syncEqualizerState();
        requireTrue(syncOk && controller.getEqualizer().preset == EqPresetId::BRIGHT &&
                    controller.getEqualizer().clearBass == 5 &&
                    controller.getEqualizer().bands == hiddenTarget.bands,
                    "EQ 同步：0x56 GET → 0x57 RET 回读刷新预设 / Clear Bass / 5 段增益",
                    controller.getLastError());

        // 0x59 NTFY：设备主动上报（不经任何指令）也要被解析
        const std::vector<uint8_t> eqNotification{
            static_cast<uint8_t>(Command::EQEBB_NTFY_PARAM), profile.eqInquiredType,
            static_cast<uint8_t>(EqPresetId::JAZZ), 0x06, 0x0A, 0x0C, 0x0B, 0x09, 0x08, 0x07};
        const bool pushed = simRaw->pushUnsolicitedNotificationForTest(eqNotification);
        const int applied = pushed ? controller.pollDeviceNotifications() : 0;
        requireTrue(pushed && applied >= 1 &&
                    controller.getEqualizer().preset == EqPresetId::JAZZ &&
                    controller.getEqualizer().clearBass == 0 &&
                    controller.getEqualizer().bands[0] == 2 && controller.getEqualizer().bands[4] == -3,
                    "EQ 通知：主动推送的 0x59 帧被解析并刷新状态（无需先发指令）",
                    controller.getLastError());

        std::vector<uint8_t> truncatedNotification{
            static_cast<uint8_t>(Command::EQEBB_NTFY_PARAM), profile.eqInquiredType,
            static_cast<uint8_t>(EqPresetId::VOCAL)}; // 只有 3 字节，缺 0x06 与增益
        const bool pushedBad = simRaw->pushUnsolicitedNotificationForTest(truncatedNotification);
        const int appliedBad = pushedBad ? controller.pollDeviceNotifications() : 0;
        requireTrue(pushedBad && appliedBad == 0 && controller.getEqualizer().preset == EqPresetId::JAZZ,
                    "EQ 通知负向拦截：字段残缺的 0x59 帧被拒绝，不污染已有状态");
    }

    // ULT WEAR 的 EQ 同步走 eqInquiredType 0x03：0x57 回包同时携带预设/五段增益与 ULT 模式位
    {
        const auto ultProfile = getProfileByModelCode("ULT");
        auto sim = std::make_unique<StatefulMdrSimulator>(ultProfile);
        HeadphoneController controller(ultProfile, std::move(sim));
        const bool connected = controller.connectDevice("AA:BB:CC:DD:EE:FF");
        const bool rawSetOk = connected &&
            controller.sendRawPayloadForTest(
                MdrProtocol::buildUltBassSetPayload(ultProfile, EqualizerConfig{}, 1), std::nullopt);
        requireTrue(rawSetOk && controller.getUltBassMode() == 1,
                    "ULT 同步前置：仅下发 0x58 时 ULT 模式已由设备 0x57 回包刷新（不再靠后续 GET）",
                    controller.getLastError());
        const bool synced = connected && controller.syncEqualizerState();
        requireTrue(synced && controller.getUltBassMode() == 1,
                    "ULT WEAR EQ 同步：0x56 子类型 0x03 → 0x57 回读 ULT 模式位", controller.getLastError());
        requireTrue(synced && controller.getEqualizer().preset == EqPresetId::OFF,
                    "ULT WEAR EQ 同步：同一 0x57 回包同时回读 EQ 预设（两维一次同步）",
                    controller.getLastError());
    }

    // =====================================================================
    // P0 根因回归（2026-10 实机定位）：设备对 0x58 SET 也会回一帧 0x57。
    //   旧实现 SET 只等 ACK，这帧 0x57 残留在 rxParser_，被紧接的 0x56 GET
    //   当成自己的回包 → 真回读被推迟一次命令，UI 每次应用后显示上一轮值。
    //   修复①：事务发送前先消费掉所有残留帧；修复②：SET 直接以 0x57 为期望回包。
    //   下面每条断言都对应一个会真实漏掉的回归点。
    // =====================================================================
    {
        const auto profile = getProfileByModelCode("XM5");
        auto sim = std::make_unique<StatefulMdrSimulator>(profile);
        auto* simRaw = sim.get();
        // 极端分片：强制 SET 的 0x57 与 ACK 分不同批次到达（复现实机"回包晚到"的时序）
        simRaw->setForceFragmentedOutput(true);
        HeadphoneController controller(profile, std::move(sim));
        const bool connected = controller.connectDevice("AC:80:0A:12:34:56");
        requireTrue(connected, "P0 慢一拍回归前置：XM5 模拟连接成功（5 字节分片流）", controller.getLastError());

        const EqualizerConfig target{EqPresetId::USER_DEFINED, 4, {5, 3, 1, -2, -4}};
        const size_t framesBefore = simRaw->getDeviceFramesSentCount();
        const bool setOk = connected && controller.setEqualizer(target);
        requireTrue(setOk && controller.getEqualizer().preset == EqPresetId::USER_DEFINED &&
                    controller.getEqualizer().clearBass == 4 &&
                    controller.getEqualizer().bands == target.bands,
                    "P0 慢一拍：setEqualizer 返回时缓存已是本次写入的新值（SET 自带的 0x57 即回读结果）",
                    controller.getLastError());
        requireTrue(simRaw->getDeviceFramesSentCount() == framesBefore + 1,
                    "P0 慢一拍：一次 EQ 写入只产生一帧设备数据回包（写入事务自带回读，不再另发 GET）",
                    "sentDelta=" + std::to_string(simRaw->getDeviceFramesSentCount() - framesBefore));

        const bool syncOk = controller.syncEqualizerState();
        requireTrue(syncOk && controller.getEqualizer().preset == EqPresetId::USER_DEFINED &&
                    controller.getEqualizer().clearBass == 4 &&
                    controller.getEqualizer().bands == target.bands,
                    "P0 慢一拍：写后第一次 syncEqualizerState 即为最新值（拖到第二拍才对即回归）",
                    controller.getLastError());

        // 残留帧场景：设备侧把上一轮的 0x57 放进接收缓冲（等效实机 SET 回包晚到），
        // 旧实现里这帧旧值会被下一次 GET 当成应答。新实现必须先消费掉它。
        const std::vector<uint8_t> staleReply{
            static_cast<uint8_t>(Command::EQEBB_RET_PARAM), profile.eqInquiredType,
            static_cast<uint8_t>(EqPresetId::JAZZ), 0x06, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A, 0x0A};
        const bool stalePushed = simRaw->pushUnsolicitedNotificationForTest(staleReply);
        const EqualizerConfig fresh{EqPresetId::BASS_BOOST, 6, {3, 2, 1, 0, -1}};
        const bool freshSet = stalePushed && controller.setEqualizer(fresh);
        requireTrue(freshSet && controller.getEqualizer().preset == EqPresetId::BASS_BOOST &&
                    controller.getEqualizer().clearBass == 6 &&
                    controller.getEqualizer().bands == fresh.bands,
                    "P0 慢一拍：缓冲里的旧 0x57 不会被写入事务当成应答（写入结果仍是本次新值）",
                    controller.getLastError());
        requireTrue(simRaw->didHostAcknowledgeAllDeviceFrames(),
                    "P0 慢一拍：残留帧被消费并回 ACK（设备侧不会卡在未确认帧上）",
                    "sent=" + std::to_string(simRaw->getDeviceFramesSentCount()) +
                        " acked=" + std::to_string(simRaw->getDeviceFramesAckedCount()));

        // 再塞一帧旧值 + 只发 GET（不写）：回读必须来自设备当前真实状态，不是残留帧里的旧值
        const bool stalePushed2 = simRaw->pushUnsolicitedNotificationForTest(staleReply);
        const bool syncAgain = stalePushed2 && controller.syncEqualizerState();
        requireTrue(syncAgain && controller.getEqualizer().preset == EqPresetId::BASS_BOOST &&
                    controller.getEqualizer().clearBass == 6 &&
                    controller.getEqualizer().bands == fresh.bands,
                    "P0 慢一拍：0x56 GET 不吃残留旧帧（sync 结果等于设备当前真实状态）",
                    controller.getLastError());
        requireTrue(controller.pollDeviceNotifications() == 0 &&
                    simRaw->didHostAcknowledgeAllDeviceFrames(),
                    "P0 慢一拍：清理后设备→主机队列已无残留帧（不会被下下一次命令再吃一次）");

        // ULT / 非 ULT 两条路径必须完全一致
        const auto ultProfile = getProfileByModelCode("ULT");
        auto ultSim = std::make_unique<StatefulMdrSimulator>(ultProfile);
        auto* ultSimRaw = ultSim.get();
        HeadphoneController ultController(ultProfile, std::move(ultSim));
        const bool ultConnected = ultController.connectDevice("AA:BB:CC:DD:EE:77");
        const size_t ultFramesBefore = ultSimRaw->getDeviceFramesSentCount();
        const bool ultSet = ultConnected && ultController.setEqualizer({EqPresetId::VOCAL, -2, {1, 2, 3, 4, 5}});
        const bool ultSync = ultSet && ultController.syncEqualizerState();
        requireTrue(ultSet && ultSync &&
                    ultController.getEqualizer().preset == EqPresetId::VOCAL &&
                    ultController.getEqualizer().clearBass == -2 &&
                    ultController.getEqualizer().bands[4] == 5 &&
                    ultSimRaw->getDeviceFramesSentCount() == ultFramesBefore + 2,
                    "P0 慢一拍：ULT 与非 ULT 路径一致——ULT 组合帧 SET 自带 0x57，首次 sync 即最新值",
                    ultController.getLastError());
    }

    // =====================================================================
    // P1 自检：本地设置持久化（纯本地文件，不涉及任何蓝牙报文）
    // =====================================================================
    {
        PersistedState wanted{};
        wanted.modelCode = "ULT";
        wanted.macAddress = "AA:BB:CC:DD:EE:FF";
        wanted.ultBassMode = 2;
        wanted.equalizer.preset = EqPresetId::CUSTOM_2;
        wanted.equalizer.clearBass = -4;
        wanted.equalizer.bands = {-3, -1, 0, 2, 5};
        wanted.ambient.mode = NcAsmMode::AMBIENT_SOUND;
        wanted.ambient.ambientLevel = 9;
        wanted.ambient.voicePassthrough = true;
        wanted.hasAmbient = true;

        const std::string text = serializePersistedState(wanted);
        PersistedState reloaded{};
        requireTrue(parsePersistedState(text, reloaded) && reloaded.valid &&
                    reloaded.modelCode == "ULT" && reloaded.ultBassMode == 2 &&
                    reloaded.equalizer.preset == EqPresetId::CUSTOM_2 &&
                    reloaded.equalizer.clearBass == -4 &&
                    reloaded.equalizer.bands[4] == 5 &&
                    reloaded.hasAmbient && reloaded.ambient.ambientLevel == 9 &&
                    reloaded.ambient.voicePassthrough,
                    "P1 本地设置：序列化→解析可完整还原机型/MAC/ULT 模式/EQ（预设+Clear Bass+五段）/环境音");

        // 两维同时保存在同一个文件里：ULT 模式不覆盖 EQ，反之亦然
        PersistedState dual{};
        const bool dualOk = parsePersistedState(text, dual) &&
                            dual.ultBassMode == 2 &&
                            dual.equalizer.preset == EqPresetId::CUSTOM_2 &&
                            dual.equalizer.clearBass == -4;
        requireTrue(dualOk,
                    "P1 本地设置：ULT 维度与 EQ 维度共同落盘，恢复时两维同时生效（不再二选一）");

        PersistedState rejected{};
        requireTrue(!parsePersistedState("version=2\nmodel=ULT\npreset=0x00\nbands=0,0,0,0,0\n", rejected),
                    "P1 本地设置负向：版本号不符的文件被拒绝（不做静默升级猜测）");
        requireTrue(!parsePersistedState("version=1\nmodel=ULT\npreset=0x00\nbands=0,0,0,0\n", rejected),
                    "P1 本地设置负向：五段增音只有 4 个值被拒绝");
        requireTrue(!parsePersistedState("version=1\nmodel=ULT\npreset=0x00\nbands=0,0,0,0,11\n", rejected),
                    "P1 本地设置负向：频段增益越界（+11 dB）被拒绝");
        requireTrue(!parsePersistedState("version=1\nmodel=NOT-A-MODEL\npreset=0x00\nbands=0,0,0,0,0\n", rejected),
                    "P1 本地设置负向：未知机型码被拒绝，不静默回落到默认机型");
        requireTrue(!parsePersistedState("version=1\nbytes-without-separator\n", rejected),
                    "P1 本地设置负向：缺少 key=value 分隔符的行使整份文件无效");

        const char* tempDir = std::getenv("TEMP");
        const std::string path = std::string(tempDir ? tempDir : ".") + "/sony_ui_state_selftest.txt";
        const bool saved = savePersistedState(path, wanted);
        PersistedState fromDisk{};
        const bool loaded = saved && loadPersistedState(path, fromDisk) && fromDisk.valid &&
                            fromDisk.ultBassMode == 2 && fromDisk.equalizer.bands[4] == 5;
        std::remove(path.c_str());
        std::remove((path + ".tmp").c_str());
        requireTrue(loaded, "P1 本地设置：落盘→读回完整往返（原子写：先 .tmp 再替换）");

        // 快照构建：从控制器取两维状态（ULT WEAR 上量维必须同时出现）
        const auto ultProfile2 = getProfileByModelCode("ULT");
        auto snapSim = std::make_unique<StatefulMdrSimulator>(ultProfile2);
        HeadphoneController snapController(ultProfile2, std::move(snapSim));
        const bool snapConnected = snapController.connectDevice("AA:BB:CC:DD:EE:02");
        const bool snapWritten = snapConnected && snapController.setUltBassMode(1) &&
                                 snapController.setEqualizer({EqPresetId::JAZZ, 3, {1, 0, -1, 0, 2}});
        const PersistedState snapshot = snapWritten
            ? makePersistedState(snapController, "ULT", "AA:BB:CC:DD:EE:02")
            : PersistedState{};
        requireTrue(snapWritten && snapshot.valid && snapshot.ultBassMode == 1 &&
                    snapshot.equalizer.preset == EqPresetId::JAZZ &&
                    snapshot.equalizer.bands[2] == -1,
                    "P1 本地设置：控制器快照同时记录 ULT 模式与 EQ 两维（重启后可原样恢复）");

        // ---- 6.6 取证启动参数解析（UI 落地前先在纯函数上锁死语义）----
        {
            const ShotStartupSpec spec = parseShotStartupArgs(
                {"SonySoundConnectUI.exe", "--page=2", "--model=ULT", "--ult=2", "--preset=4",
                 "--connect-sim"});
            requireTrue(spec.page == 2 && spec.modelCode == "ULT" && spec.ultBassMode == 2 &&
                        spec.presetIndex == 4 && spec.connectSim && spec.rejectNote.empty(),
                        "启动参数：页面/机型/ULT 模式/预设/模拟连接五项都能正确解析");

            const ShotStartupSpec bad = parseShotStartupArgs(
                {"--page=9", "--model=NOPE", "--ult=3", "--preset=99", "--preset=x"});
            requireTrue(bad.page == -1 && bad.modelCode.empty() && bad.ultBassMode == -1 &&
                        bad.presetIndex == -1 && !bad.rejectNote.empty(),
                        "启动参数负向：越界页面/未知机型/非法 ULT 模式/越界预设全部拒绝并留痕");

            const ShotStartupSpec tolerant = parseShotStartupArgs(
                {"--ult=0", "--preset=0", "--page=3"});
            requireTrue(tolerant.ultBassMode == 0 && tolerant.presetIndex == 0 && tolerant.page == 3,
                        "启动参数：0 值不会被误判成\"未指定\"（0 与 -1 语义分离）");

            requireTrue(kEqPresetOptionCount == 15,
                        "启动参数：预设索引上界 15 与 UI presetOptions() 条目数保持一致（改动需同步）");        }
    }

    if (failedAssertions == 0) {
        std::cout << "\n[自检结果: 全部通过] 本地模拟器中的拆帧、序号、载荷校验与回读断言均通过。\n"
                  << "[限制说明] 这不等于真实 Sony 耳机协议已实机验证；真实命令字、通道及状态载荷仍需按型号测试。\n"
                  << "(Exit Code = 0)\n";
        return 0;
    } else {
        std::cerr << "\n[自检结果: 失败] 共发现 " << failedAssertions << " 项断言未通过！(Exit Code = 1)\n";
        return 1;
    }
}

bool parseStrictInt(const std::string& text, int minValue, int maxValue, int& outValue) {
    if (text.empty()) return false;
    int value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end || value < minValue || value > maxValue) {
        return false;
    }
    outValue = value;
    return true;
}

void printUsage(const char* progName) {
    std::cout << "用法:\n"
              << "  1. 运行严格协议与仿真自检套件:\n"
              << "     " << progName << " --test-sim   (或 --test)\n\n"
              << "  2. 启动交互式有状态模拟模式:\n"
              << "     " << progName << " --sim [--model ULT|XM5|XM4|WF-XM5]\n\n"
              << "  3. 连接真实索尼蓝牙耳机 (需在编译时开启 -DENABLE_NATIVE_BT):\n"
              << "     " << progName << " --real <蓝牙MAC地址> --channel <RFCOMM通道> --model ULT|XM5|XM4|WF-XM5\n"
              << "     示例: " << progName << " --real AA:BB:CC:DD:EE:FF --channel 18 --model ULT\n"
              << "     注意: 真实模式必须显式指定通道和机型。\n\n"
              << "  4. 扫描本机蓝牙设备（功能 A：取名称+MAC+连接状态，不建立连接）:\n"
              << "     " << progName << " --scan            列出模拟假设备列表（不访问蓝牙硬件）\n"
              << "     " << progName << " --scan-real       枚举本机真实已配对/已知蓝牙设备 (需要 -lbthprops)\n"
              << "     说明: RFCOMM 通道是按键名推测的建议值，可在设备页上方手改。\n";
}

} // namespace

int main(int argc, char* argv[]) {
    std::string mode = "--sim";
    std::string targetMac = "AC:80:0A:12:34:56";
    std::string modelCode = "ULT";
    int rfcommChannel = -1;
    bool modelWasSpecified = false;
    bool channelWasSpecified = false;
    bool scanRequested = false;
    bool scanNative = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--test" || arg == "--test-sim") {
            return runStrictSimulationSelfTests();
        }
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
        if (arg == "--scan") {
            scanRequested = true;
            scanNative = false;
            continue;
        }
        if (arg == "--scan-real") {
            scanRequested = true;
            scanNative = true;
            continue;
        }
        if (arg == "--sim") {
            mode = "--sim";
            continue;
        }
        if (arg == "--real") {
            mode = "--real";
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                std::cerr << "[参数错误] --real 模式必须提供目标耳机的蓝牙 MAC 地址。\n";
                printUsage(argv[0]);
                return 2;
            }
            targetMac = argv[++i];
            continue;
        }
        if (arg == "--channel") {
            if (i + 1 >= argc) {
                std::cerr << "[参数错误] --channel 缺少通道号。\n";
                return 2;
            }
            if (!parseStrictInt(argv[++i], 1, 30, rfcommChannel)) {
                std::cerr << "[参数错误] RFCOMM 通道必须是 1 到 30 的整数。\n";
                return 2;
            }
            channelWasSpecified = true;
            continue;
        }
        if (arg == "--model") {
            if (i + 1 >= argc || argv[i + 1][0] == '-') {
                std::cerr << "[参数错误] --model 缺少机型代码。\n";
                return 2;
            }
            modelCode = argv[++i];
            modelWasSpecified = true;
            continue;
        }
        std::cerr << "[参数错误] 未知命令行参数: " << arg << "\n";
        printUsage(argv[0]);
        return 2;
    }

    // 功能 A 的命令行入口：只做发现与打印，不建立 RFCOMM 连接。
    // 模拟列表用于无蓝牙硬件的 CI/回归机；--scan-real 走 bthprops 枚举真实适配器。
    if (scanRequested) {
        auto discovery = makeDeviceDiscovery(scanNative);
        const DeviceScanResult scanResult = discovery->scan();
        printDeviceScanResult(scanResult, std::cout);
        if (!scanResult.succeeded()) {
            std::cerr << "[扫描失败] " << deviceScanStatusLabel(scanResult.status) << "："
                      << scanResult.message << "\n";
            return 1;
        }
        return 0;
    }

    if (mode == "--real" && !modelWasSpecified) {
        std::cerr << "[参数错误] 真实连接必须显式指定 --model，避免使用错误机型协议。\n";
        printUsage(argv[0]);
        return 2;
    }
    if (mode == "--real" && !channelWasSpecified) {
        std::cerr << "[参数错误] 真实连接必须显式提供 --channel（1–30，建议值可在界面手改）。\n";
        printUsage(argv[0]);
        return 2;
    }

    DeviceModelProfile profile;
    try {
        profile = getProfileByModelCode(modelCode);
    } catch (const std::invalid_argument& e) {
        std::cerr << "[参数错误] " << e.what() << "\n";
        return 2;
    }
    std::unique_ptr<IBluetoothConnector> connector;

    if (mode == "--real") {
        std::cerr << "[重要提示] 原生 RFCOMM 连接仅证明传输层可连接；Sony 各机型命令字/载荷仍需实机抓包验证。"
                  << " 本版本中的远程关机操作码 0x25 尤未确认，不应视为已验证功能。\n";
#if defined(ENABLE_NATIVE_BT) && defined(_WIN32)
        connector = std::make_unique<WindowsBthConnector>();
#elif defined(ENABLE_NATIVE_BT) && defined(__linux__)
        connector = std::make_unique<LinuxBlueZConnector>();
#else
        std::cerr << "[错误] 当前可执行文件未启用原生蓝牙支持，无法连接真实硬件。\n"
                  << "       请在 Windows 或 Linux 下使用 -DENABLE_NATIVE_BT 重新编译后再使用 --real 模式。\n";
        return 1;
#endif
    } else {
        std::cout << "[提示] 当前以【有状态协议模拟模式 (--sim)】启动。如需连接真实耳机，请使用 --real <MAC>。\n";
        connector = std::make_unique<StatefulMdrSimulator>(profile);
    }

    HeadphoneController controller(profile, std::move(connector));
    if (!controller.connectDevice(targetMac, rfcommChannel)) {
        std::cerr << "[连接失败] 无法连接到设备 " << targetMac << " -> " << controller.getLastError() << "\n";
        return 1;
    }

    printDeviceDashboard(controller);

    while (controller.isConnected()) {
        const bool isUltWear = controller.getModelProfile().isUltWear;
        if (isUltWear) {
            std::cout << "\n单设备控制菜单 (" << controller.getModelProfile().displayName << "):\n"
                      << "  1. 切换降噪 / 环境音 / 关闭\n"
                      << "  2. 切换 ULT Bass (0=关闭, 1=ULT 1, 2=ULT 2)\n"
                      << "  3. 一键应用场景预设（ULT WEAR 适配版）\n"
                      << "  4. 重新读取电量、降噪与 ULT Bass 状态\n"
                      << "  0. 退出程序\n";
        } else {
            std::cout << "\n单设备控制菜单 (" << controller.getModelProfile().displayName << "):\n"
                      << "  1. 调整环境声音量 / 降噪模式 (ANC/ASM)\n"
                      << "  2. 调整 EQ 均衡器 (预设 / 5频段 / Clear Bass)\n"
                      << "  3. 设置智能免摘对话 (Speak-to-Chat)\n"
                      << "  4. 设置音频与硬件选项 (LDAC模式 / DSEE Extreme / 佩戴感应)\n"
                      << "  5. 一键应用桌面端场景预设 (专注 / 办公 / 通勤 / Hi-Fi)\n"
                      << "  6. 从设备重新回读全部状态与电量\n"
                      << "  7. 远程关机（真实设备未验证，当前阻止发送）\n"
                      << "  0. 退出程序\n";
        }
        int choice = readCheckedInt(isUltWear ? "请选择 [0-4]: " : "请选择 [0-7]: ", 0, isUltWear ? 4 : 7);
        if (choice == 0) {
            break;
        }

        bool ok = true;
        switch (choice) {
            case 1: {
                int m = readCheckedInt("输入模式 (0=关闭, 1=降噪ANC, 2=环境音ASM): ", 0, 2);
                int level = 10;
                int voice = 0;
                if (m == 2) {
                    if (!controller.getModelProfile().isUltWear) {
                        level = readCheckedInt("输入环境声音量 (1~20): ", 1, 20);
                    }
                    voice = readCheckedInt("开启关注语音? (0=否, 1=是): ", 0, 1);
                }
                ok = controller.setAmbientSound({
                    static_cast<NcAsmMode>(m),
                    static_cast<uint8_t>(level),
                    voice != 0
                });
                break;
            }
            case 2: {
                if (controller.getModelProfile().isUltWear) {
                    const int mode = readCheckedInt("输入 ULT Bass 模式 (0=关闭, 1=ULT 1, 2=ULT 2): ", 0, 2);
                    ok = controller.setUltBassMode(static_cast<uint8_t>(mode));
                    break;
                }
                EqualizerConfig eq;
                int presetChoice = readCheckedInt(
                    "选择预设 (0=Off, 1=Custom1[0xA0], 2=Custom2[0xA1], 3=UserDefined[0xA2]): ", 0, 3);
                eq.preset = (presetChoice == 0) ? EqPresetId::OFF :
                            (presetChoice == 1) ? EqPresetId::CUSTOM_1 :
                            (presetChoice == 2) ? EqPresetId::CUSTOM_2 : EqPresetId::USER_DEFINED;

                // 先在 int 范围校验 [-10, 10]，再安全转换为 int8_t，防止截断溢出
                int cb = readCheckedInt("输入 Clear Bass 增益 (-10 ~ +10): ", -10, 10);
                eq.clearBass = static_cast<int8_t>(cb);

                const char* labels[5] = {"400Hz", "1kHz", "2.5kHz", "6.3kHz", "16kHz"};
                for (size_t i = 0; i < 5; ++i) {
                    int val = readCheckedInt(std::string("输入 ") + labels[i] + " 增益 (-10 ~ +10): ", -10, 10);
                    eq.bands[i] = static_cast<int8_t>(val);
                }
                ok = controller.setEqualizer(eq);
                break;
            }
            case 3: {
                if (controller.getModelProfile().isUltWear) {
                    const int p = readCheckedInt("选择场景 (0=深度专注, 1=办公协作, 2=街头通勤, 3=ULT 低频沉浸): ", 0, 3);
                    const auto report = controller.applySceneProfile(static_cast<SceneProfile>(p));
                    std::cout << "场景执行：" << report.succeededSteps << "/" << report.totalSteps << " 项成功\n";
                    for (const auto& line : report.stepLogs) std::cout << "  " << line << "\n";
                    printDeviceDashboard(controller);
                    continue;
                }
                int en = readCheckedInt("开启智能免摘对话? (0=关闭, 1=开启): ", 0, 1);
                int sens = readCheckedInt("检测灵敏度 (0=自动, 1=高, 2=低): ", 0, 2);
                ok = controller.setSpeakToChat({
                    en != 0,
                    static_cast<SpeakToChatSensitivity>(sens),
                    SpeakToChatTimeout::STANDARD
                });
                break;
            }
            case 4: {
                if (controller.getModelProfile().isUltWear) {
                    ok = controller.refreshAllDeviceStates();
                    break;
                }
                int ldac = readCheckedInt("蓝牙连接模式 (0=音质优先LDAC, 1=连接稳定优先): ", 0, 1);
                int dsee = readCheckedInt("开启 DSEE Extreme? (0=关闭, 1=开启): ", 0, 1);
                int wear = readCheckedInt("开启取下耳机自动暂停? (0=关闭, 1=开启): ", 0, 1);
                ok = controller.setBluetoothConnectionQuality(
                         ldac == 0 ? BluetoothConnectionQuality::PRIORITIZE_SOUND_QUALITY
                                   : BluetoothConnectionQuality::PRIORITIZE_STABLE_CONNECTION) &&
                     controller.setDseeExtreme(dsee != 0) &&
                     controller.setWearDetection(wear != 0);
                break;
            }
            case 5: {
                int prof = readCheckedInt("选择场景预设 (0=深度专注, 1=办公协作, 2=街头通勤, 3=Hi-Fi鉴赏): ", 0, 3);
                auto report = controller.applySceneProfile(static_cast<SceneProfile>(prof));
                for (const auto& line : report.stepLogs) {
                    std::cout << "  " << line << "\n";
                }
                ok = report.allSucceeded();
                break;
            }
            case 6: {
                ok = controller.refreshAllDeviceStates();
                break;
            }
            case 7: {
                ok = controller.powerOffHeadphone();
                if (ok) {
                    if (controller.isSimulationMode()) {
                        std::cout << ">> 模拟器已确认关机命令，模拟连接已关闭。\n";
                    } else {
                        std::cout << ">> 已收到关机命令 ACK，本地连接已关闭；耳机是否实际关机仍需实机确认。\n";
                    }
                }
                break;
            }
            default:
                break;
        }

        if (!ok) {
            std::cerr << "[操作失败] " << controller.getLastError() << "\n";
        } else if (controller.isConnected()) {
            printDeviceDashboard(controller);
        }
    }
    return 0;
}