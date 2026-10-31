#pragma once

#include <bg/scalar/scalar.hpp>

#include <fmt/args.h>

#include <cstddef>
#include <utility>

namespace bg::operators::details {

template <typename VALUE>
void push_format_argument(fmt::dynamic_format_arg_store<fmt::format_context> &store, const char *name,
                          VALUE value) {
    if (name == nullptr) {
        store.push_back(value);
    } else {
        store.push_back(fmt::arg(name, value));
    }
}

template <typename FALLBACK>
void push_scalar_format_argument(fmt::dynamic_format_arg_store<fmt::format_context> &store, const char *name,
                                 Scalar<SCALAR> value, FALLBACK &&push_fallback) {
    value.visit(
        [&](const Scalar<byte> *item) {
            push_format_argument(store, name, std::to_integer<unsigned int>(item->value()));
        },
        [&](const Scalar<bool> *item) { push_format_argument(store, name, item->value()); },
        [&](const Scalar<i64> *item) { push_format_argument(store, name, item->value()); },
        [&](const Scalar<double> *item) { push_format_argument(store, name, item->value()); },
        [&](const Scalar<date_time_t> *item) { push_format_argument(store, name, item->value()); },
        [&](const Scalar<timedelta_t> *item) { push_format_argument(store, name, item->value()); },
        [&](const Scalar<string> *item) { push_format_argument(store, name, item->value()); },
        [&](const Scalar<symbol> *item) { push_format_argument(store, name, item->value().text()); },
        [&](const auto *) { std::forward<FALLBACK>(push_fallback)(store, name, value); }, ddv::noop);
}

} // namespace bg::operators::details
