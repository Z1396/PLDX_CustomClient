// resources.h：内嵌资源加载接口——把 CMake 阶段 hex 化进二进制的地图 PNG 等资源解码为 RGBA 图像，供 UI 直接使用。
#pragma once   // 头文件保护，防止重复包含

#include <pch.h>   // 引入预编译头，获得 vector/optional/span/string_view 等基础类型
#include <expected>  // std::expected / std::unexpected（C++23）

namespace RoboGo::resource {   // 资源相关声明的命名空间

// 图片解码失败的具体原因：调用方据此区分「资源本身为空」与「stb 解码失败」。
enum class ImageDecodeError {
    EmptyBytes,
    StbDecodeFailed,
};

[[nodiscard]] inline constexpr const char *ToString(const ImageDecodeError error) noexcept {
    switch (error) {
        case ImageDecodeError::EmptyBytes:      return "empty resource bytes";
        case ImageDecodeError::StbDecodeFailed: return "stb decode failed";
    }
    return "unknown";
}

// 解码后的图像数据：保存像素缓冲与尺寸，供后续上传 GPU 纹理或 CPU 端处理使用。
struct DecodedImage {
    std::vector<uint8_t> pixels;   // RGBA8 像素缓冲(长度 = width * height * 4)
    int width = 0;   // 图像宽度(像素)
    int height = 0;   // 图像高度(像素)
};

/**
 * @brief 从内存编码字节(如 PNG)解码出图像。
 * @note 思路：见 resources.cpp 中的实现；resource_name 仅用于日志显示。
 */
std::expected<DecodedImage, ImageDecodeError> LoadEmbeddedImage(std::span<const uint8_t> encoded_bytes,
                                                                std::string_view resource_name = {});  // 默认资源名为空

/**
 * @brief 加载内嵌小地图 PNG 的便捷封装。
 * @note 思路：内部读取 CMake 生成的内嵌数组并调用 LoadEmbeddedImage。
 */
std::expected<DecodedImage, ImageDecodeError> LoadEmbeddedMapImage();

/**
 * @brief 加载内嵌的「地形分层叠加层」PNG（2027 场地高度掩膜上色版，地面透明）。
 * @note 说明：小地图底图上叠这一层就能区分高地/道路，按 M 键开关。
 */
std::expected<DecodedImage, ImageDecodeError> LoadEmbeddedMapTerrainImage();

}  // namespace RoboGo::resource
