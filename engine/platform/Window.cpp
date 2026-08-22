#include "platform/Window.h"
#include "core/Log.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

namespace fe {

  //Esto es equivalente a un static a nivel de archivo
  namespace {

    void glfwErrorCallback(int error, const char* description) {
      FE_ERROR("GLFW Error (%d): %s", error, description);
    }

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
      FE_ERROR("glfwInit has failed");
      return false;
    }

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

    m_handle = glfwCreateWindow(spec.width, spec.height, spec.title.c_str(), nullptr, nullptr);
    if (!m_handle) {
      FE_ERROR("glfwCreateWindow has failed");
      return false;
    }

    glfwSetWindowUserPointer(m_handle, this);
    glfwSetFramebufferSizeCallback(m_handle, framebufferSizeCallback);
    glfwGetFramebufferSize(m_handle, &m_width, &m_height);

    FE_INFO("Window created: %dx%d", m_width, m_height);
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
    m_width = width;
    m_height = height;
    m_resized = true;
  }

  bool Window::consumeResizeFlag() {
    bool wasResized = m_resized;
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
