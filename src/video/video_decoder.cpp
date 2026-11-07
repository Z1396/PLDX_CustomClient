// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
extern "C" {  // FFmpeg 为 C 接口，需按 C 链接方式包含
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}
// [/pch-split]
#include "video_decoder.h" // 引入本文件的类声明
#include "logging/logging.h" // 引入 spdlog 宏封装（SPDLOG_INFO/WARN/ERROR 等）
#include <libavutil/hwcontext.h> // 硬件解码设备上下文（D3D11VA）
#include "app/perf_stats.h" // 解码耗时统计

// 命名空间 RoboGo::video：业务功能层-视频模块的对外实现，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video { // 进入视频模块命名空间

    // 功能：构造解码器，按优先级选择 HEVC 解码器并初始化解析器与缓存。
    // 思路：先尝试 Nvidia 硬件解码(hevc_cuvid)，失败再回退 CPU 软解(hevc)；
    //       启用低延迟/快速解码标志，随后初始化解析器、数据包与帧缓存。
    //       任一关键步骤失败时【只降级、不终止进程】：把 is_available_ 置 false 并记录 CRITICAL，
    //       后续 decode_frame 会直接返回，界面表现为"无画面"，但小地图/血量/自定义 UI 仍可正常使用
    //       —— 这是 2027 规则明确要求的行为（图传断开时 UI 仍须可用），也避免现场因缺解码器直接闪退。
    // 功能：判断某个解码器是否支持通过 D3D11VA 设备上下文做硬件解码。
    // 思路：遍历 avcodec_get_hw_config 返回的硬件配置，找"由外部设备上下文驱动"且类型为 D3D11VA 的那一条。
    static bool CodecSupportsD3d11va(const AVCodec *codec) {
        if (codec == nullptr) { // 解码器不存在
            return false; // 直接判否
        }
        for (int index = 0;; ++index) { // 逐个查询硬件配置
            const AVCodecHWConfig *config = avcodec_get_hw_config(codec, index); // 取第 index 条配置
            if (config == nullptr) { // 没有更多配置
                break;
            }
            if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0
                && config->device_type == AV_HWDEVICE_TYPE_D3D11VA) { // 支持外部 D3D11VA 设备
                return true; // 命中
            }
        }
        return false; // 未找到
    }

    // 功能：构造解码器，按"硬件优先、软解兜底"的顺序选择 HEVC 解码器。
    // 顺序：① D3D11VA（A/N/I 三家通用）② hevc_cuvid（仅 NVIDIA）③ hevc 软解。
    // 为什么把 D3D11VA 放第一：原来只试 cuvid，队友用 AMD/Intel 核显的笔记本会直接掉到软解，
    // 1080p HEVC 软解要吃 1~2 个核；D3D11VA 是 Windows 上的通用硬解接口，能把 CPU 占用砍半。
    // 任一关键步骤失败时【只降级、不终止进程】：is_available_ 置 false，界面表现为"无画面"，
    // 其余面板照常可用（2027 规则要求图传断开时 UI 仍可用）。
    VideoDecoder::VideoDecoder(const std::function<void(const AVFrame *frame)> &frame_callback) // 构造函数：接收出帧回调
        : frame_callback(frame_callback) { // 保存每解出一帧时的回调

        // ---------- ① D3D11VA 硬解 ----------
        const AVCodec *hevc_codec = avcodec_find_decoder_by_name("hevc"); // 通用 HEVC 解码器（可挂硬件设备）
        if (hevc_codec != nullptr && CodecSupportsD3d11va(hevc_codec)) { // 当前构建支持 D3D11VA
            if (av_hwdevice_ctx_create(&hw_device_context, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) { // 建设备
                AVCodecContext *context = avcodec_alloc_context3(hevc_codec); // 分配上下文
                if (context != nullptr) { // 分配成功
                    context->hw_device_ctx = av_buffer_ref(hw_device_context); // 绑定硬件设备
                    context->flags |= AV_CODEC_FLAG_LOW_DELAY; // 低延迟模式
                    context->flags2 |= AV_CODEC_FLAG2_FAST;    // 快速解码
                    if (avcodec_open2(context, hevc_codec, nullptr) >= 0) { // 打开成功
                        av_codec_context = context; // 采用该上下文
                        is_hardware_decoder_ = true; // 标记硬解
                        SPDLOG_INFO("HEVC解码器已启用 codec=hevc 硬解=d3d11va");
                    } else { // 打不开
                        SPDLOG_WARN("D3D11VA 解码器打开失败，尝试其他候选");
                        avcodec_free_context(&context); // 释放上下文
                    }
                }
            } else { // 设备创建失败
                SPDLOG_WARN("创建 D3D11VA 设备失败，尝试其他候选");
            }
            if (av_codec_context == nullptr && hw_device_context != nullptr) { // 设备建出来但没用上
                av_buffer_unref(&hw_device_context); // 一并释放，避免占着显存
            }
        } else if (hevc_codec != nullptr) { // 构建里没有 D3D11VA 硬解
            // 这里必须说清楚，否则现场只会看到"CPU 占用高"却不知道为什么：
            // FFmpeg 需要编译时带 --enable-hwaccel=hevc_d3d11va 才有这条路径，
            // 3rdparty 里那份裁剪过的静态库目前只带了 cuvid。
            SPDLOG_INFO("当前 FFmpeg 构建未包含 HEVC D3D11VA 硬解（需 --enable-hwaccel=hevc_d3d11va 重新构建）");
        }

        // ---------- ② NVIDIA cuvid ----------
        if (av_codec_context == nullptr) { // 上一步没成功
            const AVCodec *cuvid_codec = avcodec_find_decoder_by_name("hevc_cuvid"); // NVIDIA 专用硬解
            if (cuvid_codec != nullptr) { // 当前构建里有
                AVCodecContext *context = avcodec_alloc_context3(cuvid_codec); // 分配上下文
                if (context != nullptr) { // 分配成功
                    context->flags |= AV_CODEC_FLAG_LOW_DELAY; // 低延迟
                    context->flags2 |= AV_CODEC_FLAG2_FAST;    // 快速
                    if (avcodec_open2(context, cuvid_codec, nullptr) >= 0) { // 打开成功（非 N 卡会失败）
                        av_codec_context = context; // 采用
                        is_hardware_decoder_ = true; // 标记硬解
                        SPDLOG_INFO("HEVC解码器已启用 codec=hevc_cuvid 硬解=cuvid");
                    } else { // 打不开（典型原因：没有 NVIDIA 显卡）
                        avcodec_free_context(&context); // 释放
                    }
                }
            }
        }

        // ---------- ③ 软件解码兜底 ----------
        if (av_codec_context == nullptr) { // 两种硬解都不可用
            if (hevc_codec != nullptr) { // 软解解码器存在
                AVCodecContext *context = avcodec_alloc_context3(hevc_codec); // 分配上下文
                if (context != nullptr) { // 分配成功
                    context->flags |= AV_CODEC_FLAG_LOW_DELAY; // 低延迟
                    context->flags2 |= AV_CODEC_FLAG2_FAST;    // 快速
                    if (avcodec_open2(context, hevc_codec, nullptr) >= 0) { // 打开成功
                        av_codec_context = context; // 采用
                        is_hardware_decoder_ = false; // 软解
                        SPDLOG_INFO("HEVC解码器已启用 codec=hevc 硬解=无(软解)");
                    } else { // 打不开
                        avcodec_free_context(&context); // 释放
                    }
                }
            }
        }

        if (av_codec_context == nullptr) { // 三种都不可用（FFmpeg 构建异常）
            SPDLOG_CRITICAL("未找到可用的HEVC解码器，图传将不可用（其余界面功能不受影响）"); // 严重错误，但不再终止进程
            return; // 直接返回：is_available_ 保持 false，decode_frame 会全部丢弃
        }

        // 创建解析器
        av_parser_context = av_parser_init(AV_CODEC_ID_HEVC); // 创建 HEVC 码流解析器
        if (!av_parser_context) { // 解析器创建失败
            SPDLOG_CRITICAL("未找到HEVC解析器，图传将不可用（其余界面功能不受影响）"); // 严重错误，但不终止进程
            return; // 降级：视为解码器不可用
        }

        // 缓存数据包
        packet = av_packet_alloc(); // 分配可复用的 AVPacket
        if (!packet) { // 分配失败
            SPDLOG_CRITICAL("分配AVPacket失败，图传将不可用"); // 记录内存分配失败
            return; // 降级：视为解码器不可用
        }

        // 缓存视频帧
        decoded_frame = av_frame_alloc(); // 分配可复用的 AVFrame
        if (!decoded_frame) { // 分配失败
            SPDLOG_CRITICAL("分配AVFrame失败，图传将不可用"); // 记录内存分配失败
            return; // 降级：视为解码器不可用
        }

        transfer_frame = av_frame_alloc(); // 硬解帧拷回内存用的中转帧（软解路径用不到，但只分配一次，代价可忽略）
        if (!transfer_frame) { // 分配失败
            SPDLOG_CRITICAL("分配中转AVFrame失败，图传将不可用"); // 记录内存分配失败
            return; // 降级：视为解码器不可用
        }

        is_available_ = true; // 全链路初始化成功
        SPDLOG_INFO("HEVC解析器已初始化"); // 记录解析器初始化成功
    }

    // 功能：析构函数，按资源创建的逆序释放 FFmpeg 所有相关对象。
    VideoDecoder::~VideoDecoder() { // 析构函数体
        SPDLOG_INFO("销毁视频解码器"); // 记录解码器销毁
        if (av_parser_context) { // 解析器存在才释放
            av_parser_close(av_parser_context); // 关闭并释放解析器
        } // 解析器已释放
        if (av_codec_context) { // 解码器上下文存在才释放
            avcodec_free_context(&av_codec_context); // 关闭并释放解码器上下文
        } // 解码器上下文已释放
        if (packet) { // 包缓存存在才释放
            av_packet_free(&packet); // 释放数据包缓存
        } // 包缓存已释放
        if (decoded_frame) { // 帧缓存存在才释放
            av_frame_free(&decoded_frame); // 释放解码帧缓存
        } // 帧缓存已释放
        if (transfer_frame) { // 中转帧存在才释放
            av_frame_free(&transfer_frame); // 释放中转帧
        } // 中转帧已释放
        if (hw_device_context) { // 硬件设备上下文存在才释放
            av_buffer_unref(&hw_device_context); // 释放 D3D11 设备（会顺带释放其占用的显存）
        } // 设备上下文已释放
    }

    // 功能：从载荷中定位起始码并提取 NAL 单元类型。
    // 思路：先判断是否带 4 字节(00 00 00 01)或 3 字节(00 00 01)起始码，定位到 NAL 头；
    //       对 HEVC，类型位于头字节右移 1 位后的低 6 位（H.264 同字段位宽为 5 位，此处按 HEVC 处理）。
    int VideoDecoder::get_nal_type(const std::vector<uint8_t> &payload) { // 静态方法：仅依赖入参载荷
        if (payload.size() < 4) { // 至少需要 4 字节才能容纳起始码+NAL头
            SPDLOG_DEBUG("视频帧过短，无法识别NAL类型 bytes={}", payload.size()); // 记录过短告警
            return -1; // 无法识别，返回 -1
        }

        size_t start_offset = std::string::npos; // 起始码位置（默认无），用于定位 NAL 头
        // 优先匹配 4 字节起始码
        if (payload[0] == 0x00 && payload[1] == 0x00 && // 前两字节必须是 00 00
            payload[2] == 0x00 && payload[3] == 0x01) { // 后两字节为 00 01 即构成 4 字节起始码
            start_offset = 4; // 命中 4 字节起始码
        } else if (payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) { // 尝试 3 字节起始码(00 00 01)
            // 退而求其次匹配 3 字节起始码
            start_offset = 3; // 命中 3 字节起始码
        }

        // 没有起始码或起始码越界，则无法判断 NAL 类型
        if (start_offset == std::string::npos || payload.size() <= start_offset) { // 无起始码或越界则无法判定
            SPDLOG_DEBUG("视频帧缺少起始码，无法识别NAL类型 bytes={}", payload.size()); // 记录缺少起始码
            return -1; // 无法识别，返回 -1
        }

        // 取 NAL 头字节，HEVC 中类型占高 6 位，故右移 1 位后与 0x3F 取低 6 位
        const int nal_type = payload[start_offset] >> 1 & 0x3F; // HEVC NAL 类型占头字节高 6 位（右移 1 位后取低 6 位）
        SPDLOG_TRACE("识别到NAL类型 nal_type={} frame_bytes={}", nal_type, payload.size()); // 记录识别结果
        return nal_type; // 返回 NAL 类型值
    }

    // 功能：硬解发送失败时回退到软件 HEVC 解码器，保证仍能出画面。
    bool VideoDecoder::ReinitializeWithSoftwareDecoder() {
        const AVCodec *software_codec = avcodec_find_decoder_by_name("hevc"); // 软件 HEVC 解码器
        if (!software_codec) {
            SPDLOG_ERROR("未找到软件 HEVC 解码器，无法回退"); // 理论上不会发生
            return false;
        }
        AVCodecContext *new_context = avcodec_alloc_context3(software_codec); // 新建软解上下文
        if (!new_context) {
            SPDLOG_ERROR("分配软件解码器上下文失败"); // 内存不足
            return false;
        }
        new_context->flags |= AV_CODEC_FLAG_LOW_DELAY; // 保持低延迟
        new_context->flags2 |= AV_CODEC_FLAG2_FAST;    // 保持快速解码
        if (avcodec_open2(new_context, software_codec, nullptr) < 0) { // 打开软解
            SPDLOG_ERROR("打开软件 HEVC 解码器失败");
            avcodec_free_context(&new_context);
            return false;
        }
        if (av_codec_context) { // 释放旧硬解上下文
            avcodec_free_context(&av_codec_context);
        }
        av_codec_context = new_context; // 切换到软解
        if (av_parser_context) { // 重建解析器以匹配新上下文
            av_parser_close(av_parser_context);
        }
        av_parser_context = av_parser_init(AV_CODEC_ID_HEVC);
        is_hardware_decoder_ = false; // 标记已不再使用硬解
        if (hw_device_context != nullptr) { // 之前绑定的硬件设备
            av_buffer_unref(&hw_device_context); // 一并释放，避免继续占显存
        } // 结束设备释放
        SPDLOG_WARN("硬解发送失败，已回退到软件 HEVC 解码器");
        return av_parser_context != nullptr;
    }

    // 功能：把一整帧编码数据送入解析器再喂给解码器，解出所有 AVFrame 并通过回调上抛。
    // 思路：用 av_parser_parse2 按字节流逐步切出完整包，每切出一包就 send 给解码器，
    //       再循环 receive 把所有已就绪的帧取出回调，直到缓冲区空(EAGAIN)或出错。
    void VideoDecoder::decode_frame(const std::vector<uint8_t> &frame) { // const 成员：不修改对象可见状态
        // 解码计时：只累加 send_packet 与 receive_frame 两段，**不包含帧回调**。
        // 为什么必须拆开：frame_callback 里做的是 YUV->RGBA 转换 + 入队，
        // 如果把它算进解码，性能面板上"解码"会虚高整整一个转换的时间（实测虚高 7.8ms），
        // 后续所有优化决策都会基于错误的数字。
        struct DecodeTimer {
            double milliseconds = 0.0;  // 累计耗时
            ~DecodeTimer() { RoboGo::app::perf::RecordDecode(milliseconds); }  // 析构时上报（任何 return 都会走到）
        } decode_timer;
        if (!is_available_) { // 初始化就失败了（无解码器/无解析器）：静默丢弃，避免每帧刷一条错误日志
            return; // 直接返回
        } // 结束不可用判断
        if (!av_codec_context || !av_parser_context) { // 解码器或解析器未就绪
            SPDLOG_ERROR("视频解码器未完成初始化，无法解码 frame_bytes={}", frame.size()); // 记录未初始化
            return; // 直接返回，不解码
        }

        const uint8_t *data = frame.data(); // 指向当前待解析的码流起始
        int size = static_cast<int>(frame.size()); // 待解析剩余字节数

        // 逐段喂入解析器，直到整帧数据全部被消费
        while (size > 0) { // 剩余字节未消费完就继续解析
            // 从当前剩余流中解析出至少一个完整包
            int ret = av_parser_parse2(av_parser_context, av_codec_context, // 解析器+解码器上下文（后者用于更新参数集）
                                       &packet->data, &packet->size, // 输出：切出包的数据指针与长度（指向输入缓冲，零拷贝）
                                       data, size, // 输入：当前游标与剩余长度
                                       AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0); // 不提供时间戳，末位 0 为保留参数
            if (ret < 0) { // 解析出错
                SPDLOG_ERROR("视频码流解析错误 ret={} remaining_bytes={}", ret, size); // 记录解析错误
                break; // 中止本次解码
            }
            // 推进游标，减少剩余未解析字节数
            data += ret; // 游标前移已消费字节
            size -= ret; // 剩余字节相应减少
            if (packet->size > 0) { // 切出有效包才送入解码器
                // 发送到解码器
                const auto send_start = std::chrono::steady_clock::now();  // 计时起点
                ret = avcodec_send_packet(av_codec_context, packet); // 将包送入解码器输入队列
                decode_timer.milliseconds += std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - send_start).count();  // 累计发送耗时
                // EAGAIN 表示解码器输入缓冲已满，属正常情况，可继续取值
                if (ret < 0 && ret != AVERROR(EAGAIN)) { // 除缓冲满外的错误
                    SPDLOG_ERROR("发送包到解码器失败: {}", ret); // 记录发送失败
                    if (is_hardware_decoder_) { // 硬解失败：尝试回退软解
                        if (ReinitializeWithSoftwareDecoder()) { // 切换成功
                            decode_frame(frame); // 用软解重新解当前整帧
                        }
                        return; // 本帧已处理或回退失败，结束
                    }
                    continue; // 软解仍失败：跳过该包
                }
                // 接收解码后的帧
                while (ret >= 0) { // 只要上次操作成功就继续取帧
                    const auto receive_start = std::chrono::steady_clock::now();  // 计时起点
                    ret = avcodec_receive_frame(av_codec_context, decoded_frame); // 从解码器取一帧
                    decode_timer.milliseconds += std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - receive_start).count();  // 累计取帧耗时（硬解时这里包含等 GPU）
                    // EAGAIN/EOF 表示当前没有更多帧可输出，结束取帧循环
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { // 无更多帧
                        break;
                    }
                    if (ret < 0) { // 其他解码错误
                        SPDLOG_ERROR("解码错误: {}", ret); // 记录解码错误
                        break;
                    }
                    // 解出一帧：D3D11VA 的输出是 GPU 纹理，先拷回系统内存（NV12）再交给上层；
                    // 其余路径（cuvid/软解）本来就是系统内存帧，直接回调。
                    if (decoded_frame->format == AV_PIX_FMT_D3D11) { // 硬解输出在显存里
                        av_frame_unref(transfer_frame); // 中转帧必须先清空，transfer 要求目标是干净帧
                        if (av_hwframe_transfer_data(transfer_frame, decoded_frame, 0) < 0) { // 拷回内存
                            SPDLOG_ERROR("D3D11 解码帧拷回系统内存失败"); // 记录失败
                            av_frame_unref(decoded_frame); // 释放源帧
                            break; // 本帧放弃
                        }
                        frame_callback(transfer_frame); // 回调内存帧
                        av_frame_unref(transfer_frame); // 释放中转帧引用
                    } else { // 普通内存帧
                        frame_callback(decoded_frame); // 回调上抛解码后的 AVFrame
                    }
                    av_frame_unref(decoded_frame); // 释放帧引用，供下次复用
                }
            }
        }
    }

}  // namespace RoboGo::video
