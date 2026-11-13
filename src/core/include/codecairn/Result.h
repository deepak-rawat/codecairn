#pragma once

#include <QString>
#include <optional>
#include <utility>

namespace codecairn {

enum class ErrorCode { InvalidArgument, Io, Conflict, Unavailable, Stale, Cancelled };

struct Error {
    ErrorCode code;
    QString message;
};

template <typename T> class Result {
public:
    static Result success(T value) { return Result(std::move(value), {}); }
    static Result failure(ErrorCode code, QString message) {
        return Result({}, Error{code, std::move(message)});
    }
    explicit operator bool() const { return value_.has_value(); }
    const T &value() const { return value_.value(); }
    const Error &error() const { return error_.value(); }

private:
    Result(std::optional<T> value, std::optional<Error> error)
        : value_(std::move(value)), error_(std::move(error)) {}
    std::optional<T> value_;
    std::optional<Error> error_;
};

template <> class Result<void> {
public:
    static Result success() { return Result(std::nullopt); }
    static Result failure(ErrorCode code, QString message) {
        return Result(Error{code, std::move(message)});
    }
    explicit operator bool() const { return !error_; }
    const Error &error() const { return error_.value(); }

private:
    explicit Result(std::optional<Error> error) : error_(std::move(error)) {}
    std::optional<Error> error_;
};

struct RequestId {
    quint64 value = 0;
    explicit operator bool() const { return value != 0; }
    bool operator==(const RequestId &) const = default;
};

}
