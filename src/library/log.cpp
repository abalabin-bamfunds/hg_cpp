#include "registration.hpp"
#include "string_format_helpers.hpp"

#include <bg/evaluation_context.hpp>
#include <bg/library/operators.hpp>
#include <bg/logging.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/format.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/tick_format.hpp>
#include <bg/time_series/tick_serializer.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/util/buffer.hpp>

#include <fmt/args.h>
#include <fmt/chrono.h>
#include <fmt/format.h>

#include <cstddef>
#include <deque>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace bg::operators::details::log_detail {

struct SkippedTick {};

class RetainedTick {
  public:
    explicit RetainedTick(const TimeSeriesType &type) noexcept : type_{&type} {}
    RetainedTick(const TimeSeriesType &type, util::buffer bytes) noexcept : type_{&type}, bytes_{std::move(bytes)} {}

    RetainedTick(const RetainedTick &) = delete;
    RetainedTick &operator=(const RetainedTick &) = delete;
    RetainedTick(RetainedTick &&other) noexcept
        : type_{std::exchange(other.type_, nullptr)}, bytes_{std::move(other.bytes_)} {
        other.bytes_.reset();
    }
    RetainedTick &operator=(RetainedTick &&) = delete;

    ~RetainedTick() noexcept {
        if (!bytes_) {
            return;
        }

        auto cleanup = TimeSeriesTickReader{std::move(*bytes_), *type_};
    }

    const TimeSeriesType &type() const noexcept { return *type_; }
    bool skipped() const noexcept { return !bytes_; }
    const util::buffer &bytes() const noexcept { return *bytes_; }

  private:
    const TimeSeriesType *type_;
    std::optional<util::buffer> bytes_;
};

using LogValue = std::variant<ScalarValue<>, RetainedTick>;

struct LogArgument {
    LogArgument(std::string name, ScalarValue<> value) : name{std::move(name)}, value{std::move(value)} {}
    LogArgument(std::string name, RetainedTick value) : name{std::move(name)}, value{std::move(value)} {}
    std::string name;
    LogValue value;
};

struct LogRecord {
    std::string pattern;
    std::vector<LogArgument> arguments;
};

static_assert(std::is_nothrow_move_constructible_v<LogRecord>);
static_assert(std::is_nothrow_destructible_v<LogRecord>);

std::string format_record(const LogRecord &record);

} // namespace bg::operators::details::log_detail

template <> struct fmt::formatter<bg::operators::details::log_detail::SkippedTick> {
    constexpr auto parse(fmt::format_parse_context &context) {
        const auto it = context.begin();
        if (it != context.end() && *it != '}') {
            throw fmt::format_error{"tick formatting does not accept format specifiers"};
        }
        return it;
    }

    auto format(const bg::operators::details::log_detail::SkippedTick &, fmt::format_context &context) const {
        return fmt::format_to(context.out(), "SKIP");
    }
};

