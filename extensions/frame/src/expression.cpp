#include <bg/frame/expression.hpp>

#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_registry.hpp>

#include <memory>
#include <stdexcept>

const bg::frame::ColumnBlockFunctionType &bg::frame::column_block_function_type(
    const bg::ScalarType &result_type) {
    return bg::TypeRegistry::instance().get_or_create<ColumnBlockFunctionType, std::hash<const ScalarType *>>(
        &result_type, [&] { return std::make_unique<ColumnBlockFunctionType>(result_type); });
}

void bg::frame::ColumnBlockFunctionType::format_name(std::string &result) const {
    result += "ColumnBlockFunction<";
    result_type().format_name(result);
    result += '>';
}

const bg::Type &bg::frame::ColumnBlockFunctionType::resolve(const bg::TypeMap &type_map) const {
    const auto &resolved_result_type = result_type().resolve(type_map);
    const auto *scalar_type = bg::type_cast<bg::ScalarType>(&resolved_result_type);
    if (scalar_type == nullptr)
        throw bg::TypeResolutionError{bg::TypeResolutionErrorCode::invalid_type_map,
                                      bg::util::symbol{"ColumnBlockFunction"},
                                      "column block function result did not resolve to a scalar type"};
    return column_block_function_type(*scalar_type);
}

bool bg::frame::ColumnBlockFunctionType::do_match(const bg::Type &supplied, bg::TypeMap &type_map) const {
    const auto *function = dynamic_cast<const ColumnBlockFunctionType *>(std::addressof(supplied));
    return function != nullptr && function->is_resolved() && result_type().match(function->result_type(), type_map);
}

namespace bg {

template <frame::cell_type T>
const ScalarType &scalar_type_traits<frame::ColumnBlockFunction<T>>::type() {
    return frame::column_block_function_type(cpp_scalar_type<T>());
}

template const ScalarType &scalar_type_traits<frame::ColumnBlockFunction<i64>>::type();
template const ScalarType &scalar_type_traits<frame::ColumnBlockFunction<double>>::type();
template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::ColumnBlockFunction<i64>>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::ColumnBlockFunction<double>>();

} // namespace bg
