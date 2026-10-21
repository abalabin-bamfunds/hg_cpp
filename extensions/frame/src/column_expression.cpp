#include <bg/frame/expression.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/graph.hpp>
#include <bg/graph/details/wiring_record.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/node.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/util/scope_exit.hpp>

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

namespace bg::frame {
namespace {

inline constexpr auto expression_batch_rows = std::size_t{128};

template <template <typename, auto...> typename Field>
using COLUMNS = BundleSchemaVariableDeclaration<"FRAME_EXPRESSION_COLUMNS", Field>;
template <template <typename, auto...> typename Field>
using PARAMETER_INPUTS = BundleSchemaVariableDeclaration<"FRAME_EXPRESSION_PARAMETER_INPUTS", Field>;
template <template <typename, auto...> typename Field>
using PARAMETERS = BundleSchemaVariableDeclaration<"FRAME_EXPRESSION_PARAMETERS", Field>;
using PARAMETER_SCALAR = ScalarVariable<"FRAME_EXPRESSION_PARAMETER_SCALAR">;
template <template <typename, auto...> typename Field> struct EmptyParameterSchema {};

using CompiledExpressionOwner = std::shared_ptr<const struct CompiledExpression>;

struct ColumnInputAdapter {
    ErasedColumnSeriesView (*read)(const TimeSeriesInputData &);
    bool (*bind)(const void *, const TimeSeriesInputData &);
};

struct ColumnBinding {
    std::size_t field_index{};
    ColumnInputAdapter adapter{};
};

struct ParameterBinding {
    std::size_t field_index{};
    std::size_t offset{};
    std::size_t size{};
    std::size_t alignment{};
    void (*read)(Scalar<>, void *){};
};

template <cell_type CELL> ErasedColumnSeriesView read_column(const TimeSeriesInputData &data) {
    const auto input = In<TSC<CELL>>{data};
    return ErasedColumnSeriesView{input.extension_descriptor().storage};
}

template <cell_type CELL, cell_type RESULT>
bool bind_column(const void *output, const TimeSeriesInputData &data) {
    const auto input = In<TSC<CELL>>{data};
    return static_cast<const Out<TSC<RESULT>, computed<>> *>(output)->bind(input);
}

template <cell_type CELL> void read_parameter(Scalar<> value, void *destination) {
    std::construct_at(static_cast<CELL *>(destination), Scalar<CELL>::from(value).value());
}

template <cell_type RESULT> ColumnBinding column_binding(const Type &type, std::size_t index) {
    const auto *column = dynamic_cast<const ColumnSeriesType *>(std::addressof(type));
    if (column == nullptr)
        throw WiringError{"column expression input is not a concrete column"};
    if (std::addressof(column->cell_type()) == std::addressof(cpp_scalar_type<i64>()))
        return {index, {&read_column<i64>, &bind_column<i64, RESULT>}};
    if (std::addressof(column->cell_type()) == std::addressof(cpp_scalar_type<double>()))
        return {index, {&read_column<double>, &bind_column<double, RESULT>}};
    if (std::addressof(column->cell_type()) == std::addressof(cpp_scalar_type<bool>()))
        return {index, {&read_column<bool>, &bind_column<bool, RESULT>}};
    throw WiringError{"column expression input has an unsupported cell type"};
}

ParameterBinding parameter_binding(const ScalarType &type, std::size_t index) {
    if (std::addressof(type) == std::addressof(cpp_scalar_type<i64>()))
        return {index, 0, sizeof(i64), alignof(i64), &read_parameter<i64>};
    if (std::addressof(type) == std::addressof(cpp_scalar_type<double>()))
        return {index, 0, sizeof(double), alignof(double), &read_parameter<double>};
    if (std::addressof(type) == std::addressof(cpp_scalar_type<bool>()))
        return {index, 0, sizeof(bool), alignof(bool), &read_parameter<bool>};
    throw WiringError{"column expression parameter has an unsupported cell type"};
}

const ScalarType *expression_cell_type(const Type &type) {
    const auto &resolved_type = static_cast<const TimeSeriesType &>(type).dereferenced_type();
    if (const auto *column = dynamic_cast<const ColumnSeriesType *>(std::addressof(resolved_type)))
        return std::addressof(column->cell_type());
    if (const auto *value = dynamic_cast<const TimeSeriesValueType *>(std::addressof(resolved_type)))
        return std::addressof(value->value_type());
    return nullptr;
}

struct Operand {
    enum class Kind { column, parameter, instruction };

