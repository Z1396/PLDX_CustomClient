#pragma once  // 头文件包含守卫：防止重复包含

#include <pch.h>  // 预编译头(PCH)：集中包含常用标准库与第三方库
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
// [/pch-split]

// ============================================================================
// 文件：src/io/controller_input.h
// 层级：网络/IO 层 - 控制器输入解码（纯逻辑，无 IO 依赖）
// 职责：把外接 RS232 手柄的 30 字节私有载荷，翻译成「鼠标速度 + 键盘位 + 鼠标键」。
//
// 背景（2027 规则）：
//   2027 赛季把「自定义控制器 + 自定义客户端」合并为「自定义终端」，协议里
//   0x0302 / 0x0306 / 0x0309 三条控制器命令码全部删除，操作间也不再提供 RS232 口。
//   因此手柄只能作为「自定义终端自己的输入外设」：RS232 → USB 转串口 → 本机，
//   由本模块读出来，再通过 KeyboardMouseControl（或 CustomControl）发给机器人。
//
// 设计要点：
//   1) 载荷布局是**队伍私有约定**，官方不再规定。这里给一份默认布局，
//      必须和你们手柄固件的发送结构一致；不一致时改下面 4 个偏移常量即可。
//      先用 --serial-raw 把原始帧打成十六进制看一遍，就能快速核对。
//   2) 摇杆输出的是「速度」而不是「位移」：调用方按帧间隔积分，
//      这样手柄 50Hz 还是 100Hz 发，手感都一样。
//   3) 按键映射是一张 16 项表，改键位不用改逻辑。
// 依赖：无（仅标准库）。
// ============================================================================

