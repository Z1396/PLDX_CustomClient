#include "video_decoder.h"
#include "logging/logging.h"

// 命名空间 RoboGo::video：业务功能层-视频模块的对外实现，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video {

    // 功能：构造解码器，按优先级选择 HEVC 解码器并初始化解析器与缓存。
    // 思路：先尝试 Nvidia 硬件解码(hevc_cuvid)，失败再回退 CPU 软解(hevc)；
    //       启用低延迟/快速解码标志，随后初始化解析器、数据包与帧缓存，任一关键步骤失败即终止进程。
    VideoDecoder::VideoDecoder(const std::function<void(const AVFrame *frame)> &frame_callback)
        : frame_callback(frame_callback) {
        // 查找HEVC解码器
        const AVCodec *codecs[] = {
                avcodec_find_decoder_by_name("hevc_cuvid"),// Nvidia 硬件加速
                avcodec_find_decoder_by_name("hevc"),      // CPU 软解码
        };
        // 确保两个候选解码器都能被找到（开发期断言，便于及时发现环境缺失）
        assert(codecs[0] != nullptr);
        assert(codecs[1] != nullptr);
        const AVCodec *codec = nullptr;
        // 依次尝试硬解、软解，命中可用的解码器即停止
        for (const auto &item: codecs) {
            codec = item;

            // 创建解码器上下文
            av_codec_context = avcodec_alloc_context3(codec);
            if (!av_codec_context) {
                codec = nullptr;
                continue;
            }

            // 配置低延迟参数
            av_codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY; // 低延迟模式，减少缓冲带来的延迟
            av_codec_context->flags2 |= AV_CODEC_FLAG2_FAST;    // 快速解码，牺牲少量质量换取速度

            // 尝试打开解码器，失败则释放上下文并继续尝试下一个候选
            if (avcodec_open2(av_codec_context, codec, nullptr) < 0) {
                SPDLOG_WARN("打开HEVC解码器失败 codec={}", codec->name);
                avcodec_free_context(&av_codec_context);
                codec = nullptr;
                av_codec_context = nullptr;
                continue;
            }

            SPDLOG_INFO("HEVC解码器已启用 codec={}", codec->name);
            break;
        }
        if (codec == nullptr) {
            SPDLOG_CRITICAL("Failed to find HEVC decoder");
            std::terminate();
        }

        // 创建解析器
        av_parser_context = av_parser_init(AV_CODEC_ID_HEVC);
        if (!av_parser_context) {
            SPDLOG_CRITICAL("Failed to find HEVC parser");
            std::terminate();
        }
        SPDLOG_INFO("HEVC解析器已初始化");

        // 缓存数据包
        packet = av_packet_alloc();
        if (!packet) {
            SPDLOG_ERROR("分配AVPacket失败");
            std::terminate();
        }

        // 缓存视频帧
        decoded_frame = av_frame_alloc();
        if (!decoded_frame) {
            SPDLOG_ERROR("分配AVFrame失败");
            std::terminate();
        }
    }

    // 功能：析构函数，按资源创建的逆序释放 FFmpeg 所有相关对象。
    VideoDecoder::~VideoDecoder() {
        SPDLOG_INFO("销毁视频解码器");
        if (av_parser_context) {
            av_parser_close(av_parser_context); // 关闭并释放解析器
        }
        if (av_codec_context) {
            avcodec_free_context(&av_codec_context); // 关闭并释放解码器上下文
        }
        if (packet) {
            av_packet_free(&packet); // 释放数据包缓存
        }
        if (decoded_frame) {
            av_frame_free(&decoded_frame); // 释放解码帧缓存
        }
    }

    // 功能：从载荷中定位起始码并提取 NAL 单元类型。
    // 思路：先判断是否带 4 字节(00 00 00 01)或 3 字节(00 00 01)起始码，定位到 NAL 头；
    //       对 HEVC，类型位于头字节右移 1 位后的低 6 位（H.264 同字段位宽为 5 位，此处按 HEVC 处理）。
    int VideoDecoder::get_nal_type(const std::vector<uint8_t> &payload) {
        if (payload.size() < 4) {
            SPDLOG_DEBUG("视频帧过短，无法识别NAL类型 bytes={}", payload.size());
            return -1;
        }

        size_t start_offset = std::string::npos;
        // 优先匹配 4 字节起始码
        if (payload[0] == 0x00 && payload[1] == 0x00 &&
            payload[2] == 0x00 && payload[3] == 0x01) {
            start_offset = 4;
        } else if (payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) {
            // 退而求其次匹配 3 字节起始码
            start_offset = 3;
        }

        // 没有起始码或起始码越界，则无法判断 NAL 类型
        if (start_offset == std::string::npos || payload.size() <= start_offset) {
            SPDLOG_DEBUG("视频帧缺少起始码，无法识别NAL类型 bytes={}", payload.size());
            return -1;
        }

        // 取 NAL 头字节，HEVC 中类型占高 6 位，故右移 1 位后与 0x3F 取低 6 位
        const int nal_type = payload[start_offset] >> 1 & 0x3F;
        SPDLOG_TRACE("识别到NAL类型 nal_type={} frame_bytes={}", nal_type, payload.size());
        return nal_type;
    }

    // 功能：把一整帧编码数据送入解析器再喂给解码器，解出所有 AVFrame 并通过回调上抛。
    // 思路：用 av_parser_parse2 按字节流逐步切出完整包，每切出一包就 send 给解码器，
    //       再循环 receive 把所有已就绪的帧取出回调，直到缓冲区空(EAGAIN)或出错。
    void VideoDecoder::decode_frame(const std::vector<uint8_t> &frame) const {
        if (!av_codec_context || !av_parser_context) {
            SPDLOG_ERROR("视频解码器未完成初始化，无法解码 frame_bytes={}", frame.size());
            return;
        }

        const uint8_t *data = frame.data();
        int size = static_cast<int>(frame.size());

        // 逐段喂入解析器，直到整帧数据全部被消费
        while (size > 0) {
            // 从当前剩余流中解析出至少一个完整包
            int ret = av_parser_parse2(av_parser_context, av_codec_context,
                                       &packet->data, &packet->size,
                                       data, size,
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
            if (ret < 0) {
                SPDLOG_ERROR("视频码流解析错误 ret={} remaining_bytes={}", ret, size);
                break;
            }
            // 推进游标，减少剩余未解析字节数
            data += ret;
            size -= ret;
            if (packet->size > 0) {
                // 发送到解码器
                ret = avcodec_send_packet(av_codec_context, packet);
                // EAGAIN 表示解码器输入缓冲已满，属正常情况，可继续取值
                if (ret < 0 && ret != AVERROR(EAGAIN)) {
                    SPDLOG_ERROR("发送包到解码器失败: {}", ret);
                    continue;
                }
                // 接收解码后的帧
                while (ret >= 0) {
                    ret = avcodec_receive_frame(av_codec_context, decoded_frame);
                    // EAGAIN/EOF 表示当前没有更多帧可输出，结束取帧循环
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                        break;
                    }
                    if (ret < 0) {
                        SPDLOG_ERROR("解码错误: {}", ret);
                        break;
                    }
                    // 解出一帧：回调上抛，并释放帧引用以便后续复用
                    frame_callback(decoded_frame);
                    av_frame_unref(decoded_frame);
                }
            }
        }
    }

}// namespace RoboGo::video
