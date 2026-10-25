// custom_byte_block_video_decoder.cpp：自定义字节块解码器实现——解析序列化包与传输包束两种线格式，按 H.264 解码并转换为 RGBA 帧。
#include "custom_byte_block_video_decoder.h"

namespace RoboGo::video {

namespace {

// 协议常量：自定义字节块有两种线格式，包上限统一 300 字节（受传输限制）。
constexpr std::size_t kSerializedPacketHeaderBytes = 2;  // 序列化包头：2 字节序列号
constexpr std::size_t kMaxSupportedPacketBytes = 300;     // 单条线格式包的最大字节数
constexpr std::size_t kTransportLengthPrefixBytes = 2;    // 传输包束中每子包前的 2 字节长度前缀
constexpr std::size_t kMaxInnerPacketBytes = kMaxSupportedPacketBytes - kTransportLengthPrefixBytes; // 子包净负载上限 = 298
constexpr auto kSequenceResyncIdleTimeout = std::chrono::milliseconds(800);     // 空闲超过此值即允许序列重同步
constexpr std::uint16_t kSequenceResyncBackwardThreshold = 2048;               // 向后跳变超过此阈值即重同步

// 线格式枚举：未知 / 单条序列化包 / 多条打包的传输包束。
enum class WireFormat {
    Unknown,          // 尚未识别：首包到达后按特征判定，判定前不解码
    SerializedPacket, // 单条序列化包：2 字节序列号 + 净负载，一包即一个子包
    TransportBundle,  // 传输包束：一包内含有多个「2 字节长度前缀 + 子包」，需逐段拆分
};

// 反序列化后的单条序列化包：序列号 + 净负载。
struct SerializedPacket {
    std::uint16_t sequence_id = 0;          // 包序列号（大端），用于乱序重排与丢包检测
    std::vector<std::uint8_t> payload;      // 净负载：去掉 2 字节包头后的实际码流数据
};

// 功能：按大端序读取 2 字节无符号整数（网络字节序）。
std::uint16_t read_u16_be(const std::uint8_t *input) {
    return static_cast<std::uint16_t>(
            (static_cast<std::uint16_t>(input[0]) << 8) |
            static_cast<std::uint16_t>(input[1]));
}

// 功能：把 FFmpeg 错误码转成可读字符串。
std::string ffmpeg_error_to_string(const int errnum) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{};
    av_strerror(errnum, buffer.data(), buffer.size());
    return std::string(buffer.data());
}

// 功能：校验解码配置各项上限是否为正，非法则抛异常。
void validate_config(const CustomByteBlockVideoDecoderConfig &config) {
    if (config.max_reorder_frames <= 0) {
        throw std::invalid_argument("CustomByteBlock decoder max_reorder_frames must be positive");
    }
    if (config.max_access_unit_bytes == 0U) {
        throw std::invalid_argument("CustomByteBlock decoder max_access_unit_bytes must be positive");
    }
    if (config.max_assembly_bytes == 0U) {
        throw std::invalid_argument("CustomByteBlock decoder max_assembly_bytes must be positive");
    }
    if (config.max_frame_dimension <= 0) {
        throw std::invalid_argument("CustomByteBlock decoder max_frame_dimension must be positive");
    }
    if (config.max_frame_pixels == 0U) {
        throw std::invalid_argument("CustomByteBlock decoder max_frame_pixels must be positive");
    }
}

// 功能：把线字节反序列化为一条 SerializedPacket（前 2 字节为序列号，其余为净负载）。
// 思路：先校验长度下限(含头)与上限(300)，用 read_u16_be 取序列号，剩余部分作为负载，并禁止空负载。
SerializedPacket deserialize_serialized_packet(const std::span<const std::uint8_t> wire_bytes) {
    if (wire_bytes.size() < kSerializedPacketHeaderBytes) {
        throw std::invalid_argument("wire packet is smaller than the serialized packet header");
    }
    if (wire_bytes.size() > kMaxSupportedPacketBytes) {
        throw std::invalid_argument("wire packet exceeds 300 bytes");
    }

    SerializedPacket packet;
    packet.sequence_id = read_u16_be(wire_bytes.data());
    // 跳过 2 字节头，其余全部作为净负载
    packet.payload.assign(wire_bytes.begin() + static_cast<std::ptrdiff_t>(kSerializedPacketHeaderBytes), wire_bytes.end());

    if (packet.payload.empty()) {
        throw std::invalid_argument("serialized packet has empty payload");
    }

    return packet;
}

// 功能：把「传输包束」拆成多条内部序列化包视图(span)，免去拷贝。
// 思路：按 (2 字节长度 + 负载) 循环切分；长度为 0 表示填充结束(其后必须全 0)；子包超长或截断则报错。
std::vector<std::span<const std::uint8_t>> unpack_transport_packet_spans(const std::span<const std::uint8_t> transport_packet) {
    if (transport_packet.empty()) {
        throw std::invalid_argument("transport packet is empty");
    }
    if (transport_packet.size() > kMaxSupportedPacketBytes) {
        throw std::invalid_argument("transport packet exceeds 300 bytes");
    }

    std::vector<std::span<const std::uint8_t>> packets;
    std::size_t read_offset = 0U;
    // 只要还能读到一个长度前缀就继续
    while (read_offset + kTransportLengthPrefixBytes <= transport_packet.size()) {
        const std::uint16_t packet_size = read_u16_be(
                transport_packet.data() + static_cast<std::ptrdiff_t>(read_offset));
        read_offset += kTransportLengthPrefixBytes; // 跳过长度前缀

        // 长度 0 为填充标记：其后必须全部为 0，否则视为协议错误
        if (packet_size == 0U) {
            const bool has_non_zero_padding = std::any_of(
                    transport_packet.begin() + static_cast<std::ptrdiff_t>(read_offset),
                    transport_packet.end(),
                    [](const std::uint8_t value) { return value != 0U; });
            if (has_non_zero_padding) {
                throw std::invalid_argument("transport packet has non-zero bytes after padding marker");
            }
            break;
        }

        if (packet_size > kMaxInnerPacketBytes) {
            throw std::invalid_argument("transport packet has an inner packet larger than 298 bytes");
        }
        if (read_offset + packet_size > transport_packet.size()) {
            throw std::invalid_argument("transport packet has a truncated inner packet");
        }

        // 记录该子包的视图（不拷贝），并推进读取偏移
        packets.emplace_back(
                transport_packet.data() + static_cast<std::ptrdiff_t>(read_offset),
                packet_size);
        read_offset += packet_size;
    }

    return packets;
}

// 功能：把线格式枚举转为日志用字符串。
const char *wire_format_to_string(const WireFormat format) {
    switch (format) {
        case WireFormat::SerializedPacket:
            return "serialized-packet";
        case WireFormat::TransportBundle:
            return "transport-bundle";
        case WireFormat::Unknown:
        default:
            return "unknown";
    }
}

// 功能：用 16 位回绕算术判断 lhs 是否比 rhs 更新（即更靠前）。
// 思路：无符号差值的符号位落在 [0x8000, 0xFFFF] 表示 lhs 落后，落在 (0, 0x8000) 表示领先。
bool sequence_is_newer(const std::uint16_t lhs, const std::uint16_t rhs) {
    const std::uint16_t delta = static_cast<std::uint16_t>(lhs - rhs);
    return delta != 0U && delta < 0x8000U;
}

// 功能：从 offset 起查找下一个 H.264/HEVC 起始码(00 00 00 01 或 00 00 01)，返回其位置与长度。
// 参数：prefix_len 输出 3 或 4；返回值为起始码起始下标，找不到则返回 size(哨兵)。
std::size_t find_start_code_prefix(const std::uint8_t *data,
                                   const std::size_t size,
                                   const std::size_t offset,
                                   std::size_t *prefix_len) {
    if (prefix_len == nullptr) {
        throw std::invalid_argument("find_start_code_prefix() received null prefix_len");
    }
    if (data == nullptr && size != 0U) {
        throw std::invalid_argument("find_start_code_prefix() received null data with non-zero size");
    }

    // 逐字节扫描，优先匹配 4 字节起始码，其次 3 字节起始码
    for (std::size_t i = offset; (i + 3U) <= size; ++i) {
        if ((i + 4U) <= size &&
            data[i] == 0U &&
            data[i + 1U] == 0U &&
            data[i + 2U] == 0U &&
            data[i + 3U] == 1U) {
            *prefix_len = 4U;
            return i;
        }
        if (data[i] == 0U && data[i + 1U] == 0U && data[i + 2U] == 1U) {
            *prefix_len = 3U;
            return i;
        }
    }

    return size; // 未找到
}

// 功能：判断一段访问单元是否包含 IDR 关键帧（NAL 类型 5）。
// 思路：逐个定位起始码，取 NAL 头低 5 位作为类型(H.264)，遇到类型 5 即返回 true。
bool access_unit_has_idr(const std::uint8_t *data, const std::size_t size) {
    if (data == nullptr && size != 0U) {
        throw std::invalid_argument("access_unit_has_idr() received null data with non-zero size");
    }

    std::size_t search_offset = 0U;
    while (search_offset < size) {
        std::size_t prefix_len = 0U;
        const std::size_t prefix_offset = find_start_code_prefix(data, size, search_offset, &prefix_len);
        if (prefix_offset >= size) {
            break;
        }

        const std::size_t nal_header_offset = prefix_offset + prefix_len;
        if (nal_header_offset >= size) {
            break; // 起始码后没有 NAL 头，结束
        }

        // H.264 NAL 类型占头字节低 5 位
        const std::uint8_t nal_type = data[nal_header_offset] & 0x1fU;
        if (nal_type == 5U) {
            return true;
        }

        search_offset = nal_header_offset + 1U; // 跳到下一个，继续查找
    }

    return false;
}

}// namespace

