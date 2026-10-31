#pragma once

#include <bg/optionality.hpp>
#include <bg/util/named_argument.hpp>

#include <cstdint>
#include <type_traits>

namespace bg {

enum class InputActivity : std::uint8_t { active, passive };

inline constexpr InputActivity active = InputActivity::active;
inline constexpr InputActivity passive = InputActivity::passive;

struct InputValidity {
    enum class Value : std::uint8_t { required, optional, all_valid };

    Value value{Value::required};

    constexpr InputValidity() noexcept = default;
    constexpr InputValidity(Value value) noexcept : value{value} {}
    constexpr InputValidity(Optionality validity) noexcept
        : value{validity == Optionality::required ? Value::required : Value::optional} {}

    constexpr bool operator==(const InputValidity &) const noexcept = default;

    static const InputValidity required;
    static const InputValidity optional;
    static const InputValidity all_valid;
};

inline constexpr InputValidity InputValidity::required{InputValidity::Value::required};
inline constexpr InputValidity InputValidity::optional{InputValidity::Value::optional};
inline constexpr InputValidity InputValidity::all_valid{InputValidity::Value::all_valid};

static_assert(sizeof(InputValidity) == sizeof(std::uint8_t));
static_assert(std::is_trivially_copyable_v<InputValidity>);

inline constexpr InputValidity all_valid = InputValidity::all_valid;

struct ValidItems {
    template <typename Point> auto operator()(Point &&point) const;
};
inline constexpr ValidItems valid_items{};

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
