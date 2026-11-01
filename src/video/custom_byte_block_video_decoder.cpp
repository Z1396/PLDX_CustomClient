// custom_byte_block_video_decoder.cpp：自定义字节块解码器实现——解析序列化包与传输包束两种线格式，按 H.264 解码并转换为 RGBA 帧。
#include "custom_byte_block_video_decoder.h"

// 命名空间 RoboGo::video：业务功能层-视频模块的对外实现，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video {

// 匿名命名空间：仅本编译单元可见的线格式解析、序列号处理与解码辅助纯函数，不污染对外符号。
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
}; // 线格式枚举定义结束

// 反序列化后的单条序列化包：序列号 + 净负载。
struct SerializedPacket {
    std::uint16_t sequence_id = 0;          // 包序列号（大端），用于乱序重排与丢包检测
    std::vector<std::uint8_t> payload;      // 净负载：去掉 2 字节包头后的实际码流数据
}; // 序列化包结构体定义结束

// 功能：按大端序读取 2 字节无符号整数（网络字节序）。
std::uint16_t read_u16_be(const std::uint8_t *input) {
    return static_cast<std::uint16_t>( // 按大端组合：首字节左移 8 位作为高字节
            (static_cast<std::uint16_t>(input[0]) << 8) | // 次字节作为低字节，两者按位或合并成 16 位整数
            static_cast<std::uint16_t>(input[1])); // 低位字节在前，按大端(网络序)组合成 16 位整数
} // read_u16_be 结束

// 功能：把 FFmpeg 错误码转成可读字符串。
std::string ffmpeg_error_to_string(const int errnum) {
    std::array<char, AV_ERROR_MAX_STRING_SIZE> buffer{}; // 承载错误文本的定长缓冲，FFmpeg 要求容量至少为 AV_ERROR_MAX_STRING_SIZE
    av_strerror(errnum, buffer.data(), buffer.size()); // 把 FFmpeg 的负错误码转成人类可读描述写入缓冲
    return std::string(buffer.data()); // 由 C 风格缓冲构造 std::string 返回，便于日志拼接
} // ffmpeg_error_to_string 结束

// 功能：校验解码配置各项上限是否为正，非法则抛异常。
void validate_config(const CustomByteBlockVideoDecoderConfig &config) {
    if (config.max_reorder_frames <= 0) { // 重排帧数用于解码器 B 帧重排缓冲，必须为正
        throw std::invalid_argument("CustomByteBlock decoder max_reorder_frames must be positive"); // 配置非法：直接抛 invalid_argument 拒绝构造
    } // 重排帧数校验结束
    if (config.max_access_unit_bytes == 0U) { // 单访问单元字节上限不能为 0，否则任何包都会被拒绝
        throw std::invalid_argument("CustomByteBlock decoder max_access_unit_bytes must be positive"); // 配置非法：直接抛异常
    } // 访问单元字节上限校验结束
    if (config.max_assembly_bytes == 0U) { // 组装缓冲上限同样要求非 0（预留字段）
        throw std::invalid_argument("CustomByteBlock decoder max_assembly_bytes must be positive"); // 配置非法：直接抛异常
    } // 组装缓冲上限校验结束
    if (config.max_frame_dimension <= 0) { // 单帧宽高上限必须为正
        throw std::invalid_argument("CustomByteBlock decoder max_frame_dimension must be positive"); // 配置非法：直接抛异常
    } // 宽高上限校验结束
    if (config.max_frame_pixels == 0U) { // 单帧像素上限不能为 0
        throw std::invalid_argument("CustomByteBlock decoder max_frame_pixels must be positive"); // 配置非法：直接抛异常
    } // 像素上限校验结束
} // validate_config 结束

// 功能：把线字节反序列化为一条 SerializedPacket（前 2 字节为序列号，其余为净负载）。
// 思路：先校验长度下限(含头)与上限(300)，用 read_u16_be 取序列号，剩余部分作为负载，并禁止空负载。
SerializedPacket deserialize_serialized_packet(const std::span<const std::uint8_t> wire_bytes) {
    if (wire_bytes.size() < kSerializedPacketHeaderBytes) { // 长度至少需容纳 2 字节序列号头
        throw std::invalid_argument("wire packet is smaller than the serialized packet header"); // 过短说明调用方传错，抛异常
    } // 头长度下限校验结束
    if (wire_bytes.size() > kMaxSupportedPacketBytes) { // 超过协议规定的单包上限（300 字节）视为非法
        throw std::invalid_argument("wire packet exceeds 300 bytes"); // 超长直接拒绝，避免后续越界解析
    } // 长度上限校验结束

    SerializedPacket packet; // 待填充的反序列化结果对象
    packet.sequence_id = read_u16_be(wire_bytes.data()); // 读取大端序列号，供乱序重排与丢包检测使用
    // 跳过 2 字节头，其余全部作为净负载
    packet.payload.assign(wire_bytes.begin() + static_cast<std::ptrdiff_t>(kSerializedPacketHeaderBytes), wire_bytes.end());

    if (packet.payload.empty()) { // 只有序列号没有码流数据，属于无效包
        throw std::invalid_argument("serialized packet has empty payload"); // 空负载直接拒绝
    } // 负载非空校验结束

    return packet; // 返回解析完成的序列化包
} // deserialize_serialized_packet 结束

// 功能：把「传输包束」拆成多条内部序列化包视图(span)，免去拷贝。
// 思路：按 (2 字节长度 + 负载) 循环切分；长度为 0 表示填充结束(其后必须全 0)；子包超长或截断则报错。
std::vector<std::span<const std::uint8_t>> unpack_transport_packet_spans(const std::span<const std::uint8_t> transport_packet) {
    if (transport_packet.empty()) { // 空包束无从拆分
        throw std::invalid_argument("transport packet is empty"); // 空输入直接拒绝
    } // 空输入校验结束
    if (transport_packet.size() > kMaxSupportedPacketBytes) { // 超过单包上限视为非法
        throw std::invalid_argument("transport packet exceeds 300 bytes"); // 超长直接拒绝
    } // 长度上限校验结束

    std::vector<std::span<const std::uint8_t>> packets; // 子包视图列表；span 只引用原缓冲，不拷贝数据
    std::size_t read_offset = 0U; // 当前读取位置，从 0 开始逐段推进
    // 只要还能读到一个长度前缀就继续
    while (read_offset + kTransportLengthPrefixBytes <= transport_packet.size()) {
        const std::uint16_t packet_size = read_u16_be( // 读取 2 字节长度前缀，得到子包净长度
                transport_packet.data() + static_cast<std::ptrdiff_t>(read_offset)); // 前缀地址 = 缓冲起始 + 当前读偏移
        read_offset += kTransportLengthPrefixBytes; // 跳过长度前缀

        // 长度 0 为填充标记：其后必须全部为 0，否则视为协议错误
        if (packet_size == 0U) {
            const bool has_non_zero_padding = std::any_of( // 长度为 0 是填充标记：检查其后是否全为 0
                    transport_packet.begin() + static_cast<std::ptrdiff_t>(read_offset), // 扫描起点：填充标记之后的位置
                    transport_packet.end(), // 扫描终点：包束末尾
                    [](const std::uint8_t value) { return value != 0U; }); // 任一字节非 0 即说明填充区被污染
            if (has_non_zero_padding) { // 填充区存在非 0 字节，说明协议被破坏
                throw std::invalid_argument("transport packet has non-zero bytes after padding marker"); // 抛协议错误，由上层丢弃该包
            } // 填充校验分支结束
            break; // 填充标记之后没有更多子包，结束拆分
        } // 填充标记处理结束

        if (packet_size > kMaxInnerPacketBytes) { // 子包净负载不能超过 298 字节（300 减去 2 字节前缀）
            throw std::invalid_argument("transport packet has an inner packet larger than 298 bytes"); // 超长视为协议错误
        } // 子包长度校验结束
        if (read_offset + packet_size > transport_packet.size()) { // 声明长度超出剩余字节，说明数据被截断
            throw std::invalid_argument("transport packet has a truncated inner packet"); // 截断视为协议错误
        } // 截断校验结束

        // 记录该子包的视图（不拷贝），并推进读取偏移
        packets.emplace_back(
                transport_packet.data() + static_cast<std::ptrdiff_t>(read_offset), // 视图起点：长度前缀之后的净负载首地址
                packet_size); // 视图长度：前缀声明的子包字节数
        read_offset += packet_size; // 读取游标前移，准备解析下一个子包
    } // 拆分循环结束

    return packets; // 返回全部子包视图
} // unpack_transport_packet_spans 结束

