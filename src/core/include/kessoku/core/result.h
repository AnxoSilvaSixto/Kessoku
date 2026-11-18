#pragma once

#include "kessoku/core/error.h"
#include <cassert>
#include <cstddef>
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
class [[nodiscard]] Result {
public:
    [[nodiscard]] static Result Ok(T&& value) {
        Result r;
        // A throwing payload constructor must leave r empty: the destructor
        // below only destroys storage_ after this flag is set.
        new (&r.storage_[0]) T(std::move(value));
        r.is_ok_ = true;
        r.has_value_ = true;
        return r;
    }

    [[nodiscard]] static Result Err(ErrorCode code, std::string_view message) {
        Result r;
        new (&r.storage_[0]) Error{code, std::string{message}};
        r.is_ok_ = false;
        r.has_value_ = true;
        return r;
    }

    [[nodiscard]] bool IsOk() const noexcept { return is_ok_; }
    [[nodiscard]] bool IsErr() const noexcept { return !is_ok_; }

    [[nodiscard]] const T& Value() const {
        assert(is_ok_ && has_value_);
        return *reinterpret_cast<const T*>(&storage_[0]);
    }

    [[nodiscard]] T& Value() {
        assert(is_ok_ && has_value_);
        return *reinterpret_cast<T*>(&storage_[0]);
    }

    [[nodiscard]] const Error& GetError() const {
        assert(!is_ok_ && has_value_);
        return *reinterpret_cast<const Error*>(&storage_[0]);
    }

    ~Result() {
        if (has_value_) {
            detail::DestroyValue<T>(&storage_[0], is_ok_);
        }
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
        has_value_ = true;
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
    // False until a payload has actually been constructed in storage_. A
    // throwing construction then leaves nothing for ~Result to destroy.
    bool has_value_ = false;
};

using Status = Result<void>;

template <>
class [[nodiscard]] Result<void> {
public:
    [[nodiscard]] static Result<void> Ok() {
        Result<void> r;
        r.is_ok_ = true;
        r.has_value_ = true;
        return r;
    }

    [[nodiscard]] static Result<void> Err(ErrorCode code, std::string_view message) {
        Result<void> r;
        new (&r.storage_[0]) Error{code, std::string{message}};
        r.is_ok_ = false;
        r.has_value_ = true;
        return r;
    }

    [[nodiscard]] bool IsOk() const noexcept { return is_ok_; }
    [[nodiscard]] bool IsErr() const noexcept { return !is_ok_; }

    [[nodiscard]] const Error& GetError() const {
        assert(!is_ok_ && has_value_);
        return *reinterpret_cast<const Error*>(&storage_[0]);
    }

    ~Result() {
        if (has_value_ && !is_ok_) {
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
        has_value_ = true;
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
    bool has_value_ = false;
};

} // namespace kessoku::core
