#include "expression_evaluation.hpp"

#include <hg/frame/frame.hpp>
#include <hg/frame/expression.hpp>
#include <hg/frame/tsc.hpp>
#include <hg/graph.hpp>
#include <hg/graph/details/wiring_record.hpp>
#include <hg/graph/runtime_graph.hpp>
#include <hg/library/operators.hpp>
#include <hg/node.hpp>
#include <hg/scalar/bundle/value.hpp>
#include <hg/scalar/opaque.hpp>
#include <hg/time_series/tsb/tsb.hpp>
#include <hg/time_series/tsb/details/wiring.hpp>
#include <hg/util/scope_exit.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace hg::frame {
namespace {

using namespace hg::literals;
using namespace hg::frame::details;

template <cell_type CELL> ErasedColumnSeriesView read_column(const TimeSeriesInputData &data) {
    const auto input = In<TSC<CELL>>{data};
    return ErasedColumnSeriesView{input.extension_descriptor().storage};
}

template <cell_type CELL> RowChanges column_changes(const TimeSeriesInputData &data) {
    return In<TSC<CELL>>{data}.changes();
}

template <cell_type CELL, cell_type RESULT>
bool bind_column(const void *output, const TimeSeriesInputData &data) {
    const auto input = In<TSC<CELL>>{data};
    return static_cast<const Out<TSC<RESULT>, computed<>> *>(output)->bind(input);
}

template <cell_type CELL> void read_parameter(Scalar<> value, void *destination) {
    std::construct_at(static_cast<cell_value_t<CELL> *>(destination), Scalar<CELL>::from(value).value());
}

template <cell_type RESULT> ColumnBinding column_binding(const Type &type, std::size_t index) {
    const auto *column = dynamic_cast<const ColumnSeriesType *>(std::addressof(type));
    if (column == nullptr)
        throw WiringError{"column expression input is not a concrete column"};
    try {
        return visit_cell_type(column->cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
            return ColumnBinding{index, {&read_column<Cell>, &bind_column<Cell, RESULT>, &column_changes<Cell>}};
        });
    } catch (const std::invalid_argument &) {
        throw WiringError{"column expression input has an unsupported cell type"};
    }
}

ColumnBinding input_column_binding(const Type &type, std::size_t index) {
    const auto &column = static_cast<const ColumnSeriesType &>(type);
    return visit_cell_type(column.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
        return ColumnBinding{index, {&read_column<Cell>, nullptr, &column_changes<Cell>}};
    });
}

ParameterBinding parameter_binding(const ScalarType &type, std::size_t index) {
    try {
        return visit_cell_type(type, [&]<typename Cell>(std::type_identity<Cell>) {
            using Value = cell_value_t<Cell>;
            return ParameterBinding{index, 0, sizeof(Value), alignof(Value), &read_parameter<Cell>};
        });
    } catch (const std::invalid_argument &) {
        throw WiringError{"column expression parameter has an unsupported cell type"};
    }
}

const TimeSeriesType &column_series_type(const ScalarType &type) {
    try {
        return visit_cell_type(type, []<typename Cell>(std::type_identity<Cell>) -> const TimeSeriesType & {
            return TSC<Cell>::type();
        });
    } catch (const std::invalid_argument &) {
        throw WiringError{"column expression input has an unsupported cell type"};
    }
}

const ScalarType *expression_cell_type(const Type &type) {
    const auto &resolved_type = static_cast<const TimeSeriesType &>(type).dereferenced_type();
    if (const auto *column = dynamic_cast<const ColumnSeriesType *>(std::addressof(resolved_type)))
        return std::addressof(column->cell_type());
    if (const auto *value = dynamic_cast<const TimeSeriesValueType *>(std::addressof(resolved_type)))
        return std::addressof(value->value_type());
    if (details::is_column_expression_bundle_type(resolved_type)) {
        const auto &bundle = static_cast<const TimeSeriesBundleType &>(resolved_type);
        const auto &compute = bundle.item_type(bundle.item_count() - 1).dereferenced_type();
        const auto &value = static_cast<const TimeSeriesValueType &>(compute);
        return std::addressof(static_cast<const ColumnBlockFunctionType &>(value.value_type()).result_type());
    }
    return nullptr;
}

