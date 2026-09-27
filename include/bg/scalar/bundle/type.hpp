#pragma once

#include <type_traits>

namespace bg {

template <template <template <typename, auto...> typename> typename Schema>
class Bundle {
  public:
    Bundle() = delete;

    template <template <typename, auto...> typename Field>
    using schema_type = Schema<Field>;
};

namespace details {

template <typename T> struct is_hierarchical_scalar_declaration : std::false_type {};

template <template <template <typename, auto...> typename> typename Schema>
struct is_hierarchical_scalar_declaration<Bundle<Schema>> : std::true_type {};

template <typename T>
concept hierarchical_scalar_declaration =
    is_hierarchical_scalar_declaration<std::remove_cvref_t<T>>::value;

} // namespace details

} // namespace bg
