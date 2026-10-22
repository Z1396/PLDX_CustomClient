#pragma once

#include <pch.h>

// 应用层头文件：声明程序启动参数解析与顶层应用主循环的对外接口。
#include "app/application_types.h"
#include "platform/platform_backend.h"

namespace RoboGo::app {

// 功能：解析命令行参数为结构化的 LaunchOptions（含 MQTT 服务器地址与视频端口）。
// 参数 args：从平台收集到的命令行参数字符串列表。
// 返回：填充好默认值并解析出网络端点后的启动选项。
LaunchOptions ParseLaunchOptions(const std::vector<std::string> &args);

// 功能：运行应用主循环，统筹后台服务、渲染、视频纹理上传与键鼠上报。
// 参数 platform_backend：平台后端引用(图形/窗口)；options：解析后的启动选项。
// 返回：进程退出码(0 正常，非 0 异常)。
int RunApplication(platform::PlatformBackend &platform_backend, const LaunchOptions &options);

}// namespace RoboGo::app