struct ParameterUpdateNode {
    In<TSB<PARAMETER_INPUTS>, all_valid> parameters;
    Out<TS<Bundle<PARAMETERS>>> output;

    static const Schema &resolve(schema_type<PARAMETERS>, const WiringArguments &arguments) {
        const auto &input = static_cast<const TimeSeriesBundleType &>(arguments.resolved_type("parameters"));
        auto fields = std::vector<SchemaField>{};
        fields.reserve(input.item_count());
        for (auto index = std::size_t{}; index != input.item_count(); ++index) {
            const auto &item_type = input.item_type(index).dereferenced_type();
            const auto *value_type = dynamic_cast<const TimeSeriesValueType *>(std::addressof(item_type));
            if (value_type == nullptr)
                throw WiringError{"column expression parameters must be scalar time series"};
            fields.push_back({input.item_name(index), std::addressof(value_type->value_type()), false});
        }
        return anonymous_schema(fields);
    }

    void evaluate() const {
        const auto &result_type = static_cast<const ResolvedHierarchicalScalarType &>(
            static_cast<const TimeSeriesValueRuntimeType &>(output.runtime_type()).scalar_runtime_type().type());
        auto fields = std::vector<std::optional<Scalar<>>>{};
        fields.reserve(parameters.size());
        for (auto index = std::size_t{}; index != parameters.size(); ++index) {
            const auto input = parameters[index];
            const auto parameter = In<TS<PARAMETER_SCALAR>>{input.input_data()}.value_view();
            fields.emplace_back(Scalar<>::from(parameter));
        }
        const auto snapshot = bundle(result_type.schema(), fields);
        output.set_value(snapshot.view());
    }
};

template <cell_type RESULT> struct ColumnExpressionNode {
    Scalar<Opaque<CompiledExpressionOwner>> expression;
    In<TSB<COLUMNS>, all_valid> columns;
    In<TS<Bundle<PARAMETERS>>> parameters;
    Out<TSC<RESULT>, computed<>> output;

    void evaluate(EvaluationContext *context) const {
        const auto &compiled_expression = **expression;
        auto memory = ScratchResource{context->scratch()};
        auto prepared = PreparedExpressionInputs{
            compiled_expression, columns.input_data(), parameters.input_data(), memory};
        const auto &views = prepared.columns.views;

        const auto rebuild_values = !output.valid() || parameters.modified();

        const auto &first_binding = compiled_expression.columns.front();
        const auto first_input = columns[first_binding.field_index];
        first_binding.adapter.bind(std::addressof(output), first_input.input_data());
        const auto &rows = views.front().visible_rows();
        auto selected_rows = EvaluationRows{rows, &memory};
        if (!rebuild_values)
            selected_rows.select_changes(prepared.columns.changes);

        if (rebuild_values || !selected_rows.ids.empty()) {
            compiled_expression.for_each_result_batch(
                views, prepared.parameter_addresses, selected_rows.requested, memory,
                [&](std::span<const row_id_t> ids, const ColumnBatch<> &result) {
                    output.set_batch(ids, ColumnBatch<RESULT>{result.first_slot, result.size,
                                                              static_cast<typename ColumnValueTraits<RESULT>::pointer>(
                                                                  result.values),
                                                              result.value_bit_offset,
                                                              result.valid});
                });
        }
    }
};

class ExpressionCompiler {
  public:
    using BindingFactory = ColumnBinding (*)(const Type &, std::size_t);
    explicit ExpressionCompiler(GraphWiring &wiring, BindingFactory binding_factory)
        : wiring_{&wiring}, binding_factory_{binding_factory} {}

