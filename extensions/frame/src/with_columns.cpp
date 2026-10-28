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

struct WithColumnsNode {
    In<TSF<SOURCE_SCHEMA>> source;
    In<TSB<ASSIGNMENT_SCHEMA>, all_valid> assignments;
    Out<TSF<WITH_COLUMNS_RESULT_SCHEMA>, view> output;

    static const Schema &resolve(schema_type<WITH_COLUMNS_RESULT_SCHEMA>, const WiringArguments &arguments) {
        const auto &source_type = static_cast<const FrameSeriesType &>(arguments.resolved_type("source"));
        const auto &source_schema = static_cast<const Schema &>(source_type.schema_type());
        const auto &assignment_schema =
            static_cast<const TimeSeriesBundleType &>(arguments.resolved_type("assignments"));

        auto fields = std::vector<SchemaField>{source_schema.fields().begin(), source_schema.fields().end()};
        fields.reserve(fields.size() + assignment_schema.item_count());
        const auto *key = source_schema.metadata().find("bg.frame.key");
        const auto key_names = key == nullptr
                                   ? std::optional<Scalar<List<string>>>{}
                                   : std::optional{Scalar<List<string>>::from(key->runtime_type(), key->data())};
        for (auto index = std::size_t{}; index != assignment_schema.item_count(); ++index) {
            const auto name = assignment_schema.item_name(index);
            const auto &column_type = static_cast<const ColumnSeriesType &>(
                assignment_schema.item_type(index).dereferenced_type());

            for (auto key_index = std::size_t{}; key_names && key_index != key_names->size(); ++key_index)
                if ((*key_names)[key_index].value() == name.text())
                    throw WiringError{"frame::with_columns cannot assign a key column"};

            const auto field = std::ranges::find(fields, name, &SchemaField::name);
            const auto replacement = SchemaField{name, std::addressof(column_type.cell_type()), false};
            if (field == fields.end())
                fields.push_back(replacement);
            else
                *field = replacement;
        }

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

const auto with_columns_node = node<WithColumnsNode>();

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

WiringPoint with_columns(WiringPoint source, std::span<const WiringArgument> assignments) {
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
    return with_columns_node(source, wp<TSB<ASSIGNMENT_SCHEMA>>{assigned_bundle});
}

} // namespace bg::frame::details
