#pragma once

#include <bg/optionality.hpp>
#include <bg/util/named_argument.hpp>

#include <cstdint>

namespace bg {

enum class InputActivity : std::uint8_t { active, passive };

inline constexpr InputActivity active = InputActivity::active;
inline constexpr InputActivity passive = InputActivity::passive;

using InputValidity = Optionality;

struct DynamicBinding {};
inline constexpr DynamicBinding dynamic_binding{};

struct DynamicActivity {};
inline constexpr DynamicActivity dynamic_activity{};

template <bool Enabled> class input_activity_control {
  public:
    constexpr explicit input_activity_control(bool * = nullptr) noexcept {}

  protected:
    constexpr bool *activity() const noexcept { return nullptr; }
};

template <> class input_activity_control<true> {
  public:
    explicit input_activity_control(bool *active = nullptr) noexcept : active_{active} {}

    void make_passive() const noexcept { *active_ = false; }
    void make_active() const noexcept { *active_ = true; }

  protected:
    bool *activity() const noexcept { return active_; }

  private:
    bool *active_;
};

template <bool Enabled> using InputActivityControl = input_activity_control<Enabled>;

namespace details {
template <bool Enabled> using input_activity_control = ::bg::input_activity_control<Enabled>;
}

struct ContextParameter {};
inline constexpr ContextParameter context{};

} // namespace bg
