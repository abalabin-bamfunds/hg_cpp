#include "higher_order.hpp"

#include "python_wiring_error.hpp"
#include "wiring_argument.hpp"

#include <bg/dispatch.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/wiring_context.hpp>
#include <bg/map.hpp>
#include <bg/mesh.hpp>
#include <bg/mesh/details/default_mesh_wiring.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
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

class PythonDispatchTemplate final : public WiringTemplate {
  public:
    PythonDispatchTemplate(const Signature &signature, std::span<const WiringTemplate *const> implementations)
        : signature_{&signature}, implementations_{implementations}, resolver_{signature} {}

    const Signature &default_signature() const override { return *signature_; }

    std::optional<WiringPoint> wire(GraphWiring &wiring, ResolvedSignature signature, const WiringArguments &arguments,
                                    std::span<const WiringArgument> supplied) const override {
        return bg::details::wire_dispatch(wiring, *signature_, signature_->has_output(), signature.output,
                                          *signature_, supplied, implementations_, resolver_, arguments.type_map(), {});
    }

  private:
    const Signature *signature_;
    std::span<const WiringTemplate *const> implementations_;
    Operator resolver_;
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
        bg::switch_(key.argument(wiring), std::span<const SwitchCaseArgument>{cases},
                    std::span<const WiringArgument>{arguments})));
}

nb::object wire_dispatch(const PythonGraphDefinition &contract, nb::sequence implementations,
                         nb::sequence fixed, nb::sequence positional, nb::dict named) {
    auto &wiring = WiringContext::current().wiring();
    auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
    auto memory = allocator{temporary_memory};
    auto arguments = wiring_arguments(contract.signature(), fixed, positional, named, memory, wiring,
                                      contract.wiring_parameter_types());

    auto implementation_templates = std::vector<const WiringTemplate *>{};
    implementation_templates.reserve(nb::len(implementations));
    const auto scope = PythonWiringScope::current();
    for (const auto implementation : implementations) {
        const auto native = nb::getattr(implementation, "_native_definition");
        if (nb::isinstance<PythonGraphDefinition>(native)) {
            implementation_templates.push_back(&nb::cast<const PythonGraphDefinition &>(native));
            continue;
        }
        if (!nb::isinstance<PythonNodeDefinition>(native)) {
            throw std::invalid_argument{"dispatch implementations must be Python graph or node definitions"};
        }

        const auto &definition = nb::cast<const PythonNodeDefinition &>(native);
        auto *node_template = scope->node_template(definition.identity());
        if (node_template == nullptr) {
            auto retained = std::make_unique<RuntimeSignatureNodeTemplate<PythonNodeDefinition>>(
                definition.retain_function());
            node_template = &wiring.own_node_template(std::move(retained));
            scope->add_node_template(definition.identity(), *node_template);
        }
        implementation_templates.push_back(node_template);
    }

    auto dispatch = PythonDispatchTemplate{contract.signature(), implementation_templates};
    try {
        const auto result = static_cast<const WiringTemplate &>(dispatch)(
            std::span<const WiringArgument>{arguments});
        return result ? wiring_result(*result) : nb::none();
    } catch (const nb::python_error &) {
        throw;
    } catch (const WiringError &error) {
        if (has_python_error(error)) {
            rethrow_python_error(error);
        }
        throw std::invalid_argument{std::string{"cannot wire dispatch: "} + error.what()};
    } catch (const std::exception &error) {
        throw std::invalid_argument{std::string{"cannot wire dispatch: "} + error.what()};
    }
}

} // namespace

void bind_higher_order(nb::module_ &module) {
    module.def("_map", &wire_map);
    module.def("_mesh", &wire_mesh);
    module.def("_mesh_dependency", &wire_mesh_dependency);
    module.def("_switch", &wire_switch);
    module.def("_dispatch", &wire_dispatch);
}

} // namespace bg::python::details
