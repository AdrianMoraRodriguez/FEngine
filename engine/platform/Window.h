/**
 * @file Window.h
 * @brief Operating system window and presentation surface.
 *
 * @see Window
 */

#pragma once

#include <string>

struct GLFWwindow;

namespace fe {

  /**
   * @brief Configuration for creating a window.
   *
   * All fields have reasonable default values, so that
   * `WindowSpec{}` produces a valid window without any further configuration.
   */
  struct WindowSpec {
    int         width  = 1600;  ///< Initial width of the client area, in pixels.
    int         height = 900;   ///< Initial height of the client area, in pixels.
    std::string title  = "FEngine Editor";  ///< Text of the title bar.
  };

  /**
   * @brief Window of the operating system and presentation surface.
   *
   * Wraps a GLFW window created without a graphics context
   * (`GLFW_CLIENT_API = GLFW_NO_API`), since in Vulkan the presentation surface
   * is created separately using `vkCreateSurfaceKHR`.
   *
   * The class is not copyable: it owns a system resource with unique ownership semantics.
   *
   * @note Only one instance should exist. `init()` calls `glfwInit()`
   *       and `shutdown()` calls `glfwTerminate()`, which are global functions.
   *
   * Typical usage:
   * @code
   * fe::Window window;
   * if (!window.init(fe::WindowSpec{})) {
   *     return 1;
   * }
   *
   * while (!window.shouldClose()) {
   *     window.pollEvents();
   * }
   * @endcode
   */
  class Window {
  public:
    Window() = default;
    /// Destroys the window if it is still open. Equivalent to calling shutdown().
    ~Window();
    Window(const Window&)            = delete;
    Window& operator=(const Window&) = delete;

    /**
     * @brief Initializes GLFW and creates the window.
     *
     * @param spec Desired dimensions and title.
     * @return `true` if the window was created successfully.
     *
     * In case of failure, the error is logged using FE_ERROR and the object
     * remains in the same state as before the call, so it is
     * safe to destroy it.
     */
    bool init(const WindowSpec& spec);

    /**
     * @brief Destroys the window and terminates GLFW.
     *
     * It is safe to call it multiple times or without having called init().
     */
    void shutdown();

    /// @return `true` if the user has requested to close the window.
    bool shouldClose() const;

    /// Processes the pending system events. Call once per frame.
    void pollEvents();

    /**
     * @brief Consults and clears the resize flag.
     *
     * The flag is set when the framebuffer size changes. The
     * consultation clears it, ensuring each resize is handled
     * exactly once.
     *
     * @return `true` if a resize occurred since the last call.
     *
     * @see isMinimized()
     */
    bool consumeResizeFlag();

    /**
     * @brief Indicates if the window is minimized.
     *
     * With the window minimized the framebuffer measures 0×0. Vulkan does not support
     * presenting on a swapchain of zero size, so in that
     * state the frame must be omitted.
     *
     * @return `true` if the width or height is zero.
     */
    bool isMinimized() const { return m_width == 0 || m_height == 0; }

    /// @return Current framebuffer width, in pixels.
    int width() const { return m_width; }

    /// @return Current framebuffer height, in pixels.
    int height() const { return m_height; }

    /// @return Native GLFW pointer, or `nullptr` if not initialized.
    GLFWwindow* handle() const { return m_handle; }

    /**
     * @brief Instance extensions required to create the surface.
     *
     * @param[out] outCount Number of extensions returned.
     * @return Array of names, a GLFW property. Must not be freed.
     *
     * @warning Only valid following a successful call to init().
     */
    static const char** requiredInstanceExtensions(unsigned int* outCount);

    /**
     * @brief Updates the dimensions and activates the resize notification.
     *
     * @param width  New width of the framebuffer.
     * @param height New height of the framebuffer.
     *
     * @internal Invoked by the GLFW callback. Do not call directly.
     */
    void onFramebufferResized(int width, int height);

  private:
      GLFWwindow* m_handle = nullptr;

      int  m_width   = 0;
      int  m_height  = 0;
      bool m_resized = false;
  };

}
