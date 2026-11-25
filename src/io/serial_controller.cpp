// serial_controller.cpp：RS232 串口控制器的实现——打开端口、异步读取、按帧解析、掉线重连。
//
// 2027 规则背景：官方取消了「自定义控制器」这一独立设备类别，操作间只提供 RJ45，
// 协议也删除了 0x0302/0x0306/0x0309。手柄只能作为自定义终端自己的输入外设，
// 所以这里读的不是「官方控制器链路」，而是一条完全私有的串口连线。
//
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <boost/asio.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>
// [/pch-split]
#include "io/serial_controller.h"

#include "logging/logging.h"  // SPDLOG_INFO/WARN/ERROR 日志宏

namespace RoboGo::io {  // 进入网络 IO 命名空间

namespace {

/**
 * @brief 把串口打开失败的常见原因翻译成现场可执行的提示。
 * @param message 系统错误文本。
 * @return 带排查建议的说明。
 */
std::string DescribeOpenFailure(const std::string &message) {
    return message + "（检查：① 设备管理器中确认 COM 号；② USB 转串口驱动是否装好；"
                     "③ 该 COM 口是否已被官方客户端或串口助手占用）";
}

/**
 * @brief 把一段字节转成十六进制文本（最多 max_bytes 个，超出用 "..." 表示）。
 * @param data 字节首地址；bytes 总字节数；max_bytes 本次最多打印多少。
 * @return 形如 "A5 1E 00 07 ..." 的文本。
 */
std::string BytesToHex(const std::uint8_t *data, const std::size_t bytes, const std::size_t max_bytes) {
    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    const std::size_t count = bytes < max_bytes ? bytes : max_bytes;  // 实际打印数量
    std::string text;
    text.reserve(count * 3);
    for (std::size_t i = 0; i < count; ++i) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        text.push_back(kHexDigits[data[i] >> 4]);        // 高四位
        text.push_back(kHexDigits[data[i] & 0x0Fu]);     // 低四位
    }
    if (bytes > max_bytes) {
        text += " ...";
    }
    return text;
}

}  // namespace

/**
 * @brief 构造：只保存参数，不打开端口。
 * @note 为什么不在构造函数里打开：构造发生在主线程、io_context 还没开始跑，
 *       把打开动作放到 Start() 里，失败也能走统一的重连逻辑。
 */
SerialController::SerialController(boost::asio::io_context &io_context,
                                   std::string port_name,
                                   const int baud_rate,
                                   FrameHandler on_frame)
    : io_context_(io_context),                 // 共用主程序的执行上下文
      port_name_(std::move(port_name)),        // 端口名
      baud_rate_(baud_rate),                   // 波特率
      on_frame_(std::move(on_frame)),          // 帧回调
      port_(io_context_),                      // 串口绑定同一个 io_context
      reopen_timer_(io_context_) {}            // 重连定时器同样绑定

/**
 * @brief 析构：确保端口关闭、定时器取消。
 */
SerialController::~SerialController() {
    Stop();  // 幂等；即使外部忘了停，这里也会兜住
}

/**
 * @brief 启动串口读取。
 */
void SerialController::Start() {
    if (running_.exchange(true, std::memory_order_acq_rel)) {  // 已启动则不重复打开
        return;
    }
    SPDLOG_INFO("串口控制器已启用 port={} baud={}", port_name_, baud_rate_);
    OpenAndRead();  // 立即尝试打开
}

/**
 * @brief 停止串口读取并关闭端口。
 */
void SerialController::Stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {  // 未启动或已停止
        return;
    }
    // 取消可能挂起的重连定时器。
    // 注意：本项目在 pch.h 里定义了 BOOST_ASIO_NO_DEPRECATED，带 error_code 的 cancel 重载已被移除，
    // 只能用无参版本；定时器取消不会失败，无需错误处理。
    reopen_timer_.cancel();
    ClosePort();               // 关闭端口：挂起的 async_read_some 会以 operation_aborted 返回
    SPDLOG_INFO("串口控制器已停止 port={} frames={} crc_errors={} dropped_bytes={} read_errors={}",
                port_name_,
                frame_count_.load(std::memory_order_relaxed),
                crc_errors_.load(std::memory_order_relaxed),
                dropped_bytes_.load(std::memory_order_relaxed),
                read_errors_.load(std::memory_order_relaxed));
}

