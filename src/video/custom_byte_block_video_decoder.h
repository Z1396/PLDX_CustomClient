#pragma once

#include <pch.h>

namespace RoboGo::video {

// 自定义字节块解码器配置：约束解码资源上限，防止异常输入耗尽内存或算力。
struct CustomByteBlockVideoDecoderConfig {
    int max_reorder_frames = 8;                       // 允许的最大重排帧数（B 帧重排序窗口）
    std::size_t max_access_unit_bytes = 512U * 1024U; // 单个访问单元(完整帧码流)最大字节数
    std::size_t max_assembly_bytes = 512U * 1024U;    // 组装缓冲上限（预留字段，当前未直接校验）
    int max_frame_dimension = 4096;                   // 单帧宽高上限(像素)
    std::size_t max_frame_pixels = 4096U * 4096U;     // 单帧总像素上限
};

// 解码输出的一帧：携带元数据与 RGBA 像素数据，供上层显示或旋转处理。
struct CustomByteBlockDecodedFrame {
    std::uint32_t frame_id = 0;     // 帧序号（由解码器自增分配或与分片一致）
    std::int64_t pts = 0;           // 显示时间戳
    bool keyframe = false;          // 是否为关键帧(IDR)
    int width = 0;                  // 图像宽
    int height = 0;                 // 图像高
    std::vector<std::uint8_t> rgba; // RGBA 像素数据，长度 = width*height*4
};

// 自定义字节块视频解码器：解析「序列化包/传输包束」两套线格式，按 H.264 解码并转 RGBA，输出 CustomByteBlockDecodedFrame。
class CustomByteBlockVideoDecoder {
public:
    struct Impl; // PIMPL 实现，隐藏 FFmpeg 头依赖

    explicit CustomByteBlockVideoDecoder(CustomByteBlockVideoDecoderConfig config = {});
    ~CustomByteBlockVideoDecoder();

    CustomByteBlockVideoDecoder(CustomByteBlockVideoDecoder &&) noexcept;
    CustomByteBlockVideoDecoder &operator=(CustomByteBlockVideoDecoder &&) noexcept;

    CustomByteBlockVideoDecoder(const CustomByteBlockVideoDecoder &) = delete; // 含 unique_ptr，禁止拷贝
    CustomByteBlockVideoDecoder &operator=(const CustomByteBlockVideoDecoder &) = delete;

    // 功能：送入一个线格式字节包(序列化包或传输包束)，返回本次解码出的若干帧。
    std::vector<CustomByteBlockDecodedFrame> push_packet_bytes(std::span<const std::uint8_t> wire_bytes);
    // 功能：冲刷解码器，取出所有已解码但滞留的帧（用于停流时收尾）。
    std::vector<CustomByteBlockDecodedFrame> flush();

private:
    CustomByteBlockVideoDecoderConfig config_; // 解码配置（上限约束）
    std::unique_ptr<Impl> impl_;               // 具体实现，生命周期由本类管理
};

}// namespace RoboGo::video