    std::size_t index{};
    Kind kind{Kind::column};
};

struct Instruction {
    std::array<Operand, 2> operands;
    std::size_t operand_count{};
    ScalarValue<> callback;
    const ColumnBlockFunctionAdapter *adapter{};
    std::size_t result_offset{};
};

struct CompiledExpression {
    std::vector<Instruction> instructions;
    std::vector<ColumnBinding> columns;
    std::vector<ParameterBinding> parameters;
    std::size_t parameter_storage_size{};
    std::size_t instruction_storage_size{};

    template <typename Function>
    void for_each_result_batch(std::span<const ErasedColumnSeriesView> views,
                               std::span<const void *const> parameters, const RowSet &rows,
                               std::pmr::memory_resource &memory, Function &&function) const {
        constexpr auto max_rows = expression_batch_rows;
        const auto validity_capacity = (max_rows + 7) / 8;
        const auto value_bytes = instruction_storage_size;
        auto *operation_values = value_bytes == 0 ? nullptr : memory.allocate(value_bytes, alignof(std::max_align_t));
        auto release_operation_values = util::scope_exit{[&]() noexcept {
            if (operation_values != nullptr)
                memory.deallocate(operation_values, value_bytes, alignof(std::max_align_t));
        }};
        auto operation_validity =
            std::pmr::vector<std::uint8_t>{instructions.size() * validity_capacity, 0, &memory};
        auto operation_batches = std::pmr::vector<ColumnBatch<>>{instructions.size(), &memory};

        for_each_aligned_column_batch(
            views, rows, memory,
            [&](std::span<const row_id_t> ids, std::span<const ColumnBatch<>> batches) {
                for (auto offset = std::size_t{}; offset < ids.size(); offset += max_rows) {
                    const auto count = std::min(max_rows, ids.size() - offset);
                    const auto batch_ids = ids.subspan(offset, count);
                    const auto validity_bytes = (count + 7) / 8;
                    for (auto operation = std::size_t{}; operation != instructions.size(); ++operation) {
                        const auto &instruction = instructions[operation];
                        auto operands = std::array<ColumnOperandBatch<>, 2>{};
                        for (auto operand_index = std::size_t{}; operand_index != instruction.operand_count;
                             ++operand_index) {
                            const auto operand = instruction.operands[operand_index];
                            switch (operand.kind) {
                            case Operand::Kind::column: {
                                const auto &batch = batches[operand.index];
                                operands[operand_index] =
                                    ColumnOperandBatch<>{batch.subspan(offset, count)};
                                break;
                            }
                            case Operand::Kind::parameter:
                                operands[operand_index] = ColumnOperandBatch<>{parameters[operand.index]};
                                break;
                            case Operand::Kind::instruction: {
                                const auto &batch = operation_batches[operand.index];
                                operands[operand_index] = ColumnOperandBatch<>{batch};
                                break;
                            }
                            }
                        }

                        auto *values = static_cast<std::byte *>(operation_values) + instruction.result_offset;
                        auto *validity = operation_validity.data() + operation * validity_capacity;
                        instruction.adapter->call(
                            instruction.callback.view(), batch_ids,
                            std::span<const ColumnOperandBatch<>>{operands.data(), instruction.operand_count},
                            MutableColumnBatch<>{values, count,
                                                 std::span<std::uint8_t>{validity, validity_bytes}});
                        operation_batches[operation] = ColumnBatch<>{
                            0, count, values, 0,
                            instruction.adapter->packed_result ? 1 : instruction.adapter->result_size * 8,
                            ColumnBatchValidity::from_bitmap(validity, 0)};
                    }

                    function(batch_ids, operation_batches.back());
                }
            });
    }
};

struct BindingCache {
    explicit BindingCache(std::pmr::memory_resource *memory) : columns{memory} {}
    std::pmr::vector<ErasedColumnSeriesView> columns;
};

struct ColumnActivity {
    bool modified{};
    EvaluationTime last_updated{MIN_DT};
};

struct PreparedColumns {
    PreparedColumns(const In<TSB<COLUMNS>, all_valid> &inputs, std::span<const ColumnBinding> bindings,
                    std::span<const ErasedColumnSeriesView> cached_bindings, std::pmr::memory_resource *memory)
        : views{memory}, activity{memory} {
        views.reserve(bindings.size());
        activity.reserve(bindings.size());
        for (const auto &binding : bindings) {
            const auto input = inputs[binding.field_index];
            const auto &data = input.input_data();
            views.push_back(binding.adapter.read(data));
            activity.push_back({data.modified(), data.last_updated()});
        }

        mapping_changed = cached_bindings.size() != views.size();
        if (!mapping_changed) {
            for (auto index = std::size_t{}; index != views.size(); ++index) {
                if (!cached_bindings[index].same_binding(views[index])) {
                    mapping_changed = true;
                    break;
                }
            }
        }

        for (std::size_t index = 0; index != views.size(); ++index) {
            if (activity[index].modified && views[index].change_time() != activity[index].last_updated) {
                delta_lost = true;
                break;
            }
        }

        const auto &rows = views.front().visible_rows();
        for (std::size_t index = 1; index != views.size(); ++index)
            if (views[index].visible_rows() != rows)
                throw std::invalid_argument{"column expression inputs must share one visible row selection"};
    }