/**
 * @brief 查询是否处于已启用状态（重连等待期间仍为 true）。
 */
bool SerialController::IsRunning() const {
    return running_.load(std::memory_order_acquire);
}

/**
 * @brief 查询端口当前是否真的打开。
 */
bool SerialController::IsOpen() const {
    return open_.load(std::memory_order_acquire);
}

/**
 * @brief 返回端口名（日志/界面展示用）。
 */
std::string SerialController::PortName() const {
    return port_name_;  // 构造后不再修改，按值返回无竞争
}

/**
 * @brief 返回最近一次错误文本。
 */
std::string SerialController::LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);  // 与 IO 线程写入互斥
    return last_error_;
}

/**
 * @brief 取统计快照。
 */
SerialController::Stats SerialController::Snapshot() const {
    Stats stats;
    stats.frames = frame_count_.load(std::memory_order_relaxed);
    stats.crc_errors = crc_errors_.load(std::memory_order_relaxed);
    stats.dropped_bytes = dropped_bytes_.load(std::memory_order_relaxed);
    stats.read_errors = read_errors_.load(std::memory_order_relaxed);
    stats.reopen_count = reopen_count_.load(std::memory_order_relaxed);
    return stats;
}

/**
 * @brief 开关原始字节日志。
 */
void SerialController::EnableRawByteLog(const bool enabled) {
    raw_byte_log_.store(enabled, std::memory_order_relaxed);
    raw_bytes_logged_.store(0, std::memory_order_relaxed);  // 重新计数，便于换设备后重新观察
}

/**
 * @brief 打开端口并开始异步读取；失败则安排重连。
 * @note 思路：先关掉可能残留的句柄，再 open；五项串口参数逐个设置（8N1、无流控），
 *       成功后清空解析器（丢弃上一轮可能残留的半帧），最后投递第一次读。
 */
void SerialController::OpenAndRead() {
    if (!running_.load(std::memory_order_acquire)) {  // 已被 Stop
        return;
    }

    boost::system::error_code ec;
    ClosePort();  // 关掉上次可能没关干净的句柄
    port_.open(port_name_, ec);  // 打开串口（Windows 下 Asio 会自动补 "\\\\.\\" 前缀）
    if (ec) {  // 打开失败：设备未插、COM 号错、被占用
        const std::string reason = DescribeOpenFailure(ec.message());
        SetLastError(reason);
        // 日志节流：前 3 次用 WARN，之后降为 DEBUG（Release 下整条编译掉）。
        // 否则 COM 号写错时，每 2 秒一条会在一小时的备赛里把日志刷满。
        if (reopen_count_.load(std::memory_order_relaxed) < 3) {
            SPDLOG_WARN("串口打开失败 port={} error={}（{} 秒后重试）",
                        port_name_, reason, kReopenDelay.count() / 1000);
        } else {
            SPDLOG_DEBUG("串口仍无法打开 port={} error={}", port_name_, reason);
        }
        ScheduleReopen();  // 安排下一次重试
        return;
    }

    // 串口参数：115200 / 8 数据位 / 1 停止位 / 无校验 / 无流控 —— 与旧控制器固件一致。
    port_.set_option(boost::asio::serial_port_base::baud_rate(baud_rate_), ec);
    if (ec) {
        SetLastError(ec.message());
        SPDLOG_WARN("串口设置波特率失败 port={} baud={} error={}", port_name_, baud_rate_, ec.message());
    }
    port_.set_option(boost::asio::serial_port_base::character_size(8), ec);
    port_.set_option(boost::asio::serial_port_base::stop_bits(
                             boost::asio::serial_port_base::stop_bits::one), ec);
    port_.set_option(boost::asio::serial_port_base::parity(
                             boost::asio::serial_port_base::parity::none), ec);
    port_.set_option(boost::asio::serial_port_base::flow_control(
                             boost::asio::serial_port_base::flow_control::none), ec);

    parser_.Clear();  // 新连接从干净状态开始，避免把旧半帧接上去
    open_.store(true, std::memory_order_release);
    SetLastError("");
    SPDLOG_INFO("串口已打开 port={} baud={}（8N1，无流控）", port_name_, baud_rate_);
    DoRead();  // 投递第一次异步读
}

