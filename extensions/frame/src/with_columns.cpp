#include <bg/frame/frame.hpp>
#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/operators.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/scalar/opaque.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/time_series/tsb/details/wiring.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace bg::operators {

operator_<OUT, auto(frame::TSF<SCHEMA>, TSB<FIELDS>) -> OUT> with_columns{"with_columns", {"source", "assignments"}};
operator_<OUT, auto(frame::TSF<SCHEMA>, TSB<FIELDS>) -> OUT> select{"select", {"source", "selections"}};
operator_<OUT, auto(frame::TSF<SCHEMA>, TSB<FIELDS>) -> OUT> rename{"rename", {"source", "mappings"}};

} // namespace bg::operators

namespace bg::frame::details {
namespace {

using namespace bg::literals;

template <template <typename, auto...> typename Field>
using SOURCE_SCHEMA = SchemaVariableDeclaration<"FRAME_WITH_COLUMNS_SOURCE_SCHEMA">;
template <template <typename, auto...> typename Field>
using ASSIGNMENT_SCHEMA = BundleSchemaVariableDeclaration<"FRAME_WITH_COLUMNS_ASSIGNMENT_SCHEMA", Field>;

std::optional<std::vector<std::string>> key_names(const Schema &schema) {
    const auto *key = schema.metadata().find("bg.frame.key");
    if (key == nullptr)
        return std::nullopt;

    const auto names = Scalar<List<string>>::from(key->runtime_type(), key->data());
    auto result = std::vector<std::string>{};
    result.reserve(names.size());
    for (auto index = std::size_t{}; index != names.size(); ++index)
        result.emplace_back(names[index].value());
    return result;
}

std::vector<SchemaField> with_columns_fields(const Schema &source_schema,
                                             const TimeSeriesBundleType &assignment_schema) {
    auto fields = std::vector<SchemaField>{source_schema.fields().begin(), source_schema.fields().end()};
    fields.reserve(fields.size() + assignment_schema.item_count());
    const auto keys = key_names(source_schema);
    for (auto index = std::size_t{}; index != assignment_schema.item_count(); ++index) {
        const auto name = assignment_schema.item_name(index);
        const auto &column_type = static_cast<const ColumnSeriesType &>(
            assignment_schema.item_type(index).dereferenced_type());

        if (keys)
            for (const auto &key : *keys)
                if (key == name.text())
                    throw WiringError{"frame::with_columns cannot assign a key column"};

        const auto field = std::ranges::find(fields, name, &SchemaField::name);
        const auto replacement = SchemaField{name, std::addressof(column_type.cell_type()), false};
        if (field == fields.end())
            fields.push_back(replacement);
        else
            *field = replacement;
    }
    return fields;
}

void validate_result_schema(const Schema &expected, const Schema &result) {
    if (expected.field_count() != result.field_count())
        throw WiringError{"frame::with_columns result schema has different fields"};
    for (const auto &field : expected.fields()) {
        const auto *result_field = result.find(field.name);
        if (result_field == nullptr || result_field->type != field.type || result_field->optional != field.optional)
            throw WiringError{"frame::with_columns result schema fields do not match the assignments"};
    }
    if (key_names(expected) != key_names(result))
        throw WiringError{"frame::with_columns result schema must preserve the ordered key fields"};
}

struct WithColumnsNode {
    In<TSF<SOURCE_SCHEMA>> source;
    In<TSB<ASSIGNMENT_SCHEMA>, all_valid> assignments;
    Out<TSF<frame::RESULT>, view> output;

    static const Schema &resolve(schema_type<frame::RESULT>, const WiringArguments &arguments) {
        const auto &source_type = static_cast<const FrameSeriesType &>(arguments.resolved_type("source"));
        const auto &source_schema = static_cast<const Schema &>(source_type.schema_type());
        const auto &assignment_schema =
            static_cast<const TimeSeriesBundleType &>(arguments.resolved_type("assignments"));
        const auto fields = with_columns_fields(source_schema, assignment_schema);
        return anonymous_schema(fields, source_schema.metadata());
    }

