#include "video_shards_manager.h"

namespace RoboGo::video {

    // 功能：析构时复位流状态并清空队列，确保分片占用内存被释放。
    VideoShardsManager::~VideoShardsManager() {
        reset_stream_state();
        drain_packet_queue();
    }

    // 功能：保存完整帧回调，供重组完成后通知上层。
    void VideoShardsManager::set_frame_callback(const std::function<void(const std::vector<uint8_t> &frame)> &frame_callback) {
        this->frame_callback = frame_callback;
        SPDLOG_INFO("已设置视频完整帧回调");
    }

    // 功能：把收到的一帧分片数据拷贝后入队，与解码线程解耦。
    // 思路：队列只持有裸指针+长度，需先 new 拷贝数据避免在 UDP 回调栈上悬空。
    void VideoShardsManager::on_receive_shard(const uint8_t *data, size_t size) {
        auto *packet = new uint8_t[size];
        memcpy(packet, data, size);
        packet_queue.emplace(packet, size); // 无锁入队，交给解码线程处理
    }

    // 功能：处理一个分片包：背压控制、解析包头、维护帧 ID 序列、入窗重组并输出完整帧。
    // 思路：先按队列水位丢弃陈旧分片；再解析「帧ID/分片ID/整帧长度」头；用归一化帧 ID 计算相对窗口偏移，
    //       处理 16 位帧 ID 回绕、过期丢弃、整体丢帧冲刷；最后把分片放入窗口，凑齐即拼接回调并滑窗。
    void VideoShardsManager::handle_once() {
        // 如果数据包过多导致阻塞，就丢弃旧的数据包
        if (packet_queue.size_approx() >= 512) {
            std::size_t trimmed_count = 0;
            std::pair<uint8_t *, size_t> stale_packet;
            // 仅保留最新 256 个，丢弃更旧的，避免内存无限增长与延迟累积
            while (packet_queue.size_approx() > 256 && packet_queue.try_dequeue(stale_packet)) {
                delete[] stale_packet.first;
                ++trimmed_count;
            }
            if (trimmed_count > 0) {
                SPDLOG_WARN("UDP 视频分片队列堆积，已丢弃旧 {} 个分片", trimmed_count);
            }
        }
        // 获取一个数据包
        std::pair<uint8_t *, size_t> packet;
        // 阻塞极短时间(1us)，既响应新数据又不过度占用 CPU
        if (!packet_queue.wait_dequeue_timed(packet, std::chrono::microseconds(1))) {
            return;
        }
        auto [data, size] = packet;
        // 解析包头：每片前 8 字节依次为 帧ID(2)/分片ID(2)/整帧字节数(4)，不足 8 字节视为无效
        if (size <= 8) [[unlikely]] {
            SPDLOG_WARN("收到无效视频分片 bytes={}", size);
            delete[] data;
            return;
        }
        uint16_t frame_id = ntohs(*reinterpret_cast<const uint16_t *>(data));       // 网络字节序转主机，帧 ID
        uint16_t shard_id = ntohs(*reinterpret_cast<const uint16_t *>(data + 2));   // 分片序号
        uint32_t frame_bytes = ntohl(*reinterpret_cast<const uint32_t *>(data + 4)); // 整帧期望总字节数

        // 处理帧 ID 溢出问题：首次出现时以当前帧 ID 作为基准
        if (!is_current_frame_id_valid) {
            is_current_frame_id_valid = true;
            current_frame_id = frame_id;
            SPDLOG_INFO("初始化视频帧序列 current_frame_id={}", current_frame_id);
        }
        // 把 16 位帧 ID 归一到 32 位连续空间，便于计算相对偏移
        uint32_t fixed_frame_id = fixed_frame_id_start + frame_id;
        int64_t delta_frame_id = static_cast<int64_t>(fixed_frame_id) - current_frame_id;
        // 帧 ID 大幅倒退且处于小值区，判定为 16 位计数器回绕，整体抬升基准
        if (delta_frame_id <= -100 && frame_id < cache_frame_size) {
            fixed_frame_id = max_frame_id + 1;
            fixed_frame_id_start = fixed_frame_id - frame_id;
            delta_frame_id = 1;
            SPDLOG_INFO("视频帧ID发生回绕 frame_id={} fixed_frame_id={}", frame_id, fixed_frame_id);
        }
        max_frame_id = std::max(max_frame_id, fixed_frame_id);

        // 如果接收到过去的数据包，就丢掉
        if (delta_frame_id < 0) [[unlikely]] {
            // 倒退超过一个窗口，说明流断开重连或编码器重置，需要彻底复位帧序列
            if (delta_frame_id < -static_cast<int64_t>(cache_frame_size)) {
                // 流断连后重连或编码器重置，重置帧序列
                SPDLOG_WARN("视频流序列不连续，重置帧序列 old_current={} new_frame_id={}",
                            current_frame_id, frame_id);
                reset_stream_state();
                is_current_frame_id_valid = true;
                current_frame_id = frame_id;
                fixed_frame_id = frame_id;
                delta_frame_id = 0;
            } else {
                // 轻微倒退：仅仅是过期重传分片，直接丢弃避免破坏当前窗口
                SPDLOG_WARN("丢弃过期视频分片 frame_id={} shard_id={} current_frame_id={}",
                            fixed_frame_id, shard_id, current_frame_id);
                delete[] data;
                return;
            }
        }

        // 处理丢帧：当前帧远超窗口，说明中间大量帧丢失，向前滑动窗口丢弃旧帧
        if (delta_frame_id >= cache_frame_size) [[unlikely]] {
            SPDLOG_WARN("视频帧缓存被冲刷 delta_frame_id={} cache_frame_size={}", delta_frame_id, cache_frame_size);
            for (size_t i = 0; i < cache_frame_size; ++i) {
                // 把窗口内相对靠后的帧整体前移，腾出最新位置
                size_t target_index = i + delta_frame_id - cache_frame_size + 1;
                if (target_index < cache_frame_size) {
                    std::swap(cache_frames[i], cache_frames[target_index]);
                } else {
                    // 目标已超出窗口范围，直接清空该旧帧槽位
                    reset_frame_shards(cache_frames[i]);
                }
            }
            // 更新基准帧 ID 与相对偏移，使最新帧落在窗口末尾
            current_frame_id = fixed_frame_id - cache_frame_size + 1;
            delta_frame_id = cache_frame_size - 1;
        }

        // 把数据包添加到缓存中：按相对偏移定位窗口中的帧槽位
        auto &current_frame_shards = cache_frames[delta_frame_id];
        // 首次写入该帧槽位：初始化其元数据
        if (!current_frame_shards.is_valid) [[unlikely]] {
            current_frame_shards.is_valid = true;
            current_frame_shards.frame_id = fixed_frame_id;
            current_frame_shards.frame_bytes = frame_bytes;
        } else if (current_frame_shards.frame_bytes != frame_bytes) {
            // 同一帧的整帧长度发生变化（异常），更新期望值
            SPDLOG_WARN("视频帧长度发生变化 frame_id={} old_bytes={} new_bytes={}",
                        fixed_frame_id,
                        current_frame_shards.frame_bytes,
                        frame_bytes);
            current_frame_shards.frame_bytes = frame_bytes;
        }
        // 若该分片已存在（重传），先释放旧数据并扣回已计字节数
        if (const auto iter = current_frame_shards.shards.find(shard_id); iter != current_frame_shards.shards.end()) [[unlikely]] {
            SPDLOG_WARN("收到重复视频分片 frame_id={} shard_id={} old_size={} new_size={}",
                        fixed_frame_id,
                        shard_id,
                        iter->second.second,
                        size);
            delete[] iter->second.first;
            current_frame_shards.collected_frame_bytes -= static_cast<uint32_t>(iter->second.second - 8);
        }
        // 累加本分片净负载（总长减去 8 字节头），并存入窗口
        current_frame_shards.collected_frame_bytes += size - 8;
        current_frame_shards.shards[shard_id] = {data, size};

        // 处理完整帧：仅检查窗口头(cache_frames[0])是否凑齐
        auto &first_frame_shards = cache_frames[0];
        if (first_frame_shards.is_valid && first_frame_shards.frame_bytes == first_frame_shards.collected_frame_bytes) [[unlikely]] {
            // 检测跳帧：与上一次完整帧 ID 不连续即说明中间有丢帧
            if (last_complete_frame_id.has_value() && last_complete_frame_id.value() + 1 != first_frame_shards.frame_id) [[unlikely]] {
                SPDLOG_ERROR("skip frame：{} -> {}", last_complete_frame_id.value(), first_frame_shards.frame_id);
            }
            last_complete_frame_id = first_frame_shards.frame_id;
            // 拼接：按分片 ID 顺序把各片净负载拷贝到 full_frame
            full_frame.clear();
            full_frame.reserve(first_frame_shards.frame_bytes);
            bool is_success = true;
            for (size_t j = 0; j < first_frame_shards.shards.size(); ++j) {
                // 期望分片 ID 连续从 0 开始，缺失则拼接失败
                const auto &iter = first_frame_shards.shards.find(j);
                if (iter == first_frame_shards.shards.end()) [[unlikely]] {
                    is_success = false;
                    SPDLOG_ERROR("分片ID错误");
                    break;
                }
                auto &[shard_buffer, shard_size] = iter->second;
                // 跳过每片前 8 字节头，仅拷贝净负载
                full_frame.insert(full_frame.end(), shard_buffer + 8, shard_buffer + shard_size);
            }
            if (is_success) [[likely]] {
                if (frame_callback) {
                    frame_callback(full_frame); // 回调完整帧给上层（解码器）
                } else {
                    SPDLOG_WARN("视频完整帧已生成但未设置回调 frame_id={}", first_frame_shards.frame_id);
                }
            }
            // 在缓存中清理当前帧：整体前移窗口并清空末尾槽位
            for (size_t j = 0; j < cache_frame_size - 1; ++j) {
                std::swap(cache_frames[j], cache_frames[j + 1]);
            }
            reset_frame_shards(cache_frames[cache_frame_size - 1]);
            current_frame_id += 1; // 窗口整体前进一帧
        }
    }