namespace bg::operators::details {
namespace {

using namespace bg::literals;
using log_detail::LogArgument;
using log_detail::LogRecord;
using log_detail::LogValue;
using log_detail::RetainedTick;
using log_detail::SkippedTick;

allocator logging_allocator() noexcept { return allocator{*std::pmr::new_delete_resource()}; }

bg::logging::Level parse_log_level(std::string_view value) {
    if (value == "trace") {
        return bg::logging::Level::trace;
    }
    if (value == "debug") {
        return bg::logging::Level::debug;
    }
    if (value == "info") {
        return bg::logging::Level::info;
    }
    if (value == "warning") {
        return bg::logging::Level::warning;
    }
    if (value == "error") {
        return bg::logging::Level::error;
    }
    if (value == "critical") {
        return bg::logging::Level::critical;
    }
    if (value == "off") {
        return bg::logging::Level::off;
    }
    throw std::invalid_argument{"__level__ must be trace, debug, info, warning, error, critical, or off"};
}

template <auto... Properties>
bg::logging::Level log_level(const In<TSB<FORMAT_NAMED>, Properties...> &named) {
    for (auto index = std::size_t{}; index < named.size(); ++index) {
        if (named.item_name(index).text() == "__level__") {
            return parse_log_level(named[index].template as<TS<string>>().value_view().value());
        }
    }
    return bg::logging::Level::info;
}

RequirementResult require_log_level(const WiringArguments &arguments) {
    for (auto index = std::size_t{}; index < arguments.size(); ++index) {
        if (arguments.argument_name(index).text() != "__level__") {
            continue;
        }
        if (!arguments.present(index) || &arguments.original_type(index) != &string_type()) {
            return reject("__level__ must be a scalar string");
        }

        try {
            const auto value = scalar_cast<string>(*arguments.scalar(index)).value();
            (void)parse_log_level(value);
        } catch (const std::exception &) {
            return reject("__level__ must be trace, debug, info, warning, error, critical, or off");
        }
    }
    return accept;
}

template <typename Variable, auto... Properties>
RetainedTick capture_tick(const In<Variable, Properties...> &input, EvaluationTime now) {
    constexpr auto initial_block_capacity = std::size_t{256};
    auto memory = logging_allocator();
    auto bytes = util::buffer::chain(initial_block_capacity, memory);
    auto retained = bytes;
    if (input.last_updated() == now && input.modified() && !input.valid()) {
        auto writer = util::buffer_writer{std::move(bytes)};
        auto context = ScalarBufferWriteContext{writer};
        bg::details::TickHeader{bg::details::TickOperation::tick, false}.write(context);
        writer.commit();
        return RetainedTick{input.runtime_type().type(), std::move(retained)};
    }

    auto writer = TimeSeriesTickWriter{std::move(bytes), memory};
    if (!writer.write(input, now)) {
        return RetainedTick{input.runtime_type().type()};
    }
    return RetainedTick{input.runtime_type().type(), std::move(retained)};
}

template <typename Variable, auto... Properties>
LogArgument capture_argument(const In<Variable, Properties...> &input, EvaluationTime now, std::string name = {}) {
    if (input.valid()) {
        auto memory = logging_allocator();
        auto scalar = std::optional<ScalarValue<>>{};
        input.visit([&](const In<TS<SCALAR>> *series) { scalar.emplace(series->value_view(), memory); }, ddv::noop);
        if (scalar) {
            return LogArgument{std::move(name), std::move(*scalar)};
        }
    }

    return LogArgument{std::move(name), capture_tick(input, now)};
}

void push_argument(fmt::dynamic_format_arg_store<fmt::format_context> &store, const LogArgument &argument,
                   std::deque<util::buffer_reader> &readers) {
    const auto *name = argument.name.empty() ? nullptr : argument.name.c_str();
    std::visit(
        [&](const auto &value) {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::same_as<Value, ScalarValue<>>) {
                const auto scalar = Scalar<SCALAR>::from(value);
                push_scalar_format_argument(
                    store, name, scalar,
                    [](auto &arguments, auto key, Scalar<SCALAR> item) { push_format_argument(arguments, key, item); });
            } else if constexpr (std::same_as<Value, RetainedTick>) {
                if (value.skipped()) {
                    push_format_argument(store, name, SkippedTick{});
                    return;
                }
                readers.emplace_back(value.bytes());
                push_format_argument(store, name, tick(value.type(), readers.back()));
            }
        },
        argument.value);
}

template <typename POSITIONAL_INPUT, typename NAMED_INPUT>
LogRecord capture_record(std::string_view pattern, const POSITIONAL_INPUT &positional, const NAMED_INPUT &named,
                         EvaluationTime now) {
    auto record = LogRecord{.pattern = std::string{pattern}, .arguments = {}};
    record.arguments.reserve(positional.size() + named.size());
    for (auto index = std::size_t{}; index < positional.size(); ++index) {
        record.arguments.emplace_back(capture_argument(positional[index], now));
    }
    for (auto index = std::size_t{}; index < named.size(); ++index) {
        const auto name = named.item_name(index).text();
        if (name == "__level__") {
            continue;
        }
        record.arguments.emplace_back(capture_argument(named[index], now, std::string{name}));
    }
    return record;
}

struct log_overloads {
    struct log_definition {
        static constexpr bool deduplicate_wiring = false;

