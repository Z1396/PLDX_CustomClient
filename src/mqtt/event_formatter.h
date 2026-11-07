// Event 消息的中文文案翻译与顶部提示入队：不依赖 ImGui，可单独测试。
#pragma once

#include <pch.h>
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]
#include "main_window.h"

namespace RoboGo::mqtt {

// 把协议 Event 的 (event_id, param) 翻译成一条人类可读的中文描述。
// 未识别的事件返回 "未知事件: ID=.., 参数=.."，便于后续按日志补全处理。
std::string FormatEventText(const Event &event);

// 把重要事件推进顶部提示队列（纯日志事件不会入队）。
// 级别由 event_id 决定：0 普通 / 1 重要 / 2 危急；其余事件只写日志、不弹提示。
void PushEventToast(State &state, uint32_t event_id, const std::string &text);

}  // namespace RoboGo::mqtt
