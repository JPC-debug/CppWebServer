
#pragma once

#include <iostream>
#include <mutex>
#include <sstream>
#include <utility>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <string>

// 全局日志锁
inline std::mutex& logMutex() {
    static std::mutex mtx;
    return mtx;
}

// 输出一条完整日志
inline void writeLog(
    const char* level,
    const std::string& message
) {
    std::lock_guard<std::mutex> lock(logMutex());

    // 获取当前时间
    auto now = std::chrono::system_clock::now();
    std::time_t current =
        std::chrono::system_clock::to_time_t(now);

    // Linux 线程安全时间转换
    std::tm local_time{};
    localtime_r(&current, &local_time);

    std::cout
        << "["
        << std::put_time(
            &local_time,
            "%Y-%m-%d %H:%M:%S"
        )
        << "]"
        << " [" << level << "] "
        << message
        << std::endl;
}

// 通用日志函数
template <typename... Args>
inline void logWithLevel(
    const char* level,
    Args&&... args
) {
    std::ostringstream oss;

    (oss << ... << std::forward<Args>(args));

    writeLog(level, oss.str());
}

// 普通信息
template <typename... Args>
inline void logInfo(Args&&... args) {
    logWithLevel("INFO", std::forward<Args>(args)...);
}

// 警告信息
template <typename... Args>
inline void logWarn(Args&&... args) {
    logWithLevel("WARN", std::forward<Args>(args)...);
}

// 错误信息
template <typename... Args>
inline void logError(Args&&... args) {
    logWithLevel("ERROR", std::forward<Args>(args)...);
}