    void evaluate() const { static_cast<void>(output.bind_columns(source, assignments)); }
};

template <cell_type Cell> struct BroadcastColumnNode {
    In<TSF<SOURCE_SCHEMA>> source;
    In<TS<Cell>> value;
    Out<TSC<Cell>, computed<>> output;

    void evaluate() const {
        const auto fill_all = !output.valid() || value.modified();
        output.bind(source);
        const auto cell = *value;
        auto ids = std::array<row_id_t, batch_capacity>{};
        auto size = std::size_t{};
        const auto flush = [&] {
            if (size == 0)
                return;
            write_batch(std::span<const row_id_t>{ids.data(), size}, cell);
            size = 0;
        };
        const auto append = [&](row_id_t id) {
            ids[size++] = id;
            if (size == batch_capacity)
                flush();
        };

        if (fill_all)
            source.for_each_row([&](row_id_t id, std::size_t) { append(id); });
        else
            output.for_each_added(append);
        flush();
    }

  private:
    static constexpr auto batch_capacity = std::size_t{128};

    void write_batch(std::span<const row_id_t> ids, const cell_value_t<Cell> &value) const {
        if constexpr (std::same_as<Cell, bool>) {
            auto values = std::array<std::uint8_t, (batch_capacity + 7) / 8>{};
            if (value) {
                values.fill(0xff);
            }
            output.set_batch(ids, ColumnBatch<bool>{0, ids.size(), values.data(),
                                                     ColumnBatch<bool>::Validity::all_valid()});
        } else if constexpr (std::same_as<Cell, string>) {
            const auto source = ColumnBatchSource<string>{
                std::addressof(value),
                [](const void *context, std::size_t) noexcept {
                    return *static_cast<const std::string_view *>(context);
                },
                0};
            output.set_batch(ids, ColumnBatch<string>{0, ids.size(), source,
                                                       ColumnBatch<string>::Validity::all_valid()});
        } else {
            auto values = std::array<cell_stored_t<Cell>, batch_capacity>{};
            std::ranges::fill(values, value);
            output.set_batch(ids, ColumnBatch<Cell>{0, ids.size(), values.data(),
                                                     ColumnBatch<Cell>::Validity::all_valid()});
        }
    }
};

struct ProjectionNode {
    In<TSF<SOURCE_SCHEMA>> source;
    Scalar<Opaque<std::vector<std::size_t>>> source_indices;
    Out<TSF<frame::RESULT>, view> output;