    Operand compile(WiringPoint point) {
        point = hg::details::WiringRecord::point(*point.record()->source_identity());
        if (dynamic_cast<const TimeSeriesValueType *>(std::addressof(point.type().dereferenced_type())) != nullptr)
            return add_parameter(point);

        const auto *identity = point.record();
        const auto options = point.materialisation_options();
        const auto *cell_type = expression_cell_type(point.type());
        if (details::is_column_expression_bundle_type(point.type().dereferenced_type())) {
            if (cell_type == nullptr)
                throw WiringError{"column expression result has no supported cell type"};
            if (const auto found = expressions_.find(identity); found != expressions_.end())
                return found->second;
            if (!active_.insert(identity).second)
                throw WiringError{"column expression contains a wiring cycle"};
            auto remove_active = util::scope_exit{[&]() noexcept { active_.erase(identity); }};
            const auto result = compile_operation(point, *cell_type);
            expressions_.emplace(identity, result);
            return result;
        }
        const auto *concrete_type = cell_type == nullptr ? nullptr : std::addressof(column_series_type(*cell_type));
        const auto concrete_option = std::ranges::find(options, concrete_type,
                                                       &WiringMaterialisationOption::target_type);
        if (concrete_type != nullptr && wiring_->materialisation_required(point, *concrete_type)) {
            if (concrete_option != options.end())
                return add_leaf(point, wiring_->expand_materialisation(point, *concrete_type));
            if (point.has_default_materialisation() && concrete_type->accepts(point.type()))
                return add_leaf(point, point);
        }
        if (const auto found = expressions_.find(identity); found != expressions_.end())
            return found->second;
        if (!active_.insert(identity).second)
            throw WiringError{"column expression contains a wiring cycle"};
        auto remove_active = util::scope_exit{[&]() noexcept { active_.erase(identity); }};

        const auto expression_option = std::ranges::find_if(options, [](const auto &option) {
            return details::is_column_expression_bundle_type(*option.target_type);
        });
        if (expression_option == options.end()) {
            if (concrete_type != nullptr &&
                (concrete_option != options.end() ||
                 (point.has_default_materialisation() && concrete_type->accepts(point.type()))))
                return add_leaf(point, point);
            throw WiringError{"column expression input has no concrete or expression materialisation"};
        }

        const auto bundle_point = wiring_->expand_materialisation(point, *expression_option->target_type);
        if (cell_type == nullptr)
            throw WiringError{"column expression result has no supported cell type"};
        const auto result = compile_operation(bundle_point, *cell_type);
        expressions_.emplace(identity, result);
        return result;
    }

    Operand compile_root(WiringPoint point) {
        const auto options = point.materialisation_options();
        const auto option = std::ranges::find_if(options, [&](const auto &candidate) {
            return details::is_column_expression_bundle_type(*candidate.target_type);
        });
        if (option == options.end())
            throw WiringError{"column expression root has no expression materialisation"};
        const auto bundle_point = wiring_->expand_materialisation(point, *option->target_type);
        const auto *result_type = expression_cell_type(point.type());
        if (result_type == nullptr)
            throw WiringError{"column expression root has no supported cell type"};
        return compile_operation(bundle_point, *result_type);
    }

    std::span<const WiringPoint> leaves() const noexcept { return leaves_; }
    std::span<const WiringPoint> parameters() const noexcept { return parameters_; }
    std::shared_ptr<const CompiledExpression> finish(Operand result, std::size_t first_column) && {
        expression_.result = result;
        expression_.columns = std::move(column_bindings_);
        for (auto &binding : expression_.columns)
            binding.field_index += first_column;
        expression_.parameters = std::move(parameter_bindings_);
        for (auto &binding : expression_.parameters) {
            if (binding.alignment > alignof(std::max_align_t))
                throw WiringError{"column expression parameter alignment is unsupported"};
            expression_.parameter_storage_size =
                (expression_.parameter_storage_size + binding.alignment - 1) & ~(binding.alignment - 1);
            binding.offset = expression_.parameter_storage_size;
            expression_.parameter_storage_size += binding.size;
        }
        return std::make_shared<const CompiledExpression>(std::move(expression_));
    }

