#pragma once

#include <string>

struct GLFWwindow;

namespace fe {

  struct WindowSpec {
    int width = 1600;
    int height = 900;
    std::string title = "FEngine Editor";
  };

  class Window {
   public:
    Window() = default;
    ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    bool init(const WindowSpec& spec);
    void shutdown();

    bool shouldClose() const;
    void pollEvents();

    // Devuelve true si el framebuffer cambió de tamaño desde la última consulta y limpia el aviso. El render lo usará para recrear la swapchain.
    bool consumeResizeFlag();

    bool isMinimized() const {return m_width == 0 || m_height == 0;}

    int width() const {return m_width;}
    int height() const {return m_height;}

    GLFWwindow* handle() const {return m_handle;}

    // Extensiones de instancia que GLFW necesita para crear la ventana.
    static const char** requiredInstanceExtensions(unsigned int* outCount);
    // Uso interno
    void onFramebufferResized(int width, int height);

   private:
    GLFWwindow* m_handle = nullptr;
    int m_width = 0;
    int m_height = 0;
    bool m_resized = false;
  };
}
