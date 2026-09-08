#pragma once

#include <fstream>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

class DefenseLogger {
public:
    explicit DefenseLogger(const std::string &directory = "Defesa Log") {
        namespace fs = std::filesystem;
        fs::create_directories(directory);
        std::size_t run = 1;
        while (fs::exists(fs::path(directory) /
                          ("auditoria_defesa_" + std::to_string(run) + ".log"))) {
            ++run;
        }
        path_ = (fs::path(directory) /
                 ("auditoria_defesa_" + std::to_string(run) + ".log")).string();
        file_.open(path_);
    }

    const std::string &path() const { return path_; }

    template <typename... Args>
    void print(Args &&...args) {
        std::lock_guard<std::mutex> lock(mutex_);
        write(std::cout, std::forward<Args>(args)...);
        if (file_) {
            write(file_, std::forward<Args>(args)...);
            file_.flush();
        }
    }

    template <typename... Args>
    void println(Args &&...args) {
        print(std::forward<Args>(args)..., "\n");
    }

private:
    template <typename Stream, typename... Args>
    static void write(Stream &stream, Args &&...args) {
        (stream << ... << std::forward<Args>(args));
    }

    std::ofstream file_;
    std::string path_;
    std::mutex mutex_;
};

inline constexpr const char *ANSI_RED = "\033[31m";
inline constexpr const char *ANSI_GREEN = "\033[32m";
inline constexpr const char *ANSI_YELLOW = "\033[33m";
inline constexpr const char *ANSI_RESET = "\033[0m";
