#pragma once

#include <Geode/Geode.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>

namespace gpuhelper::diagnostics {

inline std::mutex& fileMutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::filesystem::path& saveDirectoryStorage() {
    static std::filesystem::path directory;
    return directory;
}

inline std::deque<std::string>& recentEventStorage() {
    static std::deque<std::string> events;
    return events;
}

inline bool& exitHandlerRegisteredStorage() {
    static bool registered = false;
    return registered;
}

inline std::uint64_t unixMilliseconds() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

inline std::filesystem::path directoryUnlocked() {
    auto& directory = saveDirectoryStorage();
    if (directory.empty()) {
        if (auto* mod = geode::Mod::get()) {
            directory = mod->getSaveDir();
        }
    }
    return directory;
}

inline void writeTextUnlocked(
    std::filesystem::path const& path,
    std::string const& contents
) {
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (output) {
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        output.flush();
    }
}

inline void markCleanExitAtExit() {
    std::lock_guard<std::mutex> lock(fileMutex());
    try {
        auto const directory = directoryUnlocked();
        if (directory.empty()) return;
        writeTextUnlocked(
            directory / "session.state",
            "STATE=CLEAN_EXIT\nEXITED_AT_UNIX_MS=" +
            std::to_string(unixMilliseconds()) +
            "\nNOTE=The process reached its normal C runtime exit handlers.\n"
        );
    }
    catch (...) {
        // Never let diagnostics interfere with process shutdown.
    }
}

// A previous STATE=RUNNING marker indicates abnormal process termination,
// an OS kill, or force close; it is not proof of a crash.
inline std::string beginSession(std::string const& buildVersion) {
    std::lock_guard<std::mutex> lock(fileMutex());
    auto* mod = geode::Mod::get();
    if (!mod) return "SESSION_MARKER_UNAVAILABLE";

    try {
        auto const directory = mod->getSaveDir();
        saveDirectoryStorage() = directory;
        std::error_code error;
        std::filesystem::create_directories(directory, error);

        std::string previousMarker;
        auto const markerPath = directory / "session.state";
        {
            std::ifstream input(markerPath);
            if (input) {
                previousMarker.assign(
                    std::istreambuf_iterator<char>(input),
                    std::istreambuf_iterator<char>()
                );
            }
        }

        auto const latestSnapshot = directory / "crash-state.latest.log";
        error.clear();
        if (std::filesystem::exists(latestSnapshot, error) && !error) {
            error.clear();
            std::filesystem::copy_file(
                latestSnapshot,
                directory / "crash-state.previous.log",
                std::filesystem::copy_options::overwrite_existing,
                error
            );
        }

        auto const sessionId = unixMilliseconds();
        writeTextUnlocked(
            markerPath,
            "STATE=RUNNING\nSESSION_ID=" + std::to_string(sessionId) +
            "\nSTARTED_AT_UNIX_MS=" + std::to_string(sessionId) +
            "\nBUILD=" + buildVersion + "\n"
        );

        if (!exitHandlerRegisteredStorage()) {
            if (std::atexit(&markCleanExitAtExit) == 0) {
                exitHandlerRegisteredStorage() = true;
            }
        }

        return previousMarker.empty() ? "NO_PREVIOUS_SESSION_MARKER" : previousMarker;
    }
    catch (...) {
        return "SESSION_MARKER_WRITE_FAILED";
    }
}

inline void append(std::string const& line) {
    std::lock_guard<std::mutex> lock(fileMutex());
    try {
        auto const directory = directoryUnlocked();
        if (directory.empty()) return;

        auto& events = recentEventStorage();
        events.push_back(line);
        while (events.size() > 64) events.pop_front();

        std::error_code error;
        std::filesystem::create_directories(directory, error);
        auto const path = directory / "diagnostics.log";

        constexpr std::uintmax_t kMaxBytes = 2u * 1024u * 1024u;
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

inline std::string recentEvents() {
    std::lock_guard<std::mutex> lock(fileMutex());
    std::string result;
    try {
        for (auto const& event : recentEventStorage()) {
            result += event;
            result += '\n';
        }
    }
    catch (...) {}
    return result;
}

inline std::string saveDirectoryPath() {
    std::lock_guard<std::mutex> lock(fileMutex());
    try {
        return directoryUnlocked().string();
    }
    catch (...) {
        return "unavailable";
    }
}

inline void writeCrashSnapshot(std::string const& contents) {
    std::lock_guard<std::mutex> lock(fileMutex());
    try {
        auto const directory = directoryUnlocked();
        if (directory.empty()) return;

        std::error_code error;
        std::filesystem::create_directories(directory, error);
        auto const target = directory / "crash-state.latest.log";
        auto const temporary = directory / "crash-state.latest.log.tmp";
        writeTextUnlocked(temporary, contents);

        error.clear();
        std::filesystem::rename(temporary, target, error);
        if (error) {
            error.clear();
            std::filesystem::remove(target, error);
            error.clear();
            std::filesystem::rename(temporary, target, error);
        }
        if (error) {
            error.clear();
            std::filesystem::remove(temporary, error);
        }
    }
    catch (...) {
        // Snapshot failure must never interrupt gameplay.
    }
}

} // namespace gpuhelper::diagnostics