namespace RoboGo::io::controller {  // 控制器输入命名空间

inline constexpr std::uint16_t kCmdIdControllerData = 0x0302;  // 旧「自定义控制器→机器人」命令码（多数手柄固件沿用它）
inline constexpr std::uint16_t kCmdIdKeyboardMouseSim = 0x0306;  // 旧「控制器模拟键鼠」命令码（2027 已删除，仅作识别提示）

// ---------------- 默认载荷布局（30 字节，小端） ----------------
// 偏移 0..1   int16   axis_x    左/右摇杆横向，-1000(左) .. +1000(右)
// 偏移 2..3   int16   axis_y    左/右摇杆纵向，-1000(上) .. +1000(下)
// 偏移 4..5   int16   axis_z    第三轴/滚轮，-1000 .. +1000（当前不映射，仅解码保留）
// 偏移 6..7   uint16  buttons   按键位图，位含义见 kDefaultButtonMap
// 偏移 8..9   uint16  switches  拨杆/档位（保留，当前不参与映射）
// 偏移 10..29         保留
inline constexpr std::size_t kAxisXOffset = 0;
inline constexpr std::size_t kAxisYOffset = 2;
inline constexpr std::size_t kAxisZOffset = 4;
inline constexpr std::size_t kButtonsOffset = 6;
inline constexpr std::size_t kSwitchesOffset = 8;
inline constexpr std::size_t kMinPayloadSize = 10;   // 至少要能解出前 5 个字段
inline constexpr std::size_t kRecommendedPayloadSize = 30;  // 与 0x0302 的 30 字节对齐

/**
 * @brief 从载荷里读一个小端 int16。
 * @param data 载荷；offset 字节偏移。
 * @return 主机序有符号 16 位值（C++20 起由补码保证，负数为二进制补码解释）。
 */
inline std::int16_t ReadInt16(const std::span<const std::uint8_t> data, const std::size_t offset) {
    const std::uint16_t raw = static_cast<std::uint16_t>(
            data[offset] | (static_cast<std::uint16_t>(data[offset + 1]) << 8));  // 小端拼接
    return static_cast<std::int16_t>(raw);  // 重新解释为有符号
}

/**
 * @brief 从载荷里读一个小端 uint16。
 */
inline std::uint16_t ReadUint16(const std::span<const std::uint8_t> data, const std::size_t offset) {
    return static_cast<std::uint16_t>(
            data[offset] | (static_cast<std::uint16_t>(data[offset + 1]) << 8));  // 小端拼接
}

/**
 * @brief 解码后的原始控制器状态。
 */
struct RawState {
    std::int16_t axis_x = 0;      // 横轴（-1000..1000）
    std::int16_t axis_y = 0;      // 纵轴（-1000..1000）
    std::int16_t axis_z = 0;      // 第三轴（-1000..1000）
    std::uint16_t buttons = 0;    // 按键位图
    std::uint16_t switches = 0;   // 拨杆/档位
    bool valid = false;           // 载荷长度是否足够
};

/**
 * @brief 按默认布局解码 30 字节载荷。
 * @param data 帧数据段。
 * @return 解码结果；长度不足时 valid=false，其余字段保持 0。
 */
inline RawState DecodeDefaultLayout(const std::span<const std::uint8_t> data) {
    RawState state;
    if (data.size() < kMinPayloadSize) {  // 长度不够，宁可不用也不要读越界
        return state;
    }
    state.axis_x = ReadInt16(data, kAxisXOffset);        // 横轴
    state.axis_y = ReadInt16(data, kAxisYOffset);        // 纵轴
    state.axis_z = ReadInt16(data, kAxisZOffset);        // 第三轴
    state.buttons = ReadUint16(data, kButtonsOffset);    // 按键
    state.switches = ReadUint16(data, kSwitchesOffset);  // 拨杆
    state.valid = true;
    return state;
}

// ---------------- 按键映射 ----------------
// 映射目标：kind=0 不映射；kind=1 键盘位（index=0..15，对应协议 W/S/A/D/Shift/Ctrl/Q/E/R/F/G/Z/X/C/V/B）；
//          kind=2 鼠标键（index=0 左键 / 1 右键 / 2 中键）。
struct ButtonTarget {
    std::uint8_t kind = 0;   // 目标类型
    std::uint8_t index = 0;  // 目标下标
};

// 默认映射（**按你们手柄的实际按键顺序改这张表**，逻辑不用动）：
//   bit0 扳机1 → 鼠标左键（开火）        bit1 扳机2 → 鼠标右键
//   bit2 → Shift（小陀螺/加速）          bit3 → Ctrl
//   bit4 → Q    bit5 → E    bit6 → R     bit7 → F
//   bit8 → W    bit9 → S    bit10 → A    bit11 → D
//   bit12 → G   bit13 → Z   bit14/15 保留
inline constexpr std::array<ButtonTarget, 16> kDefaultButtonMap = {{
        {2, 0},  // bit0  → 鼠标左键
        {2, 1},  // bit1  → 鼠标右键
        {1, 4},  // bit2  → Shift
        {1, 5},  // bit3  → Ctrl
        {1, 6},  // bit4  → Q
        {1, 7},  // bit5  → E
        {1, 8},  // bit6  → R
        {1, 9},  // bit7  → F
        {1, 0},  // bit8  → W
        {1, 1},  // bit9  → S
        {1, 2},  // bit10 → A
        {1, 3},  // bit11 → D
        {1, 10}, // bit12 → G
        {1, 11}, // bit13 → Z
        {0, 0},  // bit14 保留
        {0, 0},  // bit15 保留
}};

/**
 * @brief 映射后的输入事件。
 */
struct MappedInput {
    std::uint32_t keyboard_bits = 0;  // 键盘位掩码（bit0..15 对应协议键位）
    std::uint32_t mouse_buttons = 0;  // 鼠标键掩码（bit0 左 / bit1 右 / bit2 中，与 keyboard_mouse_state_manager 内部定义一致）
};

/**
 * @brief 把控制器的 16 位按键位图映射成键盘位与鼠标键。
 * @param raw_buttons 控制器按键位图。
 * @return 映射结果；未映射的位直接忽略。
 */
inline MappedInput MapButtons(const std::uint16_t raw_buttons) {
    MappedInput mapped;
    for (std::size_t bit = 0; bit < kDefaultButtonMap.size(); ++bit) {  // 逐位检查
        if ((raw_buttons & (1u << bit)) == 0) {  // 该位未按下
            continue;
        }
        const ButtonTarget &target = kDefaultButtonMap[bit];  // 取该位的映射目标
        if (target.kind == 1 && target.index < 16) {  // 键盘位
            mapped.keyboard_bits |= (1u << target.index);
        } else if (target.kind == 2 && target.index < 3) {  // 鼠标键
            mapped.mouse_buttons |= (1u << target.index);
        }
    }
    return mapped;
}

// ---------------- 摇杆 → 鼠标速度 ----------------
inline constexpr int kDefaultAxisDeadzone = 120;            // 死区（千分比）：±120 以内视为回中，抑制漂移
inline constexpr float kDefaultPixelsPerSecond = 900.0f;    // 满杆时每秒注入的鼠标像素（灵敏度=1 时）

/**
 * @brief 把摇杆轴值换算成「每秒像素速度」。
 * @param axis 轴值（-1000..1000）。
 * @param deadzone 死区（千分比）；pixels_per_second 满杆速度。
 * @return 速度（像素/秒，带符号）；在死区内返回 0。
 * @note 思路：先扣掉死区再归一化，保证刚出死区时是平滑的小速度，不会「一碰就满速」。
 */
inline float AxisToVelocity(const std::int16_t axis,
                            const int deadzone = kDefaultAxisDeadzone,
                            const float pixels_per_second = kDefaultPixelsPerSecond) {
    const int value = static_cast<int>(axis);  // 提升成 int 便于取绝对值
    const int magnitude = value < 0 ? -value : value;  // 手动取绝对值，避免引入 <cstdlib>
    if (magnitude <= deadzone) {  // 死区内
        return 0.0f;
    }
    const int span = 1000 - deadzone;  // 死区外的有效行程
    if (span <= 0) {  // 参数异常（死区 >= 1000）时不做映射
        return 0.0f;
    }
    float normalized = static_cast<float>(magnitude - deadzone) / static_cast<float>(span);  // 归一化到 0..1
    if (normalized > 1.0f) {  // 超量程（个别固件会给出 >1000 的值）
        normalized = 1.0f;
    }
    return (value < 0 ? -normalized : normalized) * pixels_per_second;  // 带符号输出
}

}  // namespace RoboGo::io::controller
