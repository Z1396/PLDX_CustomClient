#pragma once  // 头文件包含守卫：防止重复包含

#include <pch.h>  // 预编译头(PCH)：集中包含常用标准库与第三方库
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>
// [/pch-split]

// ============================================================================
// 文件：src/io/referee_frame.h
// 层级：网络/IO 层 - 协议编解码（纯逻辑，无任何 IO 依赖）
// 职责：实现裁判系统串口帧的 CRC8/CRC16 校验与增量式帧解析。
//       帧格式（2027 通信协议 1.1 节）：
//         SOF(1B, 0xA5) | data_length(2B, 小端) | seq(1B) | CRC8(1B)
//         | cmd_id(2B, 小端) | data(nB) | CRC16(2B, 小端)
//       · CRC8 覆盖 data_length + seq 三个字节，初值 0xFF，多项式 x^8+x^5+x^4+1；
//       · CRC16 覆盖从 data_length 起、到 data 末尾，初值 0xFFFF，多项式 0x1021。
// 设计要点：
//       1) 全部为 inline/constexpr，可在单元测试里直接调用，不需要链接额外目标文件；
//       2) 解析器是「缓冲区 + 重同步」模型：任何一帧校验失败只丢 1 个字节再重新找 SOF，
//          这样即使中途插拔、串口噪声、半帧粘包，也能自己恢复，不会永久错位；
//       3) 缓冲区有上限，避免对端一直发垃圾时内存无限增长。
// 依赖：无（仅标准库）。
// 注意：CRC 常量与算法出自官方协议附录一，改动前请先核对最新版协议。
// ============================================================================

namespace RoboGo::io::referee {  // 裁判系统串口协议命名空间

inline constexpr std::uint8_t kSof = 0xA5;               // 帧起始字节（固定值）
inline constexpr std::size_t kHeaderSize = 5;            // SOF + data_length(2) + seq + CRC8
inline constexpr std::size_t kCmdIdSize = 2;             // cmd_id 长度
inline constexpr std::size_t kTailSize = 2;              // 帧尾 CRC16 长度
inline constexpr std::size_t kMaxDataSize = 300;         // 单帧 data 上限（对齐 0x0310 的 300 字节）
inline constexpr std::size_t kOverhead = kHeaderSize + kCmdIdSize + kTailSize;  // 帧固定开销 9 字节

/**
 * @brief 生成 CRC8 查表（多项式 0x31，即 x^8+x^5+x^4+1）。
 * @note 思路：对 0..255 每个字节做 8 次移位，最高位为 1 时异或多项式，结果与官方 CRC8_TAB 完全一致。
 * @return 256 项查表；constexpr 在编译期算完，运行期零初始化开销。
 */
inline constexpr std::array<std::uint8_t, 256> MakeCrc8Table() {
    std::array<std::uint8_t, 256> table{};  // 零初始化后逐项填充
    for (std::size_t i = 0; i < table.size(); ++i) {  // 遍历全部 256 个输入字节
        std::uint8_t crc = static_cast<std::uint8_t>(i);  // 当前项从 i 开始
        for (int bit = 0; bit < 8; ++bit) {  // 逐位处理 8 次
            if ((crc & 0x80u) != 0) {  // 最高位为 1：左移后异或多项式
                crc = static_cast<std::uint8_t>((crc << 1) ^ 0x31u);
            } else {  // 最高位为 0：只左移
                crc = static_cast<std::uint8_t>(crc << 1);
            }
        }
        table[i] = crc;  // 记录该项
    }
    return table;  // 返回编译期生成的查表
}

inline constexpr auto kCrc8Table = MakeCrc8Table();  // 全局唯一 CRC8 查表（编译期常量）

/**
 * @brief 生成 CRC16 查表（多项式 0x1021，即 CRC-16/CCITT-FALSE）。
 * @return 256 项查表；与官方 CRC16_TAB 的生成方式一致。
 */
inline constexpr std::array<std::uint16_t, 256> MakeCrc16Table() {
    std::array<std::uint16_t, 256> table{};  // 零初始化
    for (std::size_t i = 0; i < table.size(); ++i) {  // 遍历 256 个高字节
        std::uint16_t crc = static_cast<std::uint16_t>(i << 8);  // 当前项从 i 左移 8 位开始
        for (int bit = 0; bit < 8; ++bit) {  // 逐位处理
            if ((crc & 0x8000u) != 0) {  // 最高位为 1：左移后异或多项式
                crc = static_cast<std::uint16_t>((crc << 1) ^ 0x1021u);
            } else {  // 最高位为 0：只左移
                crc = static_cast<std::uint16_t>(crc << 1);
            }
        }
        table[i] = crc;  // 记录该项
    }
    return table;  // 返回编译期生成的查表
}

inline constexpr auto kCrc16Table = MakeCrc16Table();  // 全局唯一 CRC16 查表（编译期常量）

/**
 * @brief 计算 CRC8（协议帧头校验）。
 * @param data 待校验数据首地址；len 字节数；init 初值（协议要求 0xFF）。
 * @return 8 位校验值。
 */
inline std::uint8_t Crc8(const std::uint8_t *data, const std::size_t len,
                         const std::uint8_t init = 0xFF) {
    std::uint8_t crc = init;  // 从初值开始
    for (std::size_t i = 0; i < len; ++i) {  // 逐字节滚动
        crc = kCrc8Table[static_cast<std::uint8_t>(crc ^ data[i])];  // 查表：当前余数异或输入字节作下标
    }
    return crc;  // 返回最终余数
}

/**
 * @brief 计算 CRC16（协议整帧校验）。
 * @param data 待校验数据首地址；len 字节数；init 初值（协议要求 0xFFFF）。
 * @return 16 位校验值。
 */
inline std::uint16_t Crc16(const std::uint8_t *data, const std::size_t len,
                           const std::uint16_t init = 0xFFFF) {
    std::uint16_t crc = init;  // 从初值开始
    for (std::size_t i = 0; i < len; ++i) {  // 逐字节滚动
        const std::uint16_t index = static_cast<std::uint16_t>(
                (crc >> 8) ^ static_cast<std::uint16_t>(data[i]));  // 高字节与输入字节异或作下标
        crc = static_cast<std::uint16_t>(
                (crc << 8) ^ kCrc16Table[index & 0x00FFu]);  // 左移 8 位后异或查表值
    }
    return crc;  // 返回最终余数
}

/**
 * @brief 一帧完整的裁判系统串口数据。
 */
struct Frame {
    std::uint8_t seq = 0;              // 帧序号（对端自增，用于丢帧观察）
    std::uint16_t cmd_id = 0;          // 命令码（小端解析后的主机序）
    std::vector<std::uint8_t> data;    // 数据段（长度由帧头 data_length 决定）
};

/**
 * @brief 增量式帧解析器：喂字节、取整帧，自动跳过噪声并重同步。
 * @note 用法：每次串口读到数据调用 Push()，随后反复 Pop() 直到返回 false。
 *       解析器不是线程安全的，只允许在 IO 线程（或单一消费者线程）里使用。
 */
class FrameParser {
public:
    /**
     * @brief 追加一段接收到的字节并尝试解析。
     * @param bytes 本次读到的原始字节（可以是半帧、多帧或纯噪声）。
     */
    void Push(const std::span<const std::uint8_t> bytes) {
        buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());  // 先入缓冲区
        ParseAvailable();  // 尽量多解析出完整帧
        TrimIfNeeded();    // 缓冲区异常膨胀时做一次截断，避免内存无限增长
    }

