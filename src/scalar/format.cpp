#include <bg/scalar/format.hpp>

#include <bg/scalar/bundle/scalar.hpp>
#include <bg/scalar/dict/scalar.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/set/scalar.hpp>
#include <bg/scalar/struct/scalar.hpp>
#include <bg/scalar/tuple/scalar.hpp>
#include <bg/type/scalar_type.hpp>
#include <bg/util/static_storage.hpp>

#include <fmt/chrono.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace bg {
namespace {

template <typename... Arguments>
void write(fmt::format_context &context, fmt::format_string<Arguments...> pattern, Arguments &&...arguments) {
    context.advance_to(fmt::format_to(context.out(), pattern, std::forward<Arguments>(arguments)...));
}

void format_value(Scalar<SCALAR> value, fmt::format_context &context);

void format_byte(Scalar<byte> value, fmt::format_context &context) {
    write(context, "{}", std::to_integer<unsigned int>(value.value()));
}

void format_bytes(Scalar<bytes> value, fmt::format_context &context) {
    write(context, "bytes(\"");
    for (const auto item : value.value()) {
        write(context, "{:02x}", std::to_integer<unsigned int>(item));
    }
    write(context, "\")");
}

void format_date_time(Scalar<date_time_t> value, fmt::format_context &context) {
    write(context, "{:%Y-%m-%dT%H:%M:%S}Z", value.value());
}

void format_string(std::string_view value, fmt::format_context &context) { write(context, "{:?}", value); }

void format_value(Scalar<SCALAR> value, fmt::format_context &context) {
    value.visit(
        [&](const Scalar<byte> *item) { format_byte(*item, context); },
        [&](const Scalar<bytes> *item) { format_bytes(*item, context); },
        [&](const Scalar<bool> *item) { write(context, "{}", item->value()); },
        [&](const Scalar<i64> *item) { write(context, "{}", item->value()); },
        [&](const Scalar<double> *item) { write(context, "{}", item->value()); },
        [&](const Scalar<date_time_t> *item) { format_date_time(*item, context); },
        [&](const Scalar<timedelta_t> *item) { write(context, "{}us", item->value().count()); },
        [&](const Scalar<string> *item) { format_string(item->value(), context); },
        [&](const Scalar<symbol> *item) {
            if (item->value().has_value()) {
                write(context, "symbol({:?})", item->value().text());
            } else {
                write(context, "symbol()");
            }
        },
        [&](const Scalar<List<SCALAR>> *items) {
            write(context, "[");
            for (auto index = std::size_t{}; index < items->size(); ++index) {
                if (index != 0) {
                    write(context, ", ");
                }
                format_value((*items)[index], context);
            }
            write(context, "]");
        },
        [&](const Scalar<Set<SCALAR>> *items) {
            if (items->empty()) {
                write(context, "set()");
                return;
            }
            write(context, "{{");
            auto first = true;
            items->for_each([&](Scalar<SCALAR> item) {
                if (!first) {
                    write(context, ", ");
                }
                format_value(item, context);
                first = false;
            });
            write(context, "}}");
        },
        [&](const Scalar<Dict<SCALAR, SCALAR>> *items) {
            write(context, "{{");
            auto first = true;
            items->for_each([&](Scalar<SCALAR> key, Scalar<SCALAR> item) {
                if (!first) {
                    write(context, ", ");
                }
                format_value(key, context);
                write(context, ": ");
                format_value(item, context);
                first = false;
            });
            write(context, "}}");
        },
        [&](const Scalar<TUPLE> *items) {
            write(context, "(");
            for (auto index = std::size_t{}; index < items->size(); ++index) {
                if (index != 0) {
                    write(context, ", ");
                }
                format_value((*items)[index], context);
            }
            if (items->size() == 1) {
                write(context, ",");
            }
            write(context, ")");
        },
        [&](const Scalar<STRUCT> *items) {
            write(context, "{{");
            for (auto index = std::size_t{}; index < items->field_count(); ++index) {
                if (index != 0) {
                    write(context, ", ");
                }
                format_string(items->field_name(index), context);
                write(context, ": ");
                format_value((*items)[index], context);
            }
            write(context, "}}");
        },
        [&](const Scalar<HIERARCHICAL> *items) {
            write(context, "{{");
            auto first = true;
            for (auto index = std::size_t{}; index < items->field_count(); ++index) {
                if (!items->present(index)) {
                    continue;
                }
                if (!first) {
                    write(context, ", ");
                }
                format_string(items->field_name(index), context);
                write(context, ": ");
                format_value((*items)[index], context);
                first = false;
            }
            write(context, "}}");
        },
        [&](const Scalar<> *extension) {
            const auto &dispatch = ScalarDispatch<ScalarFormatOperations>::instance();
            const auto *operations = dispatch.find_exact(extension->runtime_type());
            if (operations == nullptr || operations->format == nullptr) {
                operations = dispatch.find(extension->type());
            }
            if (operations == nullptr || operations->format == nullptr) {
                auto name = std::string{};
                extension->type().format_name(name);
                throw fmt::format_error{fmt::format("no scalar formatter registered for `{}`", name)};
            }
            operations->format(*extension, context);
        },
        ddv::noop);
}

void format_native_int(Scalar<> value, fmt::format_context &context) {
    write(context, "{}", Scalar<int>::from(value).value());
}

void format_native_float(Scalar<> value, fmt::format_context &context) {
    write(context, "{}", Scalar<float>::from(value).value());
}

} // namespace

template <> ScalarDispatch<ScalarFormatOperations> &ScalarDispatch<ScalarFormatOperations>::instance() noexcept {
    static auto dispatch = util::static_storage<ScalarDispatch<ScalarFormatOperations>>{};
    static const auto initialized = [&] {
        auto &operations = dispatch.get();
        (void)operations.add(int_type(), ScalarFormatOperations{&format_native_int});
        (void)operations.add(float_type(), ScalarFormatOperations{&format_native_float});
        return true;
    }();
    (void)initialized;
    return dispatch.get();
}

void format_scalar(Scalar<> value, fmt::format_context &context) {
    format_value(Scalar<SCALAR>::from(value), context);
}

} // namespace bg
