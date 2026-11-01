#include "custom_byte_block_video_decoder_worker.h" // 引入本文件的类声明

// 命名空间 RoboGo::video：业务功能层-视频模块的对外实现，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video {

// 功能：构造工作线程，保存回调与配置并立即启动 run() 线程。
CustomByteBlockVideoDecoderWorker::CustomByteBlockVideoDecoderWorker(FrameHandler frame_handler,
                                                                     LogHandler log_handler, // 参数：日志回调（type 0=信息 2=错误）
                                                                     CustomByteBlockVideoDecoderConfig config) // 参数：解码资源上限配置
    : config_(std::move(config)), // 保存解码配置（移动避免拷贝）
      frame_handler_(std::move(frame_handler)), // 保存帧回调
      log_handler_(std::move(log_handler)), // 保存日志回调
      worker_thread_([this]() { // 构造即启动工作线程执行 run()
          run(); // 线程入口：执行主循环
      }) { // 启动工作线程并完成成员初始化
} // 构造函数体为空，线程已在初始化列表中启动

// 功能：析构时确保工作线程被停止并回收。
CustomByteBlockVideoDecoderWorker::~CustomByteBlockVideoDecoderWorker() {
    stop(); // 析构即停止（stop 可重复调用）
} // 析构函数结束

// 功能：把收到的线格式包拷贝后入队，交由工作线程消费。
// 思路：空包或已请求停止则直接忽略；入队失败（队列满）记录日志，不阻塞收包线程。
void CustomByteBlockVideoDecoderWorker::enqueue_packet_bytes(std::span<const std::uint8_t> wire_bytes) {
    if (wire_bytes.empty() || stop_requested_.load(std::memory_order_acquire)) { // 空包或已请求停止则跳过
        return; // 不入队，直接返回
    } // 跳过条件分支结束

    std::vector<std::uint8_t> packet_bytes(wire_bytes.begin(), wire_bytes.end()); // 拷贝网络数据到自有缓冲，避免回调栈悬空
    if (!packet_queue_.enqueue(std::move(packet_bytes))) { // 无锁入队，队列满则失败
        SPDLOG_ERROR("CustomByteBlock 解码队列入队失败 bytes={}", wire_bytes.size()); // 记录入队失败（队列满则丢包）
        emit_log(2, "CustomByteBlock 解码队列入队失败"); // 经日志回调上报入队失败
    } // 入队失败分支结束
} // enqueue_packet_bytes 结束

// 功能：请求停止工作线程并等待其退出。
void CustomByteBlockVideoDecoderWorker::stop() {
    stop_requested_.store(true, std::memory_order_release); // 通知线程退出循环
    if (worker_thread_.joinable()) { // 线程仍处于可连接状态才需要 join
        worker_thread_.join(); // 等待线程结束
    } // 可连接才 join，避免重复 join 抛异常
} // stop 结束

// 功能：工作线程主循环：取包→解码→回调，结束后冲刷残留帧。
// 思路：阻塞取包(10ms 超时)以兼顾停止响应；每批解码帧逐个回调并在首次成功时记日志；
//       退出前 flush 取残留帧；任何构造/解码异常均经 handle_decode_error 处理并记录。
void CustomByteBlockVideoDecoderWorker::run() {
    SPDLOG_INFO("CustomByteBlock 解码线程已启动"); // 记录线程启动

    try { // 整体异常保护：构造/运行期异常统一上报
        CustomByteBlockVideoDecoder decoder(config_); // 工作线程内持有解码器实例

        while (true) { // 主循环：持续取包并解码
            std::vector<std::uint8_t> packet_bytes; // 出队存放一包数据
            // 阻塞取包，最多等 10ms，便于及时响应停止信号
            if (!packet_queue_.wait_dequeue_timed(packet_bytes, std::chrono::milliseconds(10))) { // 阻塞取包，超时进入空闲处理
                // 超时：若已请求停止则退出循环，否则继续等待
                if (stop_requested_.load(std::memory_order_acquire)) { // 超时后检查是否应停止
                    break; // 退出主循环
                } // 停止标志检查分支结束
                continue; // 否则继续等待新包
            } // 超时处理分支结束

            try { // 单包解码异常保护
                auto decoded_frames = decoder.push_packet_bytes(packet_bytes); // 解码该包，可能解出多帧
                for (auto &decoded_frame: decoded_frames) { // 遍历本次解码出的每一帧
                    if (frame_handler_) { // 存在帧回调才向外抛出
                        frame_handler_(std::move(decoded_frame)); // 回调出帧
                    } // 帧回调存在性检查结束
                    if (!stream_started_logged_) { // 尚未记录流开始日志
                        // 首次出帧即记录流开始日志，避免重复
                        emit_log(0, "CustomByteBlock 视频流已开始"); // 首次出帧记录流开始
                        stream_started_logged_ = true; // 标记已记录，避免重复
                    } // 流开始日志分支结束
                } // 本批帧遍历结束
            } catch (const std::exception &error) { // 单包解码异常
                handle_decode_error(error.what()); // 处理异常后继续下一包
            } // 单包异常处理结束
        } // 主循环结束

        try { // 冲刷阶段异常保护
            // 流结束前冲刷解码器，回吐所有滞留帧
            auto flushed_frames = decoder.flush();
            for (auto &decoded_frame: flushed_frames) { // 遍历冲刷出的残留帧
                if (frame_handler_) { // 有回调才抛出残留帧
                    frame_handler_(std::move(decoded_frame)); // 回调残留帧
                } // 残留帧回调检查结束
                if (!stream_started_logged_) { // 残留帧阶段也补记流开始
                    emit_log(0, "CustomByteBlock 视频流已开始"); // 记录流开始
                    stream_started_logged_ = true; // 标记已记录
                } // 残留帧日志分支结束
            } // 残留帧遍历结束
        } catch (const std::exception &error) { // 冲刷阶段解码异常
            handle_decode_error(error.what()); // 处理冲刷失败，继续收尾
        } // 冲刷异常处理结束
    } catch (const std::exception &error) { // 解码器构造或整体异常
        handle_decode_error(error.what()); // 处理构造/运行期致命错误
    } // 整体异常处理结束

    SPDLOG_INFO("CustomByteBlock 解码线程已退出"); // 记录线程退出
} // run 结束

// 功能：经配置的日志回调输出一条日志（未设置回调则静默）。
void CustomByteBlockVideoDecoderWorker::emit_log(const int type, const std::string &text) const {
    if (log_handler_) { // 存在日志回调才输出
        log_handler_(type, text); // 调用回调输出日志
    } // 日志回调存在性检查结束
} // emit_log 结束

// 功能：处理解码异常并输出错误日志，按「相同错误且 1 秒内」节流避免刷屏。
void CustomByteBlockVideoDecoderWorker::handle_decode_error(const std::string &message) {
    SPDLOG_ERROR("CustomByteBlock 视频解码失败 message={}", message); // 记录解码失败详情

    const auto now = std::chrono::steady_clock::now(); // 记录当前时间，用于节流判定
    // 仅在错误信息变化或距上次已超过 1 秒时才上报，防止重复错误刷屏
    if (last_error_text_ != message || now - last_error_time_ >= std::chrono::seconds(1)) { // 文本变化或距上次超过 1 秒才上报
        last_error_text_ = message; // 更新上次错误文本
        last_error_time_ = now; // 更新上次上报时间
        emit_log(2, "CustomByteBlock 视频解码失败: " + message); // 经回调上报错误
    } // 节流条件分支结束
} // handle_decode_error 结束

}// namespace RoboGo::video
