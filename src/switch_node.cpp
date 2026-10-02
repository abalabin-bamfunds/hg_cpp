#include <bg/switch/details/switch_node.hpp>

#include <bg/allocator.hpp>
#include <bg/details/evaluation_context.hpp>
#include <bg/graph/details/compiled_graph_builder.hpp>
#include <bg/graph/details/graph_wiring_structure.hpp>
#include <bg/graph/details/nested_graph_context.hpp>
#include <bg/graph/graph_wiring.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/node/node_invocation.hpp>
#include <bg/node/runtime_signature_node_template.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/ref/ref_output.hpp>
#include <bg/time_series/ts/ts_input.hpp>
#include <bg/util/scope_exit.hpp>

#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace bg::details {
namespace {

static_assert(std::is_trivially_destructible_v<NestedGraphInput>);

struct CompiledSwitchInput {
    std::uint32_t parameter_index;
    std::uint32_t inner_input_index;
};

struct CompiledSwitchBranch {
    CompiledNestedGraph nested;
    std::pmr::vector<CompiledSwitchInput> inputs;
    std::optional<std::uint32_t> inherited_parameter_start;
};

struct CompiledSwitchCase {
    std::uint32_t first_key;
    std::uint32_t key_count;
    CompiledSwitchBranch branch;
};

struct CompiledSwitch {
    std::pmr::vector<ScalarValue<>> keys;
    std::pmr::vector<CompiledSwitchCase> cases;
    std::optional<std::uint32_t> default_case;
    bool produces_output;

    bool has_output() const noexcept { return produces_output; }
};

struct SwitchNodeIdentity {
    struct Case {
        GraphWiringStructure graph;
        std::vector<ScalarValue<>> keys;
        bool default_case;
        std::vector<std::size_t> parameter_indices;
    };

    std::vector<Case> cases;

    std::size_t hash() const noexcept {
        auto result = std::size_t{};
        const auto combine = [&](std::size_t value) {
            result ^= value + 0x9e3779b97f4a7c15ULL + (result << 6) + (result >> 2);
        };
        for (const auto &item : cases) {
            combine(item.graph.hash());
            combine(item.default_case);
            for (const auto &key : item.keys) {
                combine(key.hash());
            }
            for (const auto parameter : item.parameter_indices) {
                combine(parameter);
            }
        }
        return result;
    }

    bool cacheable() const noexcept {
        for (const auto &item : cases) {
            if (!item.graph.cacheable()) {
                return false;
            }
        }
        return true;
    }

    bool operator==(const SwitchNodeIdentity &other) const {
        if (cases.size() != other.cases.size()) {
            return false;
        }
        for (auto index = std::size_t{}; index < cases.size(); ++index) {
            const auto &left = cases[index];
            const auto &right = other.cases[index];
            if (!(left.graph == right.graph) || left.default_case != right.default_case ||
                left.parameter_indices != right.parameter_indices || left.keys.size() != right.keys.size()) {
                return false;
            }
            for (auto key_index = std::size_t{}; key_index < left.keys.size(); ++key_index) {
                if (!left.keys[key_index].equals(right.keys[key_index].view())) {
                    return false;
                }
            }
        }
        return true;
    }
};

struct SwitchSelection {
    static constexpr auto none = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t case_index{none};
    std::uint32_t key_index{none};

    bool active() const noexcept { return case_index != none; }
    bool operator==(const SwitchSelection &) const = default;
};

struct SwitchNodeState {
    NestedGraph graph;
    ScheduleHandle outer_schedule{};
    std::optional<EvaluationTime> scheduled_time;
    SwitchSelection active_selection;
};

static_assert(sizeof(SwitchNodeState) <= 96);

class SwitchNodeDefinition {
  public:
    using compiled_data_type = CompiledSwitch;
    using state_type = SwitchNodeState;

    static constexpr bool uses_cycle_callbacks = true;
    static constexpr bool uses_timed_scheduling = true;

    SwitchNodeDefinition(SwitchNodeWiring wiring, const ResolvedSignature &outer_signature)
        : wiring_{std::move(wiring)} {
        auto parameters = std::vector<Parameter>{};
        parameters.reserve(outer_signature.parameters.size());
        for (const auto &parameter : outer_signature.parameters) {
            if (!parameter.type->is_time_series()) {
                throw std::invalid_argument{"a switch runtime node accepts only time-series parameters"};
            }
            parameters.push_back({parameter.name, parameter.type, parameter.activity, parameter.validity,
                                  parameter.accepts_absent_scalar, details::VariadicMode::none, nullptr, false});
        }
        if (outer_signature.output == nullptr) {
            signature_ = std::make_unique<Signature>(outer_signature.name, std::move(parameters));
        } else {
            signature_ =
                std::make_unique<Signature>(outer_signature.name, std::move(parameters), *outer_signature.output);
        }
        validate();
    }