// 解码器实现体（PIMPL 的 Impl）：集中持有全部 FFmpeg 资源与解码中间状态，
// 目的是让公开头文件不暴露 FFmpeg 类型，避免调用方被 FFmpeg 头文件污染。
struct CustomByteBlockVideoDecoder::Impl {
    // 待匹配的解码帧元数据：解码器异步出帧时按顺序回补到 CustomByteBlockDecodedFrame。
    struct PendingFrame {
        std::uint32_t frame_id = 0; // 帧序号
        std::int64_t pts = 0;       // 显示时间戳
        bool keyframe = false;      // 是否关键帧
    };

    AVCodecContext *codec_context = nullptr; // H.264 解码器上下文
    AVCodecParserContext *parser = nullptr;  // H.264 码流解析器
    SwsContext *sws_context = nullptr;       // swscale 上下文，用于 YUV→RGBA 转换（按需重建）
    AVFrame *frame = nullptr;                // 复用的解码输出帧
    AVPacket *packet = nullptr;              // 复用的输入包
    std::deque<PendingFrame> pending_frames; // 已送入解码器但尚未出帧的帧元数据队列（FIFO 匹配）
    int source_width = 0;                    // 上次 sws 上下文对应的源宽（用于判断是否需重建）
    int source_height = 0;                   // 源高
    AVPixelFormat source_format = AV_PIX_FMT_NONE; // 源像素格式
    std::uint32_t next_frame_id = 0;          // 下一个自增帧 ID
    std::int64_t next_pts = 0;                // 下一个自增 PTS
    bool has_expected_sequence = false;       // 是否已建立期望序列号基准
    std::uint16_t expected_sequence_id = 0;   // 期望收到的下一个序列号
    bool waiting_for_keyframe = true;         // 是否仍在等待首个关键帧（丢弃非关键帧直到出现）
    WireFormat wire_format = WireFormat::Unknown; // 当前识别到的线格式
    std::chrono::steady_clock::time_point last_packet_time = std::chrono::steady_clock::time_point::min(); // 上次收包时间，用于空闲判定
};

