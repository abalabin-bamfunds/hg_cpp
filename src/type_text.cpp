#include <bg/type/type_text.hpp>

#include <bg/graph/graph_argument.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/struct/scalar_type.hpp>
#include <bg/type/signature.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>
#include <bg/type/time_series_variable.hpp>
#include <bg/type/type_of.hpp>
#include <bg/type/type_variable.hpp>

#include <array>
#include <cstddef>
#include <string_view>
#include <utility>

namespace bg {
namespace {

void append_guarantee(std::string &result, const GuaranteeParameter &guarantee) {
    if (const auto value = guarantee.value(); value && *value != TimeSeriesGuarantee::ordinary) {
        constexpr auto names = std::array<std::string_view, 4>{"", "deduplicated", "final", "const"};
        result += ", ";
        result += names[static_cast<std::size_t>(*value)];
    } else if (const auto *variable = guarantee.variable()) {
        result += ", ";
        result += variable->name().text();
    }
}

void append_scalar(std::string &result, const ScalarType &type) {
    constexpr auto primitive_names = std::array<std::pair<const ScalarType &(*)() noexcept, std::string_view>, 10>{
        std::pair{&int_type, "int"},
        std::pair{&byte_type, "byte"},
        std::pair{&bool_type, "bool"},
        std::pair{&int64_type, "i64"},
        std::pair{&float_type, "float"},
        std::pair{&double_type, "double"},
        std::pair{&evaluation_time_type, "date_time_t"},
        std::pair{&timedelta_type, "timedelta_t"},
        std::pair{&string_type, "string"},
        std::pair{&symbol_type, "symbol"},
    };
    for (const auto &[get, name] : primitive_names) {
        if (&type == &get()) {
            result += name;
            return;
        }
    }

    if (const auto *variable = dynamic_cast<const Variable *>(&type)) {
        result += variable->name().text();
    } else if (const auto *list = type_cast<ListScalarType>(&type)) {
        result += "List<";
        append_type_text(result, list->item_type());
        result += '>';
    } else if (const auto *set = type_cast<SetScalarType>(&type)) {
        result += "Set<";
        append_type_text(result, set->item_type());
        result += '>';
    } else if (const auto *dict = type_cast<DictScalarType>(&type)) {
        result += "Dict<";
        append_type_text(result, dict->key_type());
        result += ", ";
        append_type_text(result, dict->mapped_type());
        result += '>';
    } else if (const auto *structure = type_cast<StructScalarType>(&type)) {
        result += structure->name().text();
    } else if (const auto *hierarchical = type_cast<HierarchicalScalarType>(&type)) {
        result += hierarchical->name().text();
    } else if (const auto *type_value = dynamic_cast<const TypeScalarType *>(&type)) {
        result += "Type<";
        append_type_text(result, type_value->type());
        result += '>';
    } else {
        result += "scalar";
    }
}

} // namespace

void append_type_text(std::string &result, const Type &type) {
    if (const auto *graph = type_cast<GraphArgumentType>(&type)) {
        if (!graph->has_signature()) {
            result += "GRAPH_T";
            return;
        }

        result += "GRAPH<auto(";
        for (auto index = std::size_t{}; index < graph->parameters().size(); ++index) {
            if (index != 0) {
                result += ", ";
            }
            append_type_text(result, *graph->parameters()[index]);
        }
        result += ") -> ";
        if (graph->output() == nullptr) {
            result += "void";
        } else {
            append_type_text(result, *graph->output());
        }
        result += '>';
        return;
    }
    if (const auto *scalar = type_cast<ScalarType>(&type)) {
        append_scalar(result, *scalar);
        return;
    }
    if (const auto *variable = dynamic_cast<const TypeVariableType<Type> *>(&type)) {
        result += variable->name().text();
        return;
    }
    if (const auto *variable = dynamic_cast<const GenericTimeSeriesVariableType *>(&type)) {
        result += variable->variable().name().text();
        append_guarantee(result, variable->guarantee());
        return;
    }
    if (const auto *value = type_cast<TimeSeriesValueType>(&type)) {
        result += "TS<";
        append_type_text(result, value->value_type());
        append_guarantee(result, value->guarantee());
        result += '>';
        return;
    }
    if (const auto *reference = type_cast<TimeSeriesReferenceType>(&type)) {
        result += "REF<";
        append_type_text(result, reference->target_type());
        append_guarantee(result, reference->guarantee());
        result += '>';
        return;
    }
    if (const auto *list = type_cast<TimeSeriesListType>(&type)) {
        result += "TSL<";
        append_type_text(result, list->item_type());
        result += ", ";
        if (const auto size = list->size().value()) {
            result += std::to_string(*size);
        } else {
            result += list->size().variable()->name().text();
        }
        append_guarantee(result, list->guarantee());
        result += '>';
        return;
    }
    if (const auto *set = type_cast<TimeSeriesSetType>(&type)) {
        result += "TSS<";
        append_type_text(result, set->item_type());
        append_guarantee(result, set->guarantee());
        result += '>';
        return;
    }
    if (const auto *dict = type_cast<TimeSeriesDictionaryType>(&type)) {
        result += "TSD<";
        append_type_text(result, dict->key_type());
        result += ", ";
        append_type_text(result, dict->value_type());
        append_guarantee(result, dict->guarantee());
        result += '>';
        return;
    }
    if (const auto *bundle = type_cast<TimeSeriesBundleType>(&type)) {
        result += "TSB<";
        append_type_text(result, bundle->schema_type());
        append_guarantee(result, bundle->guarantee());
        result += '>';
        return;
    }
    if (const auto *schema = dynamic_cast<const Schema *>(&type)) {
        result += schema->name().text();
        return;
    }
    if (const auto *variable = dynamic_cast<const Variable *>(&type)) {
        result += variable->name().text();
        return;
    }
    result += "type";
}

} // namespace bg
