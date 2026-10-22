#pragma once

#include <pch.h>

namespace RoboGo::video {

// 功能：对 RGBA 像素缓冲做 180° 旋转（首尾对称交换像素），enabled 为 false 时原样返回。
// 参数：rgba 为宽高连续的 RGBA 数据（会被原地修改）；width/height 为图像尺寸；enabled 控制是否实际旋转。
// 返回：成功 true，参数非法或尺寸溢出返回 false。
bool ApplyRgba180Rotation(std::vector<uint8_t> &rgba,
                          int width,
                          int height,
                          bool enabled);

}// namespace RoboGo::video
