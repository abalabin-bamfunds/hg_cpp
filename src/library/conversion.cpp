#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/set/scalar.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tsl/tsl_type.hpp>

#include <cstddef>
#include <stdexcept>
#include <type_traits>

namespace bg::operators::details {
namespace {

inline constexpr auto G = guarantee_variable<"CONVERSION_G">;
using KEY = ScalarVariable<"CONVERSION_KEY", Hashable>;
using SIZE_DECLARATION = std::remove_cv_t<decltype(SIZE)>;

struct conversion_overloads {
    static const TimeSeriesType *requested_result(const WiringArguments &arguments) {
        const auto *mapping = arguments.type_map().find(::bg::details::type_parameter<CONVERT_RESULT>());
        const auto *requested_mapping = mapping == nullptr ? nullptr : std::get_if<const Type *>(mapping);
        return requested_mapping == nullptr || *requested_mapping == nullptr
                   ? nullptr
                   : type_cast<TimeSeriesType>(*requested_mapping);
    }

    static RequirementResult require_convert_result(const WiringArguments &arguments) {
        const auto *requested = requested_result(arguments);
        const auto *resolved = arguments.resolved_output_type();
        if (requested == nullptr || resolved == nullptr || !requested->accepts(*resolved)) {
            return reject("convert requires a compatible explicit time-series result type");
        }
        return accept;
    }

    static std::size_t resolve_list_size(SIZE_DECLARATION, const WiringArguments &arguments) {
        const auto *requested = requested_result(arguments);
        const auto *list = requested == nullptr ? nullptr : type_cast<TimeSeriesListType>(&requested->target_type());
        return list == nullptr ? std::dynamic_extent : list->item_count();
    }

    static RequirementResult require_fixed_list_result(const WiringArguments &arguments) {
        const auto *requested = requested_result(arguments);
        const auto *resolved = arguments.resolved_output_type();
        const auto *list = requested == nullptr ? nullptr : type_cast<TimeSeriesListType>(&requested->target_type());
        if (list == nullptr || list->item_count() == std::dynamic_extent || resolved == nullptr ||
            !requested->accepts(*resolved)) {
            return reject("convert to TSL requires a compatible explicit fixed TSL result type");
        }
        return accept;
    }

    // Copy a scalar list into a TSL snapshot.
    struct list_to_tsl {
        In<TS<List<SCALAR>, G>> value;
        Out<TSL<TS<SCALAR, G>, SIZE, G>> output;

        static std::size_t resolve(SIZE_DECLARATION, const WiringArguments &arguments) {
            return resolve_list_size(SIZE, arguments);
        }

        static RequirementResult require(const WiringArguments &arguments) {
            return require_fixed_list_result(arguments);
        }

        void evaluate() const {
            const auto values = value.value_view();
            if (values.size() != output.size()) {
                throw std::invalid_argument{"convert input list size does not match the fixed TSL result size"};
            }
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                output[index].set_value(values[index]);
            }
        }
    };

    overload<convert, list_to_tsl> convert_list_to_tsl;

    // Copy scalar collection members into a TSS snapshot.
    struct list_to_tss {
        In<TS<List<KEY>, G>> value;
        Out<TSS<KEY, G>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return require_convert_result(arguments);
        }

        void evaluate() const {
            const auto values = value.value_view();
            if (!output.valid()) {
                output.clear();
            }
            output.for_each([&](const auto &existing) {
                auto found = false;
                for (auto index = std::size_t{}; index < values.size(); ++index) {
                    if (bg::details::scalar_data(values[index]).equals(bg::details::scalar_data(existing))) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    output.erase(existing);
                }
            });
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                output.insert(values[index]);
            }
        }
    };

    overload<convert, list_to_tss> convert_list_to_tss;

    struct set_to_tss {
        In<TS<Set<KEY>, G>> value;
        Out<TSS<KEY, G>> output;

        static RequirementResult require(const WiringArguments &arguments) {
            return require_convert_result(arguments);
        }

        void evaluate() const {
            const auto values = value.value_view();
            if (!output.valid()) {
                output.clear();
            }
            output.for_each([&](const auto &existing) {
                if (!values.contains(existing)) {
                    output.erase(existing);
                }
            });
            values.for_each([&](const auto &item) { output.insert(item); });
        }
    };

    overload<convert, set_to_tss> convert_set_to_tss;

    overload<combine> combine_tsl{graph([](wp<TSL<TS_T, SIZE>, var_args> values) { return values; })};
};

} // namespace

void register_conversion_operators() { register_overloads<conversion_overloads>(); }

} // namespace bg::operators::details
