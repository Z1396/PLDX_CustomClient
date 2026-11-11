// MQTT 下行消息分发：按 topic 查表调用对应处理函数，并统一收口 protobuf 解析日志。
#pragma once

#include <pch.h>
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]
#include "main_window.h"  // State 完整定义（各处理函数写入的目标）

namespace RoboGo::mqtt {

// 单个 topic 的处理函数签名。
// state   ：待刷新的全局状态；
// topic   ：话题名，仅用于日志（由分发表传入，保证与表项一致）；
// payload ：protobuf 原始字节串。
using MessageHandler = void (*)(State &state, std::string_view topic, const std::string &payload);

// 分发表条目。整张表必须按 topic 字典序排列，二分查找依赖该前提（有 static_assert 兜底）。
struct TopicEntry {
    std::string_view topic;   // 话题名，与裁判系统协议里的消息类型一致（如 "GameStatus"）
    MessageHandler handle;    // 该话题的处理函数
};

// 反序列化 payload 到 message，并统一输出成功/失败日志。
// 返回 true 表示解析成功，调用方可以安全地把 message 写入 State。
//
// 统一收口的意义：原来每个 topic 都要重复一遍「构造 → ParseFromString → 成功日志 → 赋值 → 失败日志」，
// 22 个分支里 16 个是同一个模式；抽成模板后每个处理函数只剩 2 行，新增 topic 也不会再抄错。
template<typename Message>
bool ParseInto(std::string_view topic, const std::string &payload, Message &message) {
    /*# `message.ParseFromString(payload)`
    这是 **Google Protobuf（Protocol Buffers）** 的反序列化 API（C++ / Python 版本），RM 电控 / 视觉通信最常用腾讯云
    > 作用：**把二进制字节流 payload 解析填充到这个 message 对象里**（二进制 → proto 对象）
    ### 对应关系
    - `SerializeToString()`：序列化，对象 → 二进制字节串（发出去）
    - `ParseFromString(payload)`：反序列化，二进制字节串 → 填充对象（收到包解析）
     */
    if (!message.ParseFromString(payload)) {
        SPDLOG_ERROR("MQTT消息解析失败 topic={} bytes={}", topic, payload.size());
        return false;
    }
    SPDLOG_DEBUG("MQTT消息解析成功 topic={}", topic);
    return true;
}

// 分发表中的全部话题，供 MQTT 订阅时使用。
// 这样「订阅什么」与「能处理什么」只有一份定义：新增协议消息只改表，不会再出现
// 「表里有、订阅漏了」导致消息永远收不到、界面也不报错的情况。
std::span<const TopicEntry> AllTopics();

// 按 topic 分发一条消息；未匹配到任何处理函数时输出 WARN。
// 该 WARN 是发现「协议新增话题」的唯一线索，不要删除。
void DispatchMqttMessage(State &state, const std::string &topic, const std::string &payload);

}  // namespace RoboGo::mqtt

