/**
 * @file main.cpp
 * @brief Entry point of the FEngine editor.
 */

#include "core/Log.h"
#include "core/Version.h"
#include "platform/Window.h"

int main() {
  FE_INFO("%s", fe::engineVersionString());
  fe::Window window;
  if (!window.init(fe::WindowSpec{})) {
    return 1;
  }
  unsigned int extensionCount = 0;
  const char** extensions = fe::Window::requiredInstanceExtensions(&extensionCount);
  FE_INFO("GLFW requires %u Vulkan instance extension(s):", extensionCount);
  for (unsigned int i = 0; i < extensionCount; ++i) {
    FE_INFO("  - %s", extensions[i]);
  }
  while (!window.shouldClose()) {
    window.pollEvents();
    if (window.consumeResizeFlag()) {
      FE_TRACE("Framebuffer resized to %d x %d", window.width(), window.height());
    }
  }
  FE_INFO("Shutting down");
  return 0;
}
