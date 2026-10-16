#pragma once

#include <bg/node/input.hpp>
#include <bg/python/callback_scope.hpp>
#include <bg/python/scalar_conversion.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_input.hpp>
#include <bg/time_series/time_series_type.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>

namespace bg::python {

class NB_HIDDEN InputView {
  public:
    InputView(In<TS_T> input, const PythonScalarConversions &conversions, std::shared_ptr<CallbackLifetime> lifetime,
              std::uint64_t generation) noexcept
        : input_{input}, conversions_{&conversions}, lifetime_{std::move(lifetime)}, generation_{generation} {}

    bool valid() const;
    bool modified() const;
    nanobind::object last_modified_time() const;

  protected:
    void check_lifetime() const;
    const In<TS_T> &input() const noexcept { return input_; }
    const PythonScalarConversions &conversions() const noexcept { return *conversions_; }
    const std::shared_ptr<CallbackLifetime> &lifetime() const noexcept { return lifetime_; }
    std::uint64_t generation() const noexcept { return generation_; }

    template <typename Input> Input as() const {
        const auto result = input_.visit([](const Input *value) { return *value; }, ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input has an unexpected time-series shape"};
        }
        return *result;
    }

  private:
    In<TS_T> input_;
    const PythonScalarConversions *conversions_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
};

class NB_HIDDEN TSInput final : public InputView {
  public:
    using InputView::InputView;
    nanobind::object value() const;
    nanobind::object delta_value() const;
};

class NB_HIDDEN TSSInput final : public InputView {
  public:
    using InputView::InputView;
    nanobind::object value() const;
    nanobind::object delta_value() const;
    std::size_t size() const;
    bool contains(nanobind::handle item) const;
    nanobind::object iter() const;
    nanobind::object added() const;
    nanobind::object removed() const;
    bool was_added(nanobind::handle item) const;
    bool was_removed(nanobind::handle item) const;
};

class NB_HIDDEN TSLInput final : public InputView {
  public:
    using InputView::InputView;
    nanobind::object value() const;
    nanobind::object delta_value() const;
    std::size_t size() const;
    nanobind::object getitem(nanobind::handle key) const;
    nanobind::object iter() const;
    nanobind::object keys() const;
    nanobind::object values() const;
    nanobind::object items() const;
    nanobind::object modified_keys() const;
    nanobind::object modified_values() const;
    nanobind::object modified_items() const;
    nanobind::object valid_keys() const;
    nanobind::object valid_values() const;
    nanobind::object valid_items() const;
};

class NB_HIDDEN TSBInput final : public InputView {
  public:
    using InputView::InputView;
    nanobind::object value() const;
    nanobind::object delta_value() const;
    std::size_t size() const;
    nanobind::object getitem(nanobind::handle key) const;
    nanobind::object getattr(nanobind::handle key) const;
    nanobind::object keys() const;
    nanobind::object values() const;
    nanobind::object items() const;
    nanobind::object modified_keys() const;
    nanobind::object modified_values() const;
    nanobind::object modified_items() const;
    nanobind::object valid_keys() const;
    nanobind::object valid_values() const;
    nanobind::object valid_items() const;
};

class NB_HIDDEN TSDInput final : public InputView {
  public:
    using InputView::InputView;
    nanobind::object value() const;
    nanobind::object delta_value() const;
    std::size_t size() const;
    nanobind::object getitem(nanobind::handle key) const;
    nanobind::object get(nanobind::handle key, nanobind::handle fallback) const;
    bool contains(nanobind::handle key) const;
    nanobind::object iter() const;
    nanobind::object key_set() const;
    nanobind::object keys() const;
    nanobind::object values() const;
    nanobind::object items() const;
    nanobind::object added_keys() const;
    nanobind::object added_values() const;
    nanobind::object added_items() const;
    nanobind::object removed_keys() const;
    nanobind::object removed_values() const;
    nanobind::object removed_items() const;
    nanobind::object modified_keys() const;
    nanobind::object modified_values() const;
    nanobind::object modified_items() const;
    nanobind::object valid_keys() const;
    nanobind::object valid_values() const;
    nanobind::object valid_items() const;
};

class NB_HIDDEN REFInput final : public InputView {
  public:
    using InputView::InputView;
    nanobind::object value() const;
    nanobind::object delta_value() const;
};

nanobind::object make_input_view(In<TS_T> input, const PythonScalarConversions &conversions,
                                 const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation);
void bind_input_iterators(nanobind::module_ &module);

class NB_HIDDEN ReferenceValue {
  public:
    ReferenceValue(Reference value, std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation) noexcept
        : value_{value}, lifetime_{std::move(lifetime)}, generation_{generation} {}

    Reference value() const;
    bool valid() const;

  private:
    void check_lifetime() const;

    Reference value_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
};

} // namespace bg::python
