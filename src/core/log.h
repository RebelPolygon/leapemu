#pragma once

#include <cstdarg>
#include <cstdio>
#include <functional>
#include <string>

namespace leap {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3, Trace = 4 };

// Minimal global logger. Frontends install a sink; the default prints to stderr.
struct Log {
  static inline LogLevel level = LogLevel::Info;
  static inline std::function<void(LogLevel, const std::string&)> sink;

  static void write(LogLevel lv, const char* fmt, ...)
#if defined(__GNUC__)
      __attribute__((format(printf, 2, 3)))
#endif
  {
    if (lv > level) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (sink) {
      sink(lv, buf);
    } else {
      static const char* names[] = {"E", "W", "I", "D", "T"};
      std::fprintf(stderr, "[%s] %s\n", names[static_cast<int>(lv)], buf);
    }
  }
};

#define LOG_E(...) ::leap::Log::write(::leap::LogLevel::Error, __VA_ARGS__)
#define LOG_W(...) ::leap::Log::write(::leap::LogLevel::Warn, __VA_ARGS__)
#define LOG_I(...) ::leap::Log::write(::leap::LogLevel::Info, __VA_ARGS__)
#define LOG_D(...) \
  do { if (::leap::Log::level >= ::leap::LogLevel::Debug) ::leap::Log::write(::leap::LogLevel::Debug, __VA_ARGS__); } while (0)

}  // namespace leap
