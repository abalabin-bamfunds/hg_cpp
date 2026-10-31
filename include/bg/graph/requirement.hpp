#pragma once

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bg {

class RequirementResult {
  public:
    RequirementResult(bool accepted, std::string message = {}) : accepted_{accepted}, message_{std::move(message)} {}

    bool accepted() const noexcept { return accepted_; }
    std::string_view message() const noexcept { return message_; }

  private:
    bool accepted_;
    std::string message_;
};

inline const RequirementResult accept{true};

[[nodiscard]] inline RequirementResult reject(std::string message = {}) {
    return {false, std::move(message)};
}

[[nodiscard]] inline RequirementResult requirement_result(RequirementResult result) {
    return result;
}

[[nodiscard]] inline RequirementResult requirement_result(bool accepted) {
    return {accepted};
}

namespace details {

template <typename Function> struct CallableRequirement {
    Function function;
};

template <typename T> struct is_callable_requirement : std::false_type {};

template <typename Function> struct is_callable_requirement<CallableRequirement<Function>> : std::true_type {};

template <typename T>
inline constexpr bool is_callable_requirement_v = is_callable_requirement<std::remove_cvref_t<T>>::value;

struct CallableRequirementName {
    template <typename Function> constexpr auto operator=(Function &&function) const {
        return CallableRequirement<std::decay_t<Function>>{std::forward<Function>(function)};
    }
};

} // namespace details

inline constexpr auto require = details::CallableRequirementName{};

} // namespace bg