    std::pmr::vector<ErasedColumnSeriesView> views;
    std::pmr::vector<ColumnActivity> activity;
    bool mapping_changed{};
    bool delta_lost{};
};

struct EvaluationRows {
    EvaluationRows(const RowSet &visible_rows, std::pmr::memory_resource *memory)
        : ids{memory}, requested{visible_rows} {}

    void select_changes(std::span<const ColumnActivity> activity, std::span<const ErasedColumnSeriesView> views) {
        for (std::size_t index = 0; index != activity.size(); ++index) {
            const auto &view = views[index];
            if (!activity[index].modified || view.change_time() != activity[index].last_updated)
                continue;
            for (const auto id : view.added())
                ids.push_back(id);
            for (const auto id : view.modified())
                ids.push_back(id);
        }

        std::ranges::sort(ids);
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        requested = RowSet{
            .context = std::addressof(ids),
            .native_rows = requested.native_rows,
            .identity = requested.identity,
            .size = [](const void *context) noexcept {
                return static_cast<const std::pmr::vector<row_id_t> *>(context)->size();
            },
            .contains = [](const void *context, row_id_t id) noexcept {
                const auto &ids = *static_cast<const std::pmr::vector<row_id_t> *>(context);
                return std::ranges::binary_search(ids, id);
            },
            .id_at = [](const void *context, std::size_t index) {
                return (*static_cast<const std::pmr::vector<row_id_t> *>(context))[index];
            },
            .for_each = [](const void *context, void *target, RowSet::visit_function visit) {
                for (const auto id : *static_cast<const std::pmr::vector<row_id_t> *>(context))
                    visit(target, id);
            }};
    }

    std::pmr::vector<row_id_t> ids;
    RowSet requested;
};

class ScratchResource final : public std::pmr::memory_resource {
  public:
    explicit ScratchResource(ScratchPad &scratch) noexcept : scratch_{&scratch} {}

  private:
    void *do_allocate(std::size_t bytes, std::size_t alignment) override {
        return scratch_->allocate(bytes, alignment);
    }
    void do_deallocate(void *address, std::size_t bytes, std::size_t) override {
        scratch_->release_last(address, bytes);
    }
    bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
        return this == std::addressof(other);
    }

    ScratchPad *scratch_;
};

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
    State<BindingCache *> bindings;

