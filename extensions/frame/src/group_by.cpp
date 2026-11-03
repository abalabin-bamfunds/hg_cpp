#include <hg/frame/frame.hpp>
#include <hg/frame/tsc.hpp>
#include <hg/graph.hpp>
#include <hg/library/operators.hpp>
#include <hg/node.hpp>
#include <hg/overload.hpp>
#include <hg/scalar/list/scalar.hpp>
#include <hg/time_series/tsb/details/wiring.hpp>

#include "mixed_expression.hpp"
#include "expression_evaluation.hpp"

#include <algorithm>
#include <memory_resource>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hg::operators {

operator_<auto(frame::TSF<SCHEMA>, List<symbol>) -> frame::GroupByExpression>
    group_by{"group_by", {"source", "keys"}};
operator_<OUT, auto(frame::GroupByExpression, TSB<AGGREGATIONS>) -> OUT> agg{"agg", {"group", "aggregations"}};

} // namespace hg::operators

namespace hg::frame::details {
namespace {

using namespace hg::literals;

template <template <typename, auto...> typename Field>
using AGG_SOURCE_SCHEMA = SchemaVariableDeclaration<"FRAME_GROUP_AGG_SOURCE_SCHEMA">;
template <template <typename, auto...> typename Field>
using AGG_RESULT_SCHEMA = SchemaVariableDeclaration<"FRAME_GROUP_AGG_RESULT_SCHEMA">;
template <template <typename, auto...> typename Field>
using AGG_COLUMN_SCHEMA = BundleSchemaVariableDeclaration<"FRAME_GROUP_AGG_COLUMNS", Field>;
template <template <typename, auto...> typename Field>
using AGG_PARAMETER_SCHEMA = BundleSchemaVariableDeclaration<"FRAME_GROUP_AGG_PARAMETERS", Field>;

using GroupKey = std::pmr::vector<KeyValue>;

struct GroupKeyHash {
    std::size_t operator()(std::span<const KeyValue> key) const {
        auto hash = std::size_t{};
        for (const auto &cell : key)
            hash ^= key_value_hash(cell) + std::size_t{0x9e3779b9} + (hash << 6) + (hash >> 2);
        return hash;
    }
};

struct GroupKeyEqual {
    bool operator()(std::span<const KeyValue> left, std::span<const KeyValue> right) const {
        return key_values_equal(left, right);
    }
};

struct Group {
    row_id_t output_row{no_row};
    std::size_t row_count{};
    util::index<> modified;
};

struct GroupingColumn {
    std::size_t field_index{};
    ErasedColumnSeriesView (*read)(const TimeSeriesInputData &);
    RowChanges (*changes)(const TimeSeriesInputData &);
    void (*append)(GroupKey &, const ColumnBatch<> &, std::size_t, allocator);
};

struct AggregationDefinition {
    CompiledExpressionOwner expression;
    AggregationFunctionAdapter kernel;
};

struct GroupByDefinition {
    std::vector<GroupingColumn> keys;
    std::vector<AggregationDefinition> aggregations;
};

class GroupByState {
  public:
    explicit GroupByState(allocator memory) : memory_{memory} {}
    GroupByState(const GroupByState &) = delete;
    ~GroupByState() { clear(); }

    void clear() noexcept {
        auto &memory = memory_.memory_resource();
        kernels_.clear();
        rows_.release(occupied_, memory);
        occupied_.release(memory);
        for (auto entry : groups_)
            entry.value.modified.release(memory);
        groups_.release(memory);
        touched_.release(memory);
    }

    void evaluate(const In<TSF<AGG_SOURCE_SCHEMA>> &source, const GroupByDefinition &definition,
                  const In<TSB<AGG_COLUMN_SCHEMA>, all_valid> &columns,
                  const In<TSB<AGG_PARAMETER_SCHEMA>, all_valid> &parameters,
                  const Out<TSF<AGG_RESULT_SCHEMA>> &output, std::pmr::memory_resource &scratch) {
        const auto visible = source.extension_descriptor().rows;
        if (output.valid()) {
            apply_changes(source, definition, columns, parameters, visible, scratch);
        } else {
            output.clear();
            initialize(source, definition, columns, parameters, visible, scratch);
        }
        publish(output, definition.keys.size(), visible);
    }

