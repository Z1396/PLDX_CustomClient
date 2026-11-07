// 自定义控制包（CustomControl）下发：把敌方血量压缩成 13 字节，按固定周期发给机器人电控。
#pragma once

#include <pch.h>
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#include <google/protobuf/message.h>
#include <messages.pb.h>
// [/pch-split]

#include "io/mqtt_client.h"   // MqttClient：发布 CustomControl 话题
#include "main_window.h"      // State：读取全局单位状态与节流时间戳

// 在到达发布周期时构造并下发 CustomControl；未到周期或数据不足时静默返回。
// 由主循环每帧调用，函数内部自行节流（默认 100ms）。
void PublishCustomControlIfDue(State &state, RoboGo::io::MqttClient &mqtt_client);
