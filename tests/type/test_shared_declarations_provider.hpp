#pragma once

#include <hg/library.hpp>
#include <hg/scalar/opaque.hpp>
#include <hg/type/details/scalar_type.hpp>
#include <hg/type/size_variable_instance.hpp>

#include <array>

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

std::array<const hg::Type *, 17> provider_types();
std::array<const hg::Variable *, 4> provider_variables();
std::array<const hg::Variable *, 2> provider_reverse_variables();
const LazyType &provider_lazy_type(int index);
int provider_factory_calls(int index);

} // namespace test_shared_declarations
