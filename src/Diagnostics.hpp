#pragma once

#include <Geode/Geode.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <mutex>
#include <string>

namespace gpuhelper::diagnostics {

inline std::mutex& fileMutex() {
    static std::mutex mutex;
    return mutex;
}

inline void append(std::string const& line) {
    std::lock_guard<std::mutex> lock(fileMutex());
    auto* mod = geode::Mod::get();
    if (!mod) return;

    try {
        auto const directory = mod->getSaveDir();
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        auto const path = directory / "diagnostics.log";

        constexpr std::uintmax_t kMaxBytes = 1024u * 1024u;
        error.clear();
        auto const size = std::filesystem::file_size(path, error);
        if (!error && size > kMaxBytes) {
            auto const previous = directory / "diagnostics.previous.log";
            error.clear();
            std::filesystem::remove(previous, error);
            error.clear();
            std::filesystem::rename(path, previous, error);
        }

        std::ofstream output(path, std::ios::out | std::ios::app);
        if (output) {
            output << line << '\n';
            output.flush();
        }
    }
    catch (...) {
        // The diagnostic recorder must never interrupt gameplay.
    }
}

} // namespace gpuhelper::diagnostics
