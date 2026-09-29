#include <bg/python/input_view.hpp>

#include <bg/scalar/runtime_type.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>

#include "delta.hpp"

#include <nanobind/stl/string.h>

#include <memory_resource>
#include <stdexcept>
#include <string>

namespace nb = nanobind;

namespace bg::python {
namespace {

nb::object input_value(const In<TS_T> &input, const PythonScalarConversions &conversions,
                       const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation);

nb::object input_view(const In<TS_T> &input, const PythonScalarConversions &conversions,
                      const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
    return nb::cast(InputView{input, conversions, lifetime, generation});
}

nb::object scalar_value(const In<TS<SCALAR>> &input, const PythonScalarConversions &conversions) {
    const auto value = input.value_view();
    return conversions.to_python(Scalar<>::from(value));
}

nb::object set_value(const In<TSS<SCALAR>> &input, const PythonScalarConversions &conversions) {
    auto result = nb::set{};
    input.for_each([&](const Scalar<SCALAR> &item) { result.add(conversions.to_python(Scalar<>::from(item))); });
    return nb::frozenset{result};
}

nb::object list_value(const In<TSL<TS_T, SIZE>> &input, const PythonScalarConversions &conversions,
                      const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
    auto result = nb::list{};
    for (auto index = std::size_t{}; index < input.size(); ++index) {
        const auto item = input[index];
        result.append(item.valid() ? input_value(item, conversions, lifetime, generation) : nb::none());
    }
    return result;
}

nb::object bundle_value(const In<TSB<SCHEMA>> &input, const PythonScalarConversions &conversions,
                        const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
    auto result = nb::dict{};
    for (auto index = std::size_t{}; index < input.size(); ++index) {
        const auto item = input[index];
        const auto name = std::string{input.item_name(index).text()};
        auto value = item.valid() ? input_value(item, conversions, lifetime, generation) : nb::none();
        result[nb::str{name.c_str()}] = std::move(value);
    }
    return result;
}

nb::object dictionary_value(const In<TSD<SCALAR, TS_T>> &input, const PythonScalarConversions &conversions,
                            const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
    auto result = nb::dict{};
    input.for_each([&](const Scalar<SCALAR> &key, const In<TS_T> &item) {
        auto python_key = conversions.to_python(Scalar<>::from(key));
        result[python_key] = item.valid() ? input_value(item, conversions, lifetime, generation) : nb::none();
    });
    return result;
}

nb::object input_value(const In<TS_T> &input, const PythonScalarConversions &conversions,
                       const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
    if (!input.valid()) {
        throw std::runtime_error{"an invalid Python time-series input has no value"};
    }

    auto result = input.visit(
        [&](const In<TS<SCALAR>> *value) { return scalar_value(*value, conversions); },
        [&](const In<TSS<SCALAR>> *value) { return set_value(*value, conversions); },
        [&](const In<TSL<TS_T, SIZE>> *value) { return list_value(*value, conversions, lifetime, generation); },
        [&](const In<TSB<SCHEMA>> *value) { return bundle_value(*value, conversions, lifetime, generation); },
        [&](const In<TSD<SCALAR, TS_T>> *value) {
            return dictionary_value(*value, conversions, lifetime, generation);
        },
        [&](const In<REF<TS_T>> *value) {
            return nb::cast(ReferenceValue{value->value(), lifetime, generation});
        },
        ddv::noop);
    if (!result) {
        throw std::logic_error{"the Python input has an unsupported time-series type"};
    }
    return std::move(*result);
}

nb::object input_delta(const In<TS_T> &input, const PythonScalarConversions &conversions,
                       const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
    if (!input.modified()) {
        return nb::none();
    }

    auto result = input.visit(
        [&](const In<TS<SCALAR>> *value) { return scalar_value(*value, conversions); },
        [&](const In<TSS<SCALAR>> *value) {
            auto added = nb::set{};
            auto removed = nb::set{};
            value->for_each_added(
                [&](const Scalar<SCALAR> &item) { added.add(conversions.to_python(Scalar<>::from(item))); });
            value->for_each_removed(
                [&](const Scalar<SCALAR> &item) { removed.add(conversions.to_python(Scalar<>::from(item))); });
            return details::make_set_delta(nb::frozenset{added}, nb::frozenset{removed});
        },
        [&](const In<TSL<TS_T, SIZE>> *value) {
            auto delta = nb::dict{};
            value->for_each_updated([&](item_id_t index) {
                const auto item = (*value)[index];
                delta[nb::int_{index}] = input_delta(item, conversions, lifetime, generation);
            });
            return nb::object{std::move(delta)};
        },
        [&](const In<TSB<SCHEMA>> *value) {
            auto delta = nb::dict{};
            value->for_each_updated([&](item_id_t index) {
                const auto item = (*value)[index];
                const auto name = std::string{value->item_name(index).text()};
                delta[nb::str{name.c_str()}] = input_delta(item, conversions, lifetime, generation);
            });
            return nb::object{std::move(delta)};
        },
        [&](const In<TSD<SCALAR, TS_T>> *value) {
            auto removed = nb::set{};
            auto modified = nb::dict{};
            value->for_each_removed(
                [&](const Scalar<SCALAR> &key) { removed.add(conversions.to_python(Scalar<>::from(key))); });
            const auto add_modified = [&](const Scalar<SCALAR> &key) {
                auto python_key = conversions.to_python(Scalar<>::from(key));
                modified[python_key] = input_delta((*value)[key], conversions, lifetime, generation);
            };
            value->for_each_added(add_modified);
            value->for_each_modified(add_modified);

            return details::make_tsd_delta(nb::frozenset{removed}, modified);
        },
        [&](const In<REF<TS_T>> *value) {
            return nb::cast(ReferenceValue{value->value(), lifetime, generation});
        },
        ddv::noop);
    if (!result) {
        throw std::logic_error{"the Python input has an unsupported time-series type"};
    }
    return std::move(*result);
}

} // namespace

bool InputView::valid() const {
    check_lifetime();
    return input_.valid();
}

bool InputView::modified() const {
    check_lifetime();
    return input_.modified();
}

nb::object InputView::value() const {
    check_lifetime();
    return input_value(input_, *conversions_, lifetime_, generation_);
}

nb::object InputView::delta_value() const {
    check_lifetime();
    return input_delta(input_, *conversions_, lifetime_, generation_);
}

nb::object InputView::getitem(nb::handle key) const {
    check_lifetime();

    auto result = input_.visit(
        [&](const In<TSL<TS_T, SIZE>> *list) {
            const auto index = nb::cast<std::size_t>(key);
            if (index >= list->size()) {
                throw nb::index_error();
            }
            return input_view((*list)[index], *conversions_, lifetime_, generation_);
        },
        [&](const In<TSB<SCHEMA>> *bundle) {
            const auto name = util::symbol{nb::cast<std::string>(key)};
            const auto &type = type_cast<TimeSeriesBundleType>(bundle->runtime_type().type());
            const auto index = type.find_item_index(name);
            if (!index) {
                throw nb::key_error();
            }
            return input_view((*bundle)[*index], *conversions_, lifetime_, generation_);
        },
        [&](const In<TSD<SCALAR, TS_T>> *dictionary) {
            const auto &type = type_cast<TimeSeriesDictionaryType>(dictionary->runtime_type().type());
            auto temporary_memory = std::pmr::unsynchronized_pool_resource{};
            auto memory = allocator{temporary_memory};
            const auto scalar = conversions_->from_python(type.key_type(), key, memory);
            auto typed_key = Scalar<SCALAR>::from(scalar);
            if (!dictionary->contains(typed_key)) {
                throw nb::key_error();
            }
            return input_view((*dictionary)[typed_key], *conversions_, lifetime_, generation_);
        },
        ddv::noop);
    if (!result) {
        throw std::invalid_argument{"this Python time-series input does not contain addressable items"};
    }
    return std::move(*result);
}

void InputView::check_lifetime() const {
    if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
        throw std::runtime_error{"a Python time-series input view was used outside its callback"};
    }
}

Reference ReferenceValue::value() const {
    check_lifetime();
    return value_;
}

bool ReferenceValue::valid() const {
    check_lifetime();
    return static_cast<bool>(value_);
}

void ReferenceValue::check_lifetime() const {
    if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
        throw std::runtime_error{"a Python reference value was used outside its callback"};
    }
}

} // namespace bg::python
