#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <utility>

namespace vectorclick::win {

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE replacement = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_{};
};

class UniqueFindHandle {
public:
    UniqueFindHandle() noexcept = default;
    explicit UniqueFindHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueFindHandle() { reset(); }

    UniqueFindHandle(const UniqueFindHandle&) = delete;
    UniqueFindHandle& operator=(const UniqueFindHandle&) = delete;

    UniqueFindHandle(UniqueFindHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)) {}
    UniqueFindHandle& operator=(UniqueFindHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, INVALID_HANDLE_VALUE));
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE replacement = INVALID_HANDLE_VALUE) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            FindClose(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class UniqueGdiObject {
public:
    UniqueGdiObject() noexcept = default;
    explicit UniqueGdiObject(HGDIOBJ object) noexcept : object_(object) {}
    ~UniqueGdiObject() { reset(); }

    UniqueGdiObject(const UniqueGdiObject&) = delete;
    UniqueGdiObject& operator=(const UniqueGdiObject&) = delete;

    UniqueGdiObject(UniqueGdiObject&& other) noexcept : object_(std::exchange(other.object_, nullptr)) {}
    UniqueGdiObject& operator=(UniqueGdiObject&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.object_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] HGDIOBJ get() const noexcept { return object_; }

    void reset(HGDIOBJ replacement = nullptr) noexcept {
        if (object_ != nullptr) {
            DeleteObject(object_);
        }
        object_ = replacement;
    }

private:
    HGDIOBJ object_{};
};

} // namespace vectorclick::win