  private:
    Operand compile_operation(WiringPoint bundle_point, const ScalarType &result_type) {
        const auto &bundle = static_cast<const TimeSeriesBundleType &>(bundle_point.type());
        const auto operand_count = bundle.item_count() - 1;
        if (operand_count != 1 && operand_count != 2)
            throw WiringError{"column expression callback must have one or two operands"};

        auto operands = std::array<Operand, 2>{};
        auto operand_types = std::array<const ScalarType *, 2>{};
        for (auto index = std::size_t{}; index != operand_count; ++index) {
            const auto operand_point = hg::details::time_series_bundle_field(bundle_point, index);
            operand_types[index] = expression_cell_type(operand_point.type());
            if (operand_types[index] == nullptr)
                throw WiringError{"column expression operands must be columns or scalar time series"};
            operands[index] = compile(operand_point);
        }
        const auto compute = hg::details::time_series_bundle_field(bundle_point, operand_count);
        const auto callback = extract_function(compute);
        const auto *function_type = dynamic_cast<const ColumnBlockFunctionType *>(std::addressof(callback.type()));
        if (function_type == nullptr || std::addressof(function_type->result_type()) != std::addressof(result_type) ||
            function_type->operand_types().size() != operand_count)
            throw WiringError{"column expression callback signature does not match its result"};
        for (auto index = std::size_t{}; index != operand_count; ++index)
            if (function_type->operand_types()[index] != operand_types[index])
                throw WiringError{"column expression callback signature does not match its operands"};
        const auto result = Operand{expression_.instructions.size(), Operand::Source::instruction};
        append_instruction(std::span<const Operand>{operands.data(), operand_count}, callback);
        return result;
    }

    Operand add_leaf(WiringPoint source, WiringPoint materialized) {
        const auto *identity = source.record()->source_identity();
        if (const auto found = leaf_indices_.find(identity); found != leaf_indices_.end())
            return {found->second, Operand::Source::column};
        const auto index = leaves_.size();
        leaf_indices_.emplace(identity, index);
        leaves_.push_back(materialized);
        column_bindings_.push_back(binding_factory_(materialized.type().dereferenced_type(), index));
        return {index, Operand::Source::column};
    }

    Operand add_parameter(WiringPoint point) {
        const auto *identity = point.record()->source_identity();
        if (const auto found = parameter_indices_.find(identity); found != parameter_indices_.end())
            return {found->second, Operand::Source::parameter};
        const auto index = parameters_.size();
        parameter_indices_.emplace(identity, index);
        parameters_.push_back(point);
        const auto &parameter_type = static_cast<const TimeSeriesValueType &>(point.type().dereferenced_type());
        parameter_bindings_.push_back(parameter_binding(parameter_type.value_type(), index));
        return {index, Operand::Source::parameter};
    }

    Scalar<> extract_function(WiringPoint point) {
        const auto materialized = wiring_->materialize(point, point.type());
        const auto &node = wiring_->nodes()[wiring_->output_node_index(materialized)];
        const auto constant = node.node_template().constant_output(node.signature());
        if (!constant)
            throw WiringError{"column expression compute field must be a constant output"};
        auto scalar_index = std::size_t{};
        for (auto parameter = std::size_t{}; parameter != constant->scalar_parameter; ++parameter)
            scalar_index += static_cast<std::size_t>(!node.signature().parameters[parameter].type->is_time_series());
        const auto value = node.scalar_values()[scalar_index];
        return Scalar<>::from(value.runtime_type(), value.data());
    }

