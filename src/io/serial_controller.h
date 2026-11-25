#pragma once  // 头文件包含守卫：防止重复包含

#include <pch.h>  // 预编译头(PCH)：集中包含常用标准库与第三方库
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <boost/asio.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
// [/pch-split]
#include "io/referee_frame.h"  // 裁判系统串口帧解析（纯逻辑）

// ============================================================================
// 文件：src/io/serial_controller.h
// 层级：网络/IO 层 - 串口输入（BackgroundServices 组件之一）
// 职责：打开一个 RS232/USB 转串口设备，按异步方式持续读取，
//       用 referee::FrameParser 还原成完整帧后回调给上层。
//
// 为什么需要它（2027 规则）：
//   2027 把「自定义控制器」并入「自定义终端」，操作间不再提供 RS232 口，
//   协议也删除了 0x0302/0x0306/0x0309。手柄只能作为自定义终端自己的输入外设：
//   手柄 RS232 → USB 转串口 → 本机 COM 口 → 本类读取 → 上层转成键鼠/自定义指令。
//
// 设计要点：
//   1) 复用主程序的 io_context，不额外起线程（串口 115200 的负载可以忽略）；
//   2) 打开失败/掉线后每 2 秒自动重试，插拔 USB 转串口不需要重启客户端；
//   3) 所有网络/串口操作都在 IO 线程，主线程只调 Start/Stop 与读统计量；
//   4) Stop() 会主动 close 端口，让挂起的 async_read_some 立即以 operation_aborted 返回。
// 依赖：Boost.Asio（serial_port / steady_timer）、referee_frame.h、spdlog。
// 注意：Windows 下端口名写 "COM3"；Linux 下写 "/dev/ttyUSB0"。串口是独占资源，
//       同一端口不能被官方客户端和本程序同时打开。
// ============================================================================

namespace RoboGo::io {  // 进入网络 IO 命名空间

/**
 * @brief RS232 串口控制器：异步读取 + 自动重连 + 帧解析。
 */
class SerialController {
public:
    // 收到一帧完整数据时的回调（在 IO 线程执行，回调里不要做耗时操作）。
    using FrameHandler = std::function<void(const referee::Frame &)>;

    /**
     * @brief 构造串口控制器（不打开端口）。
     * @param io_context 主程序共用的 Asio 执行上下文。
     * @param port_name 串口设备名（"COM3" / "/dev/ttyUSB0"）。
     * @param baud_rate 波特率（旧控制器固件为 115200）。
     * @param on_frame 收到完整帧时的回调。
     */
    SerialController(boost::asio::io_context &io_context,
                     std::string port_name,
                     int baud_rate,
                     FrameHandler on_frame);

    ~SerialController();  // 析构时确保端口关闭

    SerialController(const SerialController &) = delete;             // 持有端口与回调，禁止拷贝
    SerialController &operator=(const SerialController &) = delete;  // 禁止拷贝赋值

    /**
     * @brief 启动：打开端口并投递第一次异步读取。重复调用无副作用。
     */
    void Start();

    /**
     * @brief 停止：取消重连定时器并关闭端口。幂等，可从主线程调用。
     */
    void Stop();

    [[nodiscard]] bool IsRunning() const;   // 是否处于"已启用"状态（含重连等待中）
    [[nodiscard]] bool IsOpen() const;      // 端口当前是否真的打开了
    [[nodiscard]] std::string PortName() const;  // 端口名（日志/界面展示）
    [[nodiscard]] std::string LastError() const; // 最近一次错误文本（空表示没出错）

    // 运行统计：供日志或性能面板观察链路健康度。
    struct Stats {
        std::uint64_t frames = 0;        // 累计解析出的完整帧数
        std::uint32_t crc_errors = 0;    // 校验失败次数（接线/波特率不对时会持续增长）
        std::uint32_t dropped_bytes = 0; // 被丢弃的噪声字节数
        std::uint32_t read_errors = 0;   // 读错误次数（掉线会 +1）
        std::uint32_t reopen_count = 0;  // 重连尝试次数
    };

    /**
     * @brief 取一份统计快照（原子读，可在 UI 线程调用）。
     */
    [[nodiscard]] Stats Snapshot() const;

    /**
     * @brief 打开「原始字节日志」：把串口收到的前 256 字节直接打成十六进制。
     * @note 为什么需要：如果手柄固件根本不用 0xA5 帧，帧解析器一帧都解不出来，
     *       此时只有看原始字节才能区分「完全没数据 / 波特率不对 / 协议不一样」。
     *       与 --serial-raw 一起使用。
     */
    void EnableRawByteLog(bool enabled);

    static constexpr std::chrono::milliseconds kReopenDelay{2000};  // 掉线后的重试间隔

private:
    void OpenAndRead();                                       // 打开端口并开始读
    void DoRead();                                            // 投递一次异步读
    void HandleRead(const boost::system::error_code &ec, std::size_t bytes);  // 读完成回调
    void ScheduleReopen();                                    // 安排下一次重连
    void ClosePort();                                         // 关闭端口（线程安全）
    void SetLastError(std::string message);                   // 记录最近错误

    boost::asio::io_context &io_context_;  // 共用的执行上下文
    std::string port_name_;                // 端口名
    int baud_rate_;                        // 波特率
    FrameHandler on_frame_;                // 帧回调
    boost::asio::serial_port port_;        // 串口对象
    boost::asio::steady_timer reopen_timer_;  // 重连定时器
    std::array<std::uint8_t, 512> read_buffer_{};  // 单次读取缓冲（115200 下约 11.5KB/s，512 足够）
    referee::FrameParser parser_;          // 增量帧解析器（仅 IO 线程访问）

    std::atomic<bool> running_{false};        // 是否已启用
    std::atomic<bool> open_{false};           // 端口是否打开
    std::atomic<std::uint64_t> frame_count_{0};   // 帧计数
    std::atomic<std::uint32_t> crc_errors_{0};    // 校验失败计数（从 parser_ 同步过来，供 UI 线程读）
    std::atomic<std::uint32_t> dropped_bytes_{0}; // 丢弃字节计数（同上）
    std::atomic<std::uint32_t> read_errors_{0};   // 读错误计数
    std::atomic<std::uint32_t> reopen_count_{0};  // 重连计数
    std::atomic<bool> raw_byte_log_{false};       // 是否打印原始字节
    std::atomic<std::size_t> raw_bytes_logged_{0}; // 已打印的原始字节数（上限 256，避免刷屏）
    mutable std::mutex error_mutex_;          // 保护 last_error_
    std::string last_error_;                  // 最近错误文本
};

}  // namespace RoboGo::io
