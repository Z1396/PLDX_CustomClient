// 【RoboGo 资源加载实现】从 CMake 内嵌的字节数组(地图 PNG 等)解码为可用图像资源。
#define STB_IMAGE_IMPLEMENTATION   // 在本编译单元中展开 stb_image 的函数实现(仅在此 .cpp 定义一次，避免多重定义)
#if defined(__clang__)  // Clang 分支：临时关闭告警
#pragma clang diagnostic push  // 保存当前告警状态，便于后面 pop 还原
#pragma clang diagnostic ignored "-Wunused-function"  // stb 内部有未使用的静态函数，临时屏蔽该告警
#elif defined(__GNUC__)  // GCC 分支：同样的告警抑制
#pragma GCC diagnostic push  // 保存当前告警状态
#pragma GCC diagnostic ignored "-Wunused-function"  // 屏蔽未使用函数告警
#endif
#include <pch.h>   // 引入预编译头：集中包含 STL、spdlog、imgui、stb 等公共依赖
// [pch-split] 本模块显式依赖（原由全局 pch.h 隐式提供）
#define STBI_ONLY_PNG  // 仅编译 PNG 解码器，剔除其它格式以减小体积
#include <stb_image.h>
// [/pch-split]
#if defined(__clang__)  // Clang 分支：还原告警状态
#pragma clang diagnostic pop  // 恢复之前保存的告警设置
#elif defined(__GNUC__)  // GCC 分支：还原告警状态
#pragma GCC diagnostic pop  // 恢复之前保存的告警设置
#endif
#include "resource/resources.h"   // 引入本模块头文件：DecodedImage 结构与加载函数声明
#include "embedded_map_png.h"      // 引入 CMake 生成的头文件：内嵌小地图 PNG 字节数组与其长度
#include "embedded_map_terrain_png.h"  // 内嵌地形分层叠加层 PNG（带 alpha）

