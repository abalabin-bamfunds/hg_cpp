#include <bg/frame/frame.hpp>
#include <bg/graph.hpp>
#include <bg/scalar/list/scalar.hpp>
#include <bg/time_series/tsb/tsb.hpp>

#include <algorithm>
#include <span>
#include <stdexcept>
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

const auto with_columns_node = node<WithColumnsNode>();

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
                return point;
            }
            if (assignment.is_scalar() &&
                std::addressof(assignment.type()) == std::addressof(scalar_type_traits<Expression>::type())) {
                const auto expression_value = assignment.scalar();
                return static_cast<WiringPoint>(
                    frame::bind_expression(typed_source, Scalar<Expression>::from(expression_value).value()));
            }
            throw WiringError{"frame::with_columns assignments must be TSC columns or column expressions"};
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
