#include "video_stream_manager.h"

// 命名空间 RoboGo::video：业务功能层-视频模块的对外实现，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video {

    // 功能：构造管理器，搭建「UDP 接收 → 分片重组 → 解码」链路并启动解码线程。
    // 思路：UDP 收到分片直接入队；分片管理器重组出完整帧后，先判断是否为有效起始帧(IDR/参数集)，
    //       待流就绪再交给解码器；最后单独起一个线程循环调用 handle_once 驱动重组与解码。
    VideoStreamManager::VideoStreamManager(boost::asio::io_context &io_context,
                                           boost::asio::ip::udp::endpoint endpoint,
                                           const std::function<void(const AVFrame *frame)> &frame_callback)
        : video_stream_receiver(io_context, endpoint, [this](const uint8_t *data, size_t size) {
              // 接收到数据包之后送入分片管理
              video_shards_manager.on_receive_shard(data, size);
          }),
          video_decoder(frame_callback) {
        SPDLOG_INFO("创建视频流管理器 endpoint={}:{}",
                    endpoint.address().to_string(),
                    endpoint.port());
        // 接收到完整的视频帧之后去解码
        video_shards_manager.set_frame_callback([this](const std::vector<uint8_t> &frame) {
            // 流未就绪时，需要先拿到首个有效关键帧(参数集或 IDR)才能开始解码，避免起始花屏
            if (!stream_ready) {
                int nal_type = VideoDecoder::get_nal_type(frame);
                if (nal_type == -1) {
                    SPDLOG_DEBUG("尚未拿到可解码视频帧，忽略无效NAL frame_bytes={}", frame.size());
                    return;
                }
                // HEVC 参数集 NAL 类型：VPS(32)/SPS(33)/PPS(34)
                bool is_param = (nal_type == 32 || nal_type == 33 || nal_type == 34);
                // HEVC IDR 帧 NAL 类型：IDR_W_DLP(19)/IDR_N_LP(20)/CRA(21)
                bool is_idr = (nal_type == 19 || nal_type == 20 || nal_type == 21);

                // 遇到IDR帧或已有参数集时开始流
                if (is_idr || is_param) {
                    stream_ready = true;
                    SPDLOG_INFO("视频流已就绪 nal_type={} frame_bytes={}", nal_type, frame.size());
                }
            }

            // 解码
            if (stream_ready) {
                video_decoder.decode_frame(frame);
            } else {
                SPDLOG_DEBUG("视频流尚未就绪，丢弃帧 bytes={}", frame.size());
            }
        });
        // 为视频解码单独创建一个线程
        decode_thread = std::thread([this]() {
            SPDLOG_INFO("视频解码线程已启动");
            // 循环驱动分片重组与解码，直到被要求停止
            while (is_running.load(std::memory_order_relaxed)) {
                video_shards_manager.handle_once();
            }
            SPDLOG_INFO("视频解码线程已退出");
        });
    }

    // 功能：停止视频流管线，回收线程资源。
    void VideoStreamManager::stop() {
        SPDLOG_INFO("收到视频流管理器停止请求");
        video_stream_receiver.stop(); // 停止 UDP 接收
        is_running.store(false, std::memory_order_relaxed); // 通知解码线程退出循环
        if (decode_thread.joinable()) {
            decode_thread.join(); // 等待解码线程结束，确保资源安全回收
        }
        SPDLOG_INFO("视频流管理器已停止");
    }

}// namespace RoboGo::video