    void evaluate(EvaluationContext *context) const {
        const auto &compiled_expression = **expression;
        auto *cache = *bindings;
        auto memory = ScratchResource{context->scratch()};
        auto prepared_columns = PreparedColumns{columns, std::span{compiled_expression.columns},
                                                std::span{cache->columns}, &memory};
        const auto &views = prepared_columns.views;

        const auto parameter_bytes = compiled_expression.parameter_storage_size;
        auto *parameter_data = parameter_bytes == 0 ? nullptr : memory.allocate(parameter_bytes, alignof(std::max_align_t));
        auto release_parameter_data = util::scope_exit{[&]() noexcept {
            if (parameter_data != nullptr)
                memory.deallocate(parameter_data, parameter_bytes, alignof(std::max_align_t));
        }};
        auto parameter_addresses = std::pmr::vector<const void *>{
            std::pmr::polymorphic_allocator<const void *>{&memory}};
        parameter_addresses.reserve(compiled_expression.parameters.size());
        const auto parameter_bundle = parameters.value_view();
        for (auto index = std::size_t{}; index != compiled_expression.parameters.size(); ++index) {
            const auto &binding = compiled_expression.parameters[index];
            auto *value = static_cast<std::byte *>(parameter_data) + binding.offset;
            const auto parameter = parameter_bundle[binding.field_index];
            binding.read(Scalar<>::from(parameter), value);
            parameter_addresses.push_back(value);
        }

        const auto output_invalid = !output.valid();
        const auto parameters_changed = parameters.modified();
        const auto rebuild_values =
            output_invalid || prepared_columns.mapping_changed || prepared_columns.delta_lost || parameters_changed;

        const auto &first_binding = compiled_expression.columns.front();
        const auto first_input = columns[first_binding.field_index];
        first_binding.adapter.bind(std::addressof(output), first_input.input_data());
        const auto &rows = views.front().visible_rows();
        auto selected_rows = EvaluationRows{rows, &memory};
        if (!rebuild_values)
            selected_rows.select_changes(std::span{prepared_columns.activity}, std::span{views});

        if (rebuild_values || !selected_rows.ids.empty()) {
            compiled_expression.for_each_result_batch(
                views, parameter_addresses, selected_rows.requested, memory,
                [&](std::span<const row_id_t> ids, const ColumnBatch<> &result) {
                    output.set_batch(ids, ColumnBatch<RESULT>{result.first_slot, result.size,
                                                              static_cast<typename ColumnValueTraits<RESULT>::pointer>(
                                                                  result.values),
                                                              result.value_bit_offset,
                                                              result.valid});
                });
        }

        cache->columns.assign(views.begin(), views.end());
    }

    void start(EvaluationContext *context) const {
        auto allocation = context->runtime_graph().allocation();
        *bindings = allocation.new_object<BindingCache>(&allocation.memory_resource());
    }

    void stop(EvaluationContext *context) const noexcept {
        auto allocation = context->runtime_graph().allocation();
        allocation.delete_object(*bindings);
        *bindings = nullptr;
    }
};

