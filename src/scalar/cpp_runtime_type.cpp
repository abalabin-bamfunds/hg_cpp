#include <bg/scalar/cpp_runtime_type.hpp>
#include <bg/literal/sentinel.hpp>
#include <bg/util/symbol.hpp>

namespace bg {

template const ScalarRuntimeType &cpp_scalar_runtime_type<bool>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<byte>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<int>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<i64>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<float>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<double>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<date_time_t>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<timedelta_t>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<string>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<bytes>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<symbol>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<const Type *>();
template BG_EXPORT const ScalarRuntimeType &cpp_scalar_runtime_type<literal::Sentinel>();
template BG_EXPORT const ScalarRuntimeType &cpp_scalar_runtime_type<literal::TaggedLiteral>();

} // namespace bg