    void append_instruction(std::span<const Operand> operands, Scalar<> callback) {
        if (operands.empty() || operands.size() > 2)
            throw WiringError{"column expression callback must have one or two operands"};
        const auto *adapter = ScalarDispatch<ColumnBlockFunctionAdapter>::instance().find(callback.runtime_type());
        if (adapter == nullptr)
            throw WiringError{"column expression callback signature has no registered block adapter"};
        if (adapter->result_alignment > alignof(std::max_align_t))
            throw WiringError{"column expression result alignment is unsupported"};
        auto result_offset = expression_.instruction_storage_size;
        result_offset = (result_offset + adapter->result_alignment - 1) & ~(adapter->result_alignment - 1);
        const auto result_bits = adapter->packed_result ? std::size_t{1} : adapter->result_size * 8;
        const auto result_bytes = (expression_batch_rows * result_bits + 7) / 8;
        expression_.instruction_storage_size = result_offset + result_bytes;
        auto saved_operands = std::array<Operand, 2>{};
        std::ranges::copy(operands, saved_operands.begin());
        expression_.instructions.push_back(
            {saved_operands, operands.size(), ScalarValue<>{callback}, adapter, result_offset});
    }

    GraphWiring *wiring_;
    BindingFactory binding_factory_;
    CompiledExpression expression_;
    std::vector<WiringPoint> leaves_;
    std::vector<WiringPoint> parameters_;
    std::vector<ColumnBinding> column_bindings_;
    std::vector<ParameterBinding> parameter_bindings_;
    std::unordered_map<const hg::details::WiringRecord *, std::size_t> leaf_indices_;
    std::unordered_map<const hg::details::WiringRecord *, std::size_t> parameter_indices_;
    std::unordered_map<const hg::details::WiringRecord *, Operand> expressions_;
    std::unordered_set<const hg::details::WiringRecord *> active_;
};

const auto parameter_update_node = node<ParameterUpdateNode>();

template <cell_type RESULT> const auto &column_expression_node() {
    static const auto node_template = node<ColumnExpressionNode<RESULT>>();
    return node_template;
}

template <template <template <typename, auto...> typename> typename Schema> struct ColumnFilterNode {
    Scalar<Opaque<CompiledExpressionOwner>> expression;
    In<TSF<Schema>> source;
    In<TSB<COLUMNS>, all_valid> columns;
    In<TS<Bundle<PARAMETERS>>> parameters;
    Out<TSF<Schema>, view> output;

    void evaluate(EvaluationContext *context) const {
        const auto &compiled_expression = **expression;
        auto memory = ScratchResource{context->scratch()};
        auto prepared = PreparedExpressionInputs{
            compiled_expression, columns.input_data(), parameters.input_data(), memory};
        const auto &input_rows = source.extension_descriptor().rows;
        if (prepared.columns.views.front().visible_rows() != input_rows)
            throw std::invalid_argument{"frame filter predicate columns must share the source row selection"};

        const auto rebuild_predicate = !output.valid() || parameters.modified();
        auto changed_rows = EvaluationRows{input_rows, &memory};
        if (!rebuild_predicate)
            changed_rows.select_changes(prepared.columns.changes);

        const auto evaluate_predicate = [&](const RowSet &requested, auto consume) {
            compiled_expression.for_each_result_batch(
                prepared.columns.views, prepared.parameter_addresses, requested, memory,
                [&](std::span<const row_id_t> ids, const ColumnBatch<> &result) {
                    consume(ids, ColumnBatch<bool>{
                                    result.first_slot, result.size,
                                    static_cast<typename ColumnValueTraits<bool>::pointer>(result.values),
                                    result.value_bit_offset, result.valid});
                });
        };
        output.bind_filter(source, rebuild_predicate, changed_rows.ids, evaluate_predicate);
    }

};

template <template <template <typename, auto...> typename> typename Schema>
const auto &column_filter_node() {
    static const auto node_template = node<ColumnFilterNode<Schema>>();
    return node_template;
}

