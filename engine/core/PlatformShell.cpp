#include "core/PlatformShell.h"

#include "core/Log.h"
#include "core/PathUtil.h"

#if defined(_WIN32)
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <shellapi.h>
#else
    #include <csignal>
    #include <sys/wait.h>
    #include <unistd.h>
#endif

namespace fe {

void revealInFileManager(const std::filesystem::path& path) {
    std::error_code ec;
    const bool isDir = std::filesystem::is_directory(path, ec);

#if defined(_WIN32)
    // explorer.exe /select,"<file>" opens the folder with the file selected.
    // The path goes inside quotes, and Windows paths cannot contain quotes.
    const std::wstring arguments = isDir
        ? L"\"" + path.wstring() + L"\""
        : L"/select,\"" + path.wstring() + L"\"";
    const auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr, SW_SHOWNORMAL));
    if (result <= 32)
        FE_WARN("PlatformShell: could not open the file explorer for '%s'", toUtf8(path).c_str());
#else
    // fork + exec, with the path as a separate argument: no shell is involved,
    // so no character in the name can be misread as a command.
    #if defined(__APPLE__)
        const char* program = "open";
        const std::string target = toUtf8(path);
        const char* const args[] = {program, isDir ? target.c_str() : "-R",
                                    isDir ? nullptr : target.c_str(), nullptr};
    #else
        const char* program = "xdg-open";
        const std::string target = toUtf8(isDir ? path : path.parent_path());
        const char* const args[] = {program, target.c_str(), nullptr};
    #endif

    const pid_t pid = fork();
    if (pid == 0) {
        // Child: become the file manager.
        execvp(program, const_cast<char* const*>(args));
        _exit(127);   // exec failed
    } else if (pid < 0) {
        FE_WARN("PlatformShell: could not start '%s'", program);
    } else {
        // Ignoring SIGCHLD lets the OS clean the child up when it ends, so
        // it never lingers as a zombie and the editor never has to wait for it.
        static bool ignored = false;
        if (!ignored) { std::signal(SIGCHLD, SIG_IGN); ignored = true; }
    }
#endif
}

} // namespace fe