  private:
    void initialize(const In<TSF<AGG_SOURCE_SCHEMA>> &source, const GroupByDefinition &definition,
                    const In<TSB<AGG_COLUMN_SCHEMA>, all_valid> &columns,
                    const In<TSB<AGG_PARAMETER_SCHEMA>, all_valid> &parameters, const RowSet &visible,
                    std::pmr::memory_resource &scratch) {
        for (const auto &aggregation : definition.aggregations)
            kernels_.push_back(aggregation.kernel.create(memory_));
        update_membership(source, definition, visible, scratch);

        for (auto aggregate = std::size_t{}; aggregate != definition.aggregations.size(); ++aggregate) {
            const auto &expression = *definition.aggregations[aggregate].expression;
            const auto parameter = parameters.input_data().item(static_cast<item_id_t>(aggregate));
            auto prepared = PreparedExpressionInputs{expression, columns.input_data(), parameter, scratch};
            update_aggregation(aggregate, expression, prepared, visible, visible, scratch);
        }
    }

    void apply_changes(const In<TSF<AGG_SOURCE_SCHEMA>> &source, const GroupByDefinition &definition,
                       const In<TSB<AGG_COLUMN_SCHEMA>, all_valid> &columns,
                       const In<TSB<AGG_PARAMETER_SCHEMA>, all_valid> &parameters, const RowSet &visible,
                       std::pmr::memory_resource &scratch) {
        remove_rows(source, scratch);

        auto changes = std::pmr::vector<RowChanges>{&scratch};
        for (const auto &key : definition.keys) {
            const auto input = source.input_data().item(static_cast<item_id_t>(key.field_index));
            changes.push_back(key.changes(input));
        }
        const auto additions = source.changes();
        changes.push_back({additions.added});
        auto membership = EvaluationRows{visible, &scratch};
        membership.select_changes(changes);
        if (!membership.ids.empty())
            update_membership(source, definition, membership.requested, scratch);

        const auto has_additions = additions.added != nullptr && additions.added->any();
        for (auto aggregate = std::size_t{}; aggregate != definition.aggregations.size(); ++aggregate) {
            const auto &expression = *definition.aggregations[aggregate].expression;
            const auto parameter = parameters.input_data().item(static_cast<item_id_t>(aggregate));
            const auto columns_modified = std::ranges::any_of(expression.columns, [&](const ColumnBinding &binding) {
                return columns.item_updated(binding.field_index);
            });
            if (!parameter.modified() && !columns_modified && !has_additions)
                continue;
            auto prepared = PreparedExpressionInputs{expression, columns.input_data(), parameter, scratch};
            auto requested = EvaluationRows{visible, &scratch};
            if (!parameter.modified()) {
                prepared.columns.changes.push_back({additions.added});
                requested.select_changes(prepared.columns.changes);
            }
            update_aggregation(aggregate, expression, prepared, visible, requested.requested, scratch);
        }
    }

    void update_aggregation(std::size_t aggregate, const CompiledExpression &expression,
                            const PreparedExpressionInputs &prepared, const RowSet &visible, const RowSet &requested,
                            std::pmr::memory_resource &scratch) {
        if (prepared.columns.views.front().visible_rows() != visible)
            throw std::invalid_argument{"aggregation operands must share the source row selection"};
        auto group_ids = std::pmr::vector<std::uint32_t>{&scratch};
        expression.for_each_result_batch(
            prepared.columns.views, prepared.parameter_addresses, requested, scratch,
            [&](std::span<const row_id_t> ids, const ColumnBatch<> &values) {
                group_ids.clear();
                group_ids.reserve(ids.size());
                for (const auto id : ids) {
                    const auto group = rows_[id];
                    group_ids.push_back(group);
                    mark(group, aggregate);
                }
                kernels_[aggregate]->update(ids, group_ids, values);
            });
    }

    void mark(std::uint32_t group, std::size_t aggregate) {
        auto &memory = memory_.memory_resource();
        touched_.set(group, memory);
        groups_.value_at(group).modified.set(static_cast<std::uint32_t>(aggregate), memory);
    }

