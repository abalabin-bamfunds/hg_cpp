#pragma once

#include <bg/frame/column.hpp>
#include <bg/frame/expression.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <memory>

namespace bg::frame::details {

inline bool is_column_expression(const Type &type, const ScalarType &result_type) {
    const auto *time_series = type_cast<TimeSeriesType>(&type);
    if (time_series == nullptr)
        return false;

    const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(
        std::addressof(time_series->dereferenced_type()));
    if (bundle == nullptr || bundle->item_count() < 2 || bundle->item_count() > 3)
        return false;

    const auto &compute_type = bundle->item_type(bundle->item_count() - 1).dereferenced_type();
    const auto *compute_value = dynamic_cast<const TimeSeriesValueType *>(std::addressof(compute_type));
    if (compute_value == nullptr)
        return false;

    const auto *function = dynamic_cast<const ColumnBlockFunctionType *>(
        std::addressof(compute_value->value_type()));
    return function != nullptr && function->operand_types().size() == bundle->item_count() - 1 &&
           std::addressof(function->result_type()) == std::addressof(result_type);
}

inline bool is_column_operand(const Type &type, const ScalarType &cell_type) {
    const auto *time_series = type_cast<TimeSeriesType>(&type);
    if (time_series == nullptr)
        return false;

    const auto &value_type = time_series->dereferenced_type();
    const auto *column = dynamic_cast<const ColumnSeriesType *>(std::addressof(value_type));
    return (column != nullptr && std::addressof(column->cell_type()) == std::addressof(cell_type)) ||
           is_column_expression(type, cell_type);
}

inline bool is_operand(const Type &type, const ScalarType &value_type) {
    if (is_column_operand(type, value_type))
        return true;

    const auto *time_series = type_cast<TimeSeriesType>(&type);
    if (time_series == nullptr)
        return false;

    const auto *scalar = dynamic_cast<const TimeSeriesValueType *>(
        std::addressof(time_series->dereferenced_type()));
    return scalar != nullptr && std::addressof(scalar->value_type()) == std::addressof(value_type);
}

} // namespace bg::frame::details
