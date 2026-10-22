#pragma once

#include <pch.h>

namespace RoboGo::video {

    // 视频解码器：封装 FFmpeg 的 HEVC 解码器与码流解析器，把编码后的比特流解析并解码为 AVFrame。
    class VideoDecoder {
    private:
        AVCodecContext *av_codec_context = nullptr;        // FFmpeg 解码器上下文，持有打开的解码器实例
        AVCodecParserContext *av_parser_context = nullptr; // 码流解析器上下文，用于从原始流中切分出完整包
        AVPacket *packet = nullptr;                        // 复用的数据包缓存，避免每次解码都重新分配
        AVFrame *decoded_frame = nullptr;                  // 复用的解码帧缓存，接收解码器输出
        std::function<void(const AVFrame *frame)> frame_callback; // 每解出一帧 AVFrame 时回调通知上层

    public:
        // 功能：构造解码器，优先尝试 Nvidia 硬解(hevc_cuvid)，失败则回退到 CPU 软解(hevc)。
        // 参数：frame_callback 为解出 AVFrame 后的回调；找不到任何 HEVC 解码器或解析器会直接终止进程。
        explicit VideoDecoder(const std::function<void(const AVFrame *frame)> &frame_callback);

        // 功能：析构并释放所有 FFmpeg 资源（解析器、解码器上下文、包与帧缓存）。
        ~VideoDecoder();

        // 功能：从一帧比特流载荷中识别 NAL 单元类型（H.264/HEVC 共用逻辑，取第二字节低 6 位）。
        // 参数：payload 为含起始码的完整 NAL 单元；返回值 -1 表示无法识别，否则为 NAL 类型值。
        static int get_nal_type(const std::vector<uint8_t> &payload);

        // 功能：把一整帧编码数据送入解析器再喂给解码器，解出所有 AVFrame 并通过回调上抛。
        // 参数：frame 为完整帧的字节数据；const 表示本方法不修改解码器成员以外的外部状态。
        void decode_frame(const std::vector<uint8_t> &frame) const;
    };

}// namespace RoboGo::video
