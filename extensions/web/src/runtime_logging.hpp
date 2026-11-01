#pragma once

#include <hg/logging.hpp>

#include <exception>
#include <string_view>

namespace hg::web::details {

inline void log_runtime_failure(std::string_view context, std::exception_ptr failure) noexcept {
    try {
        try {
            std::rethrow_exception(failure);
        } catch (const std::exception &error) {
            hg::log::error("{}: {}", context, error.what());
        } catch (...) {
            hg::log::error("{}: unknown exception", context);
        }
    } catch (...) {
    }
}

inline void log_runtime_failure(std::string_view context, std::string_view message) noexcept {
    try {
        hg::log::error("{}: {}", context, message);
    } catch (...) {
    }
}

} // namespace hg::web::details
