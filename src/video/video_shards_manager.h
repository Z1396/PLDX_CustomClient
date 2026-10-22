#pragma once

#include <pch.h>

namespace RoboGo::video {

    // 单帧的分片集合：记录该帧已收到的所有分片、总长度与已收集长度，用于重组成完整帧。
    struct FrameShards {
        bool is_valid = false; // 该帧槽位是否已初始化（收到过属于它的分片）
        uint32_t frame_id = 0; // 归一化后的全局帧 ID（已处理回绕）
        uint32_t frame_bytes = 0; // 整帧期望的总字节数（由包头携带）
        uint32_t collected_frame_bytes = 0; // 已收集到的分片净字节数（不含每片 8 字节头）
        std::map<uint16_t, std::pair<uint8_t *, size_t>> shards; // shard_id -> (数据指针, 含头长度)，按 id 有序便于顺序拼接
    };

    // 视频分片管理器：接收 UDP 视频分片并重组为完整帧，处理帧 ID 回绕、丢帧冲刷与过期分片丢弃。
    class VideoShardsManager {
    private:
        // 缓存数据包相关
        moodycamel::BlockingReaderWriterQueue<std::pair<uint8_t *, size_t>> packet_queue; // 无锁阻塞队列，UDP 线程入队、解码线程出队
        // 帧 ID 相关
        uint32_t fixed_frame_id_start = 0; // 16 位帧 ID 归一到 32 位时的偏移基准
        uint32_t max_frame_id = 0; // 已出现的最大归一化帧 ID
        bool is_current_frame_id_valid = false; // 当前帧 ID 基准是否已完成初始化
        uint32_t current_frame_id = 0; // 当前正在追踪的基准帧 ID（用于计算相对偏移）
        std::optional<uint32_t> last_complete_frame_id; // 上一完整帧的 ID，用于检测跳帧
        // 缓存帧片段相关
        static constexpr int cache_frame_size = 20; // 滑动窗口大小：最多缓存 20 帧的分片以容忍乱序/延迟
        std::array<FrameShards, cache_frame_size> cache_frames; // 帧分片环形缓存窗口
        // 完整帧相关
        std::vector<uint8_t> full_frame; // 重组成功后的完整帧缓存，回调前填充
        std::function<void(const std::vector<uint8_t> &frame)> frame_callback; // 完整帧就绪时回调

    public:
        // 功能：析构时清理帧序列状态并排空残留队列，释放所有分片内存。
        ~VideoShardsManager();

        // 功能：注册完整帧就绪后的回调（在构造后、解码线程启动前设置）。
        void set_frame_callback(const std::function<void(const std::vector<uint8_t> &frame)> &frame_callback);

        // 功能：UDP 接收线程调用，把原始分片数据拷贝后入队，交由解码线程处理。
        void on_receive_shard(const uint8_t *data, size_t size);

        // 功能：解码线程循环调用，从队列取一包并解析、重组、必要时回调完整帧。
        void handle_once();

    private:
        // 功能：重置整个流的帧序列状态、缓存窗口与完整帧缓冲。
        void reset_stream_state();

        // 功能：排空数据包队列并释放其中所有分片的内存。
        void drain_packet_queue();

        // 功能：释放单个帧槽位内所有分片内存并清零元数据，使其可复用。
        static void reset_frame_shards(FrameShards &frame_shards);
    };

}// namespace RoboGo::video