// 功能：把线格式枚举转为日志用字符串。
const char *wire_format_to_string(const WireFormat format) {
    switch (format) { // 按枚举值分派到对应的日志字符串
        case WireFormat::SerializedPacket: // 单条序列化包格式
            return "serialized-packet"; // 返回其日志名称
        case WireFormat::TransportBundle: // 多条打包的传输包束格式
            return "transport-bundle"; // 返回其日志名称
        case WireFormat::Unknown: // 尚未识别（含非法枚举值）
        default: // default 与 Unknown 合并处理
            return "unknown"; // 未知格式统一返回 unknown
    } // switch 结束
} // wire_format_to_string 结束

// 功能：用 16 位回绕算术判断 lhs 是否比 rhs 更新（即更靠前）。
// 思路：无符号差值的符号位落在 [0x8000, 0xFFFF] 表示 lhs 落后，落在 (0, 0x8000) 表示领先。
bool sequence_is_newer(const std::uint16_t lhs, const std::uint16_t rhs) {
    const std::uint16_t delta = static_cast<std::uint16_t>(lhs - rhs); // 16 位无符号差值：自动完成回绕，得到带方向的偏移
    return delta != 0U && delta < 0x8000U; // 差值非 0 且落在前半区说明 lhs 领先于 rhs
} // sequence_is_newer 结束

// 功能：从 offset 起查找下一个 H.264/HEVC 起始码(00 00 00 01 或 00 00 01)，返回其位置与长度。
// 参数：prefix_len 输出 3 或 4；返回值为起始码起始下标，找不到则返回 size(哨兵)。
std::size_t find_start_code_prefix(const std::uint8_t *data,
                                   const std::size_t size, // 参数：待扫描缓冲总长度
                                   const std::size_t offset, // 参数：从该下标开始扫描
                                   std::size_t *prefix_len) { // 输出参数：命中起始码的长度（3 或 4）
    if (prefix_len == nullptr) { // 输出参数不可为空，否则无法回传结果
        throw std::invalid_argument("find_start_code_prefix() received null prefix_len"); // 参数非法直接抛异常
    } // 空指针校验结束
    if (data == nullptr && size != 0U) { // 有长度却没有数据指针，属于调用错误
        throw std::invalid_argument("find_start_code_prefix() received null data with non-zero size"); // 参数非法直接抛异常
    } // 指针与长度一致性校验结束

    // 逐字节扫描，优先匹配 4 字节起始码，其次 3 字节起始码
    for (std::size_t i = offset; (i + 3U) <= size; ++i) {
        if ((i + 4U) <= size && // 还需 4 字节空间，才可能匹配 4 字节起始码
            data[i] == 0U && // 第 1 字节必须为 0
            data[i + 1U] == 0U && // 第 2 字节必须为 0
            data[i + 2U] == 0U && // 第 3 字节必须为 0
            data[i + 3U] == 1U) { // 第 4 字节为 1，构成 00 00 00 01
            *prefix_len = 4U; // 回传起始码长度 4
            return i; // 返回起始码起始下标
        } // 4 字节起始码分支结束
        if (data[i] == 0U && data[i + 1U] == 0U && data[i + 2U] == 1U) { // 退化为 3 字节起始码 00 00 01
            *prefix_len = 3U; // 回传起始码长度 3
            return i; // 返回起始码起始下标
        } // 3 字节起始码分支结束
    } // 逐字节扫描循环结束

    return size; // 未找到
} // find_start_code_prefix 结束

// 功能：判断一段访问单元是否包含 IDR 关键帧（NAL 类型 5）。
// 思路：逐个定位起始码，取 NAL 头低 5 位作为类型(H.264)，遇到类型 5 即返回 true。
bool access_unit_has_idr(const std::uint8_t *data, const std::size_t size) {
    if (data == nullptr && size != 0U) { // 有长度却没有数据指针属于调用错误
        throw std::invalid_argument("access_unit_has_idr() received null data with non-zero size"); // 参数非法直接抛异常
    } // 指针与长度一致性校验结束

    std::size_t search_offset = 0U; // 扫描游标，从缓冲开头开始
    while (search_offset < size) { // 还有剩余字节就继续扫描起始码
        std::size_t prefix_len = 0U; // 本次命中起始码的长度（由查找函数回填）
        const std::size_t prefix_offset = find_start_code_prefix(data, size, search_offset, &prefix_len); // 定位下一个起始码，取得其位置与长度
        if (prefix_offset >= size) { // 找不到起始码（函数返回 size 哨兵）
            break; // 没有更多 NAL 单元，结束扫描
        } // 无起始码的退出分支结束

        const std::size_t nal_header_offset = prefix_offset + prefix_len; // NAL 头位于起始码之后
        if (nal_header_offset >= size) { // 起始码后没有头字节，说明流被截断
            break; // 起始码后没有 NAL 头，结束
        } // 截断退出分支结束

        // H.264 NAL 类型占头字节低 5 位
        const std::uint8_t nal_type = data[nal_header_offset] & 0x1fU;
        if (nal_type == 5U) { // H.264 中 NAL 类型 5 即 IDR 关键帧
            return true; // 命中关键帧，立即返回 true
        } // 关键帧判定分支结束

        search_offset = nal_header_offset + 1U; // 跳到下一个，继续查找
    } // 扫描循环结束

    return false; // 未找到 IDR，返回 false
} // access_unit_has_idr 结束

}// namespace

// 解码器实现体（PIMPL 的 Impl）：集中持有全部 FFmpeg 资源与解码中间状态，
// 目的是让公开头文件不暴露 FFmpeg 类型，避免调用方被 FFmpeg 头文件污染。
struct CustomByteBlockVideoDecoder::Impl {
    // 待匹配的解码帧元数据：解码器异步出帧时按顺序回补到 CustomByteBlockDecodedFrame。
    struct PendingFrame {
        std::uint32_t frame_id = 0; // 帧序号
        std::int64_t pts = 0;       // 显示时间戳
        bool keyframe = false;      // 是否关键帧
    }; // 待匹配帧元数据结构体定义结束

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
}; // Impl 结构体定义结束

