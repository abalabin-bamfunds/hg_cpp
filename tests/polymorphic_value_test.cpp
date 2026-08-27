#include <bg/util/polymorphic_value.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

namespace {

class value_view {
  public:
    void bind(const std::size_t *last_updated, const void *data) noexcept {
        last_updated_ = last_updated;
        data_ = data;
    }

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr; }

    [[nodiscard]] std::size_t last_updated() const noexcept { return *last_updated_; }

    [[nodiscard]] virtual double value() const noexcept = 0;

  protected:
    const std::size_t *last_updated_{};
    const void *data_{};
};

template <typename T> class typed_value_view final : public value_view {
  public:
    [[nodiscard]] double value() const noexcept override { return static_cast<double>(*static_cast<const T *>(data_)); }
};

static_assert(sizeof(value_view) == 3 * sizeof(void *));
static_assert(sizeof(typed_value_view<float>) == sizeof(value_view));

using generic_value_view = bg::util::polymorphic_value<value_view>;

static_assert(sizeof(generic_value_view) == sizeof(value_view));

} // namespace

TEST_CASE("a polymorphic value copies and binds a concrete view", "[polymorphic_value]") {
    const auto float_prototype = generic_value_view::make<typed_value_view<float>>();
    const auto int_prototype = generic_value_view::make<typed_value_view<int>>();

    const std::size_t last_updated = 42;
    const float float_value = 1.25F;
    const int int_value = 7;

    auto float_view = float_prototype;
    float_view->bind(&last_updated, &float_value);

    REQUIRE(float_view->valid());
    CHECK(float_view->last_updated() == last_updated);
    CHECK(float_view->value() == 1.25);

    auto int_view = int_prototype;
    int_view->bind(&last_updated, &int_value);

    CHECK(int_view->value() == 7.0);
}
