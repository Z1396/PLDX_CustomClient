#include "video_shards_manager.h" // 引入本文件的类声明

// 命名空间 RoboGo::video：业务功能层-视频模块的对外实现，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video {

    // 功能：析构时复位流状态并清空队列，确保分片占用内存被释放。
    VideoShardsManager::~VideoShardsManager() {
        reset_stream_state(); // 复位帧序列与缓存状态
        drain_packet_queue(); // 排空残留队列并释放内存
    } // 析构函数结束

    // 功能：保存完整帧回调，供重组完成后通知上层。
    void VideoShardsManager::set_frame_callback(const std::function<void(const std::vector<uint8_t> &frame)> &frame_callback) {
        this->frame_callback = frame_callback; // 保存完整帧就绪回调
        SPDLOG_INFO("已设置视频完整帧回调"); // 记录回调设置成功
    } // set_frame_callback 结束

    // 功能：把收到的一帧分片数据拷贝后入队，与解码线程解耦。
    // 思路：队列只持有裸指针+长度，需先 new 拷贝数据避免在 UDP 回调栈上悬空。
    void VideoShardsManager::on_receive_shard(const uint8_t *data, size_t size) {
        auto *packet = new uint8_t[size]; // 在堆上拷贝分片数据，避免 UDP 回调栈悬空
        memcpy(packet, data, size); // 拷贝原始分片字节
        packet_queue.emplace(packet, size); // 无锁入队，交给解码线程处理
    } // on_receive_shard 结束

    // 功能：处理一个分片包：背压控制、解析包头、维护帧 ID 序列、入窗重组并输出完整帧。
    // 思路：先按队列水位丢弃陈旧分片；再解析「帧ID/分片ID/整帧长度」头；用归一化帧 ID 计算相对窗口偏移，
    //       处理 16 位帧 ID 回绕、过期丢弃、整体丢帧冲刷；最后把分片放入窗口，凑齐即拼接回调并滑窗。
    void VideoShardsManager::handle_once() {
        // 如果数据包过多导致阻塞，就丢弃旧的数据包
        if (packet_queue.size_approx() >= 512) { // 队列积压过多则触发背压丢弃
            std::size_t trimmed_count = 0; // 统计丢弃数量
            std::pair<uint8_t *, size_t> stale_packet; // 出队暂存旧包
            // 仅保留最新 256 个，丢弃更旧的，避免内存无限增长与延迟累积
            while (packet_queue.size_approx() > 256 && packet_queue.try_dequeue(stale_packet)) { // 持续丢弃直到仅剩 256
                delete[] stale_packet.first; // 释放旧包数据内存
                ++trimmed_count; // 累加丢弃计数
            } // 背压丢弃循环结束
            if (trimmed_count > 0) { // 确有丢弃才告警
                SPDLOG_WARN("UDP 视频分片队列堆积，已丢弃旧 {} 个分片", trimmed_count); // 记录本批丢弃的分片数量
            } // 确有丢弃的告警分支结束
        } // 背压控制分支结束
        // 获取一个数据包
        std::pair<uint8_t *, size_t> packet; // 出队暂存一个包
        // 阻塞极短时间(1us)，既响应新数据又不过度占用 CPU
        if (!packet_queue.wait_dequeue_timed(packet, std::chrono::microseconds(1))) {
            return; // 超时无数据，返回等待下次
        } // 取包超时分支结束
        auto [data, size] = packet; // 解包出指针与长度
        // 解析包头：每片前 8 字节依次为 帧ID(2)/分片ID(2)/整帧字节数(4)，不足 8 字节视为无效
        if (size <= 8) [[unlikely]] { // 不足包头长度则无效
            SPDLOG_WARN("收到无效视频分片 bytes={}", size); // 记录无效分片（包头不足 8 字节）
            delete[] data; // 释放无效包内存
            return; // 丢弃无效包
        } // 无效包分支结束
        uint16_t frame_id = ntohs(*reinterpret_cast<const uint16_t *>(data));       // 网络字节序转主机，帧 ID
        uint16_t shard_id = ntohs(*reinterpret_cast<const uint16_t *>(data + 2));   // 分片序号
        uint32_t frame_bytes = ntohl(*reinterpret_cast<const uint32_t *>(data + 4)); // 整帧期望总字节数

        // 处理帧 ID 溢出问题：首次出现时以当前帧 ID 作为基准
        if (!is_current_frame_id_valid) { // 尚未建立基准
            is_current_frame_id_valid = true; // 标记基准已初始化
            current_frame_id = frame_id; // 以首个帧 ID 作基准
            SPDLOG_INFO("初始化视频帧序列 current_frame_id={}", current_frame_id); // 记录初始基准帧 ID
        } // 首次建立基准分支结束
        // 把 16 位帧 ID 归一到 32 位连续空间，便于计算相对偏移
        uint32_t fixed_frame_id = fixed_frame_id_start + frame_id; // 归一到 32 位连续空间
        int64_t delta_frame_id = static_cast<int64_t>(fixed_frame_id) - current_frame_id; // 相对基准的偏移
        // 帧 ID 大幅倒退且处于小值区，判定为 16 位计数器回绕，整体抬升基准
        if (delta_frame_id <= -100 && frame_id < cache_frame_size) { // 大幅倒退且小值，判定回绕
            fixed_frame_id = max_frame_id + 1; // 整体抬升到新基准之后
            fixed_frame_id_start = fixed_frame_id - frame_id; // 重建归一直偏移
            delta_frame_id = 1; // 归正为前进 1
            SPDLOG_INFO("视频帧ID发生回绕 frame_id={} fixed_frame_id={}", frame_id, fixed_frame_id); // 记录回绕修正后的归一幅值
        } // 帧 ID 回绕处理分支结束
        max_frame_id = std::max(max_frame_id, fixed_frame_id); // 更新最大帧 ID

        // 如果接收到过去的数据包，就丢掉
        if (delta_frame_id < 0) [[unlikely]] { // 收到旧帧
            // 倒退超过一个窗口，说明流断开重连或编码器重置，需要彻底复位帧序列
            if (delta_frame_id < -static_cast<int64_t>(cache_frame_size)) { // 倒退超窗口，视为断流
                // 流断连后重连或编码器重置，重置帧序列
                SPDLOG_WARN("视频流序列不连续，重置帧序列 old_current={} new_frame_id={}",
                            current_frame_id, frame_id); // 日志参数：重置前的基准帧 ID
                reset_stream_state(); // 彻底复位帧序列
                is_current_frame_id_valid = true; // 重建基准标志
                current_frame_id = frame_id; // 以重连帧 ID 作新基准
                fixed_frame_id = frame_id; // 归一直等于自身
                delta_frame_id = 0; // 偏移归零
            } else { // 轻微倒退：视为过期重传分片
                // 轻微倒退：仅仅是过期重传分片，直接丢弃避免破坏当前窗口
                SPDLOG_WARN("丢弃过期视频分片 frame_id={} shard_id={} current_frame_id={}",
                            fixed_frame_id, shard_id, current_frame_id); // 日志参数：当前基准帧 ID
                delete[] data; // 释放过期分片
                return; // 丢弃过期分片
            } // 过期分片丢弃分支结束
        } // 收到旧帧时的处理分支结束

        // 处理丢帧：当前帧远超窗口，说明中间大量帧丢失，向前滑动窗口丢弃旧帧
        if (delta_frame_id >= cache_frame_size) [[unlikely]] { // 远超窗口，中间大量丢帧
            SPDLOG_WARN("视频帧缓存被冲刷 delta_frame_id={} cache_frame_size={}", delta_frame_id, cache_frame_size); // 记录窗口冲刷事件与偏移量
            for (size_t i = 0; i < cache_frame_size; ++i) { // 滑动窗口丢弃旧帧
                // 把窗口内相对靠后的帧整体前移，腾出最新位置
                size_t target_index = i + delta_frame_id - cache_frame_size + 1; // 计算该旧帧应前移到的目标槽位
                if (target_index < cache_frame_size) { // 目标仍在窗口内
                    std::swap(cache_frames[i], cache_frames[target_index]); // 整体前移该帧
                } else { // 目标槽位已滑出窗口范围
                    // 目标已超出窗口范围，直接清空该旧帧槽位
                    reset_frame_shards(cache_frames[i]); // 超出窗口则清空该槽位
                } // 窗口外槽位清空分支结束
            } // 窗口滑动循环结束
            // 更新基准帧 ID 与相对偏移，使最新帧落在窗口末尾
            current_frame_id = fixed_frame_id - cache_frame_size + 1; // 更新基准为窗口头
            delta_frame_id = cache_frame_size - 1; // 最新帧落在窗口末
        } // 大量丢帧冲刷分支结束

        // 把数据包添加到缓存中：按相对偏移定位窗口中的帧槽位
        auto &current_frame_shards = cache_frames[delta_frame_id]; // 定位当前帧对应的窗口槽位
        // 首次写入该帧槽位：初始化其元数据
        if (!current_frame_shards.is_valid) [[unlikely]] { // 首次写入该帧槽位
            current_frame_shards.is_valid = true; // 标记槽位已初始化
            current_frame_shards.frame_id = fixed_frame_id; // 记录归一化帧 ID
            current_frame_shards.frame_bytes = frame_bytes; // 记录整帧期望字节数
        } else if (current_frame_shards.frame_bytes != frame_bytes) { // 整帧长度变化
            // 同一帧的整帧长度发生变化（异常），更新期望值
            SPDLOG_WARN("视频帧长度发生变化 frame_id={} old_bytes={} new_bytes={}",
                        fixed_frame_id, // 日志参数：归一化帧 ID
                        current_frame_shards.frame_bytes, // 日志参数：旧的期望整帧字节数
                        frame_bytes); // 日志参数：新的期望整帧字节数
            current_frame_shards.frame_bytes = frame_bytes; // 更新期望字节数
        } // 整帧长度变化处理分支结束
        // 若该分片已存在（重传），先释放旧数据并扣回已计字节数
        if (const auto iter = current_frame_shards.shards.find(shard_id); iter != current_frame_shards.shards.end()) [[unlikely]] { // 该分片已存在（重传）
            SPDLOG_WARN("收到重复视频分片 frame_id={} shard_id={} old_size={} new_size={}", // 记录重复分片（重传）事件
                        fixed_frame_id, // 日志参数：归一化帧 ID
                        shard_id, // 日志参数：分片序号
                        iter->second.second, // 日志参数：旧分片总长
                        size); // 日志参数：新分片总长
            delete[] iter->second.first; // 释放旧分片数据
            current_frame_shards.collected_frame_bytes -= static_cast<uint32_t>(iter->second.second - 8); // 扣回旧分片已计字节
        } // 重复分片处理分支结束
        // 累加本分片净负载（总长减去 8 字节头），并存入窗口
        current_frame_shards.collected_frame_bytes += size - 8; // 累加本分片净负载
        current_frame_shards.shards[shard_id] = {data, size}; // 存入分片到窗口

        // 处理完整帧：仅检查窗口头(cache_frames[0])是否凑齐
        auto &first_frame_shards = cache_frames[0]; // 取窗口头帧检查是否完整
        if (first_frame_shards.is_valid && first_frame_shards.frame_bytes == first_frame_shards.collected_frame_bytes) [[unlikely]] { // 窗口头已凑齐整帧
            // 检测跳帧：与上一次完整帧 ID 不连续即说明中间有丢帧
            if (last_complete_frame_id.has_value() && last_complete_frame_id.value() + 1 != first_frame_shards.frame_id) [[unlikely]] { // 检测是否跳帧
                SPDLOG_ERROR("skip frame：{} -> {}", last_complete_frame_id.value(), first_frame_shards.frame_id); // 记录跳帧
            } // 跳帧检测分支结束
            last_complete_frame_id = first_frame_shards.frame_id; // 更新上一完整帧 ID
            // 拼接：按分片 ID 顺序把各片净负载拷贝到 full_frame
            full_frame.clear(); // 清空待拼接缓冲
            full_frame.reserve(first_frame_shards.frame_bytes); // 预分配整帧容量
            bool is_success = true; // 拼接成功标志
            for (size_t j = 0; j < first_frame_shards.shards.size(); ++j) { // 按分片 ID 顺序拼接
                // 期望分片 ID 连续从 0 开始，缺失则拼接失败
                const auto &iter = first_frame_shards.shards.find(j); // 查找该分片 ID
                if (iter == first_frame_shards.shards.end()) [[unlikely]] { // 缺失该分片
                    is_success = false; // 标记拼接失败
                    SPDLOG_ERROR("分片ID错误"); // 记录分片缺失
                    break; // 中止拼接
                } // 分片缺失导致中止的分支结束
                auto &[shard_buffer, shard_size] = iter->second; // 解包分片缓冲与长度
                // 跳过每片前 8 字节头，仅拷贝净负载
                full_frame.insert(full_frame.end(), shard_buffer + 8, shard_buffer + shard_size); // 跳过 8 字节头拷贝净负载
            } // 分片拼接循环结束
            if (is_success) [[likely]] { // 拼接成功才回调
                if (frame_callback) { // 存在回调
                    frame_callback(full_frame); // 回调完整帧给上层（解码器）
                } else { // 无回调
                    SPDLOG_WARN("视频完整帧已生成但未设置回调 frame_id={}", first_frame_shards.frame_id); // 记录完整帧无处投递（未设置回调）
                } // 无回调分支结束
            } // 拼接成功回调分支结束
            // 在缓存中清理当前帧：整体前移窗口并清空末尾槽位
            for (size_t j = 0; j < cache_frame_size - 1; ++j) { // 窗口整体前移
                std::swap(cache_frames[j], cache_frames[j + 1]); // 相邻帧槽前移
            } // 窗口前移循环结束
            reset_frame_shards(cache_frames[cache_frame_size - 1]); // 清空末尾旧槽位
            current_frame_id += 1; // 窗口前进一帧
        } // 完整帧处理分支结束
    } // handle_once 结束

    // 功能：复位整个流的帧序列与缓存，用于初次启动或检测到流重置时。
    void VideoShardsManager::reset_stream_state() {
        for (auto &frame_shards: cache_frames) { // 逐个槽位释放分片内存并清零
            reset_frame_shards(frame_shards); // 逐槽释放分片内存并清零
        } // 帧槽位遍历结束
        fixed_frame_id_start = 0; // 复位归一直偏移基准
        max_frame_id = 0; // 复位最大帧 ID
        is_current_frame_id_valid = false; // 清除基准有效标志
        current_frame_id = 0; // 复位当前基准帧 ID
        last_complete_frame_id.reset(); // 清除上一完整帧记录
        full_frame.clear(); // 清空完整帧缓冲
    } // reset_stream_state 结束

    // 功能：排空数据包队列，释放所有尚未处理的原始分片内存。
    void VideoShardsManager::drain_packet_queue() {
        std::pair<uint8_t *, size_t> packet; // 出队暂存
        while (packet_queue.try_dequeue(packet)) { // 持续出队直到空
            delete[] packet.first; // 释放分片内存
        } // 队列排空循环结束
    } // drain_packet_queue 结束

    // 功能：释放单帧槽位内全部分片内存并清零，使该槽位可复用。
    void VideoShardsManager::reset_frame_shards(FrameShards &frame_shards) {
        // 遍历有序分片映射，逐个释放裸指针
        for (const auto &item: frame_shards.shards | std::views::values) { // 遍历有序分片映射（取 value）
            delete[] item.first; // 释放分片裸指针
        } // 分片映射遍历结束
        frame_shards.is_valid = false; // 标记槽位失效
        frame_shards.frame_id = 0; // 清零帧 ID
        frame_shards.frame_bytes = 0; // 清零期望字节数
        frame_shards.collected_frame_bytes = 0; // 清零已收集字节数
        frame_shards.shards.clear(); // 清空分片映射
    } // reset_frame_shards 结束

}// namespace RoboGo::video
