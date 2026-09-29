#include "higher_order.hpp"

#include "wiring_argument.hpp"

#include <bg/graph/wiring_context.hpp>
#include <bg/map.hpp>
#include <bg/mesh.hpp>
#include <bg/mesh/details/default_mesh_wiring.hpp>
#include <bg/python/graph_definition.hpp>
#include <bg/python/wiring_point.hpp>
#include <bg/switch.hpp>
#include <bg/util/named_argument.hpp>

#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>

#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nb = nanobind;

namespace bg::python::details {
namespace {

class PythonArguments {
  public:
    PythonArguments(GraphWiring &wiring, allocator &memory) noexcept : wiring_{&wiring}, memory_{&memory} {}

    std::vector<MapArgument> map_arguments(nb::sequence positional, nb::sequence named) const {
        auto result = std::vector<MapArgument>{};
        result.reserve(nb::len(positional) + nb::len(named));

        for (const auto entry : positional) {
            const auto item = nb::cast<nb::tuple>(entry);
            result.push_back({argument(item[0]), nb::cast<bool>(item[1])});
        }
        for (const auto entry : named) {
            const auto item = nb::cast<nb::tuple>(entry);
            result.push_back({argument(item[1], name(item[0])), nb::cast<bool>(item[2])});
        }
        return result;
    }

    std::vector<WiringArgument> ordinary_arguments(nb::sequence positional, nb::sequence named) const {
        auto result = std::vector<WiringArgument>{};
        result.reserve(nb::len(positional) + nb::len(named));

        for (const auto value : positional) {
            result.push_back(argument(value));
        }
        for (const auto entry : named) {
            const auto item = nb::cast<nb::tuple>(entry);
            result.push_back(argument(item[1], name(item[0])));
        }
        return result;
    }

    WiringArgument argument(nb::handle value) const {
        return wiring_argument(value, *memory_, *wiring_);
    }

    WiringArgument argument(nb::handle value, util::symbol argument_name) const {
        return WiringArgument{argument_name, wiring_argument(value, *memory_, *wiring_)};
    }

  private:
    static util::symbol name(nb::handle value) {
        const auto text = nb::cast<std::string>(value);
        return util::symbol{std::string_view{text}};
    }

    GraphWiring *wiring_;
    allocator *memory_;
};

nb::object wiring_result(WiringPoint result) {
    if (!result.has_output()) {
        return nb::none();
    }
    return nb::cast(PythonWiringPoint{result, PythonWiringScope::current()});
}

nb::object wire_map(const PythonGraphDefinition &function, nb::sequence positional, nb::sequence named) {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto arguments = PythonArguments{wiring, memory}.map_arguments(positional, named);

    return wiring_result(static_cast<WiringPoint>(
        bg::map(static_cast<const WiringTemplate &>(function), std::span<const MapArgument>{arguments})));
}

nb::object wire_mesh(const PythonGraphDefinition &function, nb::sequence positional, nb::sequence named,
                     std::optional<std::string> name) {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto arguments = PythonArguments{wiring, memory}.map_arguments(positional, named);

    if (name) {
        auto metadata = bg::details::named_argument<"name", std::string>{std::move(*name)};
        return wiring_result(static_cast<WiringPoint>(bg::mesh(
            std::move(metadata), static_cast<const WiringTemplate &>(function),
            std::span<const MapArgument>{arguments})));
    }
    return wiring_result(static_cast<WiringPoint>(
        bg::mesh(static_cast<const WiringTemplate &>(function), std::span<const MapArgument>{arguments})));
}

nb::object wire_mesh_dependency(nb::handle key, const std::string &name) {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto argument = PythonArguments{wiring, memory}.argument(key);

    return wiring_result(bg::details::mesh_dependency_selection(std::move(argument), name));
}

nb::object wire_switch(const PythonWiringPoint &key, nb::sequence case_values, nb::sequence positional,
                       nb::sequence named) {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto builder = PythonArguments{wiring, memory};

    auto cases = std::vector<SwitchCaseArgument>{};
    cases.reserve(nb::len(case_values));
    for (const auto value : case_values) {
        const auto item = nb::cast<nb::tuple>(value);
        const auto &function = nb::cast<const PythonGraphDefinition &>(item[2]);
        if (nb::cast<bool>(item[0])) {
            cases.emplace_back(DEFAULT, function);
            continue;
        }

        const auto keys = nb::cast<nb::sequence>(item[1]);
        auto arguments = std::vector<WiringArgument>{};
        arguments.reserve(nb::len(keys));
        for (const auto case_key : keys) {
            arguments.push_back(builder.argument(case_key));
        }
        cases.emplace_back(std::span<const WiringArgument>{arguments}, function);
    }

    auto arguments = builder.ordinary_arguments(positional, named);
    return wiring_result(static_cast<WiringPoint>(
        bg::switch_(key.get(wiring), std::span<const SwitchCaseArgument>{cases},
                    std::span<const WiringArgument>{arguments})));
}

} // namespace

void bind_higher_order(nb::module_ &module) {
    module.def("_map", &wire_map);
    module.def("_mesh", &wire_mesh);
    module.def("_mesh_dependency", &wire_mesh_dependency);
    module.def("_switch", &wire_switch);
}

} // namespace bg::python::details