    // 功能：复位整个流的帧序列与缓存，用于初次启动或检测到流重置时。
    void VideoShardsManager::reset_stream_state() {
        for (auto &frame_shards: cache_frames) {
            reset_frame_shards(frame_shards); // 逐槽释放分片内存并清零
        }
        fixed_frame_id_start = 0;
        max_frame_id = 0;
        is_current_frame_id_valid = false;
        current_frame_id = 0;
        last_complete_frame_id.reset();
        full_frame.clear();
    }

    // 功能：排空数据包队列，释放所有尚未处理的原始分片内存。
    void VideoShardsManager::drain_packet_queue() {
        std::pair<uint8_t *, size_t> packet;
        while (packet_queue.try_dequeue(packet)) {
            delete[] packet.first;
        }
    }

    // 功能：释放单帧槽位内全部分片内存并清零，使该槽位可复用。
    void VideoShardsManager::reset_frame_shards(FrameShards &frame_shards) {
        // 遍历有序分片映射，逐个释放裸指针
        for (const auto &item: frame_shards.shards | std::views::values) {
            delete[] item.first;
        }
        frame_shards.is_valid = false;
        frame_shards.frame_id = 0;
        frame_shards.frame_bytes = 0;
        frame_shards.collected_frame_bytes = 0;
        frame_shards.shards.clear();
    }

}// namespace RoboGo::video
