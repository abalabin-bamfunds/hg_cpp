#include "registration.hpp"

#include <hg/graph.hpp>
#include <hg/library/operators.hpp>
#include <hg/node.hpp>
#include <hg/overload.hpp>
#include <hg/scalar/dict/scalar.hpp>
#include <hg/scalar/list/scalar.hpp>
#include <hg/scalar/set/scalar.hpp>
#include <hg/scalar/value.hpp>
#include <hg/type/time_series_dict_type.hpp>
#include <hg/time_series/tsd/tsd_output.hpp>
#include <hg/time_series/tss/tss_output.hpp>

#include <ranges>
#include <utility>
#include <vector>

namespace hg::operators::details {
namespace {

using namespace hg::literals;

inline constexpr auto INPUT_G = guarantee_variable<"COLLECT_INPUT_G">;
inline constexpr auto OUTPUT_G = guarantee_variable<"COLLECT_OUTPUT_G">;
inline constexpr auto ITEM_G = guarantee_variable<"COLLECT_ITEM_G">;
using KEY = ScalarVariable<"COLLECT_KEY", Hashable>;
using VALUE = ScalarVariable<"COLLECT_VALUE">;

struct collect_overloads {
    static const TimeSeriesType *requested_output(const WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(::hg::details::type_parameter<OUT>());
        const auto *requested = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
        return requested == nullptr || *requested == nullptr ? nullptr : type_cast<TimeSeriesType>(*requested);
    }

    static TimeSeriesGuarantee output_guarantee(const WiringArguments &arguments) {
        const auto *requested = requested_output(arguments);
        if (requested == nullptr || !requested->guarantee().value()) {
            return TimeSeriesGuarantee::ordinary;
        }
        return *requested->guarantee().value();
    }

    static TimeSeriesGuarantee item_guarantee(const WiringArguments &arguments) {
        const auto *requested = requested_output(arguments);
        const auto *dictionary = requested == nullptr ? nullptr
                                                       : type_cast<TimeSeriesDictionaryType>(requested);
        if (dictionary == nullptr || !dictionary->value_type().guarantee().value()) {
            return TimeSeriesGuarantee::ordinary;
        }
        return *dictionary->value_type().guarantee().value();
    }

    static RequirementResult require_output(const WiringArguments &arguments) {
        const auto *requested = requested_output(arguments);
        const auto *resolved = arguments.resolved_output_type();
        if (requested == nullptr || !requested->accepts(*resolved)) {
            return reject("collect requires a compatible explicit time-series output type");
        }

        const auto guarantee = *resolved->guarantee().value();
        if (guarantee != TimeSeriesGuarantee::ordinary && guarantee != TimeSeriesGuarantee::deduplicated) {
            return reject("collect supports ordinary or deduplicated output guarantees");
        }
        return accept;
    }

    static RequirementResult require_tsd_output(const WiringArguments &arguments) {
        const auto result = require_output(arguments);
        if (!result.accepted()) {
            return result;
        }

        const auto *output = type_cast<TimeSeriesDictionaryType>(arguments.resolved_output_type());
        // Out<TSD<...>> resolves the output shape; only the nested item guarantee needs validation.
        const auto item_guarantee = *output->value_type().guarantee().value();
        if (item_guarantee != TimeSeriesGuarantee::ordinary &&
            item_guarantee != TimeSeriesGuarantee::deduplicated) {
            return reject("collect supports ordinary or deduplicated dictionary item guarantees");
        }
        return accept;
    }

    template <typename V, typename Output>
    static void collect_list(const In<TS<V, INPUT_G>, optional> &value,
                             const In<SIGNAL, optional> *reset, const Output &output) {
        const auto reset_history = reset != nullptr && reset->valid() && reset->modified();
        const auto value_tick = value.valid() && value.modified();
        if (!reset_history && !value_tick) {
            return;
        }

        if (reset_history || !output.valid()) {
            auto items = std::views::iota(std::size_t{}, static_cast<std::size_t>(value_tick)) |
                         std::views::transform([&](std::size_t) { return value.value_view(); });
            output.set(items);
            return;
        }

        const auto previous = output.value();
        const auto item_count = previous.size() + static_cast<std::size_t>(value_tick);
        auto items = std::views::iota(std::size_t{}, item_count) | std::views::transform([&](std::size_t index) {
                          return index < previous.size() ? previous[index] : value.value_view();
                      });
        output.set(items);
    }

