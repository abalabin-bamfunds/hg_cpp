#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/evaluation_context.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/format.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tick_format.hpp>

#include <fmt/args.h>
#include <fmt/chrono.h>
#include <fmt/format.h>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto G = guarantee_variable<"STRING_FORMAT_G">;

std::string quoted_text(std::string_view text) {
    const auto quote = text.find('\'') == std::string_view::npos ? '\'' : '"';
    auto result = std::string{};
    result.reserve(text.size() + 2);
    result.push_back(quote);
    for (const auto character : text) {
        if (character == '\\' || character == quote) {
            result.push_back('\\');
            result.push_back(character);
            continue;
        }

        switch (character) {
        case '\0': result.append("\\x00"); break;
        case '\n': result.append("\\n"); break;
        case '\r': result.append("\\r"); break;
        case '\t': result.append("\\t"); break;
        default: result.push_back(character); break;
        }
    }
    result.push_back(quote);
    return result;
}

std::string duration_text(timedelta_t value) {
    constexpr auto microseconds_per_second = std::int64_t{1'000'000};
    constexpr auto microseconds_per_day = std::uint64_t{86'400'000'000};
    const auto count = value.count();
    const auto negative = count < 0;
    const auto magnitude = negative ? std::uint64_t{0} - static_cast<std::uint64_t>(count)
                                   : static_cast<std::uint64_t>(count);
    auto days = magnitude / microseconds_per_day;
    auto remainder = magnitude % microseconds_per_day;
    if (negative && remainder != 0) {
        ++days;
        remainder = microseconds_per_day - remainder;
    }

    const auto total_seconds = remainder / microseconds_per_second;
    const auto hours = total_seconds / 3'600;
    const auto minutes = total_seconds / 60 % 60;
    const auto seconds = total_seconds % 60;
    const auto fraction = remainder % microseconds_per_second;

    auto result = std::string{};
    if (days != 0) {
        if (negative) {
            result.push_back('-');
        }
        result.append(fmt::format("{} day{}, ", days, days == 1 ? "" : "s"));
    }
    result.append(fmt::format("{}:{:02}:{:02}", hours, minutes, seconds));
    if (fraction != 0) {
        result.append(fmt::format(".{:06}", fraction));
    }
    return result;
}

void append_repr(std::string &result, Scalar<SCALAR> value, bool quote_strings);

template <typename COLLECTION, typename APPEND>
void append_sequence(std::string &result, const COLLECTION &values, char open, char close, APPEND append) {
    result.push_back(open);
    auto first = true;
    for (auto index = std::size_t{}; index < values.size(); ++index) {
        if (!first) {
            result.append(", ");
        }
        append(values[index]);
        first = false;
    }
    result.push_back(close);
}

void append_repr(std::string &result, Scalar<SCALAR> value, bool quote_strings) {
    value.visit(
        [&](const Scalar<byte> *item) { result.append(fmt::format("{}", std::to_integer<unsigned int>(item->value()))); },
        [&](const Scalar<bool> *item) { result.append(item->value() ? "True" : "False"); },
        [&](const Scalar<i64> *item) { result.append(fmt::format("{}", item->value())); },
        [&](const Scalar<double> *item) {
            auto text = fmt::format("{}", item->value());
            if (std::isfinite(item->value()) && text.find_first_of(".eE") == std::string::npos) {
                text.append(".0");
            }
            result.append(text);
        },
        [&](const Scalar<date_time_t> *item) {
            const auto whole_seconds = std::chrono::floor<std::chrono::seconds>(item->value());
            auto text = fmt::format("{:%Y-%m-%d %H:%M:%S}", whole_seconds);
            const auto fraction = item->value() - whole_seconds;
            if (fraction.count() != 0) {
                text.append(fmt::format(".{:06}", fraction.count()));
            }
            result.append(text);
        },
        [&](const Scalar<timedelta_t> *item) { result.append(duration_text(item->value())); },
        [&](const Scalar<string> *item) {
            const auto text = item->value();
            result.append(quote_strings ? quoted_text(text) : std::string{text});
        },
        [&](const Scalar<symbol> *item) {
            const auto text = item->value().text();
            result.append(quote_strings ? quoted_text(text) : std::string{text});
        },
        [&](const Scalar<List<SCALAR>> *items) {
            append_sequence(result, *items, '[', ']', [&](Scalar<SCALAR> item) { append_repr(result, item, true); });
        },
        [&](const Scalar<Set<SCALAR>> *items) {
            if (items->empty()) {
                result.append("set()");
                return;
            }
            result.push_back('{');
            auto first = true;
            items->for_each([&](Scalar<SCALAR> item) {
                if (!first) {
                    result.append(", ");
                }
                append_repr(result, item, true);
                first = false;
            });
            result.push_back('}');
        },
        [&](const Scalar<Dict<SCALAR, SCALAR>> *items) {
            result.push_back('{');
            auto first = true;
            items->for_each([&](Scalar<SCALAR> key, Scalar<SCALAR> item) {
                if (!first) {
                    result.append(", ");
                }
                append_repr(result, key, true);
                result.append(": ");
                append_repr(result, item, true);
                first = false;
            });
            result.push_back('}');
        },
        [&](const Scalar<TUPLE> *items) {
            append_sequence(result, *items, '(', ')', [&](Scalar<SCALAR> item) { append_repr(result, item, true); });
            if (items->size() == 1) {
                result.insert(result.size() - 1, 1, ',');
            }
        },
        [&](const Scalar<STRUCT> *items) {
            result.push_back('{');
            for (auto index = std::size_t{}; index < items->field_count(); ++index) {
                if (index != 0) {
                    result.append(", ");
                }
                result.append(quoted_text(items->type().field(index).name.text()));
                result.append(": ");
                append_repr(result, (*items)[index], true);
            }
            result.push_back('}');
        },
        [&](const Scalar<HIERARCHICAL> *items) {
            result.push_back('{');
            auto first = true;
            for (auto index = std::size_t{}; index < items->field_count(); ++index) {
                if (!items->present(index)) {
                    continue;
                }
                if (!first) {
                    result.append(", ");
                }
                result.append(quoted_text(items->field_name(index)));
                result.append(": ");
                append_repr(result, (*items)[index], true);
                first = false;
            }
            result.push_back('}');
        },
        [&](const Scalar<> *) { throw std::invalid_argument{"str_ does not support this scalar type"}; }, ddv::noop);
}

std::string scalar_text(Scalar<SCALAR> value, bool quote_strings = false) {
    auto result = std::string{};
    append_repr(result, value, quote_strings);
    return result;
}

TimeSeriesGuarantee resolve_output(const WiringArguments &arguments) {
    auto guarantee = TimeSeriesGuarantee::const_;
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        const auto *type = type_cast<TimeSeriesType>(&arguments.resolved_type(index));
        if (type != nullptr) {
            guarantee = weakest(guarantee, *type->guarantee().value());
        }
    }
    return guarantee;
}