    const Signature &signature() const noexcept { return *signature_; }

    NodeTemplateIdentity definition_identity() const {
        auto identity = SwitchNodeIdentity{};
        identity.cases.reserve(wiring_.cases.size());
        for (const auto &item : wiring_.cases) {
            auto boundaries = std::vector<std::size_t>{};
            auto parameter_indices = std::vector<std::size_t>{};
            boundaries.reserve(item.branch.inputs.size());
            parameter_indices.reserve(item.branch.inputs.size());
            for (const auto &input : item.branch.inputs) {
                boundaries.push_back(input.input_node);
                parameter_indices.push_back(input.parameter_index);
            }
            identity.cases.push_back({
                .graph = graph_wiring_structure(*item.branch.wiring, boundaries, item.branch.output_node),
                .keys = {},
                .default_case = item.default_case,
                .parameter_indices = std::move(parameter_indices),
            });
            auto &keys = identity.cases.back().keys;
            keys.reserve(item.keys.size());
            for (const auto &key : item.keys) {
                keys.emplace_back(key->view());
            }
        }
        return NodeTemplateIdentity::from_value(std::move(identity));
    }

    CompiledSwitch compile(const ResolvedSignature &, allocator &memory) const {
        auto result = CompiledSwitch{
            .keys = std::pmr::vector<ScalarValue<>>{&memory.memory_resource()},
            .cases = std::pmr::vector<CompiledSwitchCase>{&memory.memory_resource()},
            .default_case = std::nullopt,
            .produces_output = signature_->has_output(),
        };
        auto key_count = std::size_t{};
        for (const auto &item : wiring_.cases) {
            key_count += item.keys.size();
        }
        result.keys.reserve(key_count);
        result.cases.reserve(wiring_.cases.size());

        for (auto case_index = std::size_t{}; case_index < wiring_.cases.size(); ++case_index) {
            const auto &item = wiring_.cases[case_index];
            auto node_indices = std::vector<std::size_t>{};
            node_indices.reserve(item.branch.inputs.size() +
                                 static_cast<std::size_t>(item.branch.output_node.has_value()));
            for (const auto &input : item.branch.inputs) {
                node_indices.push_back(input.input_node);
            }
            const auto output_position = node_indices.size();
            if (item.branch.output_node) {
                node_indices.push_back(*item.branch.output_node);
            }

            auto graph = compile_graph(*item.branch.wiring, memory, node_indices);
            auto compiled_case = CompiledSwitchCase{
                .first_key = static_cast<std::uint32_t>(result.keys.size()),
                .key_count = static_cast<std::uint32_t>(item.keys.size()),
                .branch =
                    {
                        .nested =
                            {
                                .graph = std::move(graph),
                                .input_nodes = std::pmr::vector<std::size_t>{&memory.memory_resource()},
                                .constant_input_nodes = std::pmr::vector<std::size_t>{&memory.memory_resource()},
                                .parent_service_binding_indices =
                                    std::pmr::vector<std::uint32_t>{&memory.memory_resource()},
                                .output_node = item.branch.output_node ? std::optional{node_indices[output_position]}
                                                                       : std::nullopt,
                            },
                        .inputs = std::pmr::vector<CompiledSwitchInput>{&memory.memory_resource()},
                        .inherited_parameter_start =
                            item.branch.inherited_parameter_start
                                ? std::optional{static_cast<std::uint32_t>(*item.branch.inherited_parameter_start)}
                                : std::nullopt,
                    },
            };
            for (const auto &key : item.keys) {
                result.keys.emplace_back(key->view(), memory);
            }
            compiled_case.branch.nested.input_nodes.reserve(item.branch.inputs.size());
            compiled_case.branch.inputs.reserve(item.branch.inputs.size());
            for (auto input_index = std::size_t{}; input_index < item.branch.inputs.size(); ++input_index) {
                compiled_case.branch.nested.input_nodes.push_back(node_indices[input_index]);
                compiled_case.branch.inputs.push_back(
                    {static_cast<std::uint32_t>(item.branch.inputs[input_index].parameter_index),
                     static_cast<std::uint32_t>(input_index)});
            }
            for (const auto &requirement : item.branch.wiring->service_binding_requirements()) {
                compiled_case.branch.nested.parent_service_binding_indices.push_back(
                    static_cast<std::uint32_t>(*requirement.parent_requirement));
            }
            if (item.default_case) {
                result.default_case = static_cast<std::uint32_t>(case_index);
            }
            result.cases.push_back(std::move(compiled_case));
        }
        return result;
    }