    void evaluate() const { output.bind_projection(source, source_indices.value()); }
};

const auto with_columns_node = node<WithColumnsNode>();
const auto projection_node = node<ProjectionNode>();

const Schema &source_frame_schema(WiringPoint source) {
    const auto *frame = dynamic_cast<const FrameSeriesType *>(std::addressof(source.type()));
    if (frame == nullptr)
        throw WiringError{"frame projection requires a TSF input"};
    const auto *schema = dynamic_cast<const Schema *>(std::addressof(frame->schema_type()));
    if (schema == nullptr)
        throw WiringError{"frame projection requires a resolved source schema"};
    return *schema;
}

const Schema *requested_frame_schema(const TypeMap &type_map, const Variable &result_variable) {
    const auto *mapping = type_map.find(result_variable);
    if (mapping == nullptr)
        return nullptr;

    const auto *mapped_type = std::get_if<const Type *>(mapping);
    const auto *time_series = mapped_type == nullptr || *mapped_type == nullptr
                                  ? nullptr
                                  : type_cast<TimeSeriesType>(*mapped_type);
    const auto *frame = dynamic_cast<const FrameSeriesType *>(time_series);
    if (frame == nullptr)
        throw WiringError{"frame operators require a TSF output type"};
    if (!frame->guarantee().is_resolved() || frame->guarantee().value() != TimeSeriesGuarantee::ordinary)
        throw WiringError{"frame operators require an ordinary TSF output guarantee"};

    const auto *schema = dynamic_cast<const Schema *>(std::addressof(frame->schema_type()));
    if (schema == nullptr || !schema->is_resolved())
        throw WiringError{"frame operators require a resolved output schema"};
    return schema;
}

RequirementResult require_frame_output(const WiringArguments &arguments, const Variable &result_variable) {
    try {
        static_cast<void>(requested_frame_schema(arguments.type_map(), result_variable));
    } catch (const WiringError &error) {
        return reject(error.what());
    }
    return accept;
}

SchemaMetadata projected_metadata(const Schema &source_schema,
                                  std::span<const std::pair<std::size_t, util::symbol>> output_fields) {
    const auto source_keys = key_names(source_schema);
    if (!source_keys)
        return source_schema.metadata();

    auto output_keys = std::vector<std::string_view>{};
    output_keys.reserve(source_keys->size());
    for (const auto &key : *source_keys) {
        auto match = std::optional<util::symbol>{};
        for (const auto &[source_index, output_name] : output_fields) {
            if (source_schema.field(source_index).name.text() != key)
                continue;
            match = output_name;
            break;
        }
        if (!match) {
            return source_schema.metadata().copy_with({}, {"bg.frame.key"});
        }
        output_keys.push_back(match->text());
    }

    const auto value = ScalarValue<List<string>>{std::span<const std::string_view>{output_keys}};
    const auto key_override = make_schema_metadata({schema_metadata_entry("bg.frame.key", value)});
    return source_schema.metadata().copy_with(key_override);
}

void validate_projection_keys(const Schema &source_schema, const Schema &result_schema,
                              std::span<const std::size_t> source_indices) {
    const auto source_keys = key_names(source_schema);
    const auto result_keys = key_names(result_schema);
    if (!result_keys)
        return;
    if (!source_keys || source_keys->size() != result_keys->size())
        throw WiringError{"frame projection target schema must preserve the complete ordered key tuple"};

    for (auto key_index = std::size_t{}; key_index != source_keys->size(); ++key_index) {
        const auto *source_key = source_schema.find(util::symbol{(*source_keys)[key_index]});
        const auto *result_key = result_schema.find(util::symbol{(*result_keys)[key_index]});
        if (source_key == nullptr || result_key == nullptr)
            throw WiringError{"frame projection target schema names an unknown key column"};
        const auto result_index = static_cast<std::size_t>(result_key - result_schema.fields().data());
        const auto source_index = static_cast<std::size_t>(source_key - source_schema.fields().data());
        if (result_index >= source_indices.size() || source_indices[result_index] != source_index)
            throw WiringError{"frame projection target schema does not preserve the ordered key tuple"};
    }
}

WiringPoint wire_projection(WiringPoint source, std::span<const SchemaField> fields,
                            std::span<const std::size_t> source_indices, SchemaMetadata metadata,
                            const Schema *requested_schema = nullptr) {
    const auto &result_schema = requested_schema == nullptr ? anonymous_schema(fields, metadata) : *requested_schema;
    if (requested_schema != nullptr) {
        if (fields.size() != result_schema.field_count())
            throw WiringError{"frame projection target schema has a different number of columns"};
        for (auto index = std::size_t{}; index != fields.size(); ++index) {
            if (fields[index].name != result_schema.field(index).name)
                throw WiringError{"frame projection target schema has different column names"};
            if (fields[index].type != result_schema.field(index).type ||
                fields[index].optional != result_schema.field(index).optional)
                throw WiringError{"frame projection target schema has incompatible column types"};
        }

        validate_projection_keys(source_frame_schema(source), result_schema, source_indices);
    }

    auto type_map = TypeMap{};
    type_map.set(bg::details::schema_variable_instance<frame::RESULT>(), &result_schema);
    const auto indices = ScalarValue<Opaque<std::vector<std::size_t>>>{
        std::vector<std::size_t>{source_indices.begin(), source_indices.end()}};
    return projection_node(type_map, wp<TSF<SOURCE_SCHEMA>>{source}, indices);
}

void validate_assignment_result_schema(const Schema &source_schema, const TimeSeriesBundleType &assignment_schema,
                                       const Schema &result_schema) {
    const auto fields = with_columns_fields(source_schema, assignment_schema);
    const auto &expected = anonymous_schema(fields, source_schema.metadata());
    validate_result_schema(expected, result_schema);
}

template <cell_type Cell> WiringPoint broadcast_column(wp<TSF<SOURCE_SCHEMA>> source, wp<TS<Cell>> value) {
    static const auto broadcast = node<BroadcastColumnNode<Cell>>();
    return broadcast(source, value);
}

void validate_cell_type(const Type &type) {
    try {
        static_cast<void>(frame::visit_cell_type(type, []<typename Cell>(std::type_identity<Cell>) { return true; }));
    } catch (const std::invalid_argument &) {
        throw WiringError{"frame::with_columns supports only bool, i64, double, string, or symbol values"};
    }
}

WiringPoint broadcast_series(wp<TSF<SOURCE_SCHEMA>> source, WiringPoint value) {
    const auto &type = static_cast<const TimeSeriesValueType &>(value.type().dereferenced_type());
    validate_cell_type(type.value_type());
    return frame::visit_cell_type(type.value_type(), [&]<typename Cell>(std::type_identity<Cell>) {
        return broadcast_column<Cell>(source, value.template as<TS<Cell>>());
    });
}

std::optional<WiringPoint> bind_assignment_expression(wp<TSF<SOURCE_SCHEMA>> source,
                                                      const WiringArgument &assignment) {
    if (assignment.is_time_series()) {
        const auto point = assignment.time_series();
        if (is_mixed_expression_bundle_type(point.type()))
            return frame::bind_expression(source, wp<>{point});
        return std::nullopt;
    }

    if (assignment.is_scalar() &&
        std::addressof(assignment.type()) == std::addressof(scalar_type_traits<Expression>::type())) {
        const auto scalar = assignment.scalar();
        const auto expression = Scalar<Expression>::from(scalar).value();
        return frame::bind_expression(source, expression);
    }

    return std::nullopt;
}

WiringPoint broadcast_scalar(wp<TSF<SOURCE_SCHEMA>> source, const WiringArgument &argument) {
    const auto scalar = argument.scalar();
    validate_cell_type(argument.type());
    return frame::visit_cell_type(argument.type(), [&]<typename Cell>(std::type_identity<Cell>) {
        const auto value = Scalar<Cell>::from(scalar);
        const auto constant = operators::const_(value).template as<TS<Cell>>();
        return broadcast_column<Cell>(source, constant);
    });
}

WiringPoint build_assignment_bundle(WiringPoint source, std::span<const WiringArgument> assignments) {
    auto &wiring = WiringContext::current().wiring();
    const auto typed_source = wp<TSF<SOURCE_SCHEMA>>{source};
    auto assigned_fields = std::vector<WiringArgument>{};
    assigned_fields.reserve(assignments.size());

    for (const auto &assignment : assignments) {
        if (!assignment.name())
            throw WiringError{"frame::with_columns assignments must have names"};
        const auto name = *assignment.name();

        auto column = [&] {
            if (const auto expression = bind_assignment_expression(typed_source, assignment))
                return *expression;
            if (assignment.is_time_series()) {
                const auto point = assignment.time_series();
                if (type_cast<TimeSeriesValueType>(std::addressof(point.type().dereferenced_type())) != nullptr)
                    return broadcast_series(typed_source, point);
                return point;
            }
            if (assignment.is_scalar())
                return broadcast_scalar(typed_source, assignment);
            throw WiringError{
                "frame::with_columns assignments must be scalar values, TS values, TSC columns, or column expressions"};
        }();

        const auto &item_type = column.type().dereferenced_type();
        const auto *column_type = dynamic_cast<const ColumnSeriesType *>(std::addressof(item_type));
        if (column_type == nullptr)
            throw WiringError{"frame::with_columns assignments must be TSC columns"};

        const auto &materialized_type = column_series_type(column_type->cell_type(), TimeSeriesGuarantee::ordinary);
        const auto materialized = wiring.materialisation_form(column, materialized_type);
        assigned_fields.emplace_back(name, materialized);
    }

    return bundle(std::span<const WiringArgument>{assigned_fields});
}

template <typename Name> util::symbol frame_column_name(const WiringArguments &arguments) {
    const auto value = arguments.scalar(1);
    if (!value)
        return {};
    if constexpr (std::same_as<Name, util::symbol>)
        return scalar_cast<util::symbol>(*value).value();
    else
        return util::symbol{scalar_cast<string>(*value).value()};
}

const TimeSeriesType *frame_column_type(const WiringArguments &arguments, util::symbol name) {
    const auto *frame = dynamic_cast<const FrameSeriesType *>(std::addressof(arguments.resolved_type(0)));
    if (frame == nullptr)
        return nullptr;
    const auto *schema = dynamic_cast<const Schema *>(std::addressof(frame->schema_type()));
    if (schema == nullptr)
        return nullptr;
    const auto *field = schema->find(name);
    if (field == nullptr)
        return nullptr;
    const auto index = static_cast<std::size_t>(field - schema->fields().data());
    return std::addressof(frame->item_type(index));
}

template <typename Name>
auto frame_column_accessor(std::initializer_list<WiringParameter> parameter_names) {
    return graph<OUT>(
        parameter_names,
        resolve<OUT> = [](const WiringArguments &arguments) -> const TimeSeriesType * {
            return frame_column_type(arguments, frame_column_name<Name>(arguments));
        },
        [](wp<TSF<SOURCE_SCHEMA>> frame, Name name) {
            return wp<OUT>{select_column<SCALAR>(frame, frame_column_symbol(name))};
        });
}

std::vector<WiringArgument> bundle_arguments(WiringPoint bundle) {
    const auto &type = static_cast<const TimeSeriesBundleType &>(bundle.type().dereferenced_type());

    auto arguments = std::vector<WiringArgument>{};
    arguments.reserve(type.item_count());
    for (auto index = std::size_t{}; index != type.item_count(); ++index) {
        auto argument = bg::details::time_series_bundle_argument(bundle, index);
        argument.set_name(type.item_name(index));
        arguments.push_back(std::move(argument));
    }
    return arguments;
}

WiringPoint build_with_columns(WiringPoint source, std::span<const WiringArgument> assignments,
                               const Schema *result_schema) {
    const auto assigned_bundle = build_assignment_bundle(source, assignments);
    const auto &assigned_type = static_cast<const TimeSeriesBundleType &>(assigned_bundle.type().dereferenced_type());
    if (result_schema == nullptr)
        return with_columns_node(source, wp<TSB<ASSIGNMENT_SCHEMA>>{assigned_bundle});

    validate_assignment_result_schema(source_frame_schema(source), assigned_type, *result_schema);
    auto type_map = TypeMap{};
    type_map.set(bg::details::schema_variable_instance<frame::RESULT>(), result_schema);
    return with_columns_node(type_map, source, wp<TSB<ASSIGNMENT_SCHEMA>>{assigned_bundle});
}

WiringPoint select(WiringPoint source, std::span<const WiringArgument> selections, const Schema *result_schema) {
    const auto &source_schema = source_frame_schema(source);
    const auto typed_source = wp<TSF<SOURCE_SCHEMA>>{source};

    auto source_columns = std::vector<WiringPoint>{};
    source_columns.reserve(source_schema.field_count());
    for (const auto &field : source_schema.fields())
        source_columns.emplace_back(select_column<SCALAR>(typed_source, field.name));

    auto borrowed_fields = std::vector<SchemaField>{};
    auto borrowed_indices = std::vector<std::size_t>{};
    auto borrowed_key_fields = std::vector<std::pair<std::size_t, util::symbol>>{};
    auto assignments = std::vector<WiringArgument>{};
    borrowed_fields.reserve(selections.size());
    borrowed_indices.reserve(selections.size());
    borrowed_key_fields.reserve(selections.size());
    assignments.reserve(selections.size());

    for (const auto &selection : selections) {
        const auto name = *selection.name();

        auto selected_point = bind_assignment_expression(typed_source, selection);
        if (!selected_point && selection.is_time_series())
            selected_point = selection.time_series();

        auto borrowed_index = std::optional<std::size_t>{};
        if (selected_point) {
            const auto point = *selected_point;
            if (dynamic_cast<const ColumnSeriesType *>(std::addressof(point.type().dereferenced_type())) != nullptr) {
                for (auto index = std::size_t{}; index != source_columns.size(); ++index) {
                    if (point.record() == source_columns[index].record()) {
                        borrowed_index = index;
                        break;
                    }
                }
            }
        }

        if (!borrowed_index) {
            if (selected_point)
                assignments.emplace_back(name, *selected_point);
            else
                assignments.emplace_back(selection);
            continue;
        }

        auto field = source_schema.field(*borrowed_index);
        field.name = name;
        borrowed_fields.push_back(field);
        borrowed_indices.push_back(*borrowed_index);
        borrowed_key_fields.emplace_back(*borrowed_index, name);
    }

    auto borrowed_metadata = result_schema == nullptr ? projected_metadata(source_schema, borrowed_key_fields)
                                                      : result_schema->metadata();
    if (result_schema != nullptr) {
        const auto &metadata_schema = anonymous_schema(borrowed_fields, borrowed_metadata);
        validate_projection_keys(source_schema, metadata_schema, borrowed_indices);
    }
    const auto &projected_schema = anonymous_schema(borrowed_fields, borrowed_metadata);
    if (assignments.empty())
        return wire_projection(source, borrowed_fields, borrowed_indices, std::move(borrowed_metadata), result_schema);

    const auto projected = wire_projection(source, borrowed_fields, borrowed_indices, borrowed_metadata);

    const auto assigned_bundle = build_assignment_bundle(source, assignments);
    const auto &assignment_schema =
        static_cast<const TimeSeriesBundleType &>(assigned_bundle.type().dereferenced_type());
    auto result_fields = std::vector<SchemaField>{};
    result_fields.reserve(selections.size());
    for (const auto &selection : selections) {
        const auto name = *selection.name();
        const auto borrowed = std::ranges::find(borrowed_fields, name, &SchemaField::name);
        if (borrowed != borrowed_fields.end()) {
            result_fields.push_back(*borrowed);
            continue;
        }

        const auto assignment_index = assignment_schema.find_item_index(name);
        if (!assignment_index)
            throw std::logic_error{"frame::select assignment is absent from its resolved bundle"};
        const auto &column_type = static_cast<const ColumnSeriesType &>(
            assignment_schema.item_type(*assignment_index).dereferenced_type());
        result_fields.push_back({name, std::addressof(column_type.cell_type()), false});
    }

    const auto &inferred_schema = anonymous_schema(result_fields, borrowed_metadata);
    const auto &output_schema = result_schema == nullptr ? inferred_schema : *result_schema;
    validate_assignment_result_schema(projected_schema, assignment_schema, output_schema);
    auto type_map = TypeMap{};
    type_map.set(bg::details::schema_variable_instance<frame::RESULT>(), &output_schema);
    return with_columns_node(type_map, projected, wp<TSB<ASSIGNMENT_SCHEMA>>{assigned_bundle});
}

WiringPoint rename(WiringPoint source, std::span<const std::pair<util::symbol, util::symbol>> mappings,
                   const Schema *result_schema) {
    const auto &source_schema = source_frame_schema(source);
    auto fields = std::vector<SchemaField>{source_schema.fields().begin(), source_schema.fields().end()};
    auto source_indices = std::vector<std::size_t>(fields.size());
    auto output_fields = std::vector<std::pair<std::size_t, util::symbol>>{};
    auto renamed_sources = std::vector<util::symbol>{};
    output_fields.reserve(fields.size());
    renamed_sources.reserve(mappings.size());
    std::iota(source_indices.begin(), source_indices.end(), std::size_t{});
    for (const auto &[old_name, new_name] : mappings) {
        const auto *field = source_schema.find(old_name);
        if (field == nullptr)
            throw WiringError{"frame::rename names an unknown source column"};
        if (std::ranges::find(renamed_sources, old_name) != renamed_sources.end())
            throw WiringError{"frame::rename contains a repeated source column"};
        renamed_sources.push_back(old_name);
        const auto index = static_cast<std::size_t>(field - source_schema.fields().data());
        fields[index].name = new_name;
    }
    for (auto index = std::size_t{}; index != fields.size(); ++index) {
        if (std::ranges::find(fields, fields[index].name, &SchemaField::name) != fields.begin() + index)
            throw WiringError{"frame::rename creates duplicate output column names"};
        output_fields.emplace_back(index, fields[index].name);
    }
    auto metadata = result_schema == nullptr ? projected_metadata(source_schema, output_fields)
                                             : result_schema->metadata();
    return wire_projection(source, fields, source_indices, std::move(metadata), result_schema);
}

WiringPoint drop(WiringPoint source, std::span<const util::symbol> names, const Schema *result_schema) {
    const auto &source_schema = source_frame_schema(source);
    auto removed = std::vector<util::symbol>{};
    removed.reserve(names.size());
    for (const auto name : names) {
        if (source_schema.find(name) == nullptr)
            throw WiringError{"frame::drop names an unknown column"};
        if (std::ranges::find(removed, name) != removed.end())
            throw WiringError{"frame::drop contains a repeated column name"};
        removed.push_back(name);
    }

    auto fields = std::vector<SchemaField>{};
    auto source_indices = std::vector<std::size_t>{};
    auto output_fields = std::vector<std::pair<std::size_t, util::symbol>>{};
    for (auto index = std::size_t{}; index != source_schema.field_count(); ++index) {
        const auto &field = source_schema.field(index);
        if (std::ranges::find(removed, field.name) != removed.end())
            continue;
        fields.push_back(field);
        source_indices.push_back(index);
        output_fields.emplace_back(index, field.name);
    }
    auto metadata = result_schema == nullptr ? projected_metadata(source_schema, output_fields)
                                             : result_schema->metadata();
    return wire_projection(source, fields, source_indices, std::move(metadata), result_schema);
}

} // namespace

} // namespace bg::frame::details