    void mark_all(std::uint32_t group) {
        touched_.set(group, memory_.memory_resource());
        for (auto aggregate = std::size_t{}; aggregate != kernels_.size(); ++aggregate)
            mark(group, aggregate);
    }

    void remove_rows(const In<TSF<AGG_SOURCE_SCHEMA>> &source, std::pmr::memory_resource &scratch) {
        auto ids = std::pmr::vector<row_id_t>{&scratch};
        auto group_ids = std::pmr::vector<std::uint32_t>{&scratch};
        const auto remove = [&](row_id_t id) {
            const auto group = rows_[id];
            ids.push_back(id);
            group_ids.push_back(group);
            --groups_.value_at(group).row_count;
            mark_all(group);
        };
        source.for_each_removed(remove);
        // Reference sampling marks every surviving row modified. Remove old contributions before any
        // current IDs are used; this also handles swaps without overwriting cached entries.
        if (source.has_row_id_remapping())
            source.for_each_row_id_remapping([&](row_id_t old_id, row_id_t) { remove(old_id); });
        if (ids.empty())
            return;
        for (auto &kernel : kernels_)
            kernel->remove(ids, group_ids);
        for (const auto id : ids) {
            rows_.erase_at(id, memory_.memory_resource());
            occupied_.reset(id);
        }
    }

    void update_membership(const In<TSF<AGG_SOURCE_SCHEMA>> &source, const GroupByDefinition &definition,
                           const RowSet &requested, std::pmr::memory_resource &scratch) {
        auto views = std::pmr::vector<ErasedColumnSeriesView>{&scratch};
        for (const auto &key : definition.keys)
            views.push_back(key.read(source.input_data().item(static_cast<item_id_t>(key.field_index))));

        auto moved = std::pmr::vector<row_id_t>{&scratch};
        auto previous_groups = std::pmr::vector<std::uint32_t>{&scratch};
        auto new_groups = std::pmr::vector<std::uint32_t>{&scratch};
        auto key = GroupKey{&memory_.memory_resource()};
        key.reserve(definition.keys.size());
        for_each_aligned_column_batch(
            views, requested, scratch,
            [&](std::span<const row_id_t> ids, std::span<const ColumnBatch<>> batches) {
                for (auto position = std::size_t{}; position != ids.size(); ++position) {
                    key.clear();
                    for (auto index = std::size_t{}; index != definition.keys.size(); ++index)
                        definition.keys[index].append(key, batches[index], position, memory_);
                    const auto lookup = std::span<const KeyValue>{key};
                    auto [group, inserted] = groups_.try_emplace(std::move(key), memory_.memory_resource());
                    const auto group_index = groups_.find_position(lookup);
                    if (inserted)
                        for (auto &kernel : kernels_)
                            kernel->add_group(group_index);
                    const auto id = ids[position];
                    if (occupied_.contains(id)) {
                        const auto previous = rows_[id];
                        if (previous == group_index)
                            continue;
                        --groups_.value_at(previous).row_count;
                        mark_all(previous);
                        moved.push_back(id);
                        previous_groups.push_back(previous);
                        new_groups.push_back(group_index);
                        rows_[id] = group_index;
                    } else {
                        rows_.emplace_at(id, memory_.memory_resource(), group_index);
                        occupied_.set(id, memory_.memory_resource());
                    }
                    ++group->row_count;
                    mark_all(group_index);
                }
            });
        if (!moved.empty())
            for (auto &kernel : kernels_)
                kernel->move(moved, previous_groups, new_groups);
    }

