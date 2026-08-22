#include "platform/Window.h"

#include "core/Log.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace fe {
  namespace {

    /// Reports GLFW errors through the engine log.
    void glfwErrorCallback(int code, const char* description) {
      FE_ERROR("GLFW (%d): %s", code, description);
    }
  
    /// Forwards framebuffer resizes to the owning Window instance.
    ///
    /// GLFW callbacks are free functions with no `this` pointer, so the instance
    /// is recovered from the user pointer stored during initialisation.
    void framebufferSizeCallback(GLFWwindow* handle, int width, int height) {
      auto* self = static_cast<Window*>(glfwGetWindowUserPointer(handle));
      if (self) {
        self->onFramebufferResized(width, height);
      }
    }
  }

  Window::~Window() {
    shutdown();
  }

  bool Window::init(const WindowSpec& spec) {
    glfwSetErrorCallback(glfwErrorCallback);

    if (!glfwInit()) {
      FE_ERROR("Failed to initialise GLFW");
      return false;
    }

    // Tell GLFW not to create an OpenGL context: Vulkan creates its drawing
    // surface separately.
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    m_handle = glfwCreateWindow(spec.width, spec.height, spec.title.c_str(),
                                nullptr, nullptr);
    if (!m_handle) {
      FE_ERROR("Failed to create window");
      glfwTerminate();
      return false;
    }

    glfwSetWindowUserPointer(m_handle, this);
    glfwSetFramebufferSizeCallback(m_handle, framebufferSizeCallback);
    glfwGetFramebufferSize(m_handle, &m_width, &m_height);

    FE_INFO("Window created: %d x %d", m_width, m_height);
    return true;
  }

  void Window::shutdown() {
    if (m_handle) {
      glfwDestroyWindow(m_handle);
      m_handle = nullptr;
      glfwTerminate();
    }
  }

  void Window::onFramebufferResized(int width, int height) {
    m_width   = width;
    m_height  = height;
    m_resized = true;
  }

  bool Window::consumeResizeFlag() {
    const bool wasResized = m_resized;
    m_resized = false;
    return wasResized;
  }

  bool Window::shouldClose() const {
    return m_handle ? glfwWindowShouldClose(m_handle) : true;
  }

  void Window::pollEvents() {
    glfwPollEvents();
  }

  const char** Window::requiredInstanceExtensions(unsigned int* outCount) {
    return glfwGetRequiredInstanceExtensions(outCount);
  }

}
