#include "rgba_image_transform.h"

namespace RoboGo::video {

// 功能：对 RGBA 缓冲执行 180° 旋转，并对输入尺寸做防溢出校验。
// 思路：先做边界与尺寸校验；随后以首尾像素为中心对称交换，四个通道逐一 swap 实现整图翻转。
bool ApplyRgba180Rotation(std::vector<uint8_t> &rgba,
                          const int width,
                          const int height,
                          const bool enabled) {
    // 尺寸不可为负
    if (width < 0 || height < 0) {
        return false;
    }

    const auto width_size = static_cast<std::size_t>(width);
    const auto height_size = static_cast<std::size_t>(height);
    constexpr std::size_t channels = 4; // RGBA 四通道
    // 防止 width*height 乘法溢出 size_t
    if (height_size != 0 && width_size > std::numeric_limits<std::size_t>::max() / height_size) {
        return false;
    }
    const std::size_t pixel_count = width_size * height_size;
    // 防止像素数*通道数溢出，且缓冲长度必须与预期完全一致
    if (pixel_count > std::numeric_limits<std::size_t>::max() / channels
        || rgba.size() != pixel_count * channels) {
        return false;
    }
    // 未启用或像素不足两个则无需处理，直接成功返回
    if (!enabled || pixel_count < 2) {
        return true;
    }

    // 双指针从两端向中间逼近，逐像素交换实现 180° 旋转
    for (std::size_t left = 0, right = pixel_count - 1; left < right; ++left, --right) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            // 交换对称位置像素的对应通道值
            std::swap(rgba[left * channels + channel], rgba[right * channels + channel]);
        }
    }
    return true;
}

}// namespace RoboGo::video
