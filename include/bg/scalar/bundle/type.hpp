#pragma once

#include <cstdint>
#include <type_traits>

namespace bg {

template <typename T> struct scalar_storage_traits;

namespace details {

struct scalar_bundle_storage {
    std::uintptr_t handle{};
};

} // namespace details

template <template <template <typename, auto...> typename> typename Schema>
class Bundle {
  public:
    Bundle() = delete;

    template <template <typename, auto...> typename Field>
    using schema_type = Schema<Field>;

  private:
    template <typename T> friend struct scalar_storage_traits;

    explicit Bundle(const details::scalar_bundle_storage &storage) noexcept : storage_{&storage} {}

    const details::scalar_bundle_storage *storage_;
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

static_assert(sizeof(bg::details::scalar_bundle_storage) == sizeof(void *));
static_assert(alignof(bg::details::scalar_bundle_storage) == alignof(void *));
