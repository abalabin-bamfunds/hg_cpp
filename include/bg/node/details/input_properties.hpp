#pragma once

#include <bg/graph/variadic_parameter.hpp>
#include <bg/node/input_properties.hpp>

#include <concepts>
#include <cstddef>
#include <string_view>
#include <type_traits>

namespace bg::details {

template <typename T> struct is_compile_time_name : std::false_type {};

template <util::compile_time_string Text> struct is_compile_time_name<compile_time_name<Text>> : std::true_type {
    static constexpr std::string_view text = Text.view();
};

template <typename T> inline constexpr bool is_activity = std::same_as<std::remove_cv_t<T>, InputActivity>;

template <typename T> inline constexpr bool is_validity = std::same_as<std::remove_cv_t<T>, InputValidity>;

template <typename T> inline constexpr bool is_name = is_compile_time_name<std::remove_cv_t<T>>::value;

template <typename T> inline constexpr bool is_dynamic_binding = std::same_as<std::remove_cv_t<T>, DynamicBinding>;

template <typename T> inline constexpr bool is_variadic_mode = std::same_as<std::remove_cv_t<T>, VariadicMode>;

template <auto Property>
consteval void apply_input_property(InputActivity &activity, InputValidity &validity, bool &dynamic_binding,
                                    std::string_view &name) {
    using PropertyType = decltype(Property);

    if constexpr (is_activity<PropertyType>) {
        activity = Property;
    } else if constexpr (is_validity<PropertyType>) {
        validity = Property;
    } else if constexpr (is_dynamic_binding<PropertyType>) {
        dynamic_binding = true;
    } else if constexpr (is_name<PropertyType>) {
        name = is_compile_time_name<std::remove_cv_t<PropertyType>>::text;
    }
}

template <bool AllowVariadic, auto... Properties> struct basic_input_properties {
    static_assert((... && (is_activity<decltype(Properties)> || is_validity<decltype(Properties)> ||
                           is_dynamic_binding<decltype(Properties)> || is_name<decltype(Properties)> ||
                           (AllowVariadic && is_variadic_mode<decltype(Properties)>))),
                  "unsupported input property");
    static_assert((std::size_t{} + ... + static_cast<std::size_t>(is_activity<decltype(Properties)>)) <= 1,
                  "an input can have only one activity property");
    static_assert((std::size_t{} + ... + static_cast<std::size_t>(is_validity<decltype(Properties)>)) <= 1,
                  "an input can have only one validity property");
    static_assert((std::size_t{} + ... + static_cast<std::size_t>(is_name<decltype(Properties)>)) <= 1,
                  "an input can have only one name property");
    static_assert((std::size_t{} + ... + static_cast<std::size_t>(is_dynamic_binding<decltype(Properties)>)) <= 1,
                  "an input can have only one dynamic_binding property");
    static_assert((std::size_t{} + ... + static_cast<std::size_t>(is_variadic_mode<decltype(Properties)>)) <= 1,
                  "an input can have only one variadic property");

    static constexpr bool valid = true;

  private:
    struct values {
        InputActivity activity{active};
        InputValidity validity{required};
        bool dynamic_binding{};
        std::string_view name;
    };

    static consteval values parse() {
        values result;
        (apply_input_property<Properties>(result.activity, result.validity, result.dynamic_binding, result.name),
         ...);

        return result;
    }

    static constexpr values parsed = parse();

  public:
    static constexpr InputActivity activity = parsed.activity;
    static constexpr InputValidity validity = parsed.validity;
    static constexpr bool dynamic_binding = parsed.dynamic_binding;
    static constexpr std::string_view name = parsed.name;
};

template <auto... Properties> using input_properties = basic_input_properties<false, Properties...>;

} // namespace bg::details
