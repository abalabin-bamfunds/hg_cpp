#include <bg/frame/frame.hpp>
#include <bg/graph.hpp>
#include <bg/library/operators.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/time_series/tsb/tsb.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace bg::frame::details {
namespace {

template <template <typename, auto...> typename Field>
using SOURCE_SCHEMA = SchemaVariableDeclaration<"FRAME_WITH_COLUMNS_SOURCE_SCHEMA">;
template <template <typename, auto...> typename Field>
using ASSIGNMENT_SCHEMA = BundleSchemaVariableDeclaration<"FRAME_WITH_COLUMNS_ASSIGNMENT_SCHEMA", Field>;
template <template <typename, auto...> typename Field>
using PROJECTION_COLUMNS_SCHEMA = BundleSchemaVariableDeclaration<"FRAME_PROJECTION_COLUMNS_SCHEMA", Field>;

std::optional<std::vector<std::string>> key_names(const Schema &schema) {
    const auto *key = schema.metadata().find("bg.frame.key");
    if (key == nullptr)
        return std::nullopt;

    const auto names = Scalar<List<string>>::from(key->runtime_type(), key->data());
    auto result = std::vector<std::string>{};
    result.reserve(names.size());
    for (const auto &name : names)
        result.emplace_back(name.value());
    return result;
}

std::vector<SchemaField> with_columns_fields(const Schema &source_schema,
                                             const TimeSeriesBundleType &assignment_schema) {
    auto fields = std::vector<SchemaField>{source_schema.fields().begin(), source_schema.fields().end()};
    fields.reserve(fields.size() + assignment_schema.item_count());
    for (auto index = std::size_t{}; index != assignment_schema.item_count(); ++index) {
        const auto name = assignment_schema.item_name(index);
        const auto &column_type = static_cast<const ColumnSeriesType &>(
            assignment_schema.item_type(index).dereferenced_type());

        const auto keys = key_names(source_schema);
        for (const auto &key : keys.value_or(std::vector<std::string>{}))
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
    Out<TSF<WITH_COLUMNS_RESULT_SCHEMA>, view> output;

    static const Schema &resolve(schema_type<WITH_COLUMNS_RESULT_SCHEMA>, const WiringArguments &arguments) {
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

template <template <typename, auto...> typename Field>
using PROJECTION_RESULT_SCHEMA = SchemaVariableDeclaration<"FRAME_PROJECTION_RESULT_SCHEMA">;

struct ProjectionNode {
    In<TSF<SOURCE_SCHEMA>> source;
    Scalar<Opaque<std::vector<std::size_t>>> source_indices;
    Out<TSF<PROJECTION_RESULT_SCHEMA>, view> output;

    void evaluate() const { output.bind_projection(source, source_indices.value()); }
};

const auto with_columns_node = node<WithColumnsNode>();
const auto projection_node = node<ProjectionNode>();

const Schema &source_frame_schema(WiringPoint source) {
    const auto *frame = type_cast<FrameSeriesType>(std::addressof(source.type()));
    if (frame == nullptr)
        throw WiringError{"frame projection requires a TSF input"};
    const auto *schema = type_cast<Schema>(std::addressof(frame->schema_type()));
    if (schema == nullptr)
        throw WiringError{"frame projection requires a resolved source schema"};
    return *schema;
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
            if (match)
                throw WiringError{"frame projection selects a key column under multiple output names"};
            match = output_name;
        }
        if (!match) {
            if (source_schema.metadata().size() != 1)
                throw WiringError{"frame projection cannot drop a key while preserving other schema metadata"};
            return {};
        }
        output_keys.push_back(match->text());
    }

    const auto value = ScalarValue<List<string>>{std::span<const std::string_view>{output_keys}};
    const auto key_override = make_schema_metadata({schema_metadata_entry("bg.frame.key", value)});
    return source_schema.metadata().with_overrides(key_override);
}

bool same_fields(std::span<const SchemaField> expected, const Schema &actual) {
    if (expected.size() != actual.field_count())
        return false;
    for (auto index = std::size_t{}; index != expected.size(); ++index)
        if (expected[index] != actual.field(index))
            return false;
    return true;
}

WiringPoint wire_projection(WiringPoint source, std::span<const SchemaField> fields,
                            std::span<const std::size_t> source_indices, SchemaMetadata metadata,
                            const Schema *requested_schema = nullptr) {
    const auto &source_schema = source_frame_schema(source);
    const auto &result_schema = requested_schema == nullptr ? anonymous_schema(fields, metadata) : *requested_schema;
    if (!same_fields(fields, result_schema) || !(result_schema.metadata() == metadata))
        throw WiringError{"frame projection result schema does not match the requested columns"};

    auto type_map = TypeMap{};
    type_map.set(bg::details::schema_variable_instance<PROJECTION_RESULT_SCHEMA>(), &result_schema);
    const auto indices = ScalarValue<Opaque<std::vector<std::size_t>>>{
        std::vector<std::size_t>{source_indices.begin(), source_indices.end()}};
    return *projection_node(type_map, wp<TSF<SOURCE_SCHEMA>>{source}, indices);
}

void validate_assignment_result_schema(const Schema &source_schema, const TimeSeriesBundleType &assignment_schema,
                                       const Schema &result_schema) {
    const auto fields = with_columns_fields(source_schema, assignment_schema);
    const auto expected = anonymous_schema(fields, source_schema.metadata());
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

WiringPoint broadcast_scalar(wp<TSF<SOURCE_SCHEMA>> source, const WiringArgument &argument) {
    const auto scalar = argument.scalar();
    validate_cell_type(argument.type());
    return frame::visit_cell_type(argument.type(), [&]<typename Cell>(std::type_identity<Cell>) {
        const auto value = Scalar<Cell>::from(scalar);
        const auto constant = operators::const_(value).template as<TS<Cell>>();
        return broadcast_column<Cell>(source, constant);
    });
}

} // namespace

WiringPoint build_with_columns(WiringPoint source, std::span<const WiringArgument> assignments,
                               const Schema *result_schema) {
    auto &wiring = WiringContext::current().wiring();
    const auto typed_source = wp<TSF<SOURCE_SCHEMA>>{source};
    auto assigned_fields = std::vector<WiringArgument>{};
    assigned_fields.reserve(assignments.size());

    for (const auto &assignment : assignments) {
        if (!assignment.name())
            throw WiringError{"frame::with_columns assignments must have names"};
        const auto name = *assignment.name();

        auto column = [&] {
            if (assignment.is_time_series()) {
                const auto point = assignment.time_series();
                if (is_mixed_expression_bundle_type(point.type()))
                    return static_cast<WiringPoint>(frame::bind_expression(typed_source, wp<>{point}));
                if (type_cast<TimeSeriesValueType>(std::addressof(point.type().dereferenced_type())) != nullptr)
                    return broadcast_series(typed_source, point);
                return point;
            }
            if (assignment.is_scalar() &&
                std::addressof(assignment.type()) == std::addressof(scalar_type_traits<Expression>::type())) {
                const auto expression_value = assignment.scalar();
                return static_cast<WiringPoint>(
                    frame::bind_expression(typed_source, Scalar<Expression>::from(expression_value).value()));
            }
            if (assignment.is_scalar())
                return broadcast_scalar(typed_source, assignment);
            throw WiringError{"frame::with_columns assignments must be scalar values, TS values, TSC columns, or column expressions"};
        }();

        const auto &item_type = column.type().dereferenced_type();
        const auto *column_type = dynamic_cast<const ColumnSeriesType *>(std::addressof(item_type));
        if (column_type == nullptr)
            throw WiringError{"frame::with_columns assignments must be TSC columns"};

        const auto &materialized_type = column_series_type(column_type->cell_type(), TimeSeriesGuarantee::ordinary);
        const auto materialized = wiring.materialisation_form(column, materialized_type);
        assigned_fields.emplace_back(name, materialized);
    }

    const auto assigned_bundle = bundle(std::span<const WiringArgument>{assigned_fields});
    const auto assigned_type = static_cast<const TimeSeriesBundleType &>(assigned_bundle.type().dereferenced_type());
    if (result_schema == nullptr)
        return with_columns_node(source, wp<TSB<ASSIGNMENT_SCHEMA>>{assigned_bundle});

    validate_assignment_result_schema(source_frame_schema(source), assigned_type, *result_schema);
    auto type_map = TypeMap{};
    type_map.set(bg::details::schema_variable_instance<WITH_COLUMNS_RESULT_SCHEMA>(), result_schema);
    return *with_columns_node(type_map, source, wp<TSB<ASSIGNMENT_SCHEMA>>{assigned_bundle});
}

WiringPoint with_columns(WiringPoint source, std::span<const WiringArgument> assignments) {
    return build_with_columns(source, assignments, nullptr);
}

WiringPoint with_columns(WiringPoint source, std::span<const WiringArgument> assignments,
                         const Schema &result_schema) {
    return build_with_columns(source, assignments, &result_schema);
}

WiringPoint project(WiringPoint source, std::span<const util::symbol> names, const Schema *result_schema) {
    const auto &source_schema = source_frame_schema(source);
    auto fields = std::vector<SchemaField>{};
    auto source_indices = std::vector<std::size_t>{};
    auto output_fields = std::vector<std::pair<std::size_t, util::symbol>>{};
    fields.reserve(names.size());
    source_indices.reserve(names.size());
    output_fields.reserve(names.size());
    for (const auto name : names) {
        const auto *field = source_schema.find(name);
        if (field == nullptr)
            throw WiringError{"frame::select names an unknown column"};
        if (std::ranges::find(fields, name, &SchemaField::name) != fields.end())
            throw WiringError{"frame::select contains a duplicate output column name"};
        const auto index = static_cast<std::size_t>(field - source_schema.fields().data());
        fields.push_back(*field);
        source_indices.push_back(index);
        output_fields.emplace_back(index, name);
    }
    auto metadata = projected_metadata(source_schema, output_fields);
    return wire_projection(source, fields, source_indices, std::move(metadata), result_schema);
}

WiringPoint rename(WiringPoint source, std::span<const std::pair<util::symbol, util::symbol>> mappings) {
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
    auto metadata = projected_metadata(source_schema, output_fields);
    return wire_projection(source, fields, source_indices, std::move(metadata));
}

WiringPoint drop(WiringPoint source, std::span<const util::symbol> names) {
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
    auto metadata = projected_metadata(source_schema, output_fields);
    return wire_projection(source, fields, source_indices, std::move(metadata));
}

} // namespace bg::frame::details