/**
 * @brief 投递一次异步读。
 */
void SerialController::DoRead() {
    if (!running_.load(std::memory_order_acquire) || !port_.is_open()) {  // 已停止或端口已关
        return;
    }
    port_.async_read_some(boost::asio::buffer(read_buffer_),
                          [this](const boost::system::error_code &ec, const std::size_t bytes) {
                              HandleRead(ec, bytes);  // 统一在 HandleRead 里处理错误与解析
                          });
}

/**
 * @brief 读完成回调：解析帧、分发回调，然后继续下一次读。
 * @param ec 错误码；bytes 本次读到的字节数。
 */
void SerialController::HandleRead(const boost::system::error_code &ec, const std::size_t bytes) {
    if (!running_.load(std::memory_order_acquire)) {  // Stop() 主动关闭导致的中断
        return;
    }
    if (ec) {
        if (ec == boost::asio::error::operation_aborted) {  // 主动关闭，不算故障
            return;
        }
        read_errors_.fetch_add(1, std::memory_order_relaxed);
        SetLastError(ec.message());
        SPDLOG_WARN("串口读取失败 port={} error={}（{} 秒后重连）",
                    port_name_, ec.message(), kReopenDelay.count() / 1000);
        ClosePort();       // 关掉坏掉的句柄
        ScheduleReopen();  // 安排重连
        return;
    }

    if (bytes > 0) {
        // 原始字节日志：只在 --serial-raw 时打开，累计打印前 256 字节。
        // 它比帧级日志更底层——即使固件完全不用 0xA5 帧，也能看到「到底有没有数据」。
        if (raw_byte_log_.load(std::memory_order_relaxed)
            && raw_bytes_logged_.load(std::memory_order_relaxed) < 256) {
            SPDLOG_INFO("串口原始字节 len={} data=[{}]",
                        bytes, BytesToHex(read_buffer_.data(), bytes, 64));
            raw_bytes_logged_.fetch_add(bytes, std::memory_order_relaxed);
        }

        parser_.Push(std::span<const std::uint8_t>(read_buffer_.data(), bytes));  // 喂给增量解析器
        // 把解析器内部计数同步到原子量，UI 线程才能安全读取
        crc_errors_.store(parser_.CrcErrors(), std::memory_order_relaxed);
        dropped_bytes_.store(parser_.DroppedBytes(), std::memory_order_relaxed);

        referee::Frame frame;
        while (parser_.Pop(frame)) {  // 一次读可能包含多帧
            frame_count_.fetch_add(1, std::memory_order_relaxed);
            if (on_frame_) {  // 回调已注册才分发
                on_frame_(frame);
            }
        }
    }
    DoRead();  // 继续读下一批
}

/**
 * @brief 安排一次延迟重连。
 */
void SerialController::ScheduleReopen() {
    if (!running_.load(std::memory_order_acquire)) {  // 已停止就不再排
        return;
    }
    reopen_count_.fetch_add(1, std::memory_order_relaxed);
    reopen_timer_.expires_after(kReopenDelay);  // 2 秒后重试，避免忙等
    reopen_timer_.async_wait([this](const boost::system::error_code &ec) {
        if (ec || !running_.load(std::memory_order_acquire)) {  // 被取消或已停止
            return;
        }
        if (reopen_count_.load(std::memory_order_relaxed) <= 3) {  // 只在前几次提示，避免刷屏
            SPDLOG_INFO("串口尝试重连 port={}", port_name_);
        } else {
            SPDLOG_DEBUG("串口仍在重连 port={}", port_name_);
        }
        OpenAndRead();  // 重新打开
    });
}

/**
 * @brief 关闭端口（可在主线程调用）。
 * @note Asio 允许从其它线程 close 正在读的句柄，挂起的读会立刻以 operation_aborted 返回。
 */
void SerialController::ClosePort() {
    open_.store(false, std::memory_order_release);
    boost::system::error_code ec;
    if (port_.is_open()) {
        port_.close(ec);  // 忽略关闭错误：句柄可能已被系统回收
    }
}

/**
 * @brief 记录最近一次错误文本。
 */
void SerialController::SetLastError(std::string message) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = std::move(message);
}

}  // namespace RoboGo::io
