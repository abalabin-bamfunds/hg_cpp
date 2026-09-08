#pragma once

#include <string>
#include <string_view>
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

} // namespace bg
