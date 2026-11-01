// imgui_setup.cpp：ImGui 初始化——创建上下文、配置键鼠与手柄导航、应用暗色主题并按 DPI 缩放，以及字体加载与后端绑定。
#include "imgui_setup.h"  // 本编译单元的主头文件(声明 InitializeCommonImGui / LoadEmbeddedImGuiFont)

#include "embedded_zcool_font.h"  // 编译期嵌入的 ZCOOL 中文字体字节数组与其长度(避免运行时读外部字体文件)

namespace RoboGo::app {  // 进入应用层命名空间

// 功能：初始化 ImGui 的通用上下文与样式（导航、主题、DPI 缩放）。
// 思路：校验版本 -> 创建上下文 -> 开启键鼠/手柄导航 -> 暗色主题 -> 按 main_scale 缩放。
// 参数 main_scale：界面整体缩放系数，适配高 DPI 显示器。
void InitializeCommonImGui(const float main_scale) {
    IMGUI_CHECKVERSION(); // 校验当前链接的 Dear ImGui 版本与头文件是否匹配，不匹配将触发断言
    ImGui::CreateContext(); // 创建并绑定全局 ImGui 上下文
    ImGuiIO &io = ImGui::GetIO(); // 获取 IO 对象以配置输入与导航行为
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // 启用键盘导航
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad; // 启用手柄导航(适配操作手外设)

    ImGui::StyleColorsDark(); // 应用暗色主题样式

    ImGuiStyle &style = ImGui::GetStyle(); // 获取样式对象以便整体缩放
    style.ScaleAllSizes(main_scale); // 按比例缩放所有界面元素的尺寸
    style.FontScaleDpi = main_scale; // 设置字体 DPI 缩放系数
} // 结束 InitializeCommonImGui：上下文已就绪，后续由 LoadEmbeddedImGuiFont 装载字体

// 功能：加载内嵌的 ZCOOL 中文字体到 ImGui 字体图集，并关闭自动保存文件。
// 思路：将编译期嵌入的 TTF 字节直接交给 ImGui 内存字体加载接口，
//       字体数据由程序常量持有(不交由图集释放)，最后禁用 ini/日志的自动写入。
void LoadEmbeddedImGuiFont() {
    ImGuiIO &io = ImGui::GetIO(); // 获取 IO 对象以便操作字体图集与关闭自动保存文件
    ImGuiStyle &style = ImGui::GetStyle(); // 取全局样式对象引用，用于设定界面基础字号 FontSizeBase
    style.FontSizeBase = 14.0f; // 设置界面基础字号

    ImFontConfig font_config; // 字体加载配置结构
    font_config.FontDataOwnedByAtlas = false; // 字体数据由内嵌常量持有，不交由图集释放，避免重复释放
    ImFont *font = io.Fonts->AddFontFromMemoryTTF( // 从内存缓冲直接构建字体图集并返回 ImFont 句柄
            const_cast<unsigned char *>(embedded_zcool_font_data), // 内嵌字体数据指针(去掉 const 以适配 API)
            static_cast<int>(embedded_zcool_font_data_len), // 字体数据字节长度
            0.0f, // 字号 0 表示沿用默认/后续由缩放控制
            &font_config); // 传入配置：数据所有权不归图集，故释放时不会 free 内嵌常量内存
    IM_ASSERT(font != nullptr); // 断言字体加载成功，失败则中断程序

    io.IniFilename = nullptr; // 关闭 ImGui 布局 ini 持久化文件
    io.LogFilename = nullptr; // 关闭 ImGui 调试日志文件
} // 结束 LoadEmbeddedImGuiFont：字体已加入图集，交由后端在首帧前构建 GPU 纹理

}// namespace RoboGo::app