namespace bg::frame {
namespace {

using namespace bg::literals;

const auto registered_with_columns = operators::with_columns.overload(graph<OUT>(
    "name"_n = "frame_with_columns",
    require = [](const WiringArguments &arguments) {
        return details::require_frame_output(
            arguments, bg::details::resolver_variable<OUT>());
    },
    [](const WiringArguments &arguments,
       wp<TSF<SCHEMA>, "source"_n> source, wp<TSB<operators::FIELDS>, "assignments"_n> assignments) -> wp<OUT> {
        const auto fields = details::bundle_arguments(assignments);
        return wp<OUT>{details::build_with_columns(
            source, fields, details::requested_frame_schema(
                               arguments.type_map(), bg::details::resolver_variable<OUT>()))};
    }));

const auto registered_select = operators::select.overload(graph<OUT>(
    "name"_n = "frame_select",
    require = [](const WiringArguments &arguments) {
        return details::require_frame_output(arguments, bg::details::resolver_variable<OUT>());
    },
    [](const WiringArguments &arguments,
       wp<TSF<SCHEMA>, "source"_n> source, wp<TSB<operators::FIELDS>, "selections"_n> selections) -> wp<OUT> {
        const auto fields = details::bundle_arguments(selections);
        return wp<OUT>{details::select(
            source, fields, details::requested_frame_schema(
                               arguments.type_map(), bg::details::resolver_variable<OUT>()))};
    }));

const auto registered_rename = operators::rename.overload(graph<OUT>(
    "name"_n = "frame_rename",
    require = [](const WiringArguments &arguments) {
        return details::require_frame_output(arguments, bg::details::resolver_variable<OUT>());
    },
    [](const WiringArguments &arguments,
       wp<TSF<SCHEMA>, "source"_n> source, wp<TSB<operators::FIELDS>, "mappings"_n> mappings) -> wp<OUT> {
        const auto fields = details::bundle_arguments(mappings);
        auto names = std::vector<std::pair<util::symbol, util::symbol>>{};
        names.reserve(fields.size());
        for (const auto &field : fields) {
            if (!field.is_scalar() ||
                std::addressof(field.type()) != std::addressof(cpp_scalar_type<symbol>()))
                throw WiringError{"frame::rename mappings must map column names to symbols"};
            names.emplace_back(*field.name(), scalar_cast<symbol>(field.scalar()).value());
        }
        return wp<OUT>{details::rename(
            source, names, details::requested_frame_schema(
                               arguments.type_map(), bg::details::resolver_variable<OUT>()))};
    }));

const auto registered_drop = operators::drop.overload(graph<OUT>(
    "name"_n = "frame_drop",
    require = [](const WiringArguments &arguments) {
        return details::require_frame_output(arguments, bg::details::resolver_variable<OUT>());
    },
    [](const WiringArguments &arguments, wp<TSF<SCHEMA>, "value"_n> source, Scalar<List<symbol>> names) -> wp<OUT> {
        auto removed = std::vector<util::symbol>{};
        removed.reserve(names.size());
        for (auto index = std::size_t{}; index != names.size(); ++index)
            removed.push_back(names[index].value());
        return wp<OUT>{details::drop(
            source, removed, details::requested_frame_schema(
                                arguments.type_map(), bg::details::resolver_variable<OUT>()))};
    }));

const auto frame_column_accessors_registered = [] {
    operators::getattr.overload(
        details::frame_column_accessor<util::symbol>({"bundle"_n, "name"_n}));
    operators::getattr.overload(
        details::frame_column_accessor<std::string_view>({"bundle"_n, "name"_n}));
    operators::getitem.overload(
        details::frame_column_accessor<util::symbol>({"collection"_n, "key"_n}));
    operators::getitem.overload(
        details::frame_column_accessor<std::string_view>({"collection"_n, "key"_n}));
    return true;
}();

} // namespace
} // namespace bg::frame