namespace {

// 功能：判断 lhs * rhs 是否会超出 size_t 表示范围（乘法溢出检测）。
// 思路：用除法反向校验——若 rhs 大于 max/lhs 则乘积必然溢出；lhs 为 0 时不会溢出，直接排除。
// 用途：分配帧缓冲前先做该检查，防止恶意尺寸数据导致整数溢出进而分配过小的缓冲造成越界写。
bool multiplication_would_overflow(const std::size_t lhs, const std::size_t rhs) {
    return lhs != 0U && rhs > (std::numeric_limits<std::size_t>::max() / lhs);
}

// 功能：根据宽高计算 RGBA 缓冲字节数，并对照配置上限做多重校验。
// 思路：先做尺寸为负/超维、像素乘积溢出、像素数超限、字节数溢出的逐级检查，最终返回 width*height*4。
std::size_t decoded_frame_byte_count(const int width,
                                     const int height,
                                     const CustomByteBlockVideoDecoderConfig &config) {
    if (width <= 0 || height <= 0) {
        throw std::runtime_error("Decoded frame has invalid dimensions");
    }
    if (width > config.max_frame_dimension || height > config.max_frame_dimension) {
        throw std::runtime_error("Decoded frame exceeds configured dimension limit");
    }

    const std::size_t width_u = static_cast<std::size_t>(width);
    const std::size_t height_u = static_cast<std::size_t>(height);
    if (multiplication_would_overflow(width_u, height_u)) {
        throw std::runtime_error("Decoded frame pixel count overflows size_t");
    }

    const std::size_t pixel_count = width_u * height_u;
    if (pixel_count > config.max_frame_pixels) {
        throw std::runtime_error("Decoded frame exceeds configured pixel limit");
    }
    if (multiplication_would_overflow(pixel_count, 4U)) {
        throw std::runtime_error("Decoded frame RGBA byte count overflows size_t");
    }

    return pixel_count * 4U;
}

// 功能：释放 Impl 内全部 FFmpeg 资源（顺序与创建相反），置空指针。被构造失败与析构复用。
void free_decoder_impl(CustomByteBlockVideoDecoder::Impl &impl) {
    if (impl.parser != nullptr) {
        av_parser_close(impl.parser);
        impl.parser = nullptr;
    }
    if (impl.packet != nullptr) {
        av_packet_free(&impl.packet);
    }
    if (impl.frame != nullptr) {
        av_frame_free(&impl.frame);
    }
    if (impl.codec_context != nullptr) {
        avcodec_free_context(&impl.codec_context);
    }
    if (impl.sws_context != nullptr) {
        sws_freeContext(impl.sws_context);
        impl.sws_context = nullptr;
    }
}

// 功能：重建 H.264 解析器（先关闭旧的再初始化新的）。
void reset_parser(CustomByteBlockVideoDecoder::Impl *impl) {
    if (impl == nullptr) {
        throw std::invalid_argument("reset_parser() received null impl");
    }
    if (impl->parser != nullptr) {
        av_parser_close(impl->parser);
        impl->parser = nullptr;
    }
    impl->parser = av_parser_init(AV_CODEC_ID_H264);
    if (impl->parser == nullptr) {
        throw std::runtime_error("Failed to allocate H.264 parser");
    }
}

// 功能：丢包/异常后复位解码管线：冲刷解码器缓冲、清空待匹配元数据、重新等待关键帧并重置解析器。
void reset_decoder_after_loss(CustomByteBlockVideoDecoder::Impl *impl) {
    if (impl == nullptr) {
        throw std::invalid_argument("reset_decoder_after_loss() received null impl");
    }
    if (impl->codec_context != nullptr) {
        avcodec_flush_buffers(impl->codec_context); // 丢弃解码器内部已缓冲的参考帧
    }
    impl->pending_frames.clear();
    impl->waiting_for_keyframe = true; // 复位后需重新等待 IDR 才能正常解码
    reset_parser(impl);
}

// 功能：确保 swscale 上下文与当前帧的源尺寸/格式匹配，不匹配则重建。
// 思路：若已存在且参数一致直接复用；否则先释放旧上下文，再创建源→RGBA 的 SWS_BILINEAR 转换器并记录参数。
void ensure_sws_context(CustomByteBlockVideoDecoder::Impl &impl, const AVFrame *frame) {
    const auto frame_format = static_cast<AVPixelFormat>(frame->format);
    if (impl.sws_context != nullptr &&
        impl.source_width == frame->width &&
        impl.source_height == frame->height &&
        impl.source_format == frame_format) {
        return;
    }

    if (impl.sws_context != nullptr) {
        sws_freeContext(impl.sws_context);
        impl.sws_context = nullptr;
    }

    // 创建同尺寸的 YUV(源格式) → RGBA 转换上下文
    impl.sws_context = sws_getContext(
            frame->width, frame->height, frame_format,
            frame->width, frame->height, AV_PIX_FMT_RGBA,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (impl.sws_context == nullptr) {
        throw std::runtime_error("Failed to create decoder swscale context");
    }

    // 记录当前源参数，供下次比较是否需重建
    impl.source_width = frame->width;
    impl.source_height = frame->height;
    impl.source_format = frame_format;
}

// 功能：循环从解码器取出所有已就绪帧，转为 RGBA 并补回帧元数据，返回这批帧。
// 思路：receive 到帧→确保 sws→分配对齐临时 RGBA 缓冲→sws_scale 转换→逐行拷贝到紧凑 buffer→匹配 pending 元数据或自增分配。
std::vector<CustomByteBlockDecodedFrame> drain_decoded_frames(CustomByteBlockVideoDecoder::Impl &impl,
                                                              const CustomByteBlockVideoDecoderConfig &config) {
    std::vector<CustomByteBlockDecodedFrame> frames;
    while (true) {
        const int ret_receive = avcodec_receive_frame(impl.codec_context, impl.frame);
        if (ret_receive == AVERROR(EAGAIN) || ret_receive == AVERROR_EOF) {
            break; // 无更多帧可输出，结束
        }
        if (ret_receive < 0) {
            throw std::runtime_error("Failed to receive decoded frame: " + ffmpeg_error_to_string(ret_receive));
        }

        ensure_sws_context(impl, impl.frame);

        CustomByteBlockDecodedFrame output;
        output.width = impl.frame->width;
        output.height = impl.frame->height;
        output.rgba.resize(decoded_frame_byte_count(output.width, output.height, config)); // 预分配紧凑 RGBA 缓冲

        std::uint8_t *dst_slices[4] = {nullptr, nullptr, nullptr, nullptr};
        int dst_stride[4] = {0, 0, 0, 0};

        // sws_scale may use SIMD stores that assume FFmpeg-style aligned buffers.
        // 用 FFmpeg 接口按 32 字节对齐分配临时 RGBA 缓冲，满足 SIMD 写入要求
        const int alloc_ret = av_image_alloc(dst_slices,
                                             dst_stride,
                                             output.width,
                                             output.height,
                                             AV_PIX_FMT_RGBA,
                                             32);
        if (alloc_ret < 0) {
            throw std::runtime_error("Failed to allocate temporary RGBA image buffer");
        }

        // 执行 YUV→RGBA 缩放转换
        const int scaled_height = sws_scale(
                impl.sws_context,
                impl.frame->data,
                impl.frame->linesize,
                0,
                impl.frame->height,
                dst_slices,
                dst_stride);
        if (scaled_height != output.height) {
            av_freep(&dst_slices[0]);
            throw std::runtime_error("Failed to convert decoded frame to RGBA");
        }
        // 逐行把带 stride 的对齐缓冲拷贝到紧凑 output.rgba（去除行对齐填充）
        for (int y = 0; y < output.height; ++y) {
            const auto *src_row = dst_slices[0] + static_cast<std::ptrdiff_t>(y) * dst_stride[0];
            auto *dst_row = output.rgba.data() +
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(output.width) * 4U;
            std::memcpy(dst_row, src_row, static_cast<std::size_t>(output.width) * 4U);
        }
        av_freep(&dst_slices[0]); // 释放临时缓冲

        // 按进入解码器的顺序匹配帧元数据；若无（如 flush 残留）则自增分配
        if (!impl.pending_frames.empty()) {
            const CustomByteBlockVideoDecoder::Impl::PendingFrame meta = impl.pending_frames.front();
            impl.pending_frames.pop_front();
            output.frame_id = meta.frame_id;
            output.pts = meta.pts;
            output.keyframe = meta.keyframe;
        } else {
            output.frame_id = impl.next_frame_id++;
            output.pts = (impl.frame->pts == AV_NOPTS_VALUE) ? impl.next_pts++ : impl.frame->pts;
            output.keyframe = (impl.frame->flags & AV_FRAME_FLAG_KEY) != 0;
        }
        frames.push_back(std::move(output));
    }
    return frames;
}

// 功能：把一个完整访问单元(NAL 集合)送入解码器并尝试取出已解码帧。
// 思路：先判关键帧与等待状态；做大小上限校验（超限即复位丢弃）；构造 AVPacket 并 send，
//       记录 pending 元数据，最后 drain 出帧；任何失败都复位解码器避免后续雪崩。
std::vector<CustomByteBlockDecodedFrame> decode_access_unit(CustomByteBlockVideoDecoder::Impl *impl,
                                                            const CustomByteBlockVideoDecoderConfig &config,
                                                            const std::uint8_t *access_unit,
                                                            const std::size_t access_unit_size) {
    if (impl == nullptr) {
        throw std::invalid_argument("decode_access_unit() received null impl");
    }
    if (access_unit == nullptr && access_unit_size != 0U) {
        throw std::invalid_argument("decode_access_unit() received null data with non-zero size");
    }
    if (access_unit_size == 0U) {
        return {};
    }

    const bool keyframe = access_unit_has_idr(access_unit, access_unit_size);
    // 仍在等待关键帧且本单元非 IDR：直接丢弃，避免花屏
    if (impl->waiting_for_keyframe && !keyframe) {
        return {};
    }

    if (access_unit_size > config.max_access_unit_bytes) {
        SPDLOG_WARN("丢弃过大的 CustomByteBlock 访问单元 bytes={} limit={}",
                    access_unit_size,
                    config.max_access_unit_bytes);
        reset_decoder_after_loss(impl);
        return {};
    }
    if (access_unit_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        SPDLOG_WARN("丢弃超出 FFmpeg 支持范围的 CustomByteBlock 访问单元 bytes={}",
                    access_unit_size);
        reset_decoder_after_loss(impl);
        return {};
    }

    // 复用 packet 并填入访问单元数据，分配可能失败
    const int ret_packet = av_new_packet(impl->packet, static_cast<int>(access_unit_size));
    if (ret_packet < 0) {
        SPDLOG_WARN("分配 CustomByteBlock 解码包失败 message={}", ffmpeg_error_to_string(ret_packet));
        reset_decoder_after_loss(impl);
        return {};
    }

    std::memcpy(impl->packet->data, access_unit, access_unit_size);
    impl->packet->pts = impl->next_pts; // 自增分配时间戳（解析器无 PTS 时用）
    impl->packet->dts = impl->next_pts;
    impl->packet->flags = keyframe ? AV_PKT_FLAG_KEY : 0; // 标记关键帧供解码器参考

    // 发送包给解码器，发送后即刻 unref 释放引用
    const int ret_send = avcodec_send_packet(impl->codec_context, impl->packet);
    av_packet_unref(impl->packet);
    if (ret_send < 0) {
        SPDLOG_WARN("CustomByteBlock 解码器拒收访问单元 keyframe={} message={}",
                    keyframe,
                    ffmpeg_error_to_string(ret_send));
        reset_decoder_after_loss(impl);
        return {};
    }

    // 记录该访问单元对应的帧元数据，待出帧时顺序匹配
    impl->pending_frames.push_back({
            impl->next_frame_id++,
            impl->next_pts++,
            keyframe,
    });
    impl->waiting_for_keyframe = false; // 已收到可解码内容，后续不再强制等关键帧

    try {
        return drain_decoded_frames(*impl, config);
    } catch (const std::exception &error) {
        SPDLOG_WARN("CustomByteBlock 解码输出失败 message={}", error.what());
        reset_decoder_after_loss(impl);
        return {};
    }
}

// 功能：把一批解码帧移动追加到目标 vector，避免额外拷贝。
void append_decoded_frames(std::vector<CustomByteBlockDecodedFrame> &target,
                           std::vector<CustomByteBlockDecodedFrame> &&incoming) {
    target.insert(target.end(),
                  std::make_move_iterator(incoming.begin()),
                  std::make_move_iterator(incoming.end()));
}

// 功能：把一段 H.264 字节流喂给解析器，切出访问单元后逐个解码并汇总输出帧。
// 思路：循环 av_parser_parse2 切包，每切出一段就解码，直到流被消费完或解析器无产出。
std::vector<CustomByteBlockDecodedFrame> feed_parser(CustomByteBlockVideoDecoder::Impl *impl,
                                                     const CustomByteBlockVideoDecoderConfig &config,
                                                     const std::uint8_t *payload,
                                                     std::size_t payload_size) {
    if (impl == nullptr) {
        throw std::invalid_argument("feed_parser() received null impl");
    }
    if (payload == nullptr && payload_size != 0U) {
        throw std::invalid_argument("feed_parser() received null payload with non-zero size");
    }

    std::vector<CustomByteBlockDecodedFrame> output;
    const std::uint8_t *cursor = payload;
    while (payload_size > 0U) {
        std::uint8_t *parsed_data = nullptr;
        int parsed_size = 0;
        // 从剩余流中解析出一段完整访问单元
        const int consumed = av_parser_parse2(
                impl->parser,
                impl->codec_context,
                &parsed_data,
                &parsed_size,
                cursor,
                static_cast<int>(payload_size),
                AV_NOPTS_VALUE,
                AV_NOPTS_VALUE,
                0);
        if (consumed < 0) {
            throw std::runtime_error("Failed to parse H.264 byte stream: " + ffmpeg_error_to_string(consumed));
        }

        cursor += consumed;
        payload_size -= static_cast<std::size_t>(consumed);

        if (parsed_size > 0) {
            // 切出有效访问单元：解码并追加到输出
            append_decoded_frames(
                    output,
                    decode_access_unit(
                            impl,
                            config,
                            parsed_data,
                            static_cast<std::size_t>(parsed_size)));
        }

        if (consumed == 0 && parsed_size == 0) {
            break; // 解析器无进展，防止死循环
        }
    }

    return output;
}

// 功能：向解析器喂入空数据(nullptr,0)以冲刷出缓冲中的最后一段访问单元并解码。
// 思路：与 feed_parser 类似，但传入空负载触发解析器 flush，循环直到无产出。
std::vector<CustomByteBlockDecodedFrame> flush_parser(CustomByteBlockVideoDecoder::Impl *impl,
                                                      const CustomByteBlockVideoDecoderConfig &config) {
    if (impl == nullptr) {
        throw std::invalid_argument("flush_parser() received null impl");
    }

    std::vector<CustomByteBlockDecodedFrame> output;
    while (true) {
        std::uint8_t *parsed_data = nullptr;
        int parsed_size = 0;
        const int consumed = av_parser_parse2(
                impl->parser,
                impl->codec_context,
                &parsed_data,
                &parsed_size,
                nullptr,
                0,
                AV_NOPTS_VALUE,
                AV_NOPTS_VALUE,
                0);
        if (consumed < 0) {
            throw std::runtime_error("Failed while flushing H.264 parser: " + ffmpeg_error_to_string(consumed));
        }
        (void) consumed; // flush 时 consumed 恒为 0，仅作占位

        if (parsed_size <= 0) {
            break;
        }

        append_decoded_frames(
                output,
                decode_access_unit(
                        impl,
                        config,
                        parsed_data,
                        static_cast<std::size_t>(parsed_size)));
    }

    return output;
}

// 功能：处理一条序列化包，维护 16 位序列号连续性，必要时复位解码器，随后喂入解析器。
// 思路：首包建立期望基准；收到期望包则递增；收到更新的包说明丢包→复位；收到旧包则按空闲/回退阈值决定是否重同步，否则丢弃。
std::vector<CustomByteBlockDecodedFrame> process_serialized_packet(CustomByteBlockVideoDecoder::Impl &impl,
                                                                   const CustomByteBlockVideoDecoderConfig &config,
                                                                   const SerializedPacket &packet) {
    const auto now = std::chrono::steady_clock::now();
    if (!impl.has_expected_sequence) {
        // 首个包：以它的下一序号作为期望基准
        impl.expected_sequence_id = static_cast<std::uint16_t>(packet.sequence_id + 1U);
        impl.has_expected_sequence = true;
    } else if (packet.sequence_id == impl.expected_sequence_id) {
        // 恰好是期望的下一包，正常推进
        impl.expected_sequence_id = static_cast<std::uint16_t>(impl.expected_sequence_id + 1U);
    } else if (sequence_is_newer(packet.sequence_id, impl.expected_sequence_id)) {
        // 收到了更新（跳号）的包，说明中途丢包，复位解码器避免错误参考
        SPDLOG_WARN("CustomByteBlock 检测到丢包，重置解码器 expected_sequence={} received_sequence={}",
                    impl.expected_sequence_id,
                    packet.sequence_id);
        reset_decoder_after_loss(&impl);
        impl.expected_sequence_id = static_cast<std::uint16_t>(packet.sequence_id + 1U);
        impl.has_expected_sequence = true;
    } else {
        // 收到较旧的包：判断是否值得重同步（空闲太久或回退过大），否则直接忽略重复/旧包
        const std::uint16_t backward_distance = static_cast<std::uint16_t>(impl.expected_sequence_id - packet.sequence_id);
        const bool idle_too_long = impl.last_packet_time != std::chrono::steady_clock::time_point::min()
                                   && (now - impl.last_packet_time) > kSequenceResyncIdleTimeout;
        const bool backward_jump_too_large = backward_distance > kSequenceResyncBackwardThreshold;
        if (idle_too_long || backward_jump_too_large) {
            const auto idle_ms = impl.last_packet_time == std::chrono::steady_clock::time_point::min()
                                         ? 0LL
                                         : std::chrono::duration_cast<std::chrono::milliseconds>(now - impl.last_packet_time).count();
            SPDLOG_INFO("CustomByteBlock 序列重同步 expected_sequence={} received_sequence={} idle_ms={} backward_distance={}",
                        impl.expected_sequence_id,
                        packet.sequence_id,
                        idle_ms,
                        backward_distance);
            reset_decoder_after_loss(&impl);
            impl.expected_sequence_id = static_cast<std::uint16_t>(packet.sequence_id + 1U);
            impl.has_expected_sequence = true;
        } else {
            // Older or duplicate packet: ignore.
            return {};
        }
    }

    impl.last_packet_time = now;
    // 序列校验通过：把净负载喂入解析器并解码
    return feed_parser(
            &impl,
            config,
            packet.payload.data(),
            packet.payload.size());
}

// 功能：反序列化一条序列化包字节并交给 process_serialized_packet 处理。
std::vector<CustomByteBlockDecodedFrame> decode_serialized_packet_bytes(CustomByteBlockVideoDecoder::Impl &impl,
                                                                         const CustomByteBlockVideoDecoderConfig &config,
                                                                         const std::span<const std::uint8_t> wire_bytes) {
    SerializedPacket packet = deserialize_serialized_packet(wire_bytes);
    return process_serialized_packet(impl, config, packet);
}

// 功能：把传输包束拆成子包，逐个当作序列化包处理并汇总输出帧。
std::vector<CustomByteBlockDecodedFrame> decode_transport_bundle_bytes(CustomByteBlockVideoDecoder::Impl &impl,
                                                                       const CustomByteBlockVideoDecoderConfig &config,
                                                                       const std::span<const std::uint8_t> wire_bytes) {
    const auto packet_spans = unpack_transport_packet_spans(wire_bytes);
    if (packet_spans.empty()) {
        return {};
    }

    std::vector<CustomByteBlockDecodedFrame> decoded_frames;
    for (const std::span<const std::uint8_t> packet_span: packet_spans) {
        SerializedPacket packet = deserialize_serialized_packet(packet_span);
        append_decoded_frames(decoded_frames, process_serialized_packet(impl, config, packet));
    }
    return decoded_frames;
}

// 功能：按指定线格式分派到对应的解码入口。
std::vector<CustomByteBlockDecodedFrame> decode_wire_bytes_with_format(CustomByteBlockVideoDecoder::Impl &impl,
                                                                       const CustomByteBlockVideoDecoderConfig &config,
                                                                       const std::span<const std::uint8_t> wire_bytes,
                                                                       const WireFormat format) {
    switch (format) {
        case WireFormat::SerializedPacket:
            return decode_serialized_packet_bytes(impl, config, wire_bytes);
        case WireFormat::TransportBundle:
            return decode_transport_bundle_bytes(impl, config, wire_bytes);
        case WireFormat::Unknown:
        default:
            throw std::invalid_argument("unsupported wire format");
    }
}

// 功能：在未确定线格式时，启发式判断应优先尝试「传输包束」还是「序列化包」。
// 思路：已知格式直接返回；未知时，满字节数(300)或首子包长度合法且能容纳者优先按包束尝试，否则按序列化包。
bool should_try_transport_first(const WireFormat wire_format, const std::span<const std::uint8_t> wire_bytes) {
    if (wire_format == WireFormat::TransportBundle) {
        return true;
    }
    if (wire_format == WireFormat::SerializedPacket) {
        return false;
    }

    if (wire_bytes.size() == kMaxSupportedPacketBytes) {
        return true;
    }
    if (wire_bytes.size() < kTransportLengthPrefixBytes) {
        return false;
    }

    const std::uint16_t first_inner_size = read_u16_be(wire_bytes.data());
    if (first_inner_size != 0U &&
        first_inner_size <= kMaxInnerPacketBytes &&
        (kTransportLengthPrefixBytes + static_cast<std::size_t>(first_inner_size)) <= wire_bytes.size()) {
        return true;
    }

    return false;
}

}// namespace

// 功能：构造解码器：校验配置，查找并打开 H.264 解码器，分配帧/包，初始化解析器。
// 思路：任何一步失败都调用 free_decoder_impl 清理已分配资源后再抛异常，保证不泄漏。
CustomByteBlockVideoDecoder::CustomByteBlockVideoDecoder(CustomByteBlockVideoDecoderConfig config)
    : config_(std::move(config)),
      impl_(std::make_unique<Impl>()) {
    validate_config(config_);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (codec == nullptr) {
        throw std::runtime_error("Unable to find an H.264 decoder");
    }

    impl_->codec_context = avcodec_alloc_context3(codec);
    if (impl_->codec_context == nullptr) {
        throw std::runtime_error("Failed to allocate decoder context");
    }

    const int ret_open = avcodec_open2(impl_->codec_context, codec, nullptr);
    if (ret_open < 0) {
        const std::string message = ffmpeg_error_to_string(ret_open);
        free_decoder_impl(*impl_);
        throw std::runtime_error("Failed to open decoder: " + message);
    }

    impl_->frame = av_frame_alloc();
    impl_->packet = av_packet_alloc();
    if (impl_->frame == nullptr || impl_->packet == nullptr) {
        free_decoder_impl(*impl_);
        throw std::runtime_error("Failed to allocate decoder frame or packet");
    }

    try {
        reset_parser(impl_.get()); // 创建 H.264 解析器
    } catch (...) {
        free_decoder_impl(*impl_);
        throw;
    }
}

// 功能：析构，释放 Impl 内全部 FFmpeg 资源。
CustomByteBlockVideoDecoder::~CustomByteBlockVideoDecoder() {
    if (impl_ != nullptr) {
        free_decoder_impl(*impl_);
    }
}

CustomByteBlockVideoDecoder::CustomByteBlockVideoDecoder(CustomByteBlockVideoDecoder &&) noexcept = default;
CustomByteBlockVideoDecoder &CustomByteBlockVideoDecoder::operator=(CustomByteBlockVideoDecoder &&) noexcept = default;

// 功能：送入一个线格式包，返回本次解码出的帧；支持两种格式的自动探测与失败回退。
// 思路：先用启发式选择优先格式，主解析失败则回退到另一种格式再试；两者皆失败则丢弃并告警，确保鲁棒。
std::vector<CustomByteBlockDecodedFrame> CustomByteBlockVideoDecoder::push_packet_bytes(std::span<const std::uint8_t> wire_bytes) {
    const bool transport_first = should_try_transport_first(impl_->wire_format, wire_bytes);
    const WireFormat primary_format = transport_first ? WireFormat::TransportBundle : WireFormat::SerializedPacket;
    const WireFormat fallback_format = transport_first ? WireFormat::SerializedPacket : WireFormat::TransportBundle;

    try {
        std::vector<CustomByteBlockDecodedFrame> decoded_frames =
                decode_wire_bytes_with_format(*impl_, config_, wire_bytes, primary_format);
        if (impl_->wire_format != primary_format) {
            SPDLOG_INFO("CustomByteBlock 输入包格式切换 old_format={} new_format={}",
                        wire_format_to_string(impl_->wire_format),
                        wire_format_to_string(primary_format));
            impl_->wire_format = primary_format; // 记录识别到的格式，后续直接以该格式优先
        }
        return decoded_frames;
    } catch (const std::exception &primary_error) {
        try {
            std::vector<CustomByteBlockDecodedFrame> decoded_frames =
                    decode_wire_bytes_with_format(*impl_, config_, wire_bytes, fallback_format);
            if (impl_->wire_format != fallback_format) {
                SPDLOG_INFO("CustomByteBlock 输入包格式切换 old_format={} new_format={}",
                            wire_format_to_string(impl_->wire_format),
                            wire_format_to_string(fallback_format));
                impl_->wire_format = fallback_format;
            }
            SPDLOG_DEBUG("CustomByteBlock 输入包按 {} 解析失败，已自动回退到 {} bytes={} primary_error={}",
                         wire_format_to_string(primary_format),
                         wire_format_to_string(fallback_format),
                         wire_bytes.size(),
                         primary_error.what());
            return decoded_frames;
        } catch (const std::exception &fallback_error) {
            SPDLOG_WARN("丢弃无效的 CustomByteBlock 输入包 bytes={} primary_format={} primary_error={} fallback_format={} fallback_error={}",
                        wire_bytes.size(),
                        wire_format_to_string(primary_format),
                        primary_error.what(),
                        wire_format_to_string(fallback_format),
                        fallback_error.what());
            return {};
        }
    }
}

// 功能：冲刷解码器，取出所有滞留的已解码帧，供停流时收尾。
// 思路：先用 flush_parser 冲刷解析器缓冲，再向解码器发送空包触发末尾帧输出，最后 drain 出帧；异常时复位解码器。
std::vector<CustomByteBlockDecodedFrame> CustomByteBlockVideoDecoder::flush() {
    std::vector<CustomByteBlockDecodedFrame> output;
    try {
        append_decoded_frames(output, flush_parser(impl_.get(), config_));

        // 发送空包告知解码器输入结束，促其输出剩余帧
        const int ret_send = avcodec_send_packet(impl_->codec_context, nullptr);
        if (ret_send < 0 && ret_send != AVERROR_EOF) {
            reset_decoder_after_loss(impl_.get());
            throw std::runtime_error("Failed to flush decoder: " + ffmpeg_error_to_string(ret_send));
        }

        append_decoded_frames(output, drain_decoded_frames(*impl_, config_));
        return output;
    } catch (...) {
        reset_decoder_after_loss(impl_.get());
        throw;
    }
}

}// namespace RoboGo::video