    template <typename V, typename Output>
    static void collect_set(const In<TS<V, INPUT_G>, optional> &value,
                            const In<SIGNAL, optional> *reset, const Output &output) {
        const auto reset_history = reset != nullptr && reset->valid() && reset->modified();
        const auto value_tick = value.valid() && value.modified();
        if (!reset_history && !value_tick) {
            return;
        }

        auto items = std::vector<Scalar<V>>{};
        if (!reset_history && output.valid()) {
            const auto previous = output.value();
            items.reserve(previous.size() + static_cast<std::size_t>(value_tick));
            previous.for_each([&](const auto &item) { items.push_back(item); });
        }
        if (value_tick) {
            items.push_back(value.value_view());
        }
        output.set(items);
    }

    template <typename K, typename V, typename Output>
    static void collect_dict(const In<TS<K, INPUT_G>, optional, passive> &key,
                             const In<TS<V, INPUT_G>, optional> &value,
                             const In<SIGNAL, optional> *reset, const Output &output) {
        const auto reset_history = reset != nullptr && reset->valid() && reset->modified();
        const auto value_tick = key.valid() && value.valid() && value.modified();
        if (!reset_history && !value_tick) {
            return;
        }

        using Entry = std::pair<Scalar<K>, Scalar<V>>;
        auto entries = std::vector<Entry>{};
        if (!reset_history && output.valid()) {
            const auto previous = output.value();
            entries.reserve(previous.size() + static_cast<std::size_t>(value_tick));
            previous.for_each([&](const auto &previous_key, const auto &previous_value) {
                entries.emplace_back(previous_key, previous_value);
            });
        }

        if (value_tick) {
            const auto current_key = key.value_view();
            const auto current_value = value.value_view();
            const auto existing = std::ranges::find_if(entries, [&](const auto &entry) {
                return Scalar<>::from(entry.first).equals(Scalar<>::from(current_key));
            });
            if (existing == entries.end()) {
                entries.emplace_back(current_key, current_value);
            } else {
                existing->second = current_value;
            }
        }

        output.set(entries);
    }

    struct collect_list_impl {
        In<TS<VALUE, INPUT_G>, optional> value;
        Out<TS<List<VALUE>, OUTPUT_G>> output;

        static RequirementResult require(const WiringArguments &arguments) { return require_output(arguments); }
        static TimeSeriesGuarantee resolve(decltype(OUTPUT_G), const WiringArguments &arguments) {
            return output_guarantee(arguments);
        }

        void evaluate() const { collect_overloads::collect_list(value, nullptr, output); }
    };

    struct collect_list_reset_impl {
        In<TS<VALUE, INPUT_G>, optional> value;
        In<SIGNAL, optional> reset;
        Out<TS<List<VALUE>, OUTPUT_G>> output;

        static RequirementResult require(const WiringArguments &arguments) { return require_output(arguments); }
        static TimeSeriesGuarantee resolve(decltype(OUTPUT_G), const WiringArguments &arguments) {
            return output_guarantee(arguments);
        }

        void evaluate() const { collect_overloads::collect_list(value, &reset, output); }
    };

    overload<collect, collect_list_impl> collect_list_without_reset;
    overload<collect, collect_list_reset_impl> collect_list_with_reset;

    struct collect_set_impl {
        In<TS<VALUE, INPUT_G>, optional> value;
        Out<TS<Set<VALUE>, OUTPUT_G>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return require_output(arguments);
        }
        static TimeSeriesGuarantee resolve(decltype(OUTPUT_G), const WiringArguments &arguments) {
            return output_guarantee(arguments);
        }

        void evaluate() const { collect_overloads::collect_set(value, nullptr, output); }
    };

    struct collect_set_reset_impl {
        In<TS<VALUE, INPUT_G>, optional> value;
        In<SIGNAL, optional> reset;
        Out<TS<Set<VALUE>, OUTPUT_G>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return require_output(arguments);
        }
        static TimeSeriesGuarantee resolve(decltype(OUTPUT_G), const WiringArguments &arguments) {
            return output_guarantee(arguments);
        }

        void evaluate() const { collect_overloads::collect_set(value, &reset, output); }
    };

    overload<collect, collect_set_impl> collect_set_without_reset;
    overload<collect, collect_set_reset_impl> collect_set_with_reset;

    struct collect_tss_impl {
        In<TS<VALUE, INPUT_G>, optional> value;
        Out<TSS<VALUE, OUTPUT_G>> output;