namespace { // 匿名命名空间：继续放置本编译单元私有的解码辅助函数

// 功能：判断 lhs * rhs 是否会超出 size_t 表示范围（乘法溢出检测）。
// 思路：用除法反向校验——若 rhs 大于 max/lhs 则乘积必然溢出；lhs 为 0 时不会溢出，直接排除。
// 用途：分配帧缓冲前先做该检查，防止恶意尺寸数据导致整数溢出进而分配过小的缓冲造成越界写。
bool multiplication_would_overflow(const std::size_t lhs, const std::size_t rhs) {
    return lhs != 0U && rhs > (std::numeric_limits<std::size_t>::max() / lhs); // 乘法溢出判定：rhs 大于 max/lhs 说明乘积放不下
} // multiplication_would_overflow 结束

// 功能：根据宽高计算 RGBA 缓冲字节数，并对照配置上限做多重校验。
// 思路：先做尺寸为负/超维、像素乘积溢出、像素数超限、字节数溢出的逐级检查，最终返回 width*height*4。
std::size_t decoded_frame_byte_count(const int width,
                                     const int height, // 参数：图像高度（像素）
                                     const CustomByteBlockVideoDecoderConfig &config) { // 参数：解码配置，提供像素与尺寸上限
    if (width <= 0 || height <= 0) { // 宽高必须为正，否则无法构图
        throw std::runtime_error("Decoded frame has invalid dimensions"); // 非法尺寸直接抛 runtime_error
    } // 尺寸为负或零的校验结束
    if (width > config.max_frame_dimension || height > config.max_frame_dimension) { // 宽或高超过配置上限，属于异常尺寸
        throw std::runtime_error("Decoded frame exceeds configured dimension limit"); // 超限直接抛异常，避免分配过大缓冲
    } // 尺寸上限校验结束

    const std::size_t width_u = static_cast<std::size_t>(width); // 转无符号，便于做乘法与溢出判断
    const std::size_t height_u = static_cast<std::size_t>(height); // 转无符号，便于做乘法与溢出判断
    if (multiplication_would_overflow(width_u, height_u)) { // 先确认宽高乘积不会溢出 size_t
        throw std::runtime_error("Decoded frame pixel count overflows size_t"); // 溢出直接抛异常，防止后续分配过小缓冲
    } // 像素数溢出校验结束

    const std::size_t pixel_count = width_u * height_u; // 宽高乘积即单帧像素总数
    if (pixel_count > config.max_frame_pixels) { // 像素总数超过配置上限，拒绝解码该帧
        throw std::runtime_error("Decoded frame exceeds configured pixel limit"); // 超限直接抛异常
    } // 像素上限校验结束
    if (multiplication_would_overflow(pixel_count, 4U)) { // 再确认像素数乘 4 字节不会溢出
        throw std::runtime_error("Decoded frame RGBA byte count overflows size_t"); // 溢出直接抛异常
    } // 字节数溢出校验结束

    return pixel_count * 4U; // 返回紧凑 RGBA 缓冲所需字节数：像素数乘以 4 通道
} // decoded_frame_byte_count 结束

// 功能：释放 Impl 内全部 FFmpeg 资源（顺序与创建相反），置空指针。被构造失败与析构复用。
void free_decoder_impl(CustomByteBlockVideoDecoder::Impl &impl) {
    if (impl.parser != nullptr) { // 解析器存在才需要关闭
        av_parser_close(impl.parser); // 关闭解析器并释放其内部缓冲
        impl.parser = nullptr; // 置空避免悬空指针
    } // 解析器释放结束
    if (impl.packet != nullptr) { // 包存在才需要释放
        av_packet_free(&impl.packet); // av_packet_free 释放结构体并把指针置空
    } // 包释放结束
    if (impl.frame != nullptr) { // 帧存在才需要释放
        av_frame_free(&impl.frame); // av_frame_free 释放帧内引用并把指针置空
    } // 帧释放结束
    if (impl.codec_context != nullptr) { // 解码器上下文存在才需要释放
        avcodec_free_context(&impl.codec_context); // 关闭解码器并释放上下文
    } // 上下文释放结束
    if (impl.sws_context != nullptr) { // 缩放上下文存在才需要释放
        sws_freeContext(impl.sws_context); // swscale 上下文需用 sws_freeContext 释放
        impl.sws_context = nullptr; // 置空避免悬空指针
    } // 缩放上下文释放结束
} // free_decoder_impl 结束

// 功能：重建 H.264 解析器（先关闭旧的再初始化新的）。
void reset_parser(CustomByteBlockVideoDecoder::Impl *impl) {
    if (impl == nullptr) { // 实现对象不可为空
        throw std::invalid_argument("reset_parser() received null impl"); // 参数非法直接抛异常
    } // 空实现校验结束
    if (impl->parser != nullptr) { // 旧解析器存在则先关闭
        av_parser_close(impl->parser); // 关闭旧解析器，避免泄漏
        impl->parser = nullptr; // 置空后再重新创建
    } // 旧解析器清理结束
    impl->parser = av_parser_init(AV_CODEC_ID_H264); // 按 H.264 码流新建解析器
    if (impl->parser == nullptr) { // 分配失败说明内存不足或编解码库异常
        throw std::runtime_error("Failed to allocate H.264 parser"); // 抛异常由上层统一清理资源
    } // 解析器创建失败分支结束
} // reset_parser 结束

// 功能：丢包/异常后复位解码管线：冲刷解码器缓冲、清空待匹配元数据、重新等待关键帧并重置解析器。
void reset_decoder_after_loss(CustomByteBlockVideoDecoder::Impl *impl) {
    if (impl == nullptr) { // 实现对象不可为空
        throw std::invalid_argument("reset_decoder_after_loss() received null impl"); // 参数非法直接抛异常
    } // 空实现校验结束
    if (impl->codec_context != nullptr) { // 解码器上下文存在才需要冲刷
        avcodec_flush_buffers(impl->codec_context); // 丢弃解码器内部已缓冲的参考帧
    } // 上下文冲刷分支结束
    impl->pending_frames.clear(); // 清空待匹配元数据，避免旧帧信息错配
    impl->waiting_for_keyframe = true; // 复位后需重新等待 IDR 才能正常解码
    reset_parser(impl); // 重建解析器，丢弃其内部残留的码流状态
} // reset_decoder_after_loss 结束

// 功能：确保 swscale 上下文与当前帧的源尺寸/格式匹配，不匹配则重建。
// 思路：若已存在且参数一致直接复用；否则先释放旧上下文，再创建源→RGBA 的 SWS_BILINEAR 转换器并记录参数。
void ensure_sws_context(CustomByteBlockVideoDecoder::Impl &impl, const AVFrame *frame) {
    const auto frame_format = static_cast<AVPixelFormat>(frame->format); // 取解码帧的像素格式（如 YUV420P、NV12）
    if (impl.sws_context != nullptr && // 已有上下文时比较源尺寸与格式是否一致
        impl.source_width == frame->width && // 宽一致才可复用
        impl.source_height == frame->height && // 高一致才可复用
        impl.source_format == frame_format) { // 像素格式一致才可复用
        return; // 参数一致，直接复用现有缩放上下文
    } // 复用分支结束

    if (impl.sws_context != nullptr) { // 参数变化，先释放旧的缩放上下文
        sws_freeContext(impl.sws_context); // 释放旧上下文，避免内存泄漏
        impl.sws_context = nullptr; // 置空并准备重建
    } // 旧上下文清理结束

    // 创建同尺寸的 YUV(源格式) → RGBA 转换上下文
    impl.sws_context = sws_getContext(
            frame->width, frame->height, frame_format, // 输入：源宽高与源像素格式
            frame->width, frame->height, AV_PIX_FMT_RGBA, // 输出：同宽同高，像素格式固定 RGBA
            SWS_BILINEAR, nullptr, nullptr, nullptr); // SWS_BILINEAR 双线性插值；后三个 nullptr 表示使用默认参数
    if (impl.sws_context == nullptr) { // 上下文创建失败通常意味着尺寸或格式非法、内存不足
        throw std::runtime_error("Failed to create decoder swscale context"); // 抛异常，由上层转为解码失败处理
    } // 上下文创建失败分支结束

    // 记录当前源参数，供下次比较是否需重建
    impl.source_width = frame->width;
    impl.source_height = frame->height; // 记录源高度，供下次比较
    impl.source_format = frame_format; // 记录源像素格式，供下次比较
} // ensure_sws_context 结束

// 功能：循环从解码器取出所有已就绪帧，转为 RGBA 并补回帧元数据，返回这批帧。
// 思路：receive 到帧→确保 sws→分配对齐临时 RGBA 缓冲→sws_scale 转换→逐行拷贝到紧凑 buffer→匹配 pending 元数据或自增分配。
std::vector<CustomByteBlockDecodedFrame> drain_decoded_frames(CustomByteBlockVideoDecoder::Impl &impl,
                                                              const CustomByteBlockVideoDecoderConfig &config) { // 参数：解码配置，用于帧尺寸上限校验
    std::vector<CustomByteBlockDecodedFrame> frames; // 累积本次 drain 循环取出的已解码帧
    while (true) { // 循环取帧，直到解码器暂时没有输出
        const int ret_receive = avcodec_receive_frame(impl.codec_context, impl.frame); // 尝试从解码器取一帧到复用的帧对象
        if (ret_receive == AVERROR(EAGAIN) || ret_receive == AVERROR_EOF) { // EAGAIN 表示暂无帧可出，EOF 表示输入已结束
            break; // 无更多帧可输出，结束
        } // 无帧可取，结束本次 drain
        if (ret_receive < 0) { // 其余负值都是真正的解码错误
            throw std::runtime_error("Failed to receive decoded frame: " + ffmpeg_error_to_string(ret_receive)); // 抛异常，交由调用方复位解码器
        } // 解码错误分支结束

        ensure_sws_context(impl, impl.frame); // 确保 swscale 上下文与当前帧的格式、尺寸匹配

        CustomByteBlockDecodedFrame output; // 构造输出帧对象，稍后填充元数据与像素
        output.width = impl.frame->width; // 记录解码帧宽度
        output.height = impl.frame->height; // 记录解码帧高度
        output.rgba.resize(decoded_frame_byte_count(output.width, output.height, config)); // 预分配紧凑 RGBA 缓冲

        std::uint8_t *dst_slices[4] = {nullptr, nullptr, nullptr, nullptr}; // sws 输出平面指针（RGBA 仅用 [0] 平面）
        int dst_stride[4] = {0, 0, 0, 0}; // 每个平面的行字节步长（含对齐填充）

        // sws_scale may use SIMD stores that assume FFmpeg-style aligned buffers.
        // 用 FFmpeg 接口按 32 字节对齐分配临时 RGBA 缓冲，满足 SIMD 写入要求
        const int alloc_ret = av_image_alloc(dst_slices,
                                             dst_stride, // 输出：各平面行字节步长
                                             output.width, // 输出图像宽度
                                             output.height, // 输出图像高度
                                             AV_PIX_FMT_RGBA, // 输出像素格式为 RGBA（单平面交织存储）
                                             32); // 32 字节对齐，满足 sws_scale 的 SIMD 写入要求
        if (alloc_ret < 0) { // 分配失败说明内存不足
            throw std::runtime_error("Failed to allocate temporary RGBA image buffer"); // 抛异常，避免后续空指针写入
        } // 临时缓冲分配失败分支结束

        // 执行 YUV→RGBA 缩放转换
        const int scaled_height = sws_scale(
                impl.sws_context, // 输入：已建好的缩放上下文
                impl.frame->data, // 输入：源帧各平面数据指针
                impl.frame->linesize, // 输入：源帧各平面行字节步长
                0, // 输入：起始行号，0 表示从第 0 行开始
                impl.frame->height, // 输入：要转换的行数，即整帧高度
                dst_slices, // 输出：目标 RGBA 平面指针数组
                dst_stride); // 输出：目标各平面行字节步长
        if (scaled_height != output.height) { // 转换行数不等于输出高度，说明转换失败
            av_freep(&dst_slices[0]); // 转换失败先释放临时缓冲，避免泄漏
            throw std::runtime_error("Failed to convert decoded frame to RGBA"); // 抛异常，交由调用方复位解码器
        } // 转换失败分支结束
        // 逐行把带 stride 的对齐缓冲拷贝到紧凑 output.rgba（去除行对齐填充）
        for (int y = 0; y < output.height; ++y) {
            const auto *src_row = dst_slices[0] + static_cast<std::ptrdiff_t>(y) * dst_stride[0]; // 取对齐缓冲中第 y 行源地址
            auto *dst_row = output.rgba.data() + // 目标行起点 = 紧凑缓冲基址 + 行号乘以行字节数
                            static_cast<std::size_t>(y) * static_cast<std::size_t>(output.width) * 4U; // 计算紧凑 RGBA 缓冲中第 y 行目标地址
            std::memcpy(dst_row, src_row, static_cast<std::size_t>(output.width) * 4U); // 拷贝一行 RGBA（去掉行对齐填充）
        } // 逐行拷贝循环结束
        av_freep(&dst_slices[0]); // 释放临时缓冲

        // 按进入解码器的顺序匹配帧元数据；若无（如 flush 残留）则自增分配
        if (!impl.pending_frames.empty()) {
            const CustomByteBlockVideoDecoder::Impl::PendingFrame meta = impl.pending_frames.front(); // 取队首元数据，与出帧顺序一一对应
            impl.pending_frames.pop_front(); // 弹出已匹配的元数据，避免下次重复使用
            output.frame_id = meta.frame_id; // 使用该访问单元的帧序号
            output.pts = meta.pts; // 使用该访问单元的 PTS
            output.keyframe = meta.keyframe; // 使用该访问单元的关键帧标记
        } else { // 没有待匹配元数据（例如 flush 产生的残留帧）
            output.frame_id = impl.next_frame_id++; // 自增分配帧序号
            output.pts = (impl.frame->pts == AV_NOPTS_VALUE) ? impl.next_pts++ : impl.frame->pts; // 解码器给了 PTS 就用，否则用自增 PTS 兜底
            output.keyframe = (impl.frame->flags & AV_FRAME_FLAG_KEY) != 0; // 由 AVFrame 的 KEY 标志判断是否关键帧
        } // 元数据匹配分支结束
        frames.push_back(std::move(output)); // 把转换完成的帧移入结果集，避免拷贝
    } // 取帧循环结束
    return frames; // 返回本次取出的全部帧
} // drain_decoded_frames 结束

// 功能：把一个完整访问单元(NAL 集合)送入解码器并尝试取出已解码帧。
// 思路：先判关键帧与等待状态；做大小上限校验（超限即复位丢弃）；构造 AVPacket 并 send，
//       记录 pending 元数据，最后 drain 出帧；任何失败都复位解码器避免后续雪崩。
std::vector<CustomByteBlockDecodedFrame> decode_access_unit(CustomByteBlockVideoDecoder::Impl *impl,
                                                            const CustomByteBlockVideoDecoderConfig &config, // 参数：解码配置，用于访问单元大小校验
                                                            const std::uint8_t *access_unit, // 参数：完整访问单元（NAL 集合）数据指针
                                                            const std::size_t access_unit_size) { // 参数：访问单元字节数
    if (impl == nullptr) { // 实现对象不可为空
        throw std::invalid_argument("decode_access_unit() received null impl"); // 参数非法直接抛异常
    } // 空实现校验结束
    if (access_unit == nullptr && access_unit_size != 0U) { // 有长度却没有数据指针，属于调用错误
        throw std::invalid_argument("decode_access_unit() received null data with non-zero size"); // 参数非法直接抛异常
    } // 指针与长度一致性校验结束
    if (access_unit_size == 0U) { // 空访问单元无需解码
        return {}; // 直接返回空结果
    } // 空访问单元分支结束

    const bool keyframe = access_unit_has_idr(access_unit, access_unit_size); // 扫描起始码判断是否含 IDR，决定能否作为解码起点
    // 仍在等待关键帧且本单元非 IDR：直接丢弃，避免花屏
    if (impl->waiting_for_keyframe && !keyframe) {
        return {}; // 仍在等关键帧，丢弃该非关键帧，避免花屏
    } // 等待关键帧分支结束

    if (access_unit_size > config.max_access_unit_bytes) { // 超过配置的单访问单元字节上限，属于异常输入
        SPDLOG_WARN("丢弃过大的 CustomByteBlock 访问单元 bytes={} limit={}", // 记录丢弃原因与上限值
                    access_unit_size, // 日志参数：实际字节数
                    config.max_access_unit_bytes); // 日志参数：配置上限
        reset_decoder_after_loss(impl); // 复位解码器，丢弃已损坏的参考状态
        return {}; // 返回空结果
    } // 过大访问单元分支结束
    if (access_unit_size > static_cast<std::size_t>(std::numeric_limits<int>::max())) { // 超过 int 表示范围则无法传给 FFmpeg 的 int 接口
        SPDLOG_WARN("丢弃超出 FFmpeg 支持范围的 CustomByteBlock 访问单元 bytes={}", // 记录超出 FFmpeg 支持范围
                    access_unit_size); // 日志参数：实际字节数
        reset_decoder_after_loss(impl); // 复位解码器
        return {}; // 返回空结果
    } // 超范围访问单元分支结束

    // 复用 packet 并填入访问单元数据，分配可能失败
    const int ret_packet = av_new_packet(impl->packet, static_cast<int>(access_unit_size));
    if (ret_packet < 0) { // 分配失败说明内存不足
        SPDLOG_WARN("分配 CustomByteBlock 解码包失败 message={}", ffmpeg_error_to_string(ret_packet)); // 记录组包内存分配失败
        reset_decoder_after_loss(impl); // 复位解码器
        return {}; // 返回空结果
    } // 组包失败分支结束

    std::memcpy(impl->packet->data, access_unit, access_unit_size); // 把访问单元字节拷贝进包的 data 缓冲
    impl->packet->pts = impl->next_pts; // 自增分配时间戳（解析器无 PTS 时用）
    impl->packet->dts = impl->next_pts; // dts 与 pts 取同值，本场景无 B 帧重排延迟
    impl->packet->flags = keyframe ? AV_PKT_FLAG_KEY : 0; // 标记关键帧供解码器参考

    // 发送包给解码器，发送后即刻 unref 释放引用
    const int ret_send = avcodec_send_packet(impl->codec_context, impl->packet);
    av_packet_unref(impl->packet); // 发送后立即释放包引用，避免解码器继续持有该包
    if (ret_send < 0) { // 负返回值说明解码器拒收该包
        SPDLOG_WARN("CustomByteBlock 解码器拒收访问单元 keyframe={} message={}", // 记录拒收原因
                    keyframe, // 日志参数：是否关键帧
                    ffmpeg_error_to_string(ret_send)); // 日志参数：FFmpeg 错误描述
        reset_decoder_after_loss(impl); // 复位解码器，避免后续帧连锁失败
        return {}; // 返回空结果
    } // 拒收分支结束

    // 记录该访问单元对应的帧元数据，待出帧时顺序匹配
    impl->pending_frames.push_back({
            impl->next_frame_id++, // 元数据：帧序号，取用后自增
            impl->next_pts++, // 元数据：时间戳，取用后自增
            keyframe, // 元数据：是否关键帧
    }); // 元数据对象构造结束
    impl->waiting_for_keyframe = false; // 已收到可解码内容，后续不再强制等关键帧

    try { // 取帧阶段可能抛异常，需局部捕获
        return drain_decoded_frames(*impl, config); // 正常路径：把解码器已就绪的帧全部取出
    } catch (const std::exception &error) { // 取帧或像素转换过程中出现异常
        SPDLOG_WARN("CustomByteBlock 解码输出失败 message={}", error.what()); // 记录出错原因
        reset_decoder_after_loss(impl); // 复位解码器，避免错误状态累积
        return {}; // 返回空结果，上层继续处理后续包
    } // 取帧异常分支结束
} // decode_access_unit 结束

// 功能：把一批解码帧移动追加到目标 vector，避免额外拷贝。
void append_decoded_frames(std::vector<CustomByteBlockDecodedFrame> &target,
                           std::vector<CustomByteBlockDecodedFrame> &&incoming) { // 参数：待追加的源帧集合（右值，可被移空）
    target.insert(target.end(), // 追加到目标容器末尾
                  std::make_move_iterator(incoming.begin()), // 源起始迭代器包装为移动迭代器，避免拷贝帧数据
                  std::make_move_iterator(incoming.end())); // 以移动迭代器把 incoming 全部帧追加到末尾，零拷贝
} // append_decoded_frames 结束

// 功能：把一段 H.264 字节流喂给解析器，切出访问单元后逐个解码并汇总输出帧。
// 思路：循环 av_parser_parse2 切包，每切出一段就解码，直到流被消费完或解析器无产出。
std::vector<CustomByteBlockDecodedFrame> feed_parser(CustomByteBlockVideoDecoder::Impl *impl,
                                                     const CustomByteBlockVideoDecoderConfig &config, // 参数：解码配置，向下透传给解码流程
                                                     const std::uint8_t *payload, // 参数：待解析的 H.264 字节流起始地址
                                                     std::size_t payload_size) { // 参数：待解析字节数
    if (impl == nullptr) { // 实现对象不可为空
        throw std::invalid_argument("feed_parser() received null impl"); // 参数非法直接抛异常
    } // 空实现校验结束
    if (payload == nullptr && payload_size != 0U) { // 有长度却没有数据指针，属于调用错误
        throw std::invalid_argument("feed_parser() received null payload with non-zero size"); // 参数非法直接抛异常
    } // 指针与长度一致性校验结束

    std::vector<CustomByteBlockDecodedFrame> output; // 累积本次喂入流解码出的所有帧
    const std::uint8_t *cursor = payload; // 输入游标，随解析推进向前移动
    while (payload_size > 0U) { // 剩余字节未消费完就继续切包
        std::uint8_t *parsed_data = nullptr; // 解析器输出的本段访问单元数据指针
        int parsed_size = 0; // 本段访问单元的数据长度
        // 从剩余流中解析出一段完整访问单元
        const int consumed = av_parser_parse2(
                impl->parser, // 解析器上下文
                impl->codec_context, // 解码器上下文：解析器会把参数集更新进去
                &parsed_data, // 输出：切出的访问单元数据指针
                &parsed_size, // 输出：切出的访问单元字节数
                cursor, // 输入：当前游标
                static_cast<int>(payload_size), // 输入：剩余字节数（FFmpeg 接口为 int）
                AV_NOPTS_VALUE, // 不提供 PTS
                AV_NOPTS_VALUE, // 不提供 DTS
                0); // 末位保留参数固定传 0
        if (consumed < 0) { // 负值表示解析失败
            throw std::runtime_error("Failed to parse H.264 byte stream: " + ffmpeg_error_to_string(consumed)); // 抛异常，交由上层按包丢弃
        } // 解析失败分支结束

        cursor += consumed; // 输入游标前移已消费的字节数
        payload_size -= static_cast<std::size_t>(consumed); // 剩余待解析字节数相应减少

        if (parsed_size > 0) { // 切出有效访问单元才解码
            // 切出有效访问单元：解码并追加到输出
            append_decoded_frames(
                    output, // 结果集
                    decode_access_unit( // 解码该访问单元（内部含大小上限校验）
                            impl, // 参数：解码器实现
                            config, // 参数：解码配置
                            parsed_data, // 参数：访问单元数据
                            static_cast<std::size_t>(parsed_size))); // 参数：访问单元长度
        } // 访问单元解码分支结束

        if (consumed == 0 && parsed_size == 0) { // 解析器既没消费输入也没产出包，说明无法推进
            break; // 解析器无进展，防止死循环
        } // 解析无进展分支结束
    } // 解析循环结束

    return output; // 返回本次喂入流解出的全部帧
} // feed_parser 结束

// 功能：向解析器喂入空数据(nullptr,0)以冲刷出缓冲中的最后一段访问单元并解码。
// 思路：与 feed_parser 类似，但传入空负载触发解析器 flush，循环直到无产出。
std::vector<CustomByteBlockDecodedFrame> flush_parser(CustomByteBlockVideoDecoder::Impl *impl,
                                                      const CustomByteBlockVideoDecoderConfig &config) { // 参数：解码配置，向下透传
    if (impl == nullptr) { // 实现对象不可为空
        throw std::invalid_argument("flush_parser() received null impl"); // 参数非法直接抛异常
    } // 空实现校验结束

    std::vector<CustomByteBlockDecodedFrame> output; // 累积冲刷解析器缓冲取出的帧
    while (true) { // 反复冲刷直到解析器不再产出
        std::uint8_t *parsed_data = nullptr; // 解析器输出的本段数据指针
        int parsed_size = 0; // 本段数据长度
        const int consumed = av_parser_parse2( // 传空输入触发解析器 flush 出缓冲中的最后一段
                impl->parser, // 解析器上下文
                impl->codec_context, // 解码器上下文
                &parsed_data, // 输出：切出的访问单元数据指针
                &parsed_size, // 输出：切出的访问单元字节数
                nullptr, // 输入为空指针，表示进入冲刷模式
                0, // 输入长度为 0
                AV_NOPTS_VALUE, // 不提供 PTS
                AV_NOPTS_VALUE, // 不提供 DTS
                0); // 末位保留参数固定传 0
        if (consumed < 0) { // 负值表示冲刷过程出错
            throw std::runtime_error("Failed while flushing H.264 parser: " + ffmpeg_error_to_string(consumed)); // 抛异常，交由上层复位
        } // 冲刷出错分支结束
        (void) consumed; // flush 时 consumed 恒为 0，仅作占位

        if (parsed_size <= 0) { // 无更多缓冲数据可冲刷
            break; // 结束冲刷循环
        } // 缓冲已空退出分支结束

        append_decoded_frames( // 把冲刷出的帧移动追加到结果集
                output, // 结果集
                decode_access_unit( // 解码冲刷出的访问单元
                        impl, // 参数：解码器实现
                        config, // 参数：解码配置
                        parsed_data, // 参数：访问单元数据
                        static_cast<std::size_t>(parsed_size))); // 参数：访问单元长度
    } // 冲刷循环结束

    return output; // 返回冲刷出的全部帧
} // flush_parser 结束

// 功能：处理一条序列化包，维护 16 位序列号连续性，必要时复位解码器，随后喂入解析器。
// 思路：首包建立期望基准；收到期望包则递增；收到更新的包说明丢包→复位；收到旧包则按空闲/回退阈值决定是否重同步，否则丢弃。
std::vector<CustomByteBlockDecodedFrame> process_serialized_packet(CustomByteBlockVideoDecoder::Impl &impl,
                                                                   const CustomByteBlockVideoDecoderConfig &config, // 参数：解码配置
                                                                   const SerializedPacket &packet) { // 参数：已反序列化的序列化包
    const auto now = std::chrono::steady_clock::now(); // 记录本次处理时刻，用于空闲/重同步判定
    if (!impl.has_expected_sequence) { // 尚未建立序列号基准（首个包）
        // 首个包：以它的下一序号作为期望基准
        impl.expected_sequence_id = static_cast<std::uint16_t>(packet.sequence_id + 1U);
        impl.has_expected_sequence = true; // 标记基准已建立
    } else if (packet.sequence_id == impl.expected_sequence_id) { // 恰好是期望的下一包，正常推进
        // 恰好是期望的下一包，正常推进
        impl.expected_sequence_id = static_cast<std::uint16_t>(impl.expected_sequence_id + 1U);
    } else if (sequence_is_newer(packet.sequence_id, impl.expected_sequence_id)) { // 收到比期望更新的包，说明中间发生丢包
        // 收到了更新（跳号）的包，说明中途丢包，复位解码器避免错误参考
        SPDLOG_WARN("CustomByteBlock 检测到丢包，重置解码器 expected_sequence={} received_sequence={}",
                    impl.expected_sequence_id, // 日志参数：期望的序列号
                    packet.sequence_id); // 日志参数：实际收到的序列号
        reset_decoder_after_loss(&impl); // 复位解码器，避免用错误的参考帧解码
        impl.expected_sequence_id = static_cast<std::uint16_t>(packet.sequence_id + 1U); // 以该包下一序号重建期望
        impl.has_expected_sequence = true; // 保持基准有效
    } else { // 收到较旧的包：判断是否值得重同步
        // 收到较旧的包：判断是否值得重同步（空闲太久或回退过大），否则直接忽略重复/旧包
        const std::uint16_t backward_distance = static_cast<std::uint16_t>(impl.expected_sequence_id - packet.sequence_id);
        const bool idle_too_long = impl.last_packet_time != std::chrono::steady_clock::time_point::min() // 距上次收包过久（超过阈值）则允许重同步
                                   && (now - impl.last_packet_time) > kSequenceResyncIdleTimeout; // 条件续行：与空闲超时阈值比较
        const bool backward_jump_too_large = backward_distance > kSequenceResyncBackwardThreshold; // 回退距离超过阈值同样允许重同步
        if (idle_too_long || backward_jump_too_large) { // 满足任一条件即执行重同步
            const auto idle_ms = impl.last_packet_time == std::chrono::steady_clock::time_point::min() // 计算空闲时长；从未收过包时按 0 处理
                                         ? 0LL // 从未收包时的空闲时长取 0
                                         : std::chrono::duration_cast<std::chrono::milliseconds>(now - impl.last_packet_time).count(); // 否则把时间差换算为毫秒
            SPDLOG_INFO("CustomByteBlock 序列重同步 expected_sequence={} received_sequence={} idle_ms={} backward_distance={}", // 记录重同步事件与判定依据
                        impl.expected_sequence_id, // 日志参数：期望的序列号
                        packet.sequence_id, // 日志参数：实际收到的序列号
                        idle_ms, // 日志参数：空闲毫秒数
                        backward_distance); // 日志参数：回退的序号距离
            reset_decoder_after_loss(&impl); // 复位解码器，丢弃旧参考状态
            impl.expected_sequence_id = static_cast<std::uint16_t>(packet.sequence_id + 1U); // 以该包下一序号重建期望
            impl.has_expected_sequence = true; // 保持基准有效
        } else { // 既不超时也未大幅回退，视为重复或过期包
            // Older or duplicate packet: ignore.
            return {};
        } // 忽略该包，不产生任何帧
    } // 旧包处理分支结束

    impl.last_packet_time = now; // 记录本次收包时刻，供下次空闲判定
    // 序列校验通过：把净负载喂入解析器并解码
    return feed_parser(
            &impl, // 参数：解码器实现
            config, // 参数：解码配置
            packet.payload.data(), // 参数：净负载数据指针
            packet.payload.size()); // 参数：净负载字节数
} // process_serialized_packet 结束

// 功能：反序列化一条序列化包字节并交给 process_serialized_packet 处理。
std::vector<CustomByteBlockDecodedFrame> decode_serialized_packet_bytes(CustomByteBlockVideoDecoder::Impl &impl,
                                                                         const CustomByteBlockVideoDecoderConfig &config, // 参数：解码配置
                                                                         const std::span<const std::uint8_t> wire_bytes) { // 参数：原始线格式字节
    SerializedPacket packet = deserialize_serialized_packet(wire_bytes); // 先按线格式反序列化出序列号与净负载
    return process_serialized_packet(impl, config, packet); // 再做序列号校验并喂入解析器解码
} // decode_serialized_packet_bytes 结束

// 功能：把传输包束拆成子包，逐个当作序列化包处理并汇总输出帧。
std::vector<CustomByteBlockDecodedFrame> decode_transport_bundle_bytes(CustomByteBlockVideoDecoder::Impl &impl,
                                                                       const CustomByteBlockVideoDecoderConfig &config, // 参数：解码配置
                                                                       const std::span<const std::uint8_t> wire_bytes) { // 参数：原始线格式字节
    const auto packet_spans = unpack_transport_packet_spans(wire_bytes); // 先按传输包束格式拆出各子包视图
    if (packet_spans.empty()) { // 没有子包（全为填充）则无帧可解
        return {}; // 返回空结果
    } // 无子包分支结束

    std::vector<CustomByteBlockDecodedFrame> decoded_frames; // 累积各子包解码出的帧
    for (const std::span<const std::uint8_t> packet_span: packet_spans) { // 逐个处理子包（每个子包自身是一个序列化包）
        SerializedPacket packet = deserialize_serialized_packet(packet_span); // 反序列化出序列号与净负载
        append_decoded_frames(decoded_frames, process_serialized_packet(impl, config, packet)); // 处理该子包并移动追加结果
    } // 子包遍历结束
    return decoded_frames; // 返回包束解出的全部帧
} // decode_transport_bundle_bytes 结束

// 功能：按指定线格式分派到对应的解码入口。
std::vector<CustomByteBlockDecodedFrame> decode_wire_bytes_with_format(CustomByteBlockVideoDecoder::Impl &impl,
                                                                       const CustomByteBlockVideoDecoderConfig &config, // 参数：解码配置
                                                                       const std::span<const std::uint8_t> wire_bytes, // 参数：原始线格式字节
                                                                       const WireFormat format) { // 参数：已确定的线格式
    switch (format) { // 按线格式分派到对应的解码入口
        case WireFormat::SerializedPacket: // 单条序列化包
            return decode_serialized_packet_bytes(impl, config, wire_bytes); // 交给序列化包解码路径
        case WireFormat::TransportBundle: // 传输包束
            return decode_transport_bundle_bytes(impl, config, wire_bytes); // 交给包束拆分解码路径
        case WireFormat::Unknown: // 尚未识别（含非法枚举值）
        default: // default 与 Unknown 合并处理
            throw std::invalid_argument("unsupported wire format"); // 未知格式属于调用错误
    } // switch 结束
} // decode_wire_bytes_with_format 结束

// 功能：在未确定线格式时，启发式判断应优先尝试「传输包束」还是「序列化包」。
// 思路：已知格式直接返回；未知时，满字节数(300)或首子包长度合法且能容纳者优先按包束尝试，否则按序列化包。
bool should_try_transport_first(const WireFormat wire_format, const std::span<const std::uint8_t> wire_bytes) {
    if (wire_format == WireFormat::TransportBundle) { // 已知为包束格式，优先按包束解析
        return true; // 返回：优先尝试包束
    } // 包束判定结束
    if (wire_format == WireFormat::SerializedPacket) { // 已知为序列化包格式，优先按单包解析
        return false; // 返回：不尝试包束
    } // 单包判定结束

    if (wire_bytes.size() == kMaxSupportedPacketBytes) { // 恰好 300 字节的满包几乎只能是包束
        return true; // 返回：优先尝试包束
    } // 满包判定结束
    if (wire_bytes.size() < kTransportLengthPrefixBytes) { // 不足 2 字节连长度前缀都放不下
        return false; // 返回：按单包处理
    } // 过短判定结束

    const std::uint16_t first_inner_size = read_u16_be(wire_bytes.data()); // 读取首个子包的长度前缀
    if (first_inner_size != 0U && // 首子包长度非 0
        first_inner_size <= kMaxInnerPacketBytes && // 且不超过子包净负载上限
        (kTransportLengthPrefixBytes + static_cast<std::size_t>(first_inner_size)) <= wire_bytes.size()) { // 且前缀加子包长度不越界，说明符合包束结构
        return true; // 返回：优先尝试包束
    } // 结构匹配分支结束

    return false; // 结构不匹配，按单包处理
} // should_try_transport_first 结束

}// namespace

