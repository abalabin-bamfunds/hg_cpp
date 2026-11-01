#include <hg/scalar/cpp_runtime_type.hpp>
#include <hg/literal/sentinel.hpp>
#include <hg/util/symbol.hpp>

namespace hg {

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
template HG_EXPORT const ScalarRuntimeType &cpp_scalar_runtime_type<literal::Sentinel>();
template HG_EXPORT const ScalarRuntimeType &cpp_scalar_runtime_type<literal::TaggedLiteral>();

} // namespace hg
