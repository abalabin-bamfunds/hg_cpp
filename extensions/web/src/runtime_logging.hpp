#pragma once

#include <bg/logging.hpp>

#include <exception>
#include <string_view>

namespace bg::web::details {

inline void log_runtime_failure(std::string_view context, std::exception_ptr failure) noexcept {
    try {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception &error) {
            bg::log::error("{}: {}", context, error.what());
        } catch (...) {
            bg::log::error("{}: unknown exception", context);
        }
    } catch (...) {
    }
}

inline void log_runtime_failure(std::string_view context, std::string_view message) noexcept {
    try {
        bg::log::error("{}: {}", context, message);
    } catch (...) {
    }
}

} // namespace bg::web::details