// 功能：构造解码器：校验配置，查找并打开 H.264 解码器，分配帧/包，初始化解析器。
// 思路：任何一步失败都调用 free_decoder_impl 清理已分配资源后再抛异常，保证不泄漏。
CustomByteBlockVideoDecoder::CustomByteBlockVideoDecoder(CustomByteBlockVideoDecoderConfig config)
    : config_(std::move(config)), // 保存配置（移动避免拷贝）
      impl_(std::make_unique<Impl>()) { // 创建 PIMPL 实现实例，持有全部 FFmpeg 资源
    validate_config(config_); // 校验配置各项上限，非法即抛异常

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264); // 查找 libavcodec 内置的 H.264 解码器
    if (codec == nullptr) { // 未找到说明 FFmpeg 构建缺少该解码器
        throw std::runtime_error("Unable to find an H.264 decoder"); // 抛异常，由调用方处理
    } // 解码器查找失败分支结束

    impl_->codec_context = avcodec_alloc_context3(codec); // 按解码器创建上下文，持有解码状态
    if (impl_->codec_context == nullptr) { // 分配失败说明内存不足
        throw std::runtime_error("Failed to allocate decoder context"); // 抛异常，由上层清理
    } // 上下文分配失败分支结束

    const int ret_open = avcodec_open2(impl_->codec_context, codec, nullptr); // 打开解码器，完成内部表与线程初始化
    if (ret_open < 0) { // 打开失败需先释放已分配资源再抛异常
        const std::string message = ffmpeg_error_to_string(ret_open); // 把错误码转成可读文本
        free_decoder_impl(*impl_); // 释放上下文、解析器、帧包等已分配资源
        throw std::runtime_error("Failed to open decoder: " + message); // 抛异常，保证不泄漏
    } // 打开失败分支结束

    impl_->frame = av_frame_alloc(); // 分配复用的解码输出帧
    impl_->packet = av_packet_alloc(); // 分配复用的输入包
    if (impl_->frame == nullptr || impl_->packet == nullptr) { // 任一分配失败都要统一清理
        free_decoder_impl(*impl_); // 释放已分配的全部资源
        throw std::runtime_error("Failed to allocate decoder frame or packet"); // 抛异常，保证不泄漏
    } // 帧包分配失败分支结束

    try { // 解析器创建可能抛异常，需清理后再抛出
        reset_parser(impl_.get()); // 创建 H.264 解析器
    } catch (...) { // 创建失败则先释放已分配资源
        free_decoder_impl(*impl_); // 释放已分配的全部资源
        throw; // 原样抛出，保留异常类型与信息
    } // 解析器创建异常分支结束
} // 构造函数结束