template <typename VALUE>
void push_argument(fmt::dynamic_format_arg_store<fmt::format_context> &store, const char *name, VALUE value) {
    if (name == nullptr) {
        store.push_back(value);
    } else {
        store.push_back(fmt::arg(name, value));
    }
}

template <typename FALLBACK>
void push_scalar(fmt::dynamic_format_arg_store<fmt::format_context> &store, const char *name, Scalar<SCALAR> value,
                 FALLBACK &&push_fallback) {
    value.visit(
        [&](const Scalar<byte> *item) { push_argument(store, name, std::to_integer<unsigned int>(item->value())); },
        [&](const Scalar<bool> *item) { push_argument(store, name, item->value()); },
        [&](const Scalar<i64> *item) { push_argument(store, name, item->value()); },
        [&](const Scalar<double> *item) { push_argument(store, name, item->value()); },
        [&](const Scalar<date_time_t> *item) { push_argument(store, name, item->value()); },
        [&](const Scalar<timedelta_t> *item) { push_argument(store, name, item->value()); },
        [&](const Scalar<string> *item) { push_argument(store, name, item->value()); },
        [&](const Scalar<symbol> *item) { push_argument(store, name, item->value().text()); },
        [&](const auto *) { std::forward<FALLBACK>(push_fallback)(store, name, value); },
        ddv::noop);
}

