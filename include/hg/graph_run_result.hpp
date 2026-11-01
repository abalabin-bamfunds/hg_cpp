#pragma once

#include <bg/details/graph_run.hpp>
#include <bg/scalar/format.hpp>
#include <bg/time_series/tick_format.hpp>

#include <cstddef>
#include <functional>
#include <initializer_list>
#include <ostream>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace bg::details {
class LiteralMatcher;
}

namespace bg {

template <typename T = void> class GraphRunResult {
  public:
    explicit GraphRunResult(details::CapturedValues captured) : captured_{std::move(captured)} {}

    std::size_t size() const noexcept { return captured_.size(); }
    std::size_t event_count() const noexcept { return captured_.events().size(); }
    EvaluationTime start_time() const noexcept { return captured_.start_time(); }
    auto events() const noexcept { return captured_.events(); }

    // Visit one captured tick without taking ownership of its encoded values. The reader is valid only for the
    // duration of the callback. An invalid tick contains only its header.
    template <typename Visitor> bool visit_tick(std::size_t index, Visitor &&visitor) const {
        if (index >= size()) {
            throw std::out_of_range{"graph run tick index is out of range"};
        }
        const auto *event = captured_.event(index);
        if (event == nullptr) {
            return false;
        }

        auto reader = util::buffer_reader{*event};
        const auto header = bg::details::TickHeader::read(reader);
        if (header.operation != bg::details::TickOperation::tick) {
            throw std::invalid_argument{"graph run result tick does not start with a tick header"};
        }

        const auto *type = captured_.type();
        if (type == nullptr) {
            throw std::logic_error{"graph run result has captured bytes without a time-series type"};
        }

        std::invoke(std::forward<Visitor>(visitor), *type, header.valid, reader);
        return true;
    }

    template <typename Visitor> bool visit_event(std::size_t index, Visitor &&visitor) const {
        const auto event_times = captured_.events();
        if (index >= event_times.size()) {
            throw std::out_of_range{"graph run event index is out of range"};
        }
        const auto &event_time = event_times[index];
        auto reader = util::buffer_reader{captured_.event_data(index)};
        const auto header = bg::details::TickHeader::read(reader);
        if (header.operation != bg::details::TickOperation::tick) {
            throw std::invalid_argument{"graph run event does not start with a tick header"};
        }

        const auto *type = captured_.type();
        if (type == nullptr) {
            throw std::logic_error{"graph run result has captured bytes without a time-series type"};
        }

        std::invoke(std::forward<Visitor>(visitor), event_time, *type, header.valid, reader);
        return true;
    }

    auto operator[](std::size_t index) const {
        if constexpr (std::same_as<T, void>) {
            auto value = std::optional<ScalarValue<>>{};
            visit_scalar(index, [&](Scalar<> actual) {
                auto memory = allocator{};
                value.emplace(actual, memory);
            });
            return value;
        } else {
            auto value = std::optional<ScalarValue<T>>{};
            visit_scalar(index, [&](Scalar<> actual) {
                auto memory = allocator{};
                value.emplace(scalar_cast<T>(actual), memory);
            });
            return value;
        }
    }

    template <typename Value = T>
        requires(!std::same_as<Value, void>)
    bool equals(
        std::initializer_list<std::optional<std::remove_cvref_t<typename bg::scalar_storage_traits<Value>::value_type>>>
            expected) const {
        if (size() != expected.size()) {
            return false;
        }

        for (auto index = std::size_t{}; index < expected.size(); ++index) {
            const auto &value = *(expected.begin() + static_cast<std::ptrdiff_t>(index));
            auto matched = false;
            const auto present = visit_scalar(index, [&](Scalar<> actual) {
                if (!value) {
                    return;
                }
                const auto actual_scalar = scalar_cast<Value>(actual);
                decltype(auto) actual_value = actual_scalar.value();
                matched = scalar_storage_traits<Value>::equals(actual_value, *value);
            });
            if (present != value.has_value() || (value && !matched)) {
                return false;
            }
        }

        return true;
    }

    template <typename Value = T>
        requires(!std::same_as<Value, void>)
    bool operator==(
        std::initializer_list<std::optional<std::remove_cvref_t<typename bg::scalar_storage_traits<Value>::value_type>>>
            expected) const {
        return equals(expected);
    }

    bool equals(std::initializer_list<literal::TimeSeriesLiteral> expected) const
        requires std::same_as<T, void>;

    bool operator==(std::initializer_list<literal::TimeSeriesLiteral> expected) const
        requires std::same_as<T, void> {
        return equals(expected);
    }

  private:
    template <typename Function> bool visit_scalar(std::size_t index, Function &&function) const {
        auto present = false;
        visit_tick(index, [&](const TimeSeriesType &type, bool valid, util::buffer_reader &reader) {
            if (!valid) {
                return;
            }

            const auto *value_type = type_cast<TimeSeriesValueType>(&type);
            if (value_type == nullptr) {
                throw std::invalid_argument{"graph run result tick is a collection"};
            }
            present = bg::details::TimeSeriesTickScalarRead::visit(reader, value_type->value_type(),
                                                                    std::forward<Function>(function));
        });
        return present;
    }

    details::CapturedValues captured_;
};

// Print small results as dense values and large results as a sparse summary. Root skips and invalid ticks print as
// None.
template <typename T> std::ostream &operator<<(std::ostream &stream, const GraphRunResult<T> &result) {
    if (result.size() > 32) {
        const auto start_time = ScalarValue<EvaluationTime>{result.start_time()};
        return stream << fmt::format("GraphRunResult(start_time={}, event_count={}, size={})", start_time.view(),
                                     result.event_count(), result.size());
    }

    stream << '[';
    for (auto index = std::size_t{}; index < result.size(); ++index) {
        if (index != 0) {
            stream << ", ";
        }

        const auto present = result.visit_tick(index, [&](const TimeSeriesType &type, bool valid,
                                                          util::buffer_reader &reader) {
            if (valid) {
                stream << fmt::format("{}", tick(type, valid, reader));
            } else {
                stream << "None";
            }
        });
        if (!present) {
            stream << "None";
        }
    }
    return stream << ']';
}

template <typename T>
bool GraphRunResult<T>::equals(std::initializer_list<literal::TimeSeriesLiteral> expected) const
    requires std::same_as<T, void>
{
    return details::LiteralMatcher::matches_sequence(
        captured_, captured_.type(), std::span{expected.begin(), expected.size()});
}

} // namespace bg