        static RequirementResult require(const WiringArguments &arguments) { return require_output(arguments); }
        static TimeSeriesGuarantee resolve(decltype(OUTPUT_G), const WiringArguments &arguments) {
            return output_guarantee(arguments);
        }

        void evaluate() const {
            if (value.valid() && value.modified()) {
                output.insert(value.value_view());
            }
        }
    };

    struct collect_tss_reset_impl {
        In<TS<VALUE, INPUT_G>, optional> value;
        In<SIGNAL, optional> reset;
        Out<TSS<VALUE, OUTPUT_G>> output;

        static RequirementResult require(const WiringArguments &arguments) { return require_output(arguments); }
        static TimeSeriesGuarantee resolve(decltype(OUTPUT_G), const WiringArguments &arguments) {
            return output_guarantee(arguments);
        }

        void evaluate() const {
            if (reset.valid() && reset.modified()) {
                output.clear();
            }
            if (value.valid() && value.modified()) {
                output.insert(value.value_view());
            }
        }
    };

    overload<collect, collect_tss_impl> collect_tss_without_reset;
    overload<collect, collect_tss_reset_impl> collect_tss_with_reset;

    struct collect_dict_impl {
        In<TS<KEY, INPUT_G>, optional, passive> key;
        In<TS<VALUE, INPUT_G>, optional> value;
        Out<TS<Dict<KEY, VALUE>, OUTPUT_G>> output;

        static RequirementResult require(const WiringArguments &arguments) { return require_output(arguments); }
        static TimeSeriesGuarantee resolve(decltype(OUTPUT_G), const WiringArguments &arguments) {
            return output_guarantee(arguments);
        }

        void evaluate() const { collect_overloads::collect_dict(key, value, nullptr, output); }
    };

    struct collect_dict_reset_impl {
        In<TS<KEY, INPUT_G>, optional, passive> key;
        In<TS<VALUE, INPUT_G>, optional> value;
        In<SIGNAL, optional> reset;
        Out<TS<Dict<KEY, VALUE>, OUTPUT_G>> output;

        static RequirementResult require(const WiringArguments &arguments) { return require_output(arguments); }
        static TimeSeriesGuarantee resolve(decltype(OUTPUT_G), const WiringArguments &arguments) {
            return output_guarantee(arguments);
        }

        void evaluate() const { collect_overloads::collect_dict(key, value, &reset, output); }
    };

    overload<collect, collect_dict_impl> collect_dict_without_reset;
    overload<collect, collect_dict_reset_impl> collect_dict_with_reset;

    struct collect_tsd_impl {
        In<TS<KEY, INPUT_G>, optional, passive> key;
        In<TS<VALUE, INPUT_G>, optional> value;
        Out<TSD<KEY, TS<VALUE, ITEM_G>, OUTPUT_G>> output;

        static RequirementResult require(const WiringArguments &arguments) { return require_tsd_output(arguments); }
        static TimeSeriesGuarantee resolve(decltype(OUTPUT_G), const WiringArguments &arguments) {
            return output_guarantee(arguments);
        }
        static TimeSeriesGuarantee resolve(decltype(ITEM_G), const WiringArguments &arguments) {
            return item_guarantee(arguments);
        }

        void evaluate() const {
            if (value.valid() && value.modified() && key.valid()) {
                output[key.value_view()].copy(value);
            }
        }
    };

    struct collect_tsd_reset_impl {
        In<TS<KEY, INPUT_G>, optional, passive> key;
        In<TS<VALUE, INPUT_G>, optional> value;
        In<SIGNAL, optional> reset;
        Out<TSD<KEY, TS<VALUE, ITEM_G>, OUTPUT_G>> output;

        static RequirementResult require(const WiringArguments &arguments) { return require_tsd_output(arguments); }
        static TimeSeriesGuarantee resolve(decltype(OUTPUT_G), const WiringArguments &arguments) {
            return output_guarantee(arguments);
        }
        static TimeSeriesGuarantee resolve(decltype(ITEM_G), const WiringArguments &arguments) {
            return item_guarantee(arguments);
        }

        void evaluate() const {
            if (reset.valid() && reset.modified()) {
                output.clear();
            }
            if (value.valid() && value.modified() && key.valid()) {
                output[key.value_view()].copy(value);
            }
        }
    };

    overload<collect, collect_tsd_impl> collect_tsd_without_reset;
    overload<collect, collect_tsd_reset_impl> collect_tsd_with_reset;
};

} // namespace

void register_collect_operators() { register_overloads<collect_overloads>(); }

} // namespace hg::operators::details