struct string_format_overloads {
    static void add_input(fmt::dynamic_format_arg_store<fmt::format_context> &store,
                          std::vector<std::string> &names, const In<TS_T> &input, std::string_view name = {}) {
        auto value = std::optional<Scalar<SCALAR>>{};
        input.visit([&](const In<TS<SCALAR>> *series) { value.emplace(series->value_view()); }, ddv::noop);
        if (!value) {
            throw std::invalid_argument{"format_ arguments must be scalar values or scalar time series"};
        }

        const auto *argument_name = static_cast<const char *>(nullptr);
        if (!name.empty()) {
            names.emplace_back(name);
            argument_name = names.back().c_str();
        }
        push_scalar(store, argument_name, *value, [](auto &arguments, auto argument_name, Scalar<SCALAR> item) {
            auto text = scalar_text(item, true);
            push_argument(arguments, argument_name, std::move(text));
        });
    }

    static std::string format(std::string_view pattern,
                              const In<TSB<FORMAT_POSITIONAL>, var_args, bg::all_valid> &positional,
                              const In<TSB<FORMAT_NAMED>, var_kwargs, bg::all_valid> &named) {
        auto store = fmt::dynamic_format_arg_store<fmt::format_context>{};
        auto names = std::vector<std::string>{};
        names.reserve(positional.size() + named.size());

        for (auto index = std::size_t{}; index < positional.size(); ++index) {
            add_input(store, names, positional[index]);
        }
        for (auto index = std::size_t{}; index < named.size(); ++index) {
            const auto name = named.item_name(index).text();
            add_input(store, names, named[index], name);
        }
        return fmt::vformat(fmt::string_view{pattern.data(), pattern.size()}, store);
    }

    static RequirementResult require_print_output(const WiringArguments &arguments) {
        for (auto index = std::size_t{}; index < arguments.size(); ++index) {
            if (arguments.argument_name(index).text() != "__std_err__") {
                continue;
            }
            if (!arguments.present(index)) {
                return reject("__std_err__ must be a scalar bool");
            }
            const auto *type = type_cast<ScalarType>(&arguments.resolved_type(index));
            if (type == nullptr || type != &bool_type()) {
                return reject("__std_err__ must be a scalar bool");
            }
        }
        return accept;
    }

    template <typename Variable, auto... Properties>
    static void add_print_input(fmt::dynamic_format_arg_store<fmt::format_context> &store,
                                std::vector<std::string> &names, const In<Variable, Properties...> &input,
                                EvaluationTime now, std::string_view name = {}) {
        const auto *argument_name = static_cast<const char *>(nullptr);
        if (!name.empty()) {
            names.emplace_back(name);
            argument_name = names.back().c_str();
        }

        auto scalar_series = false;
        if (input.valid()) {
            input.visit(
                [&](const In<TS<SCALAR>> *series) {
                    scalar_series = true;
                    push_scalar(store, argument_name, series->value_view(),
                                [](auto &arguments, auto name, Scalar<SCALAR> value) {
                                    push_argument(arguments, name, value);
                                });
                },
                ddv::noop);
        }
        if (scalar_series) {
            return;
        }

        push_argument(store, argument_name, tick(input, now));
    }

    template <typename POSITIONAL, typename NAMED>
    static void print(std::string_view pattern, const POSITIONAL &positional, const NAMED &named,
                      EvaluationTime now) {
        auto store = fmt::dynamic_format_arg_store<fmt::format_context>{};
        auto names = std::vector<std::string>{};
        names.reserve(positional.size() + named.size());
        const auto standard_error = read_standard_error(named);

        for (auto index = std::size_t{}; index < positional.size(); ++index) {
            add_print_input(store, names, positional[index], now);
        }
        for (auto index = std::size_t{}; index < named.size(); ++index) {
            const auto name = named.item_name(index).text();
            if (name == "__std_err__") {
                continue;
            }
            add_print_input(store, names, named[index], now, name);
        }

        auto &stream = standard_error ? std::cerr : std::cout;
        fmt::vformat_to(std::ostreambuf_iterator<char>{stream}, fmt::string_view{pattern.data(), pattern.size()}, store);
        stream.put('\n');
    }

