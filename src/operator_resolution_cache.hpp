#pragma once

#include <bg/graph/wiring_argument.hpp>
#include <bg/graph/wiring_template.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/type_map.hpp>

#include <cstddef>
#include <deque>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace bg::details {

struct OperatorArgumentKey {
    const Type *type{};
    std::optional<GraphArgumentStructures> graph_structures;
    bool present;
    std::optional<util::symbol> name;
    bool requires_optional{};
    bool requires_valid_items{};
    VariadicMode variadic_capture_mode{VariadicMode::none};
    bool has_default_materialisation{};
    std::vector<std::pair<const TimeSeriesType *, double>> materialisation_options;
    std::size_t source_group{static_cast<std::size_t>(-1)};

    bool operator==(const OperatorArgumentKey &other) const {
        if (type != other.type || present != other.present || name != other.name ||
            requires_optional != other.requires_optional || requires_valid_items != other.requires_valid_items ||
            variadic_capture_mode != other.variadic_capture_mode ||
            static_cast<bool>(graph_structures) != static_cast<bool>(other.graph_structures) ||
            has_default_materialisation != other.has_default_materialisation ||
            materialisation_options != other.materialisation_options || source_group != other.source_group) {
            return false;
        }
        if (graph_structures) {
            return *graph_structures == *other.graph_structures;
        }
        return true;
    }
};

struct OperatorScalarRead {
    const WiringTemplate *implementation;
    std::size_t index;
    ScalarValue<> value;
};

struct OperatorResolutionCache {
    static constexpr auto maximum_entries = std::size_t{64};

    struct Entry {
        std::vector<OperatorArgumentKey> arguments;
        std::optional<TypeMap> seed_type_map;
        std::vector<OperatorScalarRead> scalar_reads;
        const WiringTemplate *implementation;
        SignatureResolution resolution;
        std::vector<const TimeSeriesType *> argument_forms;
    };

    std::shared_ptr<const void> operator_identity;
    std::size_t overload_count{};
    std::deque<Entry> entries;
};

} // namespace bg::details