    void publish(const Out<TSF<AGG_RESULT_SCHEMA>> &output, std::size_t key_count, const RowSet &visible) {
        const auto membership = AggregationRows{
            visible, this, [](const void *context, row_id_t id) {
                return static_cast<const GroupByState *>(context)->rows_[id];
            }};
        auto &memory = memory_.memory_resource();
        for (const auto group_index : touched_) {
            auto &group = groups_.value_at(group_index);
            const auto &key = groups_.key_at(group_index);
            if (group.row_count == 0) {
                if (group.output_row != no_row)
                    output.erase(group.output_row);
                for (auto &kernel : kernels_)
                    kernel->erase_group(group_index);
                group.modified.release(memory);
                groups_.erase(key, memory);
                continue;
            }
            if (group.output_row == no_row) {
                auto values = std::pmr::vector<std::optional<Scalar<>>>{key_count + kernels_.size(), &memory};
                for (auto index = std::size_t{}; index != key.size(); ++index) {
                    std::visit([&](const auto &value) {
                        using Value = std::remove_cvref_t<decltype(value)>;
                        if constexpr (std::same_as<Value, ScalarValue<string>>)
                            values[index] = Scalar<>::from(value);
                        else if constexpr (!std::same_as<Value, std::monostate>)
                            values[index] = Scalar<>::from<Value>(value);
                    }, key[index]);
                }
                for (auto aggregate = std::size_t{}; aggregate != kernels_.size(); ++aggregate)
                    values[key_count + aggregate] = kernels_[aggregate]->result(group_index, membership);
                group.output_row = output.add(values);
            } else {
                auto patch = std::pmr::vector<std::pair<util::symbol, std::optional<Scalar<>>>>{&memory};
                for (const auto aggregate : group.modified)
                    patch.emplace_back(output.schema().field(key_count + aggregate).name,
                                       kernels_[aggregate]->result(group_index, membership));
                if (!patch.empty())
                    output.set(group.output_row, patch);
            }
            group.modified.clear();
        }
        touched_.clear();
    }

    allocator memory_;
    util::hash_dictionary<GroupKey, Group, GroupKeyHash, GroupKeyEqual> groups_;
    util::sparse_bunch<std::uint32_t> rows_;
    util::index<> occupied_;
    util::index<> touched_;
    std::vector<std::unique_ptr<AggregationState>> kernels_;
};

struct GroupByNode {
    In<TSF<AGG_SOURCE_SCHEMA>> source;
    Scalar<Opaque<GroupByDefinition>> definition;
    In<TSB<AGG_COLUMN_SCHEMA>, all_valid> columns;
    In<TSB<AGG_PARAMETER_SCHEMA>, all_valid> parameters;
    State<GroupByState *> state;
    Out<TSF<AGG_RESULT_SCHEMA>> output;