    /**
     * @brief 取出一帧。
     * @param out 出参：解析好的帧（仅返回 true 时有效）。
     * @return true 表示取到一帧；false 表示当前没有待取的帧。
     */
    [[nodiscard]] bool Pop(Frame &out) {
        if (pending_.empty()) {  // 没有待取帧
            return false;
        }
        out = std::move(pending_.front());  // 取出最早的一帧（保持到达顺序）
        pending_.pop_front();               // 从待取队列移除
        return true;
    }

    /**
     * @brief 清空全部内部状态（串口重连后调用，丢弃上一轮的半帧）。
     */
    void Clear() {
        buffer_.clear();
        pending_.clear();
    }

    [[nodiscard]] std::uint32_t CrcErrors() const { return crc_errors_; }        // 校验失败次数
    [[nodiscard]] std::uint32_t DroppedBytes() const { return dropped_bytes_; }  // 被丢弃的噪声字节数
    [[nodiscard]] std::size_t BufferedBytes() const { return buffer_.size(); }   // 当前缓冲区积压

private:
    /**
     * @brief 尽可能从缓冲区头部解析出完整帧。
     * @note 思路：循环执行「找 SOF → 校验长度 → 校验 CRC8 → 校验 CRC16 → 产出帧」；
     *       任一步失败只丢 1 个字节再重试，因此不会因一帧坏数据而永久错位。
     */
    void ParseAvailable() {
        while (true) {
            std::size_t first_sof = 0;  // 缓冲区里第一个 SOF 的位置
            while (first_sof < buffer_.size() && buffer_[first_sof] != kSof) {  // 跳过 SOF 之前的噪声
                ++first_sof;
            }
            if (first_sof > 0) {  // 前面确实有噪声
                dropped_bytes_ += static_cast<std::uint32_t>(first_sof);
                buffer_.erase(buffer_.begin(),
                              buffer_.begin() + static_cast<std::ptrdiff_t>(first_sof));  // 丢掉噪声
            }
            if (buffer_.size() < kHeaderSize) {  // 连帧头都不够，等下一次读
                return;
            }

            const std::uint16_t data_length = static_cast<std::uint16_t>(
                    buffer_[1] | (static_cast<std::uint16_t>(buffer_[2]) << 8));  // 小端拼长度
            if (data_length > kMaxDataSize) {  // 长度离谱：几乎可以确定是伪 SOF
                ++crc_errors_;  // 计入校验失败，便于现场判断波特率/接线是否正常
                DropOneByte();  // 丢 1 字节后重新找帧头
                continue;
            }

            const std::size_t total = kOverhead + data_length;  // 整帧字节数
            if (buffer_.size() < total) {  // 半帧：等后续字节
                return;
            }

            const std::uint8_t crc8 = Crc8(buffer_.data() + 1, 3);  // 帧头 CRC8 覆盖 data_length+seq
            if (crc8 != buffer_[4]) {  // 帧头校验失败
                ++crc_errors_;
                DropOneByte();
                continue;
            }

            const std::uint16_t crc16 = Crc16(buffer_.data() + 1, total - 3);  // 整帧校验（不含 SOF 与 CRC16 本身）
            const std::uint16_t crc16_received = static_cast<std::uint16_t>(
                    buffer_[total - 2] | (static_cast<std::uint16_t>(buffer_[total - 1]) << 8));  // 小端拼收到的校验值
            if (crc16 != crc16_received) {  // 整帧校验失败
                ++crc_errors_;
                DropOneByte();
                continue;
            }

            Frame frame;  // 组装一帧
            frame.seq = buffer_[3];  // 帧序号
            frame.cmd_id = static_cast<std::uint16_t>(
                    buffer_[5] | (static_cast<std::uint16_t>(buffer_[6]) << 8));  // 小端拼命令码
            frame.data.assign(buffer_.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + kCmdIdSize),  // data 起点
                              buffer_.begin() + static_cast<std::ptrdiff_t>(kHeaderSize + kCmdIdSize + data_length));  // data 终点
            pending_.push_back(std::move(frame));  // 放入待取队列
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(total));  // 从缓冲区移除整帧
        }
    }

    /**
     * @brief 丢弃缓冲区最前面的 1 个字节（重同步用）。
     */
    void DropOneByte() {
        if (!buffer_.empty()) {
            buffer_.erase(buffer_.begin());
            ++dropped_bytes_;
        }
    }

    /**
     * @brief 缓冲区超过上限时的兜底截断。
     * @note 思路：正常情况下 ParseAvailable 会把缓冲区清到不足一帧；一旦长时间解不出帧
     *       （例如波特率完全不匹配，收到的全是随机字节），就只保留最后一个 SOF 之后的内容。
     */
    void TrimIfNeeded() {
        constexpr std::size_t kLimit = kOverhead + kMaxDataSize + 64;  // 允许的最大积压
        if (buffer_.size() <= kLimit) {
            return;
        }
        std::size_t last_sof = buffer_.size();
        while (last_sof > 0) {
            --last_sof;
            if (buffer_[last_sof] == kSof) {
                break;
            }
        }
        if (buffer_[last_sof] == kSof) {  // 找到了可用的重同步点
            dropped_bytes_ += static_cast<std::uint32_t>(last_sof);
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + static_cast<std::ptrdiff_t>(last_sof));
        } else {  // 一个 SOF 都没有，全部丢弃
            dropped_bytes_ += static_cast<std::uint32_t>(buffer_.size());
            buffer_.clear();
        }
    }

    std::vector<std::uint8_t> buffer_;       // 未消费的原始字节
    std::deque<Frame> pending_;              // 已解析、等待 Pop 的帧
    std::uint32_t crc_errors_ = 0;           // 校验失败计数
    std::uint32_t dropped_bytes_ = 0;        // 丢弃字节计数
};

