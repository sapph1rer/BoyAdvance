#pragma once
#include <chrono>
#include <cstdint>
#include <ctime>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace emulog {

enum class Level : uint32_t {
    Debug = 1u << 0,
    Stub = 1u << 1,
    Info = 1u << 2,
    Warning = 1u << 3,
    Error = 1u << 4,
    Fatal = 1u << 5,
    GameError = 1u << 6
};

inline constexpr uint32_t levelBit(Level level) {
    return static_cast<uint32_t>(level);
}

inline const char* levelName(Level level) {
    switch (level) {
        case Level::Debug: return "Debug";
        case Level::Stub: return "Stub";
        case Level::Info: return "Info";
        case Level::Warning: return "Warning";
        case Level::Error: return "Error";
        case Level::Fatal: return "Fatal";
        case Level::GameError: return "Game Error";
        default: return "Unknown";
    }
}

struct Entry {
    uint64_t sequence = 0;
    std::chrono::system_clock::time_point when{};
    Level level = Level::Info;
    std::string tag;
    std::string message;
};

class Logger {
public:
    static Logger& instance() {
        static Logger inst;
        return inst;
    }

    void setMaxEntries(size_t maxEntries) {
        if (maxEntries < 1) maxEntries = 1;
        std::lock_guard<std::mutex> lock(mu);
        maxKeep = maxEntries;
        trimLocked();
    }

    size_t maxEntries() const {
        std::lock_guard<std::mutex> lock(mu);
        return maxKeep;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu);
        entries.clear();
    }

    void log(Level level, const std::string& tag, const std::string& message) {
        Entry e;
        {
            std::lock_guard<std::mutex> lock(mu);
            e.sequence = ++seq;
            e.when = std::chrono::system_clock::now();
            e.level = level;
            e.tag = tag;
            e.message = message;
            entries.push_back(std::move(e));
            trimLocked();
        }
    }

    std::vector<Entry> snapshot() const {
        std::lock_guard<std::mutex> lock(mu);
        return std::vector<Entry>(entries.begin(), entries.end());
    }

    std::vector<Entry> snapshotSince(uint64_t sequenceExclusive) const {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<Entry> out;
        out.reserve(entries.size());
        for (const auto& e : entries) {
            if (e.sequence > sequenceExclusive) out.push_back(e);
        }
        return out;
    }

    uint64_t latestSequence() const {
        std::lock_guard<std::mutex> lock(mu);
        if (entries.empty()) return 0;
        return entries.back().sequence;
    }

    static std::string formatEntry(const Entry& e) {
        std::time_t t = std::chrono::system_clock::to_time_t(e.when);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      e.when.time_since_epoch()) % 1000;
        std::tm tmBuf{};
#if defined(_WIN32)
        localtime_s(&tmBuf, &t);
#else
        localtime_r(&t, &tmBuf);
#endif
        std::ostringstream ss;
        ss << std::put_time(&tmBuf, "%H:%M:%S") << '.'
           << std::setw(3) << std::setfill('0') << ms.count()
           << " [" << levelName(e.level) << "] [" << e.tag << "] " << e.message;
        return ss.str();
    }

    // Writes every currently-held entry to `path`, one per line. Returns false if
    // the file could not be opened. Safe to call from a GUI "Save logs" button;
    // takes a snapshot under the lock and does the (slow) file I/O outside it.
    bool saveToFile(const std::string& path) const {
        std::vector<Entry> copy = snapshot();
        std::ofstream out(path, std::ios::out | std::ios::trunc);
        if (!out.is_open()) return false;
        for (const auto& e : copy) {
            out << formatEntry(e) << '\n';
        }
        return out.good();
    }

private:
    void trimLocked() {
        while (entries.size() > maxKeep) entries.pop_front();
    }

    mutable std::mutex mu;
    std::deque<Entry> entries;
    size_t maxKeep = 5000;
    uint64_t seq = 0;
};

}  // namespace emulog
