#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <istream>
#include <ostream>
#include <type_traits>
#include <vector>

namespace stateio {

template <typename T>
inline bool writePod(std::ostream& os, const T& v) {
    static_assert(std::is_trivially_copyable_v<T>, "writePod requires trivially copyable type");
    os.write(reinterpret_cast<const char*>(&v), sizeof(T));
    return static_cast<bool>(os);
}

template <typename T>
inline bool readPod(std::istream& is, T& v) {
    static_assert(std::is_trivially_copyable_v<T>, "readPod requires trivially copyable type");
    is.read(reinterpret_cast<char*>(&v), sizeof(T));
    return static_cast<bool>(is);
}

inline bool writeBool(std::ostream& os, bool v) {
    uint8_t b = v ? 1u : 0u;
    return writePod(os, b);
}

inline bool readBool(std::istream& is, bool& v) {
    uint8_t b = 0;
    if (!readPod(is, b)) return false;
    v = b != 0;
    return true;
}

template <typename T, size_t N>
inline bool writeArray(std::ostream& os, const std::array<T, N>& a) {
    if constexpr (std::is_trivially_copyable_v<T>) {
        os.write(reinterpret_cast<const char*>(a.data()), sizeof(T) * N);
        return static_cast<bool>(os);
    } else {
        for (const auto& v : a) if (!writePod(os, v)) return false;
        return true;
    }
}

template <typename T, size_t N>
inline bool readArray(std::istream& is, std::array<T, N>& a) {
    if constexpr (std::is_trivially_copyable_v<T>) {
        is.read(reinterpret_cast<char*>(a.data()), sizeof(T) * N);
        return static_cast<bool>(is);
    } else {
        for (auto& v : a) if (!readPod(is, v)) return false;
        return true;
    }
}

template <typename T>
inline bool writeVector(std::ostream& os, const std::vector<T>& v) {
    uint32_t n = static_cast<uint32_t>(v.size());
    if (!writePod(os, n)) return false;
    if constexpr (std::is_trivially_copyable_v<T>) {
        if (n > 0) os.write(reinterpret_cast<const char*>(v.data()), sizeof(T) * n);
        return static_cast<bool>(os);
    } else {
        for (const auto& e : v) if (!writePod(os, e)) return false;
        return true;
    }
}

template <typename T>
inline bool readVector(std::istream& is, std::vector<T>& v) {
    uint32_t n = 0;
    if (!readPod(is, n)) return false;
    v.resize(n);
    if constexpr (std::is_trivially_copyable_v<T>) {
        if (n > 0) is.read(reinterpret_cast<char*>(v.data()), sizeof(T) * n);
        return static_cast<bool>(is);
    } else {
        for (auto& e : v) if (!readPod(is, e)) return false;
        return true;
    }
}

template <typename T>
inline bool writeDeque(std::ostream& os, const std::deque<T>& d) {
    uint32_t n = static_cast<uint32_t>(d.size());
    if (!writePod(os, n)) return false;
    for (const auto& e : d) if (!writePod(os, e)) return false;
    return true;
}

template <typename T>
inline bool readDeque(std::istream& is, std::deque<T>& d) {
    uint32_t n = 0;
    if (!readPod(is, n)) return false;
    d.clear();
    for (uint32_t i = 0; i < n; ++i) {
        T e{};
        if (!readPod(is, e)) return false;
        d.push_back(e);
    }
    return true;
}

} // namespace stateio

