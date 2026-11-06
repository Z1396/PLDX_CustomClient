// rgba_image_transform.cpp：RGBA 图像 180° 旋转实现——以首尾对称交换的方式原地翻转像素顺序。
#include "rgba_image_transform.h" // 引入本文件对应的接口声明

// 命名空间 RoboGo::video：业务功能层-视频模块的对外实现，集中放置视频解码与管线相关类型与函数。
namespace RoboGo::video { // 进入视频模块命名空间

// 功能：对 RGBA 缓冲执行 180° 旋转，并对输入尺寸做防溢出校验。
// 思路：先做边界与尺寸校验；随后以首尾像素为中心对称交换，四个通道逐一 swap 实现整图翻转。
bool ApplyRgba180Rotation(std::vector<uint8_t> &rgba, // 待原地翻转的 RGBA 像素缓冲（长度须为 width*height*4）
                          const int width,            // 图像宽度（像素）
                          const int height,           // 图像高度（像素）
                          const bool enabled) {       // 是否真正执行旋转，false 时校验通过即返回
    // 尺寸不可为负
    if (width < 0 || height < 0) { // 尺寸不可为负
        return false; // 参数非法，拒绝
    }

    const auto width_size = static_cast<std::size_t>(width); // 转无符号便于乘法与比较
    const auto height_size = static_cast<std::size_t>(height); // 转无符号便于乘法与比较
    constexpr std::size_t channels = 4; // RGBA 四通道
    // 防止 width*height 乘法溢出 size_t
    if (height_size != 0 && width_size > std::numeric_limits<std::size_t>::max() / height_size) { // 检测乘法溢出风险
        return false; // 溢出风险，拒绝
    }
    const std::size_t pixel_count = width_size * height_size; // 计算单帧总像素数
    // 防止像素数*通道数溢出，且缓冲长度必须与预期完全一致
    if (pixel_count > std::numeric_limits<std::size_t>::max() / channels // 像素总数乘 4 是否仍能放进 size_t
        || rgba.size() != pixel_count * channels) { // 防止像素*通道溢出且缓冲长度须精确匹配
        return false; // 像素数溢出或缓冲长度不符，拒绝
    }
    // 未启用或像素不足两个则无需处理，直接成功返回
    if (!enabled || pixel_count < 2) { // 未启用或像素不足两个则无需旋转
        return true; // 原样返回成功
    }

    // 双指针从两端向中间逼近，逐像素交换实现 180° 旋转
    for (std::size_t left = 0, right = pixel_count - 1; left < right; ++left, --right) { // 双指针从两端向中间逼近
        for (std::size_t channel = 0; channel < channels; ++channel) { // 逐通道处理
            // 交换对称位置像素的对应通道值
            std::swap(rgba[left * channels + channel], rgba[right * channels + channel]); // 交换对称像素的对应通道值
        } // 四个通道交换完毕
    } // 双指针相遇，整图翻转完成
    return true; // 180° 旋转完成
}

}  // namespace RoboGo::video
