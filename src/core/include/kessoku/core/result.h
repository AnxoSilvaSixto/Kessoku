#pragma once

#include "kessoku/core/error.h"
#include <string_view>
#include <type_traits>
#include <utility>

namespace kessoku::core {

namespace detail {

template <typename T>
void DestroyValue(void* ptr, bool is_ok) {
    if (is_ok) {
        reinterpret_cast<T*>(ptr)->~T();
    } else {
        reinterpret_cast<Error*>(ptr)->~Error();
    }
}

} // namespace detail

template <typename T>
class Result {
public:
    static Result Ok(T&& value) {
        Result r;
        new (&r.storage_[0]) T(std::move(value));
        r.is_ok_ = true;
        return r;
    }

    static Result Err(ErrorCode code, std::string_view message) {
        Result r;
        new (&r.storage_[0]) Error{code, std::string{message}};
        r.is_ok_ = false;
        return r;
    }

    bool IsOk() const noexcept { return is_ok_; }
    bool IsErr() const noexcept { return !is_ok_; }

    const T& Value() const {
        return *reinterpret_cast<const T*>(&storage_[0]);
    }

    T& Value() {
        return *reinterpret_cast<T*>(&storage_[0]);
    }

    const Error& GetError() const {
        return *reinterpret_cast<const Error*>(&storage_[0]);
    }

    ~Result() {
        detail::DestroyValue<T>(&storage_[0], is_ok_);
    }

    // Non-copyable
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    // Movable
    Result(Result&& other) noexcept {
        if (other.is_ok_) {
            new (&storage_[0]) T(std::move(*reinterpret_cast<T*>(&other.storage_[0])));
        } else {
            new (&storage_[0]) Error(std::move(*reinterpret_cast<Error*>(&other.storage_[0])));
        }
        is_ok_ = other.is_ok_;
    }

    Result& operator=(Result&& other) noexcept {
        if (this == &other) return *this;
        this->~Result();
        new (this) Result(std::move(other));
        return *this;
    }

private:
    Result() = default;

    alignas(max_align_t) unsigned char storage_[sizeof(T) > sizeof(Error) ? sizeof(T) : sizeof(Error)];
    bool is_ok_ = false;
};

using Status = Result<void>;

template <>
class Result<void> {
public:
    static Result<void> Ok() {
        Result<void> r;
        r.is_ok_ = true;
        return r;
    }

    static Result<void> Err(ErrorCode code, std::string_view message) {
        Result<void> r;
        new (&r.storage_[0]) Error{code, std::string{message}};
        r.is_ok_ = false;
        return r;
    }

    bool IsOk() const noexcept { return is_ok_; }
    bool IsErr() const noexcept { return !is_ok_; }

    const Error& GetError() const {
        return *reinterpret_cast<const Error*>(&storage_[0]);
    }

    ~Result() {
        if (!is_ok_) {
            reinterpret_cast<Error*>(&storage_[0])->~Error();
        }
    }

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;

    Result(Result&& other) noexcept {
        is_ok_ = other.is_ok_;
        if (!is_ok_) {
            new (&storage_[0]) Error(std::move(*reinterpret_cast<Error*>(&other.storage_[0])));
        }
    }

    Result& operator=(Result&& other) noexcept {
        if (this == &other) return *this;
        this->~Result();
        new (this) Result(std::move(other));
        return *this;
    }

private:
    Result() = default;

    alignas(max_align_t) unsigned char storage_[sizeof(Error)];
    bool is_ok_ = false;
};

} // namespace kessoku::core