    static void start(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        invocation.context().schedule_now();
    }

    static bool evaluate(NodeInvocation<compiled_data_type, state_type> invocation) {
        auto &state = invocation.state();
        const auto &compiled = invocation.compiled_data();
        if (state.graph.evaluation_pending()) {
            return resume(compiled, invocation);
        }

        const auto selected = select_case(compiled, invocation);

        if (selected != state.active_selection) {
            stop_active(invocation.context(), state);
            if (selected.active()) {
                start_case(compiled.cases[selected.case_index].branch, invocation, selected);
            }
        } else if (selected.active()) {
            update_inputs(compiled.cases[selected.case_index].branch, invocation);
        }

        return resume(compiled, invocation);
    }

    template <typename Invocation> static bool resume(const CompiledSwitch &compiled, Invocation invocation) {
        auto &state = invocation.state();
        if (!state.graph.evaluate(invocation.context())) {
            return false;
        }

        if (compiled.has_output()) {
            const auto &branch = compiled.cases[state.active_selection.case_index].branch;
            invocation.output().visit(
                [&](Out<REF<TS_T>> *output) {
                    if (!branch.nested.output_node) {
                        output->invalidate();
                        return;
                    }
                    const auto value = state.graph.output(branch.nested);
                    if (value) {
                        output->set_value(value);
                    } else {
                        output->invalidate();
                    }
                },
                ddv::noop);
        }
        return true;
    }

    static void stop(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        auto &state = invocation.state();
        clear_timed_schedule(invocation.context(), state);
        state.graph.stop(invocation.context());
        state.active_selection = {};
    }

    static void destroy_state(NodeInvocation<compiled_data_type, state_type> invocation) noexcept {
        auto &state = invocation.state();
        clear_timed_schedule(invocation.context(), state);
        state.graph.stop_and_destroy(invocation.context());
    }

    static void inner_graph_scheduled(void *, RuntimeGraphOwnerCallbacks::token_type) noexcept {}

    static void inner_graph_timed_schedule_changed(RuntimeGraphOwnerContext &context, void *receiver,
                                                   RuntimeGraphOwnerCallbacks::token_type,
                                                   std::optional<EvaluationTime> time) {
        auto &state = *static_cast<SwitchNodeState *>(receiver);
        if (state.scheduled_time) {
            context.cancel(state.outer_schedule);
            state.outer_schedule = {};
        }
        state.scheduled_time = time;
        if (time) {
            state.outer_schedule = context.schedule_at(*time);
        }
    }

    static constexpr RuntimeGraphOwnerCallbacks owner_callbacks{
        .inner_scheduled = inner_graph_scheduled,
        .timed_schedule_changed = inner_graph_timed_schedule_changed,
    };

  private:
    template <typename Invocation>
    static SwitchSelection select_case(const CompiledSwitch &compiled, Invocation invocation) {
        const auto key = invocation.template input<In<TS<SCALAR>>>(0, 0);
        if (key.valid()) {
            for (auto index = std::uint32_t{}; index < compiled.cases.size(); ++index) {
                const auto &item = compiled.cases[index];
                for (auto offset = std::uint32_t{}; offset < item.key_count; ++offset) {
                    const auto key_index = item.first_key + offset;
                    if (compiled.keys[key_index].equals(key.value_view())) {
                        return {.case_index = index, .key_index = key_index};
                    }
                }
            }
        }
        if (compiled.default_case) {
            return {.case_index = *compiled.default_case};
        }
        throw std::out_of_range{"switch_ key does not match a case and no DEFAULT case is defined"};
    }

    template <typename Invocation>
    static NestedGraphInput input_reference(Invocation invocation, std::size_t parameter_index) {
        const auto input = invocation.template input<In<REF<TS_T>>>(parameter_index, parameter_index);
        return {input.valid() ? input.value() : Reference{}};
    }

