#include "video_decoder.h" // 引入本文件的类声明
#include "logging/logging.h" // 引入 spdlog 宏封装（SPDLOG_INFO/WARN/ERROR 等）

// 命名空间 RoboGo::video：业务功能层-视频模块的对外实现，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video { // 进入视频模块命名空间

    // 功能：构造解码器，按优先级选择 HEVC 解码器并初始化解析器与缓存。
    // 思路：先尝试 Nvidia 硬件解码(hevc_cuvid)，失败再回退 CPU 软解(hevc)；
    //       启用低延迟/快速解码标志，随后初始化解析器、数据包与帧缓存。
    //       任一关键步骤失败时【只降级、不终止进程】：把 is_available_ 置 false 并记录 CRITICAL，
    //       后续 decode_frame 会直接返回，界面表现为"无画面"，但小地图/血量/自定义 UI 仍可正常使用
    //       —— 这是 2027 规则明确要求的行为（图传断开时 UI 仍须可用），也避免现场因缺解码器直接闪退。
    VideoDecoder::VideoDecoder(const std::function<void(const AVFrame *frame)> &frame_callback) // 构造函数：接收出帧回调
        : frame_callback(frame_callback) { // 保存每解出一帧时的回调
        // 查找HEVC解码器
        const AVCodec *codecs[] = { // 候选解码器数组，按优先级从前往后尝试
                avcodec_find_decoder_by_name("hevc_cuvid"), // Nvidia 硬件加速（cuvid，走 GPU 解码）
                avcodec_find_decoder_by_name("hevc"),      // CPU 软解码（libavcodec 内置 HEVC 解码器）
        }; // 候选列表结束
        // 依次尝试硬解、软解，命中可用的解码器即停止；某个候选在当前 FFmpeg 构建里不存在时直接跳过
        for (const AVCodec *candidate: codecs) { // 依次尝试硬解、软解候选
            if (candidate == nullptr) { // 该候选未编译进当前 FFmpeg 构建
                continue; // 直接跳过（原实现用 assert，Debug 下会直接 abort，Release 下会把空指针传进 avcodec_open2）
            } // 结束候选空指针判断

            // 创建解码器上下文
            av_codec_context = avcodec_alloc_context3(candidate); // 为该解码器创建上下文
            if (!av_codec_context) { // 分配失败
                continue; // 尝试下一个候选
            } // 上下文分配失败分支结束

            // 配置低延迟参数
            av_codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY; // 低延迟模式，减少缓冲带来的延迟
            av_codec_context->flags2 |= AV_CODEC_FLAG2_FAST;    // 快速解码，牺牲少量质量换取速度

            // 尝试打开解码器，失败则释放上下文并继续尝试下一个候选
            if (avcodec_open2(av_codec_context, candidate, nullptr) < 0) { // 尝试打开解码器
                SPDLOG_WARN("打开HEVC解码器失败 codec={}", candidate->name); // 记录该候选打开失败
                avcodec_free_context(&av_codec_context); // 释放上下文
                av_codec_context = nullptr; // 复位上下文指针
                continue; // 尝试下一个候选
            } // 打开失败清理分支结束

            is_hardware_decoder_ = (std::string_view(candidate->name) == "hevc_cuvid"); // 记录是否为硬解
            SPDLOG_INFO("HEVC解码器已启用 codec={}", candidate->name); // 记录最终生效的解码器名称
            break; // 打开成功，跳出候选循环
        } // 候选遍历结束
        if (av_codec_context == nullptr) { // 两个候选都不可用（FFmpeg 构建里没有 HEVC 解码器）
            SPDLOG_CRITICAL("未找到可用的HEVC解码器，图传将不可用（其余界面功能不受影响）"); // 严重错误，但不再终止进程
            return; // 直接返回：is_available_ 保持 false，decode_frame 会全部丢弃
        } // 无可用解码器的兜底检查结束

        // 创建解析器
        av_parser_context = av_parser_init(AV_CODEC_ID_HEVC); // 创建 HEVC 码流解析器
        if (!av_parser_context) { // 解析器创建失败
            SPDLOG_CRITICAL("未找到HEVC解析器，图传将不可用（其余界面功能不受影响）"); // 严重错误，但不终止进程
            return; // 降级：视为解码器不可用
        } // 解析器失败检查结束

        // 缓存数据包
        packet = av_packet_alloc(); // 分配可复用的 AVPacket
        if (!packet) { // 分配失败
            SPDLOG_CRITICAL("分配AVPacket失败，图传将不可用"); // 记录内存分配失败
            return; // 降级：视为解码器不可用
        } // 包分配失败检查结束

        // 缓存视频帧
        decoded_frame = av_frame_alloc(); // 分配可复用的 AVFrame
        if (!decoded_frame) { // 分配失败
            SPDLOG_CRITICAL("分配AVFrame失败，图传将不可用"); // 记录内存分配失败
            return; // 降级：视为解码器不可用
        } // 帧分配失败检查结束

        is_available_ = true; // 全链路初始化成功
        SPDLOG_INFO("HEVC解析器已初始化"); // 记录解析器初始化成功
    } // 构造函数结束

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
    } // 析构函数结束

    // 功能：从载荷中定位起始码并提取 NAL 单元类型。
    // 思路：先判断是否带 4 字节(00 00 00 01)或 3 字节(00 00 01)起始码，定位到 NAL 头；
    //       对 HEVC，类型位于头字节右移 1 位后的低 6 位（H.264 同字段位宽为 5 位，此处按 HEVC 处理）。
    int VideoDecoder::get_nal_type(const std::vector<uint8_t> &payload) { // 静态方法：仅依赖入参载荷
        if (payload.size() < 4) { // 至少需要 4 字节才能容纳起始码+NAL头
            SPDLOG_DEBUG("视频帧过短，无法识别NAL类型 bytes={}", payload.size()); // 记录过短告警
            return -1; // 无法识别，返回 -1
        } // 载荷过短检查结束

        size_t start_offset = std::string::npos; // 起始码位置（默认无），用于定位 NAL 头
        // 优先匹配 4 字节起始码
        if (payload[0] == 0x00 && payload[1] == 0x00 && // 前两字节必须是 00 00
            payload[2] == 0x00 && payload[3] == 0x01) { // 后两字节为 00 01 即构成 4 字节起始码
            start_offset = 4; // 命中 4 字节起始码
        } else if (payload[0] == 0x00 && payload[1] == 0x00 && payload[2] == 0x01) { // 尝试 3 字节起始码(00 00 01)
            // 退而求其次匹配 3 字节起始码
            start_offset = 3; // 命中 3 字节起始码
        } // 起始码匹配结束

        // 没有起始码或起始码越界，则无法判断 NAL 类型
        if (start_offset == std::string::npos || payload.size() <= start_offset) { // 无起始码或越界则无法判定
            SPDLOG_DEBUG("视频帧缺少起始码，无法识别NAL类型 bytes={}", payload.size()); // 记录缺少起始码
            return -1; // 无法识别，返回 -1
        } // 起始码有效性检查结束

        // 取 NAL 头字节，HEVC 中类型占高 6 位，故右移 1 位后与 0x3F 取低 6 位
        const int nal_type = payload[start_offset] >> 1 & 0x3F; // HEVC NAL 类型占头字节高 6 位（右移 1 位后取低 6 位）
        SPDLOG_TRACE("识别到NAL类型 nal_type={} frame_bytes={}", nal_type, payload.size()); // 记录识别结果
        return nal_type; // 返回 NAL 类型值
    } // get_nal_type 结束

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
        SPDLOG_WARN("hevc_cuvid 发送失败，已回退到软件 HEVC 解码器");
        return av_parser_context != nullptr;
    }

    // 功能：把一整帧编码数据送入解析器再喂给解码器，解出所有 AVFrame 并通过回调上抛。
    // 思路：用 av_parser_parse2 按字节流逐步切出完整包，每切出一包就 send 给解码器，
    //       再循环 receive 把所有已就绪的帧取出回调，直到缓冲区空(EAGAIN)或出错。
    void VideoDecoder::decode_frame(const std::vector<uint8_t> &frame) { // const 成员：不修改对象可见状态
        if (!is_available_) { // 初始化就失败了（无解码器/无解析器）：静默丢弃，避免每帧刷一条错误日志
            return; // 直接返回
        } // 结束不可用判断
        if (!av_codec_context || !av_parser_context) { // 解码器或解析器未就绪
            SPDLOG_ERROR("视频解码器未完成初始化，无法解码 frame_bytes={}", frame.size()); // 记录未初始化
            return; // 直接返回，不解码
        } // 未初始化检查结束

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
            } // 解析错误分支结束
            // 推进游标，减少剩余未解析字节数
            data += ret; // 游标前移已消费字节
            size -= ret; // 剩余字节相应减少
            if (packet->size > 0) { // 切出有效包才送入解码器
                // 发送到解码器
                ret = avcodec_send_packet(av_codec_context, packet); // 将包送入解码器输入队列
                // EAGAIN 表示解码器输入缓冲已满，属正常情况，可继续取值
                if (ret < 0 && ret != AVERROR(EAGAIN)) { // 除缓冲满外的错误
                    SPDLOG_ERROR("发送包到解码器失败: {}", ret); // 记录发送失败
                    if (is_hardware_decoder_) { // 硬解失败：尝试回退软解
                        if (ReinitializeWithSoftwareDecoder()) { // 切换成功
                            decode_frame(frame); // 用软解重新解当前整帧
                        } // if 结束
                        return; // 本帧已处理或回退失败，结束
                    } // if 结束
                    continue; // 软解仍失败：跳过该包
                } // 发送失败分支结束
                // 接收解码后的帧
                while (ret >= 0) { // 只要上次操作成功就继续取帧
                    ret = avcodec_receive_frame(av_codec_context, decoded_frame); // 从解码器取一帧
                    // EAGAIN/EOF 表示当前没有更多帧可输出，结束取帧循环
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) { // 无更多帧
                        break; // 结束取帧循环
                    } // 无帧可取的退出分支结束
                    if (ret < 0) { // 其他解码错误
                        SPDLOG_ERROR("解码错误: {}", ret); // 记录解码错误
                        break; // 结束取帧循环
                    } // 解码错误分支结束
                    // 解出一帧：回调上抛，并释放帧引用以便后续复用
                    frame_callback(decoded_frame); // 回调上抛解码后的 AVFrame
                    av_frame_unref(decoded_frame); // 释放帧引用，供下次复用
                } // 取帧循环结束
            } // 有效包处理结束
        } // 整帧消费循环结束
    } // decode_frame 结束

}// namespace RoboGo::video // 结束视频模块命名空间