ExpressionNodeInputs prepare_expression_node_inputs(GraphWiring &graph_wiring,
                                                    ExpressionCompiler &&compiler, Operand result,
                                                    std::size_t first_column = 0) {
    const auto leaf_points = std::vector<WiringPoint>{compiler.leaves().begin(), compiler.leaves().end()};
    const auto parameter_points = std::vector<WiringPoint>{compiler.parameters().begin(), compiler.parameters().end()};
    auto expression = std::move(compiler).finish(result, first_column);

    const auto named_fields = [](std::span<const WiringPoint> points, std::string_view prefix) {
        auto fields = std::vector<WiringArgument>{};
        fields.reserve(points.size());
        for (auto index = std::size_t{}; index != points.size(); ++index) {
            const auto name = std::string{prefix} + std::to_string(index);
            fields.emplace_back(util::symbol{name}, points[index]);
        }
        return fields;
    };
    const auto bundle_type = [](std::span<const WiringArgument> fields) -> const TimeSeriesType & {
        auto schema_fields = std::vector<SchemaField>{};
        schema_fields.reserve(fields.size());
        for (const auto &field : fields)
            schema_fields.push_back({*field.name(), &field.time_series().type().dereferenced_type(), false});
        return TimeSeriesBundleType::from_fields(schema_fields);
    };

    const auto column_fields = named_fields(leaf_points, "c");
    const auto free_columns = static_cast<WiringPoint>(hg::bundle(std::span<const WiringArgument>{column_fields}));
    auto column_bundle = graph_wiring.materialize(free_columns, bundle_type(column_fields));
    auto parameter_values = [&] {
        if (parameter_points.empty()) {
            auto parameter_memory = allocator{};
            return operators::const_(hg::details::make_hierarchical_bundle<EmptyParameterSchema>(
                                         std::addressof(parameter_memory)))
                .as<TS<Bundle<PARAMETERS>>>();
        }
        const auto parameter_fields = named_fields(parameter_points, "p");
        const auto free_parameters =
            static_cast<WiringPoint>(hg::bundle(std::span<const WiringArgument>{parameter_fields}));
        auto parameter_bundle = graph_wiring.materialize(free_parameters, bundle_type(parameter_fields));
        const auto parameter_inputs = wp<TSB<PARAMETER_INPUTS>>{parameter_bundle};
        return parameter_update_node(parameter_inputs);
    }();

    return {ScalarValue<Opaque<CompiledExpressionOwner>>{std::move(expression)}, column_bundle, parameter_values};
}

template <cell_type RESULT>
WiringPoint materialize_column_expression_impl(WiringPoint result) {
    auto &graph_wiring = static_cast<WiringPoint>(result).record()->wiring();
    auto compiler = ExpressionCompiler{graph_wiring, &column_binding<RESULT>};
    const auto root = compiler.compile_root(result);
    if (compiler.leaves().empty())
        throw WiringError{"column expression requires at least one column operand"};
    auto inputs = prepare_expression_node_inputs(graph_wiring, std::move(compiler), root);
    return column_expression_node<RESULT>()(
        std::move(inputs.expression), wp<TSB<COLUMNS>>{inputs.columns}, wp<TS<Bundle<PARAMETERS>>>{inputs.parameters});
}

template <template <template <typename, auto...> typename> typename Schema>
WiringPoint materialize_column_filter(WiringPoint condition, wp<TSF<Schema>> source) {
    auto &graph_wiring = static_cast<WiringPoint>(source).record()->wiring();
    auto compiler = ExpressionCompiler{graph_wiring, &column_binding<bool>};
    const auto result = compiler.compile(condition);
    if (compiler.leaves().empty())
        throw WiringError{"frame filter predicate requires at least one column operand"};
    auto inputs = prepare_expression_node_inputs(graph_wiring, std::move(compiler), result);
    return column_filter_node<Schema>()(
        std::move(inputs.expression), source, wp<TSB<COLUMNS>>{inputs.columns},
        wp<TS<Bundle<PARAMETERS>>>{inputs.parameters});
}