    void start(EvaluationContext *context) const {
        auto memory = context->runtime_graph().allocation();
        *state = memory.new_object<GroupByState>(memory);
    }
    void evaluate(EvaluationContext *context) const {
        auto scratch = ScratchResource{context->scratch()};
        (*state)->evaluate(source, *definition, columns, parameters, output, scratch);
    }
    void on_invalidation() const {
        (*state)->clear();
        output.invalidate();
    }
    void stop(EvaluationContext *context) const noexcept {
        context->runtime_graph().allocation().delete_object(*state);
        *state = nullptr;
    }
};

const auto group_by_node = node<GroupByNode>();

const Schema &resolved_frame_schema(WiringPoint source) {
    const auto *frame_type = dynamic_cast<const FrameSeriesType *>(std::addressof(source.type().dereferenced_type()));
    if (frame_type == nullptr)
        throw WiringError{"frame::group_by requires a TSF input"};
    const auto *schema = dynamic_cast<const Schema *>(std::addressof(frame_type->schema_type()));
    if (schema == nullptr || !schema->is_resolved())
        throw WiringError{"frame::group_by requires a resolved source schema"};
    return *schema;
}

void validate_grouping(const Schema &schema, std::span<const util::symbol> names) {
    if (names.empty())
        throw WiringError{"frame::group_by requires at least one column"};

    for (auto index = std::size_t{}; index != names.size(); ++index) {
        const auto name = names[index];
        const auto *field = schema.find(name);
        if (field == nullptr)
            throw WiringError{"frame::group_by names an unknown column"};
        if (std::ranges::find(names.first(index), name) != names.begin() + index)
            throw WiringError{"frame::group_by contains a repeated column name"};
        const auto &type = *field->type;
        if (std::addressof(type) != std::addressof(cpp_scalar_type<i64>()) &&
            std::addressof(type) != std::addressof(cpp_scalar_type<string>()) &&
            std::addressof(type) != std::addressof(cpp_scalar_type<symbol>()))
            throw WiringError{"frame::group_by supports only i64, string, or symbol columns"};
    }
}

WiringArgument bundle_argument(WiringPoint bundle, util::symbol name) {
    const auto &type = static_cast<const TimeSeriesBundleType &>(bundle.type().dereferenced_type());
    const auto index = type.find_item_index(name);
    if (!index)
        throw WiringError{"frame group expression is missing a required field"};
    return hg::details::time_series_bundle_argument(bundle, *index);
}

std::vector<WiringArgument> bundle_arguments(WiringPoint bundle) {
    const auto &type = static_cast<const TimeSeriesBundleType &>(bundle.type().dereferenced_type());
    auto arguments = std::vector<WiringArgument>{};
    arguments.reserve(type.item_count());
    for (auto index = std::size_t{}; index != type.item_count(); ++index) {
        auto argument = hg::details::time_series_bundle_argument(bundle, index);
        argument.set_name(type.item_name(index));
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

std::vector<util::symbol> read_keys(WiringPoint group) {
    const auto argument = bundle_argument(group, util::symbol{"keys"});
    if (!argument.is_scalar() ||
        std::addressof(argument.type()) != std::addressof(scalar_type_traits<List<symbol>>::type()))
        throw WiringError{"frame group key names are not a wiring-time List<symbol>"};
    const auto scalar = argument.scalar();
    const auto values = Scalar<List<symbol>>::from(scalar);
    auto names = std::vector<util::symbol>{};
    names.reserve(values.size());
    for (auto index = std::size_t{}; index != values.size(); ++index)
        names.push_back(values[index].value());
    return names;
}

WiringPoint read_source(WiringPoint group) {
    const auto argument = bundle_argument(group, util::symbol{"source"});
    if (!argument.is_time_series())
        throw WiringError{"frame group expression does not retain its source TSF"};
    return argument.time_series();
}

struct BoundAggregation {
    WiringPoint operand;
    AggregationFunctionAdapter kernel;
};

BoundAggregation bind_aggregation(WiringPoint source, const WiringArgument &descriptor) {
    if (!descriptor.is_time_series() || !AggregationExpression::type().accepts(descriptor.time_series().type()))
        throw WiringError{"frame::agg requires aggregation expressions"};
    const auto point = descriptor.time_series();
    const auto operation = bundle_argument(point, util::symbol{"operation"});
    if (!operation.is_scalar() || &operation.type() != &scalar_type_traits<AggregationOperation>::type())
        throw WiringError{"aggregation operation must be available during wiring"};
    const auto operation_value = operation.scalar();
    const auto &kernels = *Scalar<AggregationOperation>::from(operation_value).value();
    const auto argument = bundle_argument(point, util::symbol{"operand"});
    const auto typed_source = wp<TSF<AGG_SOURCE_SCHEMA>>{source};
    const auto operand = [&]() -> WiringPoint {
        if (argument.is_scalar() && &argument.type() == &scalar_type_traits<Expression>::type()) {
            const auto scalar = argument.scalar();
            return bind_expression(typed_source, Scalar<Expression>::from(scalar).value());
        }
        if (argument.is_time_series() && is_mixed_expression_bundle_type(argument.time_series().type()))
            return bind_expression(typed_source, wp<>{argument.time_series()});
        throw WiringError{"aggregation requires a wiring-time Expression or a mixed column expression"};
    }();
    const auto &cell = static_cast<const ColumnSeriesType &>(operand.type().dereferenced_type()).cell_type();
    const auto *kernel = kernels.find(cell);
    if (kernel == nullptr)
        throw WiringError{"aggregation has no kernel for its operand cell type"};
    return {operand, *kernel};
}

const Schema &infer_group_schema(WiringPoint group, std::span<const WiringArgument> aggregations,
                                 std::vector<BoundAggregation> &bound, std::vector<util::symbol> &keys) {
    const auto source = read_source(group);
    const auto &source_schema = resolved_frame_schema(source);
    keys = read_keys(group);
    validate_grouping(source_schema, keys);

    auto fields = std::vector<SchemaField>{};
    fields.reserve(keys.size() + aggregations.size());
    for (const auto name : keys)
        fields.push_back(*source_schema.find(name));

    auto output_names = keys;
    bound.reserve(aggregations.size());
    for (const auto &aggregation : aggregations) {
        if (!aggregation.name())
            throw WiringError{"frame::agg assignments must have names"};
        const auto name = *aggregation.name();
        if (std::ranges::find(output_names, name) != output_names.end())
            throw WiringError{"frame::agg contains a duplicate output name"};
        output_names.push_back(name);
        auto resolved = bind_aggregation(source, aggregation);
        fields.push_back({name, resolved.kernel.result_type, false});
        bound.push_back(std::move(resolved));
    }

    auto key_names = std::vector<std::string_view>{};
    key_names.reserve(keys.size());
    for (const auto name : keys)
        key_names.push_back(name.text());
    const auto value = ScalarValue<List<string>>{std::span<const std::string_view>{key_names}};
    return anonymous_schema(fields, make_schema_metadata({schema_metadata_entry("hg.frame.key", value)}));
}

GroupingColumn grouping_column(const Schema &schema, util::symbol name) {
    const auto *field = schema.find(name);
    const auto index = static_cast<std::size_t>(field - schema.fields().data());
    return visit_cell_type(static_cast<const ScalarType &>(*field->type),
        [&]<typename Cell>(std::type_identity<Cell>) -> GroupingColumn {
            if constexpr (std::same_as<Cell, i64> || std::same_as<Cell, string> || std::same_as<Cell, symbol>) {
                return {
                    index,
                    [](const TimeSeriesInputData &data) {
                        return ErasedColumnSeriesView{In<TSC<Cell>>{data}.extension_descriptor().storage};
                    },
                    [](const TimeSeriesInputData &data) { return In<TSC<Cell>>{data}.changes(); },
                    [](GroupKey &key, const ColumnBatch<> &batch, std::size_t position, allocator memory) {
                        if (!batch.is_valid(position)) {
                            key.emplace_back();
                            return;
                        }
                        const auto &value = static_cast<const cell_stored_t<Cell> *>(batch.values)[position];
                        if constexpr (std::same_as<Cell, string>)
                            key.emplace_back(ScalarValue<string>{Scalar<string>::from(value), memory});
                        else
                            key.emplace_back(scalar_storage_traits<Cell>::read(value));
                    }};
            } else {
                throw WiringError{"unsupported grouping cell type"};
            }
        });
}

WiringPoint materialize_aggregation(WiringPoint group, std::span<const WiringArgument> aggregations) {
    auto bound = std::vector<BoundAggregation>{};
    auto keys = std::vector<util::symbol>{};
    const auto &result_schema = infer_group_schema(group, aggregations, bound, keys);
    const auto source = read_source(group);
    const auto &source_schema = resolved_frame_schema(source);
    auto definition = GroupByDefinition{};
    for (const auto key : keys)
        definition.keys.push_back(grouping_column(source_schema, key));
    auto column_arguments = std::vector<WiringArgument>{};
    auto parameter_arguments = std::vector<WiringArgument>{};
    for (auto index = std::size_t{}; index != bound.size(); ++index) {
        auto inputs = compile_column_expression(bound[index].operand, column_arguments.size());
        definition.aggregations.push_back({inputs.expression.view().value(), bound[index].kernel});
        for (auto column : bundle_arguments(inputs.columns)) {
            column.set_name(util::symbol{"c" + std::to_string(column_arguments.size())});
            column_arguments.push_back(std::move(column));
        }
        parameter_arguments.emplace_back(*aggregations[index].name(), inputs.parameters);
    }
    const auto columns = bundle(std::span<const WiringArgument>{column_arguments});
    const auto parameters = bundle(std::span<const WiringArgument>{parameter_arguments});
    auto type_map = TypeMap{};
    type_map.set(hg::details::schema_variable_instance<AGG_SOURCE_SCHEMA>(), &source_schema);
    type_map.set(hg::details::schema_variable_instance<AGG_RESULT_SCHEMA>(), &result_schema);
    return group_by_node(type_map, wp<TSF<AGG_SOURCE_SCHEMA>>{source},
                         ScalarValue<Opaque<GroupByDefinition>>{std::move(definition)},
                         wp<TSB<AGG_COLUMN_SCHEMA>>{columns}, wp<TSB<AGG_PARAMETER_SCHEMA>>{parameters});
}

WiringPoint build_aggregation(wp<GroupByExpression> group, wp<TSB<operators::AGGREGATIONS>> aggregations) {
    auto bound = std::vector<BoundAggregation>{};
    auto keys = std::vector<util::symbol>{};
    const auto fields = bundle_arguments(aggregations);
    const auto &schema = infer_group_schema(group, fields, bound, keys);
    const auto &type = frame_series_type(schema, TimeSeriesGuarantee::ordinary);
    auto &wiring = WiringContext::current().wiring();
    auto output = wiring.create_wp(type, hg::details::make_wiring_arguments(group, aggregations));
    const auto builder = [](wp<GroupByExpression> group, wp<TSB<operators::AGGREGATIONS>> aggregations) {
        return materialize_aggregation(group, bundle_arguments(aggregations));
    };
    output.add_materialisation_option(type, 0.0,
        hg::details::make_graph_input_signature<decltype(builder)>(util::symbol{"materialisation"}),
        WiringMaterialisationBuilder{[builder](std::span<const WiringArgument> arguments) {
            return hg::details::invoke_graph_function(builder, arguments);
        }});
    return output;
}

RequirementResult require_frame_output(const WiringArguments &arguments) {
    const auto mapping = arguments.type_map().find(hg::details::resolver_variable<OUT>());
    if (mapping == nullptr)
        return accept;
    const auto *type = std::get_if<const Type *>(mapping);
    const auto *series = type == nullptr || *type == nullptr ? nullptr : dynamic_cast<const TimeSeriesType *>(*type);
    if (dynamic_cast<const FrameSeriesType *>(series) == nullptr)
        return reject("frame::agg requires a TSF output type");
    return accept;
}

} // namespace

} // namespace hg::frame::details

namespace hg::frame {
namespace {

using namespace hg::literals;

const auto sum_expression = operators::sum_.overload(graph<OUT>(
    "name"_n = "frame_sum_expression",
    [](Scalar<Expression> expression) -> wp<OUT> {
        const auto operation = ScalarValue<AggregationOperation>{&sum_aggregation_kernels()};
        return wp<OUT>{bundle<AggregationExpression>("operation"_n = operation.view(), "operand"_n = expression)};
    }));

const auto sum_mixed_expression = operators::sum_.overload(graph<OUT>(
    "name"_n = "frame_sum_mixed_expression",
    [](wp<details::MIXED_SERIES_LEFT> expression) -> wp<OUT> {
        const auto operation = ScalarValue<AggregationOperation>{&sum_aggregation_kernels()};
        return wp<OUT>{bundle<AggregationExpression>("operation"_n = operation.view(), "operand"_n = expression)};
    }));

const auto group_by_graph = operators::group_by.overload(graph(
    "name"_n = "frame_group_by",
    [](wp<TSF<SCHEMA>> source, Scalar<List<symbol>> keys) -> wp<GroupByExpression> {
        auto names = std::vector<util::symbol>{};
        names.reserve(keys.size());
        for (auto index = std::size_t{}; index != keys.size(); ++index)
            names.push_back(keys[index].value());
        details::validate_grouping(details::resolved_frame_schema(source), names);

        return bundle<GroupByExpression>("source"_n = source, "keys"_n = keys);
    }));

const auto aggregate_expression = operators::agg.overload(graph<OUT>(
    "name"_n = "frame_agg",
    require = [](const WiringArguments &arguments) { return details::require_frame_output(arguments); },
    [](wp<GroupByExpression> group,
       wp<TSB<operators::AGGREGATIONS>, var_kwargs> aggregations) -> wp<OUT> {
        return wp<OUT>{details::build_aggregation(group, wp<TSB<operators::AGGREGATIONS>>{aggregations})};
    }));

} // namespace
} // namespace hg::frame
