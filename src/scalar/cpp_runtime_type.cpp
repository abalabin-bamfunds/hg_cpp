#include <bg/scalar/details/cpp_runtime_type.hpp>
#include <bg/util/symbol.hpp>

namespace bg {

template const ScalarRuntimeType &cpp_scalar_runtime_type<bool>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<int>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<std::int64_t>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<float>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<double>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<EvaluationTime>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<string>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<util::symbol>();

} // namespace bg