template <template <typename, auto...> typename Field>
using FILTER_EXPRESSION = BundleSchemaVariableDeclaration<"FRAME_FILTER_EXPRESSION", Field>;
using FILTER_CONDITION = Union<"FRAME_FILTER_CONDITION", TSC<bool>, TSB<FILTER_EXPRESSION>>;
template <template <typename, auto...> typename Field>
using FILTER_SCHEMA = SchemaVariableDeclaration<"FRAME_FILTER_SCHEMA">;

const auto registered_filter = operators::filter_.overload(graph<schema_type<FILTER_SCHEMA>>(
    "name"_n = "frame_filter",
    require = [](const WiringArguments &arguments) {
        return expression_cell_type(arguments.original_type("condition")) == std::addressof(cpp_scalar_type<bool>())
                   ? accept
                   : reject("frame filter condition must produce bool");
    },
    [](wp<FILTER_CONDITION, "condition"_n> condition,
       wp<TSF<FILTER_SCHEMA>, "value"_n> value) -> wp<TSF<FILTER_SCHEMA>> {
        auto &wiring = WiringContext::current().wiring();
        const auto &output_type = value.type().with_guarantee(TimeSeriesGuarantee::ordinary);
        const auto arguments = hg::details::make_wiring_arguments(condition, value);
        auto output = wiring.create_wp(output_type, arguments);

        const auto builder = [](wp<FILTER_CONDITION, "condition"_n> condition,
                                wp<TSF<FILTER_SCHEMA>, "value"_n> value) {
            return materialize_column_filter(static_cast<WiringPoint>(condition), wp<TSF<FILTER_SCHEMA>>{value});
        };
        auto input_signature = hg::details::make_graph_input_signature<decltype(builder)>(
            util::symbol{"materialisation"});
        auto erased_builder = [builder](std::span<const WiringArgument> builder_arguments) -> WiringPoint {
            return hg::details::invoke_graph_function(builder, builder_arguments);
        };
        output.add_materialisation_option(output_type, 0.0, std::move(input_signature),
                                          WiringMaterialisationBuilder{std::move(erased_builder)});
        return output.template as<TSF<FILTER_SCHEMA>>();
    }));

} // namespace

hg::frame::details::ExpressionNodeInputs hg::frame::details::compile_column_expression(WiringPoint point,
                                                                                   std::size_t first_column) {
    auto &wiring = point.record()->wiring();
    auto compiler = ExpressionCompiler{wiring, &input_column_binding};
    const auto result = compiler.compile(point);
    if (compiler.leaves().empty())
        throw WiringError{"aggregation expression requires at least one column operand"};
    return prepare_expression_node_inputs(wiring, std::move(compiler), result, first_column);
}

bool hg::frame::details::is_column_expression_bundle_type(const Type &type) {
    const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(std::addressof(type));
    if (bundle == nullptr || bundle->item_count() < 2 || bundle->item_count() > 3)
        return false;
    const auto &compute = bundle->item_type(bundle->item_count() - 1).dereferenced_type();
    const auto *value = dynamic_cast<const TimeSeriesValueType *>(std::addressof(compute));
    return value != nullptr &&
           dynamic_cast<const ColumnBlockFunctionType *>(std::addressof(value->value_type())) != nullptr;
}

WiringPoint materialize_column_expression(WiringPoint result) {
    const auto *cell_type = expression_cell_type(result.type());
    if (cell_type == std::addressof(cpp_scalar_type<i64>()))
        return materialize_column_expression_impl<i64>(result);
    if (cell_type == std::addressof(cpp_scalar_type<double>()))
        return materialize_column_expression_impl<double>(result);
    if (cell_type == std::addressof(cpp_scalar_type<bool>()))
        return materialize_column_expression_impl<bool>(result);
    if (cell_type == std::addressof(cpp_scalar_type<symbol>()))
        return materialize_column_expression_impl<symbol>(result);
    throw WiringError{"column expression result has an unsupported cell type"};
}

} // namespace hg::frame