namespace RoboGo::resource {   // 进入资源命名空间，集中管理内嵌资源的加载逻辑

/**
 * @brief 从内存中的编码字节流(如 PNG)解码出像素图像。
 * @note 思路：借助 stb_image 的 stbi_load_from_memory 将压缩图像解码为 RGBA8 像素；先做空数据校验，再解码，失败则记录日志并返回 unexpected(带原因)
 *        ，成功则拷贝像素到 DecodedImage 并返回。
 * @note 参数：encoded_bytes 为原始编码字节(只读视图)；resource_name 仅用于日志显示的资源名。
 * @return 解码成功返回含宽高与像素的 DecodedImage，失败返回 unexpected(ImageDecodeError)。
 */
std::expected<DecodedImage, ImageDecodeError> LoadEmbeddedImage(const std::span<const uint8_t> encoded_bytes,  // 编码后的图像字节(PNG/JPG 等)
                                              const std::string_view resource_name) {  // 资源名，仅用于日志输出
    const std::string_view display_name = resource_name.empty() ? "内嵌资源" : resource_name;  // 资源名为空时给出默认显示名，便于日志定位
    if (encoded_bytes.empty()) {   // 防御性校验：传入字节为空无法解码
        SPDLOG_ERROR("{}解码失败 reason=empty resource bytes", display_name);   // 记录错误日志
        return std::unexpected(ImageDecodeError::EmptyBytes);   // 带原因的解码失败
    }

    int width = 0;       // 解码后的图像宽度
    int height = 0;      // 解码后的图像高度
    int channels_in_file = 0;   // 准备接收解码后的图像尺寸与文件实际通道数
    /*`stbi_load_from_memory`：stb_image 提供的内存版解码接口，不是从文件读，是直接拿已经在内存里的图片字节（比如网络下载、资源包里读出来的 `encoded_bytes`）解码。

    - 入参：图片数据指针、数据长度
    - 输出参数：`width/height/channels_in_file` 带出原图宽高、原图通道数
    - 最后参数 `STBI_rgb_alpha`：强制输出 4 通道 RGBA，不管原图是 RGB 3 通道还是带透明的 PNG，统一转 RGBA，GPU 纹理上传逻辑就不用写多分支。
    - 返回值：stb 内部 `malloc` 出来的一块像素缓冲区，必须手动 stbi_image_free 释放，否则内存泄漏；失败返回 `nullptr`。
     */
    stbi_uc *decoded_pixels = stbi_load_from_memory(  
            encoded_bytes.data(),         // 指向编码字节流的指针
            static_cast<int>(encoded_bytes.size()),   // 编码字节流的字节数
            &width,   // 接收解码后的图像宽度
            &height,   // 接收解码后的图像高度
            &channels_in_file,   // 输出参数：文件中的原始通道数
            STBI_rgb_alpha);   // 传递 STBI_rgb_alpha 作为参数，强制解码为 RGBA(4 通道)，统一像素格式便于后续上传 GPU 纹理
    if (decoded_pixels == nullptr) {   // stb 解码失败会返回空指针
        const char *failure_reason = stbi_failure_reason();   // 获取 stb 内部失败原因字符串
        SPDLOG_ERROR("{}解码失败 reason={}", display_name, failure_reason != nullptr ? failure_reason : "unknown");   // 记录失败原因(空则标记 unknown)
        return std::unexpected(ImageDecodeError::StbDecodeFailed);   // 带原因的解码失败
    }

    DecodedImage image;   // 构造返回用的图像结构
    image.width = width;  // 赋值解码后的图像宽度
    image.height = height;    // 赋值解码后的图像高度
    const std::size_t pixel_count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4u;   // 计算总像素字节数 = 宽 * 高 * 4(RGBA)
    //`.assign(起始指针, 结束指针)` 是 vector 的一个成员函数：把 [a, b) 这一段内存里的数据，复制进来，替换掉 vector 原来里面所有内容。
    // 区间是左闭右开：包含 `decoded_pixels` 指向的那个字节，**不包含** `decoded_pixels + pixel_count` 这个地址。刚好就是我们全部像素的范围。
    image.pixels.assign(decoded_pixels, decoded_pixels + pixel_count);   // 将 stb 分配的像素拷贝进 vector 托管，便于自动释放
    stbi_image_free(decoded_pixels);   // 释放 stb 内部 malloc 的内存，避免泄漏

    SPDLOG_INFO("{}已解码 width={} height={} channels={}", display_name, width, height, channels_in_file);   // 输出解码成功信息，便于调试
    return image;   // 返回解码完成的图像
}

/**
 * @brief 加载内嵌的小地图 PNG 图像。
 * @note 思路：把 CMake 生成的内嵌数组 embedded_map_png_data/len 包装为 span，转交给通用解码函数处理。
 */
std::expected<DecodedImage, ImageDecodeError> LoadEmbeddedMapImage() {  // 无参数：数据在编译期已经内嵌进可执行文件
    return LoadEmbeddedImage(  // 直接返回通用解码结果
            std::span<const uint8_t>(embedded_map_png_data, static_cast<std::size_t>(embedded_map_png_data_len)),   // 用内嵌字节数组与其长度构造只读 span
            "内嵌小地图");   // 指定资源显示名为"内嵌小地图"
}

/**
 * @brief 加载内嵌的地形分层叠加层（2027 高度掩膜上色版）。
 * @note 思路：与底图相同，只是换一份内嵌数组；图像本身带 alpha，地面区域完全透明。
 */
std::expected<DecodedImage, ImageDecodeError> LoadEmbeddedMapTerrainImage() {
    return LoadEmbeddedImage(
            std::span<const uint8_t>(embedded_map_terrain_png_data, static_cast<std::size_t>(embedded_map_terrain_png_data_len)),
            "内嵌地形分层");
}

}  // namespace RoboGo::resource

