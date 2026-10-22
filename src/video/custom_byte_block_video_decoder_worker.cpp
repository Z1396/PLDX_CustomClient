#include "custom_byte_block_video_decoder_worker.h"

namespace RoboGo::video {

// 功能：构造工作线程，保存回调与配置并立即启动 run() 线程。
CustomByteBlockVideoDecoderWorker::CustomByteBlockVideoDecoderWorker(FrameHandler frame_handler,
                                                                     LogHandler log_handler,
                                                                     CustomByteBlockVideoDecoderConfig config)
    : config_(std::move(config)),
      frame_handler_(std::move(frame_handler)),
      log_handler_(std::move(log_handler)),
      worker_thread_([this]() {
          run();
      }) {
}

// 功能：析构时确保工作线程被停止并回收。
CustomByteBlockVideoDecoderWorker::~CustomByteBlockVideoDecoderWorker() {
    stop();
}

// 功能：把收到的线格式包拷贝后入队，交由工作线程消费。
// 思路：空包或已请求停止则直接忽略；入队失败（队列满）记录日志，不阻塞收包线程。
void CustomByteBlockVideoDecoderWorker::enqueue_packet_bytes(std::span<const std::uint8_t> wire_bytes) {
    if (wire_bytes.empty() || stop_requested_.load(std::memory_order_acquire)) {
        return;
    }

    std::vector<std::uint8_t> packet_bytes(wire_bytes.begin(), wire_bytes.end());
    if (!packet_queue_.enqueue(std::move(packet_bytes))) {
        SPDLOG_ERROR("CustomByteBlock 解码队列入队失败 bytes={}", wire_bytes.size());
        emit_log(2, "CustomByteBlock 解码队列入队失败");
    }
}

// 功能：请求停止工作线程并等待其退出。
void CustomByteBlockVideoDecoderWorker::stop() {
    stop_requested_.store(true, std::memory_order_release); // 通知线程退出循环
    if (worker_thread_.joinable()) {
        worker_thread_.join(); // 等待线程结束
    }
}

// 功能：工作线程主循环：取包→解码→回调，结束后冲刷残留帧。
// 思路：阻塞取包(10ms 超时)以兼顾停止响应；每批解码帧逐个回调并在首次成功时记日志；
//       退出前 flush 取残留帧；任何构造/解码异常均经 handle_decode_error 处理并记录。
void CustomByteBlockVideoDecoderWorker::run() {
    SPDLOG_INFO("CustomByteBlock 解码线程已启动");

    try {
        CustomByteBlockVideoDecoder decoder(config_);

        while (true) {
            std::vector<std::uint8_t> packet_bytes;
            // 阻塞取包，最多等 10ms，便于及时响应停止信号
            if (!packet_queue_.wait_dequeue_timed(packet_bytes, std::chrono::milliseconds(10))) {
                // 超时：若已请求停止则退出循环，否则继续等待
                if (stop_requested_.load(std::memory_order_acquire)) {
                    break;
                }
                continue;
            }

            try {
                auto decoded_frames = decoder.push_packet_bytes(packet_bytes);
                for (auto &decoded_frame: decoded_frames) {
                    if (frame_handler_) {
                        frame_handler_(std::move(decoded_frame)); // 回调出帧
                    }
                    if (!stream_started_logged_) {
                        // 首次出帧即记录流开始日志，避免重复
                        emit_log(0, "CustomByteBlock 视频流已开始");
                        stream_started_logged_ = true;
                    }
                }
            } catch (const std::exception &error) {
                handle_decode_error(error.what());
            }
        }

        try {
            // 流结束前冲刷解码器，回吐所有滞留帧
            auto flushed_frames = decoder.flush();
            for (auto &decoded_frame: flushed_frames) {
                if (frame_handler_) {
                    frame_handler_(std::move(decoded_frame));
                }
                if (!stream_started_logged_) {
                    emit_log(0, "CustomByteBlock 视频流已开始");
                    stream_started_logged_ = true;
                }
            }
        } catch (const std::exception &error) {
            handle_decode_error(error.what());
        }
    } catch (const std::exception &error) {
        handle_decode_error(error.what());
    }

    SPDLOG_INFO("CustomByteBlock 解码线程已退出");
}

// 功能：经配置的日志回调输出一条日志（未设置回调则静默）。
void CustomByteBlockVideoDecoderWorker::emit_log(const int type, const std::string &text) const {
    if (log_handler_) {
        log_handler_(type, text);
    }
}

// 功能：处理解码异常并输出错误日志，按「相同错误且 1 秒内」节流避免刷屏。
void CustomByteBlockVideoDecoderWorker::handle_decode_error(const std::string &message) {
    SPDLOG_ERROR("CustomByteBlock 视频解码失败 message={}", message);

    const auto now = std::chrono::steady_clock::now();
    // 仅在错误信息变化或距上次已超过 1 秒时才上报，防止重复错误刷屏
    if (last_error_text_ != message || now - last_error_time_ >= std::chrono::seconds(1)) {
        last_error_text_ = message;
        last_error_time_ = now;
        emit_log(2, "CustomByteBlock 视频解码失败: " + message);
    }
}

}// namespace RoboGo::video
