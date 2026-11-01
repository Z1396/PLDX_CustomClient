// custom_byte_block_video_decoder_worker.h：解码工作线程接口——在独立线程消费包队列并解码，把 RGBA 帧回调给上层，与收包线程解耦。
#pragma once // 头文件只包含一次，防止重复定义

#include <pch.h> // 预编译头：引入 thread/atomic/function/vector 等依赖

#include "video/custom_byte_block_video_decoder.h" // 解码器与解码帧/配置类型定义

// 命名空间 RoboGo::video：业务功能层-视频模块的对外声明，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video { // 进入视频模块命名空间

// 自定义字节块解码工作线程：在独立线程中消费线格式包队列，解码为 RGBA 帧并回调，与收包线程解耦。
class CustomByteBlockVideoDecoderWorker { // 工作线程类定义开始
public: // ---------- 公开类型与接口 ----------
    using FrameHandler = std::function<void(CustomByteBlockDecodedFrame &&frame)>; // 解出一帧时的回调
    using LogHandler = std::function<void(int type, const std::string &text)>;    // 日志回调（type: 0=信息 2=错误）

    // 功能：构造解码工作线程——保存回调与配置，并立即启动后台线程执行 run() 主循环。
    // 参数 frame_handler：解出一帧时的回调（帧以右值传出）；
    //      log_handler：日志回调（type: 0=信息 2=错误）；
    //      config：解码器资源上限配置，默认使用结构体默认值。
    explicit CustomByteBlockVideoDecoderWorker(FrameHandler frame_handler, // 出帧回调（移动语义交出帧所有权）
                                               LogHandler log_handler, // 日志回调（上报状态与错误）
                                               CustomByteBlockVideoDecoderConfig config = {}); // 解码资源上限配置，缺省用默认值
    // 功能：析构——若外部未显式调用 stop()，则在此请求停止并等待工作线程退出，防止线程悬空。
    ~CustomByteBlockVideoDecoderWorker(); // 析构中兜底调用 stop()，保证线程被 join

    CustomByteBlockVideoDecoderWorker(const CustomByteBlockVideoDecoderWorker &) = delete; // 持有线程，禁止拷贝
    CustomByteBlockVideoDecoderWorker &operator=(const CustomByteBlockVideoDecoderWorker &) = delete; // 禁止拷贝赋值
    CustomByteBlockVideoDecoderWorker(CustomByteBlockVideoDecoderWorker &&) = delete;       // 持有线程，禁止移动
    CustomByteBlockVideoDecoderWorker &operator=(CustomByteBlockVideoDecoderWorker &&) = delete; // 禁止移动赋值

    // 功能：把收到的线格式包送入内部队列，由工作线程异步解码（停止后或空包直接忽略）。
    void enqueue_packet_bytes(std::span<const std::uint8_t> wire_bytes); // 非阻塞入队：队列满时丢包并告警
    // 功能：请求停止并等待工作线程退出。
    void stop(); // 置停止标志后 join 线程，可重复调用

private: // ---------- 私有实现细节 ----------
    void run();                                       // 工作线程主循环
    void emit_log(int type, const std::string &text) const; // 经 log_handler_ 输出日志
    void handle_decode_error(const std::string &message);   // 处理解码异常，带节流防止刷屏

    CustomByteBlockVideoDecoderConfig config_; // 解码配置
    moodycamel::BlockingReaderWriterQueue<std::vector<std::uint8_t>> packet_queue_{256}; // 包队列，容量 256
    FrameHandler frame_handler_;  // 帧回调
    LogHandler log_handler_;      // 日志回调
    std::atomic<bool> stop_requested_{false}; // 停止标志
    std::thread worker_thread_;             // 工作线程
    bool stream_started_logged_ = false;    // 是否已记录「视频流开始」日志（避免重复）
    std::string last_error_text_;           // 上次错误文本，用于节流
    std::chrono::steady_clock::time_point last_error_time_ = // 上次错误时间点，用于节流判定
            std::chrono::steady_clock::now() - std::chrono::seconds(10); // 初始化为 10 秒前，保证首条错误立即上报
}; // 工作线程类定义结束

}// namespace RoboGo::video // 结束视频模块命名空间