// 功能：析构，释放 Impl 内全部 FFmpeg 资源。
CustomByteBlockVideoDecoder::~CustomByteBlockVideoDecoder() {
    if (impl_ != nullptr) { // PIMPL 存在才需要释放内部资源
        free_decoder_impl(*impl_); // 释放 FFmpeg 资源（Impl 本身由 unique_ptr 自动析构）
    } // 空实现检查结束
} // 析构函数结束

CustomByteBlockVideoDecoder::CustomByteBlockVideoDecoder(CustomByteBlockVideoDecoder &&) noexcept = default; // 移动构造：默认转移 Impl 与配置所有权
CustomByteBlockVideoDecoder &CustomByteBlockVideoDecoder::operator=(CustomByteBlockVideoDecoder &&) noexcept = default; // 移动赋值：默认转移资源所有权

// 功能：送入一个线格式包，返回本次解码出的帧；支持两种格式的自动探测与失败回退。
// 思路：先用启发式选择优先格式，主解析失败则回退到另一种格式再试；两者皆失败则丢弃并告警，确保鲁棒。
std::vector<CustomByteBlockDecodedFrame> CustomByteBlockVideoDecoder::push_packet_bytes(std::span<const std::uint8_t> wire_bytes) {
    const bool transport_first = should_try_transport_first(impl_->wire_format, wire_bytes); // 用启发式选择优先尝试的格式
    const WireFormat primary_format = transport_first ? WireFormat::TransportBundle : WireFormat::SerializedPacket; // 主格式：优先尝试的那一种
    const WireFormat fallback_format = transport_first ? WireFormat::SerializedPacket : WireFormat::TransportBundle; // 回退格式：主格式失败时再试的那一种

    try { // 主格式尝试可能抛异常，需捕获后回退
        std::vector<CustomByteBlockDecodedFrame> decoded_frames = // 优先按主格式尝试解码
                decode_wire_bytes_with_format(*impl_, config_, wire_bytes, primary_format); // 按主格式解码该包
        if (impl_->wire_format != primary_format) { // 识别到的格式与当前记录不同，说明发生切换
            SPDLOG_INFO("CustomByteBlock 输入包格式切换 old_format={} new_format={}", // 记录格式切换事件
                        wire_format_to_string(impl_->wire_format), // 日志参数：旧格式
                        wire_format_to_string(primary_format)); // 日志参数：新格式
            impl_->wire_format = primary_format; // 记录识别到的格式，后续直接以该格式优先
        } // 更新记录，后续直接以该格式优先
        return decoded_frames; // 返回主格式解码结果
    } catch (const std::exception &primary_error) { // 主格式失败，尝试回退格式
        try { // 回退尝试同样可能失败
            std::vector<CustomByteBlockDecodedFrame> decoded_frames = // 主格式失败，回退到另一种格式
                    decode_wire_bytes_with_format(*impl_, config_, wire_bytes, fallback_format); // 按回退格式解码该包
            if (impl_->wire_format != fallback_format) { // 回退格式与当前记录不同
                SPDLOG_INFO("CustomByteBlock 输入包格式切换 old_format={} new_format={}", // 记录格式切换事件
                            wire_format_to_string(impl_->wire_format), // 日志参数：旧格式
                            wire_format_to_string(fallback_format)); // 日志参数：新格式
                impl_->wire_format = fallback_format; // 更新记录，后续以回退格式优先
            } // 格式切换分支结束
            SPDLOG_DEBUG("CustomByteBlock 输入包按 {} 解析失败，已自动回退到 {} bytes={} primary_error={}", // 记录一次自动回退（主格式解析失败）
                         wire_format_to_string(primary_format), // 日志参数：主格式名
                         wire_format_to_string(fallback_format), // 日志参数：回退格式名
                         wire_bytes.size(), // 日志参数：包字节数
                         primary_error.what()); // 日志参数：主格式的错误信息
            return decoded_frames; // 返回回退格式解码结果
        } catch (const std::exception &fallback_error) { // 两种格式都失败，丢弃该包
            SPDLOG_WARN("丢弃无效的 CustomByteBlock 输入包 bytes={} primary_format={} primary_error={} fallback_format={} fallback_error={}", // 记录最终丢弃原因与两种格式的错误
                        wire_bytes.size(), // 日志参数：包字节数
                        wire_format_to_string(primary_format), // 日志参数：主格式名
                        primary_error.what(), // 日志参数：主格式错误
                        wire_format_to_string(fallback_format), // 日志参数：回退格式名
                        fallback_error.what()); // 日志参数：回退格式错误
            return {}; // 无帧可返回，返回空结果
        } // 双格式失败分支结束
    } // 主格式异常处理结束
} // push_packet_bytes 结束

