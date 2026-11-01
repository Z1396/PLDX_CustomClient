// video_decoder.h：视频解码器接口——封装 FFmpeg HEVC 解码器与码流解析器，把编码比特流解码为 AVFrame 并回调通知上层。
#pragma once // 头文件只包含一次，防止重复定义

#include <pch.h> // 预编译头：引入 AVCodecContext/AVPacket/AVFrame/std::function 等依赖

// 命名空间 RoboGo::video：业务功能层-视频模块的对外声明，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video { // 进入视频模块命名空间

    // 视频解码器：封装 FFmpeg 的 HEVC 解码器与码流解析器，把编码后的比特流解析并解码为 AVFrame。
    class VideoDecoder { // 解码器类定义开始
    private: // ---------- 私有成员：全部 FFmpeg 原生资源 ----------
        AVCodecContext *av_codec_context = nullptr;        // FFmpeg 解码器上下文，持有打开的解码器实例
        AVCodecParserContext *av_parser_context = nullptr; // 码流解析器上下文，用于从原始流中切分出完整包
        AVPacket *packet = nullptr;                        // 复用的数据包缓存，避免每次解码都重新分配
        AVFrame *decoded_frame = nullptr;                  // 复用的解码帧缓存，接收解码器输出
        std::function<void(const AVFrame *frame)> frame_callback; // 每解出一帧 AVFrame 时回调通知上层

    public: // ---------- 公开接口 ----------
        // 功能：构造解码器，优先尝试 Nvidia 硬解(hevc_cuvid)，失败则回退到 CPU 软解(hevc)。
        // 参数：frame_callback 为解出 AVFrame 后的回调；找不到任何 HEVC 解码器或解析器会直接终止进程。
        explicit VideoDecoder(const std::function<void(const AVFrame *frame)> &frame_callback); // 传入帧回调并完成解码器初始化

        // 功能：析构并释放所有 FFmpeg 资源（解析器、解码器上下文、包与帧缓存）。
        ~VideoDecoder(); // 逆序释放解析器、解码器上下文、包与帧缓存四类资源

        // 功能：从一帧比特流载荷中识别 NAL 单元类型（H.264/HEVC 共用逻辑，取第二字节低 6 位）。
        // 参数：payload 为含起始码的完整 NAL 单元；返回值 -1 表示无法识别，否则为 NAL 类型值。
        static int get_nal_type(const std::vector<uint8_t> &payload); // 静态工具函数：仅依赖入参，不访问成员状态

        // 功能：把一整帧编码数据送入解析器再喂给解码器，解出所有 AVFrame 并通过回调上抛。
        // 参数：frame 为完整帧的字节数据；const 表示本方法不修改解码器成员以外的外部状态。
        void decode_frame(const std::vector<uint8_t> &frame) const; // const 成员函数：只写 FFmpeg 上下文内部缓冲
    }; // 解码器类定义结束

}// namespace RoboGo::video // 结束视频模块命名空间
