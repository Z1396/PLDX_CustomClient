// rgba_image_transform.h：RGBA 图像变换工具——对像素缓冲做 180° 旋转，用于翻转 UDP 图传画面的显示朝向。
#pragma once // 头文件只包含一次，防止重复定义

#include <pch.h> // 预编译头：引入 vector/uint8_t/std::size_t 等基础依赖

// 命名空间 RoboGo::video：业务功能层-视频模块的对外声明，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video { // 进入视频模块命名空间

// 功能：对 RGBA 像素缓冲做 180° 旋转（首尾对称交换像素），enabled 为 false 时原样返回。
// 参数：rgba 为宽高连续的 RGBA 数据（会被原地修改）；width/height 为图像尺寸；enabled 控制是否实际旋转。
// 返回：成功 true，参数非法或尺寸溢出返回 false。
bool ApplyRgba180Rotation(std::vector<uint8_t> &rgba, // 待原地翻转的 RGBA 像素缓冲（长度须为 width*height*4）
                          int width,                   // 图像宽度（像素），为负时视为非法
                          int height,                  // 图像高度（像素），为负时视为非法
                          bool enabled);               // 是否真正执行旋转，false 时仅做校验后直接返回

}// namespace RoboGo::video // 结束视频模块命名空间