    static bool read_standard_error(const auto &named) {
        for (auto index = std::size_t{}; index < named.size(); ++index) {
            if (named.item_name(index).text() != "__std_err__") {
                continue;
            }
            return named[index].template as<TS<bool>>().value_view().value();
        }
        return false;
    }

    overload<str_> str_scalar{node(
        eval = [](Scalar<SCALAR> value, Out<TS<string, G>> output) {
            output.set_value(ScalarValue<string>{scalar_text(value)});
        },
        resolve<decltype(G)> = resolve_output)};

    overload<str_> str_impl{node(
        eval = [](In<TS<SCALAR, G>> value, Out<TS<string, G>> output) {
            output.set_value(ScalarValue<string>{scalar_text(value.value_view())});
        })};

    overload<format_> format_impl{node(
        eval = [](std::string_view pattern, In<TSB<FORMAT_POSITIONAL>, var_args, bg::all_valid> positional,
                 In<TSB<FORMAT_NAMED>, var_kwargs, bg::all_valid> named, Out<TS<string, G>> output) {
            output.set_value(ScalarValue<string>{format(pattern, positional, named)});
        },
        resolve<decltype(G)> = resolve_output)};

    struct print_definition {
        static constexpr bool deduplicate_wiring = false;

        std::string_view pattern;
        In<TSB<FORMAT_POSITIONAL>, var_args, optional> positional;
        In<TSB<FORMAT_NAMED>, var_kwargs, optional> named;

        static RequirementResult require(const WiringArguments &arguments) {
            const auto &pattern_type = arguments.original_type("pattern");
            const auto *pattern_series = pattern_type.is_time_series()
                                             ? type_cast<TimeSeriesValueType>(
                                                   &type_cast<TimeSeriesType>(pattern_type).dereferenced_type())
                                             : nullptr;
            if (pattern_series != nullptr && &pattern_series->value_type() == &string_type()) {
                auto has_format_argument = false;
                for (auto index = std::size_t{}; index < arguments.size(); ++index) {
                    const auto name = arguments.argument_name(index).text();
                    if (name != "pattern" && name != "__std_err__") {
                        has_format_argument = true;
                        break;
                    }
                }
                if (!has_format_argument) {
                    return reject("a string time series without format arguments is a print_ tick input");
                }
            }
            return require_print_output(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            print(pattern, positional, named, context->evaluation_time());
        }
    };

    overload<print_> print_impl{node<print_definition>()};

    struct print_tick_definition {
        static constexpr bool deduplicate_wiring = false;

        In<TS_T, optional> value;
        In<TSB<FORMAT_NAMED>, var_kwargs> named;

        static RequirementResult require(const WiringArguments &arguments) {
            if (!arguments.original_type("value").is_time_series()) {
                return reject("print_ tick input must be a time series");
            }
            for (auto index = std::size_t{}; index < arguments.size(); ++index) {
                const auto name = arguments.argument_name(index).text();
                if (name != "value" && name != "__std_err__") {
                    return reject("print_ tick input accepts only the __std_err__ option");
                }
            }
            return require_print_output(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            const auto now = context->evaluation_time();
            if (value.last_updated() != now || !value.modified()) {
                return;
            }

            auto &stream = read_standard_error(named) ? std::cerr : std::cout;
            fmt::format_to(std::ostreambuf_iterator<char>{stream}, "{}\n", tick(value, now));
        }
    };

    overload<print_, print_tick_definition> print_tick_impl;
};

} // namespace

void register_string_format_operators() { register_overloads<string_format_overloads>(); }

} // namespace bg::operators::details
