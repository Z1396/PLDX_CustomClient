// rgba_image_transform.h：RGBA 图像变换工具——对像素缓冲做 180° 旋转，用于翻转 UDP 图传画面的显示朝向。
#pragma once

#include <pch.h>

// 命名空间 RoboGo::video：业务功能层-视频模块的对外声明，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video {

// 功能：对 RGBA 像素缓冲做 180° 旋转（首尾对称交换像素），enabled 为 false 时原样返回。
// 参数：rgba 为宽高连续的 RGBA 数据（会被原地修改）；width/height 为图像尺寸；enabled 控制是否实际旋转。
// 返回：成功 true，参数非法或尺寸溢出返回 false。
bool ApplyRgba180Rotation(std::vector<uint8_t> &rgba,
                          int width,
                          int height,
                          bool enabled);

}// namespace RoboGo::video