        std::string_view pattern;
        In<TSB<FORMAT_POSITIONAL>, var_args, optional> positional;
        In<TSB<FORMAT_NAMED>, var_kwargs, optional> named;

        static RequirementResult require(const WiringArguments &arguments) {
            auto result = require_log_level(arguments);
            if (!result.accepted()) {
                return result;
            }

            const auto &pattern_type = arguments.original_type("pattern");
            const auto *pattern_series = pattern_type.is_time_series()
                                             ? type_cast<TimeSeriesValueType>(
                                                   &type_cast<TimeSeriesType>(pattern_type).dereferenced_type())
                                             : nullptr;
            if (pattern_series != nullptr && &pattern_series->value_type() == &string_type()) {
                auto has_format_argument = false;
                for (auto index = std::size_t{}; index < arguments.size(); ++index) {
                    const auto name = arguments.argument_name(index).text();
                    if (name != "pattern" && name != "__level__") {
                        has_format_argument = true;
                        break;
                    }
                }
                if (!has_format_argument) {
                    return reject("a string time series without format arguments is a log_ tick input");
                }
            }
            return accept;
        }

        void evaluate(EvaluationContext *context) const {
            const auto level = log_level(named);
            if (!bg::logging::enabled(level)) {
                return;
            }
            auto record = capture_record(pattern, positional, named, context->evaluation_time());
            bg::logging::details::submit(level, [record = std::move(record)] {
                return log_detail::format_record(record);
            });
        }
    };

    struct log_tick_definition {
        static constexpr bool deduplicate_wiring = false;

        In<TS_T, optional> value;
        In<TSB<FORMAT_NAMED>, var_kwargs> named;

        static RequirementResult require(const WiringArguments &arguments) {
            if (!arguments.original_type("value").is_time_series()) {
                return reject("log_ tick input must be a time series");
            }
            for (auto index = std::size_t{}; index < arguments.size(); ++index) {
                const auto name = arguments.argument_name(index).text();
                if (name != "value" && name != "__level__") {
                    return reject("log_ tick input accepts only the __level__ option");
                }
            }
            return require_log_level(arguments);
        }

        void evaluate(EvaluationContext *context) const {
            const auto level = log_level(named);
            if (!bg::logging::enabled(level) || value.last_updated() != context->evaluation_time() ||
                !value.modified()) {
                return;
            }

            auto record = LogRecord{.pattern = "{}", .arguments = {}};
            record.arguments.emplace_back(std::string{}, capture_tick(value, context->evaluation_time()));
            bg::logging::details::submit(level, [record = std::move(record)] {
                return log_detail::format_record(record);
            });
        }
    };

    overload<log_> log_impl{node<log_definition>()};
    overload<log_, log_tick_definition> log_tick_impl;
};

} // namespace
} // namespace bg::operators::details

namespace bg::operators::details::log_detail {

std::string format_record(const LogRecord &record) {
    auto store = fmt::dynamic_format_arg_store<fmt::format_context>{};
    auto readers = std::deque<util::buffer_reader>{};
    for (const auto &argument : record.arguments) {
        bg::operators::details::push_argument(store, argument, readers);
    }
    return fmt::vformat(fmt::string_view{record.pattern.data(), record.pattern.size()}, store);
}

} // namespace bg::operators::details::log_detail

namespace bg::operators::details {

void register_log_operators() { register_overloads<log_overloads>(); }

} // namespace bg::operators::details