// 功能：冲刷解码器，取出所有滞留的已解码帧，供停流时收尾。
// 思路：先用 flush_parser 冲刷解析器缓冲，再向解码器发送空包触发末尾帧输出，最后 drain 出帧；异常时复位解码器。
std::vector<CustomByteBlockDecodedFrame> CustomByteBlockVideoDecoder::flush() {
    std::vector<CustomByteBlockDecodedFrame> output; // 累积冲刷过程中取出的全部滞留帧
    try { // 冲刷过程任何异常都要复位解码器后抛出
        append_decoded_frames(output, flush_parser(impl_.get(), config_)); // 先冲刷解析器，取出其缓冲中的最后一段访问单元

        // 发送空包告知解码器输入结束，促其输出剩余帧
        const int ret_send = avcodec_send_packet(impl_->codec_context, nullptr);
        if (ret_send < 0 && ret_send != AVERROR_EOF) { // 发送空包触发解码器输出尾帧；EOF 属正常情况
            reset_decoder_after_loss(impl_.get()); // 发送失败先复位解码器
            throw std::runtime_error("Failed to flush decoder: " + ffmpeg_error_to_string(ret_send)); // 再抛异常告知上层冲刷失败
        } // 发送失败分支结束

        append_decoded_frames(output, drain_decoded_frames(*impl_, config_)); // 取出解码器中所有就绪的残留帧
        return output; // 返回冲刷出的全部帧
    } catch (...) { // 任意异常统一复位后重新抛出
        reset_decoder_after_loss(impl_.get()); // 复位解码器，避免错误状态残留
        throw; // 原样抛出，保留异常类型与信息
    } // 异常处理结束
} // flush 结束

}// namespace RoboGo::video
