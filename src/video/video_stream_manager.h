// video_stream_manager.h：视频流管理器——串联 UDP 接收、分片重组与解码三个环节，并独占一个解码线程驱动整条链路。
#pragma once

#include "io/video_stream_receiver.h"
#include "video/video_decoder.h"
#include "video/video_shards_manager.h"
#include <pch.h>

namespace RoboGo::video {

    // 视频流管理器：用 Boost.Asio 接收 UDP 视频分片，重组为完整帧后交由解码器解码，并独占一个解码线程。
    class VideoStreamManager {
    private:
        io::VideoStreamReceiver video_stream_receiver; // Boost.Asio UDP 接收器，负责收原始视频分片包
        VideoDecoder video_decoder;                    // HEVC 解码器，把重组后的完整帧解成 AVFrame
        VideoShardsManager video_shards_manager;       // 分片重组器，把分片拼回完整帧
        std::thread decode_thread;                     // 独立的解码线程，循环处理分片队列
        bool stream_ready = false;                     // 是否已收到首个可解码帧（IDR 或参数集），用于跳过无效起始
        std::atomic<bool> is_running = true;           // 线程运行标志，停止时置 false

    public:
        // 功能：构造管理器，连接 UDP 接收、分片重组与解码，并启动独立解码线程。
        // 参数：io_context 为 Asio 事件循环；endpoint 为监听的 UDP 地址与端口；frame_callback 为解出帧的回调。
        explicit VideoStreamManager(boost::asio::io_context &io_context,
                                    boost::asio::ip::udp::endpoint endpoint,
                                    const std::function<void(const AVFrame *frame)> &frame_callback);

        // 功能：停止接收、置停止标志并等待解码线程退出，完成资源回收。
        void stop();
    };

}// namespace RoboGo::video
