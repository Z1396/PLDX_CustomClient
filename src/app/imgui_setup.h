#pragma once  // 头文件包含守卫：防止重复包含

#include <pch.h>  // 预编译头(PCH)：集中包含常用标准库与第三方库

// ImGui 初始化模块：负责 ImGui 上下文、导航与样式缩放，以及内嵌中文字体的加载。
namespace RoboGo::app {  // 进入应用层命名空间

// 功能：创建 ImGui 上下文并配置通用设置(键盘/手柄导航、暗色主题、按 DPI 缩放)。
// 参数 main_scale：界面整体缩放系数(用于高 DPI 屏幕)。
void InitializeCommonImGui(float main_scale);

// 功能：向 ImGui 字体图集加载内嵌的 ZCOOL 中文字体，并关闭自动保存的 ini/日志文件。
void LoadEmbeddedImGuiFont();

}// namespace RoboGo::app
