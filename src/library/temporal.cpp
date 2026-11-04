#include "registration.hpp"

#include <hg/graph.hpp>
#include <hg/library/operators.hpp>
#include <hg/node.hpp>
#include <hg/overload.hpp>
#include <hg/scalar/value.hpp>
#include <hg/type/scalar_type.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace hg::operators::details {
namespace {

using ATTRIBUTE = ScalarVariable<"TEMPORAL_ATTRIBUTE", string, symbol>;
using VALUE = hg::operators::VALUE;
inline constexpr auto G = guarantee_variable<"TEMPORAL_GETATTR_G">;

constexpr auto microseconds_per_second = i64{1'000'000};
constexpr auto microseconds_per_day = i64{86'400} * microseconds_per_second;
constexpr auto microseconds_per_hour = i64{3'600} * microseconds_per_second;
constexpr auto microseconds_per_minute = i64{60} * microseconds_per_second;

std::chrono::year_month_day calendar_date(date_time_t timestamp) {
    const auto day = std::chrono::floor<std::chrono::days>(timestamp);
    constexpr auto first_day = std::chrono::sys_days{
        std::chrono::year::min() / std::chrono::January / std::chrono::day{1}};
    constexpr auto last_day = std::chrono::sys_days{
        std::chrono::year::max() / std::chrono::December / std::chrono::day{31}};
    if (day < first_day || day > last_day) {
        throw std::out_of_range{"date_time_t calendar field is outside the supported year range"};
    }
    return std::chrono::year_month_day{day};
}

i64 time_of_day_microseconds(date_time_t timestamp) {
    auto remainder = timestamp.time_since_epoch().count() % microseconds_per_day;
    if (remainder < 0) {
        remainder += microseconds_per_day;
    }
    return remainder;
}

std::optional<std::string_view> attribute_name(const Scalar<ATTRIBUTE> &name) {
    const auto value = Scalar<>::from(name);
    if (&value.type() == &symbol_type()) {
        const auto symbol_name = scalar_cast<symbol>(value).value();
        if (!symbol_name.has_value()) {
            return std::nullopt;
        }
        return symbol_name.text();
    }
    return scalar_cast<string>(value).value();
}

std::optional<std::string_view> attribute_name(const WiringArguments &arguments) {
    const auto value = arguments.scalar("name");
    if (!value) {
        return std::nullopt;
    }
    return attribute_name(Scalar<ATTRIBUTE>::from(*value));
}

struct date_time_getattr_graph {
    wp<TS<date_time_t, G>> value;
    Scalar<ATTRIBUTE> name;

    wp<TS<i64, G>> wire() const {
        const auto name = attribute_name(this->name);
        if (!name) {
            throw std::invalid_argument{"date_time_t getattr requires a supported attribute name"};
        }
        const auto field = *name;
        if (field == "year") {
            static const auto year = node([](In<TS<date_time_t, G>> timestamp, Out<TS<i64, G>> output) {
                const auto date = calendar_date(timestamp.value());
                output.set(static_cast<i64>(static_cast<int>(date.year())));
            });
            return year(value);
        }
        if (field == "month") {
            static const auto month = node([](In<TS<date_time_t, G>> timestamp, Out<TS<i64, G>> output) {
                const auto date = calendar_date(timestamp.value());
                output.set(static_cast<i64>(static_cast<unsigned>(date.month())));
            });
            return month(value);
        }
        if (field == "day") {
            static const auto day = node([](In<TS<date_time_t, G>> timestamp, Out<TS<i64, G>> output) {
                const auto date = calendar_date(timestamp.value());
                output.set(static_cast<i64>(static_cast<unsigned>(date.day())));
            });
            return day(value);
        }
        if (field == "hour") {
            static const auto hour = node([](In<TS<date_time_t, G>> timestamp, Out<TS<i64, G>> output) {
                output.set(time_of_day_microseconds(timestamp.value()) / microseconds_per_hour);
            });
            return hour(value);
        }
        if (field == "minute") {
            static const auto minute = node([](In<TS<date_time_t, G>> timestamp, Out<TS<i64, G>> output) {
                output.set(time_of_day_microseconds(timestamp.value()) / microseconds_per_minute % 60);
            });
            return minute(value);
        }
        if (field == "second") {
            static const auto second = node([](In<TS<date_time_t, G>> timestamp, Out<TS<i64, G>> output) {
                output.set(time_of_day_microseconds(timestamp.value()) / microseconds_per_second % 60);
            });
            return second(value);
        }
        if (field == "microsecond") {
            static const auto microsecond = node([](In<TS<date_time_t, G>> timestamp, Out<TS<i64, G>> output) {
                output.set(time_of_day_microseconds(timestamp.value()) % microseconds_per_second);
            });
            return microsecond(value);
        }
        if (field == "weekday") {
            static const auto weekday = node([](In<TS<date_time_t, G>> timestamp, Out<TS<i64, G>> output) {
                const auto day = std::chrono::floor<std::chrono::days>(timestamp.value());
                const auto weekday = std::chrono::weekday{day}.c_encoding();
                output.set(static_cast<i64>((weekday + 6U) % 7U));
            });
            return weekday(value);
        }

        if (field != "isoweekday") {
            throw std::invalid_argument{"unsupported date_time_t attribute"};
        }

        static const auto isoweekday = node([](In<TS<date_time_t, G>> timestamp, Out<TS<i64, G>> output) {
            const auto day = std::chrono::floor<std::chrono::days>(timestamp.value());
            const auto weekday = std::chrono::weekday{day}.c_encoding();
            output.set(static_cast<i64>((weekday + 6U) % 7U + 1U));
        });
        return isoweekday(value);
    }
};

struct timedelta_getattr_graph {
    wp<TS<timedelta_t, G>> value;
    Scalar<ATTRIBUTE> name;

    static const ScalarType *resolve(VALUE, const WiringArguments &arguments) {
        const auto name = attribute_name(arguments);
        if (!name) {
            throw std::invalid_argument{"timedelta_t getattr requires a supported attribute name"};
        }
        if (*name == "days" || *name == "seconds" || *name == "microseconds") {
            return &hg::details::scalar_type_for<i64>();
        }
        if (*name == "total_seconds") {
            return &hg::details::scalar_type_for<double>();
        }
        throw std::invalid_argument{"unsupported timedelta_t attribute"};
    }

    wp<TS<VALUE, G>> wire() const {
        const auto name = attribute_name(this->name);
        if (!name) {
            throw std::invalid_argument{"timedelta_t getattr requires a supported attribute name"};
        }
        const auto field = *name;
        if (field == "days") {
            static const auto days = node([](In<TS<timedelta_t, G>> duration, Out<TS<i64, G>> output) {
                output.set(std::chrono::floor<std::chrono::days>(duration.value()).count());
            });
            return wp<TS<VALUE, G>>{days(value)};
        }
        if (field == "seconds") {
            static const auto seconds = node([](In<TS<timedelta_t, G>> duration, Out<TS<i64, G>> output) {
                auto remainder = duration.value().count() % microseconds_per_day;
                if (remainder < 0) {
                    remainder += microseconds_per_day;
                }
                output.set(remainder / microseconds_per_second);
            });
            return wp<TS<VALUE, G>>{seconds(value)};
        }

        if (field == "microseconds") {
            static const auto microseconds = node([](In<TS<timedelta_t, G>> duration, Out<TS<i64, G>> output) {
                auto remainder = duration.value().count() % microseconds_per_second;
                if (remainder < 0) {
                    remainder += microseconds_per_second;
                }
                output.set(remainder);
            });
            return wp<TS<VALUE, G>>{microseconds(value)};
        }

        if (field != "total_seconds") {
            throw std::invalid_argument{"unsupported timedelta_t attribute"};
        }

        static const auto total_seconds = node([](In<TS<timedelta_t, G>> duration, Out<TS<double, G>> output) {
            output.set(static_cast<double>(duration.value().count()) / 1'000'000.0);
        });
        return wp<TS<VALUE, G>>{total_seconds(value)};
    }
};

struct temporal_overloads {
    overload<getattr> getattr_date_time_i64{graph<date_time_getattr_graph>()};
    overload<getattr> getattr_timedelta{graph<VALUE, timedelta_getattr_graph>()};
};

} // namespace

void register_temporal_operators() { register_overloads<temporal_overloads>(); }

} // namespace hg::operators::details