/**
 * @brief 按协议封一帧（仅用于单元测试与自检；真实设备由固件负责封装）。
 * @param cmd_id 命令码；seq 帧序号；data 数据段。
 * @return 完整帧字节（含 SOF、两个 CRC）。
 */
inline std::vector<std::uint8_t> BuildFrame(const std::uint16_t cmd_id, const std::uint8_t seq,
                                            const std::span<const std::uint8_t> data) {
    std::vector<std::uint8_t> frame;  // 输出缓冲
    frame.reserve(kOverhead + data.size());
    frame.push_back(kSof);  // SOF
    frame.push_back(static_cast<std::uint8_t>(data.size() & 0xFFu));         // 长度低字节
    frame.push_back(static_cast<std::uint8_t>((data.size() >> 8) & 0xFFu));  // 长度高字节
    frame.push_back(seq);  // 帧序号
    frame.push_back(Crc8(frame.data() + 1, 3));  // 帧头 CRC8
    frame.push_back(static_cast<std::uint8_t>(cmd_id & 0xFFu));         // 命令码低字节
    frame.push_back(static_cast<std::uint8_t>((cmd_id >> 8) & 0xFFu));  // 命令码高字节
    frame.insert(frame.end(), data.begin(), data.end());  // 数据段
    const std::uint16_t crc16 = Crc16(frame.data() + 1, frame.size() - 1);  // 从 data_length 到 data 末尾
    frame.push_back(static_cast<std::uint8_t>(crc16 & 0xFFu));         // CRC16 低字节
    frame.push_back(static_cast<std::uint8_t>((crc16 >> 8) & 0xFFu));  // CRC16 高字节
    return frame;  // 返回完整帧
}

}  // namespace RoboGo::io::referee
