#include "editor/FileDrop.h"

#include <GLFW/glfw3.h>

#include <string>

namespace fe {
namespace {

std::vector<std::filesystem::path> g_dropped;

void onDrop(GLFWwindow*, int count, const char** paths) {
    for (int i = 0; i < count; ++i) {
        // GLFW hands over UTF-8 on every platform. Going through u8string
        // keeps names with accents, CJK characters, etc. intact on Windows.
        g_dropped.emplace_back(std::u8string(reinterpret_cast<const char8_t*>(paths[i])));
    }
}

} // namespace

void FileDrop::install(GLFWwindow* window) {
    glfwSetDropCallback(window, onDrop);
}

std::vector<std::filesystem::path> FileDrop::take() {
    std::vector<std::filesystem::path> result;
    result.swap(g_dropped);
    return result;
}

} // namespace fe
