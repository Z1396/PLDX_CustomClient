#include "video_decoder.h" // 引入本文件的类声明
#include "logging/logging.h" // 引入 spdlog 宏封装（SPDLOG_INFO/WARN/ERROR 等）

// 命名空间 RoboGo::video：业务功能层-视频模块的对外实现，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video { // 进入视频模块命名空间

    // 功能：构造解码器，按优先级选择 HEVC 解码器并初始化解析器与缓存。
    // 思路：先尝试 Nvidia 硬件解码(hevc_cuvid)，失败再回退 CPU 软解(hevc)；
    //       启用低延迟/快速解码标志，随后初始化解析器、数据包与帧缓存，任一关键步骤失败即终止进程。
    VideoDecoder::VideoDecoder(const std::function<void(const AVFrame *frame)> &frame_callback) // 构造函数：接收出帧回调
        : frame_callback(frame_callback) { // 保存每解出一帧时的回调
        // 查找HEVC解码器
        const AVCodec *codecs[] = { // 候选解码器数组，按优先级从前往后尝试
                avcodec_find_decoder_by_name("hevc_cuvid"), // Nvidia 硬件加速（cuvid，走 GPU 解码）
                avcodec_find_decoder_by_name("hevc"),      // CPU 软解码（libavcodec 内置 HEVC 解码器）
        }; // 候选列表结束
        // 确保两个候选解码器都能被找到（开发期断言，便于及时发现环境缺失）
        assert(codecs[0] != nullptr); // 断言硬解解码器注册存在
        assert(codecs[1] != nullptr); // 断言软解解码器注册存在
        const AVCodec *codec = nullptr; // 当前选中的解码器指针（初始为空）
        // 依次尝试硬解、软解，命中可用的解码器即停止
        for (const auto &item: codecs) { // 依次尝试硬解、软解候选
            codec = item; // 先假设该候选可用

            // 创建解码器上下文
            av_codec_context = avcodec_alloc_context3(codec); // 为该解码器创建上下文
            if (!av_codec_context) { // 分配失败
                codec = nullptr; // 放弃该候选
                continue; // 尝试下一个候选
            } // 上下文分配失败分支结束

            // 配置低延迟参数
            av_codec_context->flags |= AV_CODEC_FLAG_LOW_DELAY; // 低延迟模式，减少缓冲带来的延迟
            av_codec_context->flags2 |= AV_CODEC_FLAG2_FAST;    // 快速解码，牺牲少量质量换取速度

            // 尝试打开解码器，失败则释放上下文并继续尝试下一个候选
            if (avcodec_open2(av_codec_context, codec, nullptr) < 0) { // 尝试打开解码器
                SPDLOG_WARN("打开HEVC解码器失败 codec={}", codec->name); // 记录该候选打开失败
                avcodec_free_context(&av_codec_context); // 释放上下文
                codec = nullptr; // 标记该候选无效
                av_codec_context = nullptr; // 复位上下文指针
                continue; // 尝试下一个候选
            } // 打开失败清理分支结束

            SPDLOG_INFO("HEVC解码器已启用 codec={}", codec->name); // 记录最终生效的解码器名称
            break; // 打开成功，跳出候选循环
        } // 候选遍历结束
        if (codec == nullptr) { // 两个候选都不可用
            SPDLOG_CRITICAL("Failed to find HEVC decoder"); // 严重错误：无解码器可用
            std::terminate(); // 无解码器可继续，终止进程
        } // 无可用解码器的兜底检查结束

        // 创建解析器
        av_parser_context = av_parser_init(AV_CODEC_ID_HEVC); // 创建 HEVC 码流解析器
        if (!av_parser_context) { // 解析器创建失败
            SPDLOG_CRITICAL("Failed to find HEVC parser"); // 严重错误：无解析器可用
            std::terminate(); // 无解析器，终止进程
        } // 解析器失败检查结束
        SPDLOG_INFO("HEVC解析器已初始化"); // 记录解析器初始化成功

        // 缓存数据包
        packet = av_packet_alloc(); // 分配可复用的 AVPacket
        if (!packet) { // 分配失败
            SPDLOG_ERROR("分配AVPacket失败"); // 记录内存分配失败
            std::terminate(); // 致命错误，终止进程
        } // 包分配失败检查结束

        // 缓存视频帧
        decoded_frame = av_frame_alloc(); // 分配可复用的 AVFrame
        if (!decoded_frame) { // 分配失败
            SPDLOG_ERROR("分配AVFrame失败"); // 记录内存分配失败
            std::terminate(); // 致命错误，终止进程
        } // 帧分配失败检查结束
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

    // 功能：把一整帧编码数据送入解析器再喂给解码器，解出所有 AVFrame 并通过回调上抛。
    // 思路：用 av_parser_parse2 按字节流逐步切出完整包，每切出一包就 send 给解码器，
    //       再循环 receive 把所有已就绪的帧取出回调，直到缓冲区空(EAGAIN)或出错。
    void VideoDecoder::decode_frame(const std::vector<uint8_t> &frame) const { // const 成员：不修改对象可见状态
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
                    continue; // 跳过该包
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
