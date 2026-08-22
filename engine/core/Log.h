#pragma once

#include <cstdio>

namespace fe {
  enum class LogLevel {
    Trace,
    Info,
    Warn,
    Error,
  };

  inline const char* LogLevelName(LogLevel level) {
    switch (level) {
      case LogLevel::Trace: return "TRACE";
      case LogLevel::Info: return "INFO";
      case LogLevel::Warn: return "WARN";
      case LogLevel::Error: return "ERROR";
      default: return "UNKNOWN";
    }
  }

  template <typename... Args>
  void log(LogLevel level, const char* format, Args... args) {
    std::FILE* stream = (level == LogLevel::Error || level == LogLevel::Warn) ? stderr : stdout;
    std::fprintf(stream, "[%s] ", LogLevelName(level));
    std::fprintf(stream, format, args...);
    std::fprintf(stream, "\n");
    std::fflush(stream);
  }
}

#define FE_TRACE(...) ::fe::log(::fe::LogLevel::Trace, __VA_ARGS__)
#define FE_INFO(...)  ::fe::log(::fe::LogLevel::Info,  __VA_ARGS__)
#define FE_WARN(...)  ::fe::log(::fe::LogLevel::Warn,  __VA_ARGS__)
#define FE_ERROR(...) ::fe::log(::fe::LogLevel::Error, __VA_ARGS__)
