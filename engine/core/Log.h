/**
 * @file Log.h
 * @brief Minimal logging facility.
 *
 * Logging is exposed through the @ref FE_TRACE, @ref FE_INFO, @ref FE_WARN
 * and @ref FE_ERROR macros rather than through direct calls to log(). Using
 * the macros keeps call sites stable: the implementation behind them can be
 * replaced without touching engine code.
 *
 * The current implementation writes to the standard streams and is
 * deliberately simple. Level filtering, colour output and routing to an
 * editor panel are expected to replace it later.
 *
 * @warning Not thread-safe. Concurrent calls may interleave their output.
 */
#pragma once
#include <cstdio>

namespace fe {

  /**
   * @brief Severity of a log message.
   *
   * Ordered from least to most severe. Messages at @ref LogLevel::Warn and
   * above are written to `stderr`; the rest go to `stdout`.
   */
  enum class LogLevel {
    Trace,  ///< Verbose diagnostics useful only while debugging.
    Info,   ///< Normal operation: lifecycle events, detected hardware.
    Warn,   ///< Recoverable problems that may indicate a defect.
    Error   ///< Failures that prevent an operation from completing.
  };

  /**
   * @brief Returns the fixed-width display name of a severity level.
   *
   * All returned names are padded to five characters so that log output
   * stays column-aligned.
   *
   * @param level The severity to name.
   * @return A null-terminated string with static storage duration.
   */
  inline const char* logLevelName(LogLevel level) {
    switch (level) {
      case LogLevel::Trace: return "TRACE";
      case LogLevel::Info:  return "INFO ";
      case LogLevel::Warn:  return "WARN ";
      case LogLevel::Error: return "ERROR";
    }
    return "?????";
  }

  /**
   * @brief Writes a formatted message to the appropriate output stream.
   *
   * A severity tag is prepended and a newline appended, so @p format should
   * contain neither. The stream is flushed on every call: without this, the
   * last messages before a crash would be lost from the buffer, which are
   * precisely the ones worth reading.
   *
   * @tparam Args Types of the format arguments, deduced at the call site.
   * @param level  Severity of the message.
   * @param format A `printf`-style format string.
   * @param args   Values substituted into @p format.
   *
   * @warning @p format must be a literal or a trusted string. It is passed to
   *          `std::fprintf`, so a mismatch between the format specifiers and
   *          @p args is undefined behaviour.
   *
   * @note Call the FE_INFO() family of macros instead of calling this directly.
   */
  template <typename... Args>
  void log(LogLevel level, const char* format, Args... args) {
    std::FILE* stream =
        (level == LogLevel::Error || level == LogLevel::Warn) ? stderr : stdout;

    std::fprintf(stream, "[%s] ", logLevelName(level));
    std::fprintf(stream, format, args...);
    std::fprintf(stream, "\n");
    std::fflush(stream);
  }

}

/// @brief Logs a message at @ref fe::LogLevel::Trace severity.
#define FE_TRACE(...) ::fe::log(::fe::LogLevel::Trace, __VA_ARGS__)

/// @brief Logs a message at @ref fe::LogLevel::Info severity.
#define FE_INFO(...)  ::fe::log(::fe::LogLevel::Info,  __VA_ARGS__)

/// @brief Logs a message at @ref fe::LogLevel::Warn severity.
#define FE_WARN(...)  ::fe::log(::fe::LogLevel::Warn,  __VA_ARGS__)

/// @brief Logs a message at @ref fe::LogLevel::Error severity.
#define FE_ERROR(...) ::fe::log(::fe::LogLevel::Error, __VA_ARGS__)