template <cell_type RESULT> class ExpressionCompiler {
  public:
    explicit ExpressionCompiler(GraphWiring &wiring) : wiring_{&wiring} {}

    Operand compile(WiringPoint point) {
        point = bg::details::WiringRecord::point(*point.record()->source_identity());
        if (dynamic_cast<const TimeSeriesValueType *>(std::addressof(point.type().dereferenced_type())) != nullptr)
            return add_parameter(point);

        const auto *identity = point.record();
        const auto options = point.materialisation_options();
        const auto *cell_type = expression_cell_type(point.type());
        const auto *concrete_type = cell_type == std::addressof(cpp_scalar_type<bool>())
                                        ? std::addressof(TSC<bool>::type())
                                        : cell_type == std::addressof(cpp_scalar_type<i64>())
                                              ? std::addressof(TSC<i64>::type())
                                              : cell_type == std::addressof(cpp_scalar_type<double>())
                                                    ? std::addressof(TSC<double>::type())
                                                    : nullptr;
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

    void compile_root(WiringPoint point) {
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
        static_cast<void>(compile_operation(bundle_point, *result_type));
    }

    std::span<const WiringPoint> leaves() const noexcept { return leaves_; }
    std::span<const WiringPoint> parameters() const noexcept { return parameters_; }
    std::shared_ptr<const CompiledExpression> finish() && {
        expression_.columns = std::move(column_bindings_);
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
            const auto operand_point = bg::details::time_series_bundle_field(bundle_point, index);
            operand_types[index] = expression_cell_type(operand_point.type());
            if (operand_types[index] == nullptr)
                throw WiringError{"column expression operands must be columns or scalar time series"};
            operands[index] = compile(operand_point);
        }
        const auto compute = bg::details::time_series_bundle_field(bundle_point, operand_count);
        const auto callback = extract_function(compute);
        const auto *function_type = dynamic_cast<const ColumnBlockFunctionType *>(std::addressof(callback.type()));
        if (function_type == nullptr || std::addressof(function_type->result_type()) != std::addressof(result_type) ||
            function_type->operand_types().size() != operand_count)
            throw WiringError{"column expression callback signature does not match its result"};
        for (auto index = std::size_t{}; index != operand_count; ++index)
            if (function_type->operand_types()[index] != operand_types[index])
                throw WiringError{"column expression callback signature does not match its operands"};
        const auto result = Operand{expression_.instructions.size(), Operand::Kind::instruction};
        append_instruction(std::span<const Operand>{operands.data(), operand_count}, callback);
        return result;
    }

    Operand add_leaf(WiringPoint source, WiringPoint materialized) {
        const auto *identity = source.record()->source_identity();
        if (const auto found = leaf_indices_.find(identity); found != leaf_indices_.end())
            return {found->second, Operand::Kind::column};
        const auto index = leaves_.size();
        leaf_indices_.emplace(identity, index);
        leaves_.push_back(materialized);
        column_bindings_.push_back(column_binding<RESULT>(materialized.type().dereferenced_type(), index));
        return {index, Operand::Kind::column};
    }

    Operand add_parameter(WiringPoint point) {
        const auto *identity = point.record()->source_identity();
        if (const auto found = parameter_indices_.find(identity); found != parameter_indices_.end())
            return {found->second, Operand::Kind::parameter};
        const auto index = parameters_.size();
        parameter_indices_.emplace(identity, index);
        parameters_.push_back(point);
        const auto &parameter_type = static_cast<const TimeSeriesValueType &>(point.type().dereferenced_type());
        parameter_bindings_.push_back(parameter_binding(parameter_type.value_type(), index));
        return {index, Operand::Kind::parameter};
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
    CompiledExpression expression_;
    std::vector<WiringPoint> leaves_;
    std::vector<WiringPoint> parameters_;
    std::vector<ColumnBinding> column_bindings_;
    std::vector<ParameterBinding> parameter_bindings_;
    std::unordered_map<const bg::details::WiringRecord *, std::size_t> leaf_indices_;
    std::unordered_map<const bg::details::WiringRecord *, std::size_t> parameter_indices_;
    std::unordered_map<const bg::details::WiringRecord *, Operand> expressions_;
    std::unordered_set<const bg::details::WiringRecord *> active_;
};

const auto parameter_update_node = node<ParameterUpdateNode>();

template <cell_type RESULT> const auto &column_expression_node() {
    static const auto node_template = node<ColumnExpressionNode<RESULT>>();
    return node_template;
}

template <cell_type RESULT>
WiringPoint materialize_column_expression_impl(WiringPoint result) {
    auto &graph_wiring = static_cast<WiringPoint>(result).record()->wiring();
    auto compiler = ExpressionCompiler<RESULT>{graph_wiring};
    compiler.compile_root(result);
    if (compiler.leaves().empty())
        throw WiringError{"column expression requires at least one column operand"};
    const auto leaf_points = std::vector<WiringPoint>{compiler.leaves().begin(), compiler.leaves().end()};
    const auto parameter_points = std::vector<WiringPoint>{compiler.parameters().begin(), compiler.parameters().end()};
    const auto expression = std::move(compiler).finish();

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
    const auto free_columns = static_cast<WiringPoint>(bg::bundle(std::span<const WiringArgument>{column_fields}));
    const auto column_bundle = graph_wiring.materialize(free_columns, bundle_type(column_fields));
    const auto parameter_values = [&] {
        if (parameter_points.empty()) {
            auto parameter_memory = allocator{};
            return operators::const_(bg::details::make_hierarchical_bundle<EmptyParameterSchema>(
                                         std::addressof(parameter_memory)))
                .as<TS<Bundle<PARAMETERS>>>();
        }
        const auto parameter_fields = named_fields(parameter_points, "p");
        const auto free_parameters =
            static_cast<WiringPoint>(bg::bundle(std::span<const WiringArgument>{parameter_fields}));
        const auto parameter_bundle = graph_wiring.materialize(free_parameters, bundle_type(parameter_fields));
        const auto parameter_inputs = wp<TSB<PARAMETER_INPUTS>>{parameter_bundle};
        return parameter_update_node(parameter_inputs);
    }();

    return column_expression_node<RESULT>()(
        ScalarValue<Opaque<CompiledExpressionOwner>>{expression}, wp<TSB<COLUMNS>>{column_bundle}, parameter_values);
}

} // namespace

bool bg::frame::details::is_column_expression_bundle_type(const Type &type) {
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
    throw WiringError{"column expression result has an unsupported cell type"};
}

} // namespace bg::frame
