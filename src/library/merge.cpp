#include "registration.hpp"

#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/map.hpp>
#include <bg/node.hpp>
#include <bg/overload.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsd/tsd_type.hpp>
#include <bg/time_series/tsl/tsl.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/time_series/tss/tss_output.hpp>
#include <bg/time_series/tss/tss_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_list_type.hpp>

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

namespace bg::operators::details {
namespace {

using namespace bg::literals;

inline constexpr auto N = SizeVariableDeclaration<"N">{};

struct merge_overloads {
    static const TimeSeriesType *merge_output_type(const WiringArguments &arguments) {
        const TimeSeriesType *common_input = nullptr;
        for (auto index = std::size_t{}; index < arguments.size(); ++index) {
            const auto &input = type_cast<TimeSeriesType>(arguments.original_type(index)).dereferenced_type();
            common_input = common_input == nullptr ? &input : common_input->common_type(input);
            if (common_input == nullptr) {
                return nullptr;
            }
        }
        if (common_input == nullptr || arguments.size() == 1) {
            return common_input;
        }
        return &common_input->with_guarantee_limit(TimeSeriesGuarantee::ordinary);
    }

    struct merge_scalar_impl {
        In<TSL<TS<SCALAR>, SIZE>, var_args, optional> values;
        Out<TS<SCALAR>> output;

        void evaluate() const {
            auto selected = std::numeric_limits<item_id_t>::max();
            values.for_each_updated([&](item_id_t index) {
                if (values[index].valid()) {
                    selected = std::min(selected, index);
                }
            });

            if (selected != std::numeric_limits<item_id_t>::max()) {
                output.copy_tick(values[selected]);
            }
        }
    };

    // Scalar time series select the leftmost valid input that ticks.
    overload<merge> merge_scalar{graph(
        require = [](const WiringArguments &arguments) {
            if (arguments.size() == 0) {
                return reject("merge requires at least one input");
            }
            return accept;
        },
        [](wp<TSL<TS<SCALAR>, SIZE>, "values"_n, var_args, optional> values) -> wp<TS<SCALAR>> {
            if (values.size() == 1) {
                return wp<TS<SCALAR>>::from_validated(values[0].time_series());
            }
            static const auto impl = node<merge_scalar_impl>();
            return wp<TS<SCALAR>>::from_validated(impl(values));
        })};

    // Fixed lists merge corresponding items independently.
    struct merge_lists_graph {
        wp<TSL<TSL<TS_T, N>, SIZE>, "values"_n, var_args, optional> values;

        static RequirementResult require(const WiringArguments &arguments) {
            if (arguments.size() == 0) {
                return reject("merge requires at least one input");
            }

            const auto &input = type_cast<TimeSeriesType>(arguments.original_type(0)).dereferenced_type();
            const auto &list_type = type_cast<TimeSeriesListType>(input);
            if (list_type.item_count() == std::dynamic_extent) {
                return reject("merge requires fixed-size TSL inputs");
            }

            return accept;
        }

        wp<OUT> wire() const {
            if (values.size() == 1) {
                return wp<OUT>{values[0].time_series()};
            }

            const auto first = values[0].time_series().as<TSL<TS_T, N>>();
            if (first.size() == 0) {
                return wp<OUT>{first};
            }

            auto merged_items = std::vector<WiringPoint>{};
            merged_items.reserve(first.size());
            for (auto item_index = std::size_t{}; item_index < first.size(); ++item_index) {
                auto inputs = std::vector<WiringPoint>{};
                inputs.reserve(values.size());
                for (auto input_index = std::size_t{}; input_index < values.size(); ++input_index) {
                    inputs.push_back(operators::getitem(
                        values[input_index].time_series(), static_cast<i64>(item_index)));
                }
                merged_items.push_back(operators::merge(std::span<const WiringPoint>{inputs}));
            }

            return wp<OUT>{tsl(std::span<const WiringPoint>{merged_items})};
        }

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            return merge_overloads::merge_output_type(arguments);
        }
    };

    overload<merge> merge_lists{graph<merge_lists_graph>()};

    // Fixed bundles merge corresponding fields independently.
    struct merge_bundles_graph {
        wp<TSL<TSB<SCHEMA>, SIZE>, "values"_n, var_args, optional> values;

        static RequirementResult require(const WiringArguments &arguments) {
            return arguments.size() == 0 ? reject("merge requires at least one input") : accept;
        }