    template <typename Invocation>
    static void start_case(const CompiledSwitchBranch &branch, Invocation invocation, SwitchSelection selection) {
        const auto inherited_count = branch.nested.graph->inherited_time_series_count();
        const auto input_count = branch.inputs.size();
        const auto storage_size = sizeof(NestedGraphInput) * input_count;
        auto &scratch = invocation.context().scratch();
        auto *inputs = input_count == 0
                           ? nullptr
                           : static_cast<NestedGraphInput *>(scratch.allocate(storage_size, alignof(NestedGraphInput)));
        auto release_inputs = util::scope_exit{[&]() noexcept {
            if (inputs != nullptr) {
                scratch.release_last(inputs, storage_size);
            }
        }};
        for (auto index = std::size_t{}; index < branch.inputs.size(); ++index) {
            std::construct_at(inputs + index, input_reference(invocation, branch.inputs[index].parameter_index));
        }
        auto &state = invocation.state();
        const auto owner = RuntimeGraphOwner{
            .receiver = &state,
            .token = selection.case_index,
            .callbacks = &owner_callbacks,
        };
        const auto inherited_start = branch.inherited_parameter_start.value_or(invocation.parameter_count());
        auto startup_context = NestedGraphStartupContext{invocation, branch.nested, inherited_start, inherited_count};
        state.graph.start(branch.nested, startup_context.context(),
                          std::span<NestedGraphInput>{inputs, branch.inputs.size()}, {}, owner);
        state.active_selection = selection;
    }

    template <typename Invocation>
    static void update_inputs(const CompiledSwitchBranch &branch, Invocation invocation) {
        for (const auto &input : branch.inputs) {
            branch_input_update(branch, invocation, input);
        }
    }

    template <typename Invocation>
    static void branch_input_update(const CompiledSwitchBranch &branch, Invocation invocation,
                                    const CompiledSwitchInput &input) {
        invocation.state().graph.update_input(branch.nested, input.inner_input_index,
                                              input_reference(invocation, input.parameter_index), invocation.context());
    }

    static void stop_active(EvaluationContext &context, SwitchNodeState &state) {
        if (!state.active_selection.active()) {
            return;
        }
        clear_timed_schedule(context, state);
        state.graph.stop(context);
        state.active_selection = {};
    }

    static void clear_timed_schedule(EvaluationContext &context, SwitchNodeState &state) noexcept {
        if (state.scheduled_time) {
            context.cancel(state.outer_schedule);
            state.outer_schedule = {};
        }
        state.scheduled_time.reset();
    }

    void validate() const {
        if (wiring_.cases.empty()) {
            throw std::invalid_argument{"a switch runtime node requires at least one case"};
        }
        if (signature_->parameter_count() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument{"a switch runtime node has too many parameters"};
        }
        if (wiring_.cases.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument{"a switch runtime node has too many cases"};
        }
        auto key_count = std::size_t{};
        for (const auto &item : wiring_.cases) {
            key_count += item.keys.size();
            if (key_count > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument{"a switch runtime node has too many case keys"};
            }
            if (item.branch.wiring == nullptr) {
                throw std::invalid_argument{"a switch case requires inner graph wiring"};
            }
            const auto inherited_count = item.branch.wiring->inherited_requirements().size();
            if (item.branch.inherited_parameter_start.has_value() != (inherited_count != 0) ||
                (inherited_count != 0 &&
                 *item.branch.inherited_parameter_start + inherited_count > signature_->parameter_count())) {
                throw std::invalid_argument{"a switch case has invalid inherited time series parameters"};
            }
            const auto outputless_default = signature_->has_output() && item.default_case && !item.branch.output_node;
            if (item.branch.output_node.has_value() != signature_->has_output() && !outputless_default) {
                throw std::invalid_argument{"a switch case output does not match the switch signature"};
            }
            for (const auto &input : item.branch.inputs) {
                if (input.parameter_index == 0 || input.parameter_index >= signature_->parameter_count()) {
                    throw std::invalid_argument{"a switch case input parameter index is out of range"};
                }
            }
        }
    }

    SwitchNodeWiring wiring_;
    std::unique_ptr<Signature> signature_;
};

} // namespace

std::unique_ptr<NodeTemplate> make_switch_node(SwitchNodeWiring wiring, const ResolvedSignature &outer_signature) {
    return std::make_unique<RuntimeSignatureNodeTemplate<SwitchNodeDefinition>>(
        SwitchNodeDefinition{std::move(wiring), outer_signature});
}

} // namespace bg::details
