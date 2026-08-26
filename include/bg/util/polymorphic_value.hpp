#pragma once

#include <concepts>
#include <cstddef>
#include <cstring>
#include <memory>
#include <type_traits>

namespace bg::util {

// This utility relies on the compiler ABI. Copying the bytes does not start a C++ object lifetime.
template <typename Interface>
class polymorphic_value {
    static_assert(std::is_polymorphic_v<Interface>);
    static_assert(std::is_trivially_destructible_v<Interface>);

#if defined(__arm64e__)
    static_assert(!std::same_as<Interface, Interface>,
                  "polymorphic_value does not support address-signed vtable pointers");
#endif

  public:
    template <typename Implementation>
        requires std::derived_from<Implementation, Interface> &&
                 std::default_initializable<Implementation>
    [[nodiscard]] static polymorphic_value make() noexcept {
        static_assert(sizeof(Implementation) == sizeof(Interface));
        static_assert(alignof(Implementation) == alignof(Interface));
        static_assert(std::is_trivially_destructible_v<Implementation>);

        Implementation prototype{};
        polymorphic_value value;
        std::memcpy(value.bytes_, std::addressof(prototype), sizeof(value.bytes_));

        return value;
    }

    polymorphic_value(const polymorphic_value &) noexcept = default;
    polymorphic_value &operator=(const polymorphic_value &) noexcept = default;

    [[nodiscard]] Interface &get() noexcept {
        return *std::launder(reinterpret_cast<Interface *>(bytes_));
    }

    [[nodiscard]] const Interface &get() const noexcept {
        return *std::launder(reinterpret_cast<const Interface *>(bytes_));
    }

    [[nodiscard]] Interface *operator->() noexcept { return &get(); }

    [[nodiscard]] const Interface *operator->() const noexcept { return &get(); }

  private:
    polymorphic_value() noexcept = default;

    alignas(Interface) std::byte bytes_[sizeof(Interface)];
};

} // namespace bg::util
