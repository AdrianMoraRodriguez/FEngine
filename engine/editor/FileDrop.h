#pragma once

/**
 * @file FileDrop.h
 * @brief Collects the files and folders the user drops on the editor window
 *        from the operating system's file explorer.
 */

#include <filesystem>
#include <vector>

struct GLFWwindow;

namespace fe {

/**
 * @brief Receives GLFW's drop events and queues them for the editor.
 *
 * GLFW calls its drop callback from inside `glfwPollEvents()` on the main
 * thread, which is also where the queue is read, so no locking is needed.
 * The mouse position of the drop is not stored here: ImGui already tracks
 * the cursor, and the panel that receives the files asks it.
 */
class FileDrop {
public:
    /// Starts listening on @p window. Call once, after the window exists.
    static void install(GLFWwindow* window);

    /**
     * @brief Returns the paths dropped since the last call, and forgets them.
     *
     * One drop of several items arrives together, in the order the OS gave.
     * Empty when nothing was dropped.
     */
    static std::vector<std::filesystem::path> take();
};

} // namespace fe
