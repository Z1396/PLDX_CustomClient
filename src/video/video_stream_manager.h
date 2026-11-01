// video_stream_manager.h：视频流管理器——串联 UDP 接收、分片重组与解码三个环节，并独占一个解码线程驱动整条链路。
#pragma once // 头文件只包含一次，防止重复定义

#include "io/video_stream_receiver.h" // UDP 视频分片接收器（Boost.Asio 异步收包）
#include "video/video_decoder.h" // HEVC 解码器
#include "video/video_shards_manager.h" // 分片重组器（把分片拼回完整帧）
#include <pch.h> // 预编译头：引入 std::thread/atomic/function 等依赖

// 命名空间 RoboGo::video：业务功能层-视频模块的对外声明，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video { // 进入视频模块命名空间

    // 视频流管理器：用 Boost.Asio 接收 UDP 视频分片，重组为完整帧后交由解码器解码，并独占一个解码线程。
    class VideoStreamManager { // 视频流管线管理器类定义开始
    private: // ---------- 私有成员：三段链路 + 驱动线程 ----------
        io::VideoStreamReceiver video_stream_receiver; // Boost.Asio UDP 接收器，负责收原始视频分片包
        VideoDecoder video_decoder;                    // HEVC 解码器，把重组后的完整帧解成 AVFrame
        VideoShardsManager video_shards_manager;       // 分片重组器，把分片拼回完整帧
        std::thread decode_thread;                     // 独立的解码线程，循环处理分片队列
        bool stream_ready = false;                     // 是否已收到首个可解码帧（IDR 或参数集），用于跳过无效起始
        std::atomic<bool> is_running = true;           // 线程运行标志，停止时置 false

    public: // ---------- 公开接口 ----------
        // 功能：构造管理器，连接 UDP 接收、分片重组与解码，并启动独立解码线程。
        // 参数：io_context 为 Asio 事件循环；endpoint 为监听的 UDP 地址与端口；frame_callback 为解出帧的回调。
        explicit VideoStreamManager(boost::asio::io_context &io_context, // Asio 事件循环，接收器在其上注册异步读
                                    boost::asio::ip::udp::endpoint endpoint, // 监听的 UDP 地址与端口
                                    const std::function<void(const AVFrame *frame)> &frame_callback); // 解出 AVFrame 后的回调

        // 功能：停止接收、置停止标志并等待解码线程退出，完成资源回收。
        void stop(); // 幂等停止：先停收包，再通知线程退出并 join
    }; // 管理器类定义结束

}// namespace RoboGo::video // 结束视频模块命名空间
