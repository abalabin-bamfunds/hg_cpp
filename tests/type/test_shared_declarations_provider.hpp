#pragma once

#include <hg/library.hpp>
#include <hg/scalar/opaque.hpp>
#include <hg/type/details/scalar_type.hpp>
#include <hg/type/size_variable_instance.hpp>

#include <array>

#if defined(_WIN32)
#define HG_TEST_SHARED_DECLARATIONS_EXPORT
#else
#define HG_TEST_SHARED_DECLARATIONS_EXPORT __attribute__((visibility("default")))
#endif

namespace test_shared_declarations {

using SCALAR = hg::ScalarVariable<"shared_scalar">;
using SERIES = hg::TimeSeriesVariable<"shared_series">;
using EITHER = hg::TypeVariable<"shared_union", hg::TS<SCALAR>, hg::TS<hg::List<SCALAR>>>;

template <template <typename, auto...> typename Field>
using SCHEMA = hg::BundleSchemaVariableDeclaration<"shared_schema", Field>;

inline constexpr auto G = hg::guarantee_variable<"shared_guarantee">;
inline constexpr auto N = hg::SizeVariableDeclaration<"shared_size">{};
inline constexpr auto REVERSE_G = hg::guarantee_variable<"reverse_guarantee">;
inline constexpr auto REVERSE_N = hg::SizeVariableDeclaration<"reverse_size">{};

struct OpaquePayload {
    int value{};
};

inline auto declaration_types() {
    return std::array<const hg::Type *, 17>{
        &SCALAR::type(),
        &SERIES::type(),
        &SERIES::with<G>::type(),
        &EITHER::type(),
        &hg::schema_type<SCHEMA>::type(),
        &hg::cpp_scalar_type<hg::Opaque<OpaquePayload>>(),
        &hg::scalar_type_traits<hg::List<SCALAR>>::type(),
        &hg::scalar_type_traits<hg::Set<SCALAR>>::type(),
        &hg::scalar_type_traits<hg::Dict<SCALAR, SCALAR>>::type(),
        &hg::scalar_type_traits<hg::Tuple<SCALAR, hg::i64>>::type(),
        &hg::scalar_type_traits<hg::Bundle<SCHEMA>>::type(),
        &hg::TS<SCALAR, G>::type(),
        &hg::TSL<hg::TS<SCALAR, G>, N, G>::type(),
        &hg::TSS<SCALAR, G>::type(),
        &hg::TSD<SCALAR, hg::TS<SCALAR, G>, G>::type(),
        &hg::REF<SERIES, G>::type(),
        &hg::TSB<SCHEMA, G>::type(),
    };
}

inline auto declaration_variables() {
    return std::array<const hg::Variable *, 4>{
        &hg::details::type_parameter<SCALAR>(),
        &hg::details::type_parameter<SERIES>(),
        &hg::details::guarantee_variable_instance<G>(),
        &hg::details::size_variable_instance<N>(),
    };
}

inline auto reverse_variables() {
    return std::array<const hg::Variable *, 2>{
        &hg::details::guarantee_variable_instance<REVERSE_G>(),
        &hg::details::size_variable_instance<REVERSE_N>(),
    };
}

template <int Index> struct LazyDeclaration {};

class LazyType final : public hg::ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const hg::Type &resolve(const hg::TypeMap &) const override { return *this; }
};

template <int Index> struct RuntimePayload {
    hg::i64 value;
};

template <int Index> struct RuntimeRecord {
    hg::i64 quantity;
    double price;
};

template <int Index> auto scalar_runtime_types() {
    return std::array<const hg::ScalarRuntimeType *, 4>{
        &hg::cpp_scalar_runtime_type<hg::Opaque<RuntimePayload<Index> *>>(),
        &hg::cpp_scalar_runtime_type<RuntimeRecord<Index>>(),
        &hg::cpp_scalar_runtime_type<hg::List<RuntimeRecord<Index>>>(),
        &hg::cpp_scalar_runtime_type<hg::Dict<hg::i64, hg::List<hg::Opaque<RuntimePayload<Index> *>>>>(),
    };
}

template <int Index> auto series_runtime_types() {
    using Opaque = hg::Opaque<RuntimePayload<Index> *>;
    using Record = RuntimeRecord<Index>;
    return std::array<const hg::TimeSeriesRuntimeType *, 4>{
        &hg::cpp_time_series_runtime_type<hg::TS<Opaque>>(hg::TS<Opaque>::type()),
        &hg::cpp_time_series_runtime_type<hg::TS<Record>>(hg::TS<Record>::type()),
        &hg::cpp_time_series_runtime_type<hg::TS<hg::List<Record>>>(hg::TS<hg::List<Record>>::type()),
        &hg::cpp_time_series_runtime_type<hg::TSD<hg::i64, hg::TS<hg::List<Opaque>>>>(
            hg::TSD<hg::i64, hg::TS<hg::List<Opaque>>>::type()),
    };
}

HG_TEST_SHARED_DECLARATIONS_EXPORT std::array<const hg::Type *, 17> provider_types();
HG_TEST_SHARED_DECLARATIONS_EXPORT std::array<const hg::Variable *, 4> provider_variables();
HG_TEST_SHARED_DECLARATIONS_EXPORT std::array<const hg::Variable *, 2> provider_reverse_variables();
HG_TEST_SHARED_DECLARATIONS_EXPORT const LazyType &provider_lazy_type(int index);
HG_TEST_SHARED_DECLARATIONS_EXPORT int provider_factory_calls(int index);
HG_TEST_SHARED_DECLARATIONS_EXPORT std::array<const hg::ScalarRuntimeType *, 4> provider_scalar_runtime_types(int index);
HG_TEST_SHARED_DECLARATIONS_EXPORT std::array<const hg::TimeSeriesRuntimeType *, 4> provider_series_runtime_types(int index);

} // namespace test_shared_declarations

#undef HG_TEST_SHARED_DECLARATIONS_EXPORT