        wp<OUT> wire() const {
            const auto &outer_type = type_cast<TimeSeriesListType>(values.type());
            const auto &bundle_type = type_cast<TimeSeriesBundleType>(outer_type.item_type().target_type());
            if (values.size() == 1) {
                return wp<OUT>{values[0].time_series()};
            }

            auto fields = std::vector<WiringArgument>{};
            fields.reserve(bundle_type.item_count());
            for (auto field_index = std::size_t{}; field_index < bundle_type.item_count(); ++field_index) {
                const auto name = bundle_type.item_name(field_index);
                auto inputs = std::vector<WiringPoint>{};
                inputs.reserve(values.size());
                for (auto input_index = std::size_t{}; input_index < values.size(); ++input_index) {
                    inputs.push_back(operators::getattr(values[input_index].time_series(), name));
                }
                fields.emplace_back(name, operators::merge(std::span<const WiringPoint>{inputs}));
            }

            return wp<OUT>{bundle(std::span<const WiringArgument>{fields})};
        }

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            return merge_overloads::merge_output_type(arguments);
        }
    };

    overload<merge> merge_bundles{graph<merge_bundles_graph>()};

    // Dictionaries merge values for the union of their keys.
    struct merge_dictionary_items_graph {
        wp<TSL<TS_T, N>, "values"_n, var_args, optional> values;

        wp<OUT> wire() const { return wp<OUT>{operators::merge(values)}; }

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            return merge_overloads::merge_output_type(arguments);
        }
    };

    struct merge_dictionaries_graph {
        wp<TSL<TSD<SCALAR, TS_T>, SIZE>, "values"_n, var_args, optional> values;

        static RequirementResult require(const WiringArguments &arguments) {
            return arguments.size() == 0 ? reject("merge requires at least one input") : accept;
        }

        wp<OUT> wire() const {
            if (values.size() == 1) {
                return wp<OUT>{values[0].time_series()};
            }

            auto arguments = std::vector<MapArgument>{};
            arguments.reserve(values.size());
            for (auto index = std::size_t{}; index < values.size(); ++index) {
                auto value = values[index];
                value.clear_name();
                arguments.push_back({std::move(value), false});
            }
            static const auto merge_items = graph<merge_dictionary_items_graph>();
            return wp<OUT>{bg::map(merge_items, std::span<const MapArgument>{arguments})};
        }

        static const TimeSeriesType *resolve(OUT, const WiringArguments &arguments) {
            return merge_output_type(arguments);
        }
    };

    overload<merge> merge_dictionaries{graph<merge_dictionaries_graph>()};

    struct merge_set_impl {
        In<TSL<TSS<SCALAR>, SIZE>, var_args, optional> values;
        Out<TSS<SCALAR>> output;

        void evaluate() const {
            if (!values.valid()) {
                if (output.valid()) {
                    output.clear();
                }
                return;
            }

            if (!output.valid()) {
                output.clear();
                for (auto index = std::size_t{}; index < values.size(); ++index) {
                    values[index].for_each([&](const auto &key) { output.insert(key); });
                }
                return;
            }

            values.for_each_updated([&](item_id_t index) {
                values[index].for_each_added([&](const auto &key) { output.insert(key); });
            });

            values.for_each_updated([&](item_id_t index) {
                values[index].for_each_removed([&](const auto &key) {
                    auto present = false;
                    for (auto other = std::size_t{}; other < values.size(); ++other) {
                        if (values[other].contains(key)) {
                            present = true;
                            break;
                        }
                    }

                    if (!present) {
                        output.erase(key);
                    }
                });
            });
        }
    };

    // Sets merge membership changes without publishing keys that remain in another input.
    overload<merge> merge_sets{graph(
        require = [](const WiringArguments &arguments) {
            return arguments.size() == 0 ? reject("merge requires at least one input") : accept;
        },
        [](wp<TSL<TSS<SCALAR>, SIZE>, "values"_n, var_args, optional> values) -> wp<TSS<SCALAR>> {
            if (values.size() == 1) {
                return wp<TSS<SCALAR>>::from_validated(values[0].time_series());
            }
            static const auto impl = node<merge_set_impl>();
            return wp<TSS<SCALAR>>::from_validated(impl(values));
        })};
};

} // namespace

void register_merge_operators() {
    register_overloads<merge_overloads>();
}

} // namespace bg::operators::details
