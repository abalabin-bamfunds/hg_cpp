#pragma once

#include <bg/python/input_view.hpp>
#include <bg/scalar/dict/scalar.hpp>

#include <nanobind/nanobind.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace bg::python {

class PythonDictCursorIterator {
  public:
    static constexpr auto batch_capacity = std::size_t{16};
    enum class Projection : std::uint8_t { keys, values, items };

    PythonDictCursorIterator(Scalar<Dict<SCALAR, SCALAR>> dictionary, const PythonScalarConversions &conversions,
                             std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation,
                             Projection projection)
        : runtime_type_{&dictionary.runtime_type()}, cursor_{dictionary.make_cursor()}, conversions_{&conversions},
          key_to_python_{conversions.to_python_handler(dictionary.runtime_type().key_runtime_type())},
          mapped_to_python_{conversions.to_python_handler(dictionary.runtime_type().mapped_runtime_type())},
          lifetime_{std::move(lifetime)}, generation_{generation}, projection_{projection} {}

    nanobind::object next();

  private:
    void check_lifetime() const;

    const ScalarDictRuntimeType *runtime_type_;
    std::unique_ptr<ScalarDictRuntimeType::Cursor> cursor_;
    const PythonScalarConversions *conversions_;
    ScalarToPython key_to_python_;
    ScalarToPython mapped_to_python_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
    Projection projection_;
    // next_batch fills the returned prefix, and next() never reads outside it.
    std::array<ScalarDictRuntimeType::BatchEntry, batch_capacity> batch_;
    std::size_t batch_size_{};
    std::size_t batch_index_{};
};

class PythonDictEntryView {
  public:
    PythonDictEntryView(Scalar<Dict<SCALAR, SCALAR>> dictionary, const PythonScalarConversions &conversions,
                        std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation,
                        PythonDictCursorIterator::Projection projection) noexcept
        : dictionary_{dictionary}, conversions_{&conversions}, lifetime_{std::move(lifetime)}, generation_{generation},
          projection_{projection} {}

    std::size_t size() const;
    nanobind::object iter() const;

  private:
    void check_lifetime() const;

    Scalar<Dict<SCALAR, SCALAR>> dictionary_;
    const PythonScalarConversions *conversions_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
    PythonDictCursorIterator::Projection projection_;
};

class PythonDictView {
  public:
    PythonDictView(Scalar<Dict<SCALAR, SCALAR>> dictionary, const PythonScalarConversions &conversions,
                   std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation) noexcept
        : dictionary_{dictionary}, conversions_{&conversions},
          key_conversion_{conversions.from_python_handler(dictionary.runtime_type().key_runtime_type().type())},
          value_conversion_{conversions.to_python_handler(dictionary.runtime_type().mapped_runtime_type())},
          lifetime_{std::move(lifetime)}, generation_{generation} {
    }

    std::size_t size() const;
    nanobind::object getitem(nanobind::handle key) const;
    bool contains(nanobind::handle key) const;
    nanobind::object get(nanobind::handle key, nanobind::handle fallback) const;
    PythonDictCursorIterator iter() const;
    nanobind::object keys() const;
    nanobind::object items() const;
    nanobind::object values() const;
    nanobind::object copy() const;

  private:
    void check_lifetime() const;
    ScalarValue<> key(nanobind::handle value) const;

    Scalar<Dict<SCALAR, SCALAR>> dictionary_;
    const PythonScalarConversions *conversions_;
    PythonToScalar key_conversion_;
    ScalarToPython value_conversion_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
};

void bind_dict_view(nanobind::module_ &module);

} // namespace bg::python
