#pragma once
#include <cstdint>
#include <mutex>

namespace emudebug {

enum Domain : uint32_t {
    CPU  = 1u << 0,
    GPU  = 1u << 1,
    DMA  = 1u << 2,
    RAM  = 1u << 3,
    BIOS = 1u << 4,
    ROM  = 1u << 5
};

class Settings {
public:
    static Settings& instance() {
        static Settings s;
        return s;
    }

    void setEnabled(bool v) {
        std::lock_guard<std::mutex> lock(mu);
        enabled = v;
    }

    bool isEnabled() const {
        std::lock_guard<std::mutex> lock(mu);
        return enabled;
    }

    void setDomains(uint32_t m) {
        std::lock_guard<std::mutex> lock(mu);
        domains = m;
    }

    uint32_t getDomains() const {
        std::lock_guard<std::mutex> lock(mu);
        return domains;
    }

    bool enabledFor(uint32_t domainBit) const {
        std::lock_guard<std::mutex> lock(mu);
        return enabled && ((domains & domainBit) != 0);
    }

    void setFrameInterval(int n) {
        if (n < 1) n = 1;
        std::lock_guard<std::mutex> lock(mu);
        frameInterval = n;
    }

    int getFrameInterval() const {
        std::lock_guard<std::mutex> lock(mu);
        return frameInterval;
    }

private:
    mutable std::mutex mu;
    bool enabled = false;
    uint32_t domains = CPU | GPU | DMA;
    int frameInterval = 60;
};

}  // namespace emudebug
