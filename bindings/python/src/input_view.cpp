#include <bg/python/input_view.hpp>

#include <bg/scalar/runtime_type.hpp>
#include <bg/time_series/ref/ref_input.hpp>
#include <bg/time_series/tsb/tsb_input.hpp>
#include <bg/time_series/tsd/tsd_input.hpp>
#include <bg/time_series/tsl/tsl_input.hpp>
#include <bg/time_series/tss/tss_input.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_set_type.hpp>

#include "delta.hpp"

#include <nanobind/stl/chrono.h>
#include <nanobind/stl/string.h>

#include <memory_resource>
#include <stdexcept>
#include <string>

namespace nb = nanobind;
using namespace nanobind::literals;

namespace bg::python {
namespace {

nb::object scalar_value(const In<TS<SCALAR>> &input, const PythonScalarConversions &conversions) {
    const auto value = input.value_view();
    return conversions.to_python(Scalar<>::from(value));
}

enum class CollectionRange : std::uint8_t { current, added, removed, modified, valid, updated, changes };
enum class CollectionProjection : std::uint8_t { key, input, value, item };

nb::object make_time_series_value(In<TS_T> input, const PythonScalarConversions &conversions,
                                  const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation,
                                  CollectionRange range);

class CollectionIterator {
  public:
    CollectionIterator(In<TS_T> input, const PythonScalarConversions &conversions,
                       std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation, CollectionRange range,
                       CollectionProjection projection) noexcept
        : input_{input}, conversions_{&conversions}, lifetime_{std::move(lifetime)}, generation_{generation},
          range_{range}, projection_{projection} {}

    CollectionIterator &iter() noexcept { return *this; }

    nb::object next();

  private:
    void check_lifetime() const {
        if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
            throw std::runtime_error{"a Python collection iterator was used outside its callback"};
        }
    }

    nb::object child(In<TS_T> value, bool removed = false) const {
        if (projection_ == CollectionProjection::input) {
            return make_input_view(value, *conversions_, lifetime_, generation_);
        }
        if (removed) {
            return nb::borrow<nb::object>(details::remove_sentinel());
        }
        return make_time_series_value(value, *conversions_, lifetime_, generation_, range_);
    }

    nb::object project(nb::object key, In<TS_T> value, bool removed = false) const {
        if (projection_ == CollectionProjection::key) {
            return key;
        }
        auto item = child(value, removed);
        return projection_ == CollectionProjection::item ? nb::make_tuple(std::move(key), item) : item;
    }

    nb::object next(const In<TSS<SCALAR>> &set) {
        const auto id = range_ == CollectionRange::current ? set.next_item_id(cursor_)
                        : range_ == CollectionRange::added ? set.next_added_id(cursor_)
                                                           : set.next_removed_id(cursor_);
        if (id == util::occupancy_index::npos) {
            throw nb::stop_iteration();
        }
        cursor_ = id + 1;
        const auto item = set.item_at(id);
        return conversions_->to_python(Scalar<>::from(item));
    }

    template <typename Item, auto Size, auto... Properties>
    nb::object next(const In<TSL<Item, Size, Properties...>> &list) {
        while (cursor_ < list.size()) {
            const auto index = cursor_++;
            const auto value = list[index];
            if (range_ == CollectionRange::updated && !list.item_updated(index)) {
                continue;
            }
            if (range_ == CollectionRange::valid && !value.valid()) {
                continue;
            }
            if (range_ != CollectionRange::current && range_ != CollectionRange::updated &&
                range_ != CollectionRange::valid) {
                break;
            }
            return project(nb::int_(index), value);
        }
        throw nb::stop_iteration();
    }

    template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
    nb::object next(const In<TSB<Schema, Properties...>> &bundle) {
        while (cursor_ < bundle.size()) {
            const auto index = cursor_++;
            const auto value = bundle[index];
            if (range_ == CollectionRange::updated && !bundle.item_updated(index)) {
                continue;
            }
            if (range_ == CollectionRange::valid && !value.valid()) {
                continue;
            }
            if (range_ != CollectionRange::current && range_ != CollectionRange::updated &&
                range_ != CollectionRange::valid) {
                break;
            }
            return project(nb::cast(std::string{bundle.item_name(index).text()}), value);
        }
        throw nb::stop_iteration();
    }

    template <typename Key, typename Item, auto... Properties>
    nb::object next(const In<TSD<Key, Item, Properties...>> &dictionary) {
        const auto next_id = [&](CollectionRange range) {
            switch (range) {
            case CollectionRange::current:
            case CollectionRange::valid:
                return dictionary.next_item_id(cursor_);
            case CollectionRange::added:
                return dictionary.next_added_id(cursor_);
            case CollectionRange::removed:
                return dictionary.next_removed_id(cursor_);
            case CollectionRange::modified:
                return dictionary.next_modified_id(cursor_);
            default:
                return util::occupancy_index::npos;
            }
        };

        while (true) {
            auto range = range_;
            if (range_ == CollectionRange::changes) {
                range = phase_ == 0   ? CollectionRange::added
                        : phase_ == 1 ? CollectionRange::modified
                                      : CollectionRange::removed;
            }

            const auto id = next_id(range);
            if (id == util::occupancy_index::npos) {
                if (range_ == CollectionRange::changes && phase_ < 2) {
                    ++phase_;
                    cursor_ = 0;
                    continue;
                }
                throw nb::stop_iteration();
            }

            cursor_ = id + 1;
            const auto value = dictionary.item_at(id);
            if (range_ == CollectionRange::valid && !value.valid()) {
                continue;
            }
            const auto key = dictionary.key_at(id);
            return project(conversions_->to_python(Scalar<>::from(key)), value, range == CollectionRange::removed);
        }
    }

    In<TS_T> input_;
    const PythonScalarConversions *conversions_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
    CollectionRange range_;
    CollectionProjection projection_;
    item_id_t cursor_{};
    unsigned phase_{};
};

class CollectionEntries {
  public:
    CollectionEntries(In<TS_T> input, const PythonScalarConversions &conversions,
                      std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation, CollectionRange range,
                      CollectionProjection projection) noexcept
        : input_{input}, conversions_{&conversions}, lifetime_{std::move(lifetime)}, generation_{generation},
          range_{range}, projection_{projection} {}

    nb::object iter() const {
        check_lifetime();
        return nb::cast(CollectionIterator{input_, *conversions_, lifetime_, generation_, range_, projection_});
    }

  private:
    void check_lifetime() const {
        if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
            throw std::runtime_error{"a Python collection view was used outside its callback"};
        }
    }

    In<TS_T> input_;
    const PythonScalarConversions *conversions_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
    CollectionRange range_;
    CollectionProjection projection_;
};

class CollectionValue {
  public:
    CollectionValue(In<TS_T> input, const PythonScalarConversions &conversions,
                    std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation,
                    CollectionRange range) noexcept
        : input_{input}, conversions_{&conversions}, lifetime_{std::move(lifetime)}, generation_{generation},
          range_{range} {}

    std::size_t size() const;
    nb::object getitem(nb::handle key) const;
    nb::object get(nb::handle key, nb::handle fallback) const;
    bool contains(nb::handle key) const;
    nb::object iter() const;
    nb::object keys() const;
    nb::object values() const;
    nb::object items() const;
    nb::object getattr(nb::handle key) const;
    nb::object copy_set() const;
    nb::object copy_list() const;
    nb::object copy_mapping() const;

  private:
    void check_lifetime() const {
        if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
            throw std::runtime_error{"a Python collection value was used outside its callback"};
        }
    }

    nb::object entries(CollectionProjection projection) const {
        return nb::cast(CollectionEntries{input_, *conversions_, lifetime_, generation_, range_, projection});
    }

    In<TS_T> input_;
    const PythonScalarConversions *conversions_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
    CollectionRange range_;
};

class SetValueView final : public CollectionValue {
  public:
    using CollectionValue::CollectionValue;
};

class ListValueView final : public CollectionValue {
  public:
    using CollectionValue::CollectionValue;
};

class ChangeValueView final : public CollectionValue {
  public:
    using CollectionValue::CollectionValue;
};

class BundleValueView final : public CollectionValue {
  public:
    using CollectionValue::CollectionValue;
};

class DictionaryValueView final : public CollectionValue {
  public:
    using CollectionValue::CollectionValue;
};

class SetDeltaView {
  public:
    SetDeltaView(In<TS_T> input, const PythonScalarConversions &conversions, std::shared_ptr<CallbackLifetime> lifetime,
                 std::uint64_t generation) noexcept
        : input_{input}, conversions_{&conversions}, lifetime_{std::move(lifetime)}, generation_{generation} {}

    nb::object added() const {
        check_lifetime();
        return nb::cast(SetValueView{input_, *conversions_, lifetime_, generation_, CollectionRange::added});
    }

    nb::object removed() const {
        check_lifetime();
        return nb::cast(SetValueView{input_, *conversions_, lifetime_, generation_, CollectionRange::removed});
    }

  private:
    void check_lifetime() const {
        if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
            throw std::runtime_error{"a Python set delta view was used outside its callback"};
        }
    }

    In<TS_T> input_;
    const PythonScalarConversions *conversions_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
};

nb::object make_time_series_value(In<TS_T> input, const PythonScalarConversions &conversions,
                                  const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation,
                                  CollectionRange range) {
    const auto delta = range != CollectionRange::current && range != CollectionRange::valid;
    if ((delta && !input.modified()) || (!delta && !input.valid())) {
        return nb::none();
    }

    const auto result = input.visit(
        [&](const In<TS<SCALAR>> *value) { return scalar_value(*value, conversions); },
        [&](const In<TSS<SCALAR>> *) -> nb::object {
            if (delta) {
                return nb::cast(SetDeltaView{input, conversions, lifetime, generation});
            }
            return nb::cast(SetValueView{input, conversions, lifetime, generation, CollectionRange::current});
        },
        [&](const In<TSL<TS_T, SIZE>> *) -> nb::object {
            const auto child_range = delta ? CollectionRange::updated : CollectionRange::current;
            if (delta) {
                return nb::cast(ChangeValueView{input, conversions, lifetime, generation, child_range});
            }
            return nb::cast(ListValueView{input, conversions, lifetime, generation, child_range});
        },
        [&](const In<TSB<SCHEMA>> *) -> nb::object {
            const auto child_range = delta ? CollectionRange::updated : CollectionRange::current;
            return nb::cast(BundleValueView{input, conversions, lifetime, generation, child_range});
        },
        [&](const In<TSD<SCALAR, TS_T>> *) -> nb::object {
            const auto child_range = delta ? CollectionRange::changes : CollectionRange::current;
            return nb::cast(DictionaryValueView{input, conversions, lifetime, generation, child_range});
        },
        [&](const In<REF<TS_T>> *value) { return nb::cast(ReferenceValue{value->value(), lifetime, generation}); },
        ddv::noop);
    if (!result) {
        throw std::logic_error{"the Python input has an unsupported time-series type"};
    }
    return std::move(*result);
}

nb::object CollectionIterator::next() {
    check_lifetime();
    const auto result = input_.visit([&](const In<TSS<SCALAR>> *value) { return next(*value); },
                                     [&](const In<TSL<TS_T, SIZE>> *value) { return next(*value); },
                                     [&](const In<TSB<SCHEMA>> *value) { return next(*value); },
                                     [&](const In<TSD<SCALAR, TS_T>> *value) { return next(*value); }, ddv::noop);
    if (!result) {
        throw std::logic_error{"the Python input is not a collection"};
    }
    return std::move(*result);
}

nb::object CollectionValue::getitem(nb::handle key) const {
    check_lifetime();
    const auto result = input_.visit(
        [&](const In<TSL<TS_T, SIZE>> *list) -> nb::object {
            const auto index = nb::cast<std::size_t>(key);
            if (index >= list->size()) {
                throw nb::index_error();
            }
            if (range_ == CollectionRange::updated && !list->item_updated(index)) {
                throw nb::key_error();
            }
            if (range_ != CollectionRange::current && range_ != CollectionRange::updated) {
                throw nb::key_error();
            }
            const auto value = (*list)[index];
            return make_time_series_value(value, *conversions_, lifetime_, generation_, range_);
        },
        [&](const In<TSB<SCHEMA>> *bundle) -> nb::object {
            auto index = std::size_t{};
            if (PyIndex_Check(key.ptr())) {
                index = nb::cast<std::size_t>(key);
            } else {
                const auto found = type_cast<TimeSeriesBundleType>(bundle->runtime_type().type())
                                       .find_item_index(util::symbol{nb::cast<std::string>(key)});
                if (!found) {
                    throw nb::key_error();
                }
                index = *found;
            }
            if (index >= bundle->size()) {
                throw nb::index_error();
            }
            if (range_ == CollectionRange::updated && !bundle->item_updated(index)) {
                throw nb::key_error();
            }
            if (range_ != CollectionRange::current && range_ != CollectionRange::updated) {
                throw nb::key_error();
            }
            const auto value = (*bundle)[index];
            return make_time_series_value(value, *conversions_, lifetime_, generation_, range_);
        },
        [&](const In<TSD<SCALAR, TS_T>> *dictionary) -> nb::object {
            const auto &type = type_cast<TimeSeriesDictionaryType>(dictionary->runtime_type().type());
            auto memory = std::pmr::unsynchronized_pool_resource{};
            auto allocation = allocator{memory};
            const auto scalar = conversions_->from_python(type.key_type(), key, allocation);
            const auto typed_key = Scalar<SCALAR>::from(scalar);
            auto id = util::occupancy_index::npos;
            auto removed = false;

            if (range_ == CollectionRange::current || range_ == CollectionRange::valid) {
                id = dictionary->find_item_id(typed_key);
            }
            if (range_ == CollectionRange::added) {
                id = dictionary->find_added_id(typed_key);
            }
            if (range_ == CollectionRange::removed) {
                id = dictionary->find_removed_id(typed_key);
                removed = true;
            }
            if (range_ == CollectionRange::modified) {
                id = dictionary->find_modified_id(typed_key);
            }
            if (range_ == CollectionRange::changes) {
                id = dictionary->find_added_id(typed_key);
                if (id == util::occupancy_index::npos) {
                    id = dictionary->find_modified_id(typed_key);
                }
                if (id == util::occupancy_index::npos) {
                    id = dictionary->find_removed_id(typed_key);
                    removed = id != util::occupancy_index::npos;
                }
            }
            if (id == util::occupancy_index::npos) {
                PyErr_SetObject(PyExc_KeyError, key.ptr());
                return {};
            }
            if (removed) {
                return nb::borrow<nb::object>(details::remove_sentinel());
            }
            const auto value = dictionary->item_at(id);
            return make_time_series_value(value, *conversions_, lifetime_, generation_, range_);
        },
        ddv::noop);
    if (!result) {
        throw nb::type_error("this collection value does not support indexed lookup");
    }
    return std::move(*result);
}

std::size_t CollectionValue::size() const {
    check_lifetime();
    if (range_ == CollectionRange::current) {
        const auto result =
            input_.visit([](const In<TSS<SCALAR>> *set) { return set->size(); },
                         [](const In<TSL<TS_T, SIZE>> *list) { return list->size(); },
                         [](const In<TSB<SCHEMA>> *bundle) { return bundle->size(); },
                         [](const In<TSD<SCALAR, TS_T>> *dictionary) { return dictionary->size(); }, ddv::noop);
        if (result) {
            return *result;
        }
    }

    auto iterator =
        CollectionIterator{input_, *conversions_, lifetime_, generation_, range_, CollectionProjection::key};
    auto count = std::size_t{};
    try {
        while (true) {
            iterator.next();
            ++count;
        }
    } catch (const nb::builtin_exception &error) {
        if (error.type() == nb::exception_type::stop_iteration) {
            return count;
        }
        throw;
    }
}

nb::object CollectionValue::get(nb::handle key, nb::handle fallback) const {
    try {
        auto result = getitem(key);
        if (result) {
            return result;
        }
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            return nb::borrow<nb::object>(fallback);
        }
        throw nb::python_error();
    } catch (const nb::builtin_exception &error) {
        if (error.type() == nb::exception_type::key_error) {
            return nb::borrow<nb::object>(fallback);
        }
        throw;
    } catch (const nb::python_error &) {
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            return nb::borrow<nb::object>(fallback);
        }
        throw;
    }
}

bool CollectionValue::contains(nb::handle key) const {
    check_lifetime();
    const auto result = input_.visit(
        [&](const In<TSS<SCALAR>> *set) {
            const auto &type = type_cast<TimeSeriesSetType>(set->runtime_type().type());
            auto memory = std::pmr::unsynchronized_pool_resource{};
            auto allocation = allocator{memory};
            const auto scalar = conversions_->from_python(type.item_type(), key, allocation);
            const auto item = Scalar<SCALAR>::from(scalar);
            if (range_ == CollectionRange::added) {
                return set->was_added(item);
            }
            if (range_ == CollectionRange::removed) {
                return set->was_removed(item);
            }
            return range_ == CollectionRange::current && set->contains(item);
        },
        [&](const In<TSD<SCALAR, TS_T>> *dictionary) {
            const auto &type = type_cast<TimeSeriesDictionaryType>(dictionary->runtime_type().type());
            auto memory = std::pmr::unsynchronized_pool_resource{};
            auto allocation = allocator{memory};
            const auto scalar = conversions_->from_python(type.key_type(), key, allocation);
            const auto key_value = Scalar<SCALAR>::from(scalar);
            if (range_ == CollectionRange::changes) {
                return dictionary->find_added_id(key_value) != util::occupancy_index::npos ||
                       dictionary->find_modified_id(key_value) != util::occupancy_index::npos ||
                       dictionary->find_removed_id(key_value) != util::occupancy_index::npos;
            }
            if (range_ == CollectionRange::added) {
                return dictionary->find_added_id(key_value) != util::occupancy_index::npos;
            }
            if (range_ == CollectionRange::removed) {
                return dictionary->find_removed_id(key_value) != util::occupancy_index::npos;
            }
            if (range_ == CollectionRange::modified) {
                return dictionary->find_modified_id(key_value) != util::occupancy_index::npos;
            }
            return range_ == CollectionRange::current &&
                   dictionary->find_item_id(key_value) != util::occupancy_index::npos;
        },
        ddv::noop);
    return result.value_or(false);
}

nb::object CollectionValue::iter() const {
    check_lifetime();
    const auto projection = input_.visit(
        [&](const In<TSL<TS_T, SIZE>> *) {
            return range_ == CollectionRange::current ? CollectionProjection::value : CollectionProjection::key;
        },
        [](const In<TSS<SCALAR>> *) { return CollectionProjection::value; },
        [](const In<TSB<SCHEMA>> *) { return CollectionProjection::key; },
        [](const In<TSD<SCALAR, TS_T>> *) { return CollectionProjection::key; }, ddv::noop);
    if (!projection) {
        throw nb::type_error("this value is not a collection");
    }
    return nb::cast(CollectionIterator{input_, *conversions_, lifetime_, generation_, range_, *projection});
}

nb::object CollectionValue::keys() const {
    check_lifetime();
    return entries(CollectionProjection::key);
}

nb::object CollectionValue::values() const {
    check_lifetime();
    return entries(CollectionProjection::value);
}

nb::object CollectionValue::items() const {
    check_lifetime();
    return entries(CollectionProjection::item);
}

nb::object CollectionValue::getattr(nb::handle key) const {
    try {
        auto result = getitem(key);
        if (result) {
            return result;
        }
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            throw nb::attribute_error();
        }
        throw nb::python_error();
    } catch (const nb::builtin_exception &error) {
        if (error.type() == nb::exception_type::key_error) {
            throw nb::attribute_error();
        }
        throw;
    } catch (const nb::python_error &) {
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            throw nb::attribute_error();
        }
        throw;
    }
}

nb::object CollectionValue::copy_set() const {
    check_lifetime();
    return nb::module_::import_("builtins").attr("set")(iter());
}

nb::object CollectionValue::copy_list() const {
    check_lifetime();
    return nb::module_::import_("builtins").attr("list")(iter());
}

nb::object CollectionValue::copy_mapping() const {
    check_lifetime();
    return nb::module_::import_("builtins").attr("dict")(entries(CollectionProjection::item));
}

} // namespace

nb::object make_input_view(In<TS_T> input, const PythonScalarConversions &conversions,
                           const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
    const auto result = input.visit(
        [&](const In<TS<SCALAR>> *) { return nb::cast(TSInput{input, conversions, lifetime, generation}); },
        [&](const In<TSS<SCALAR>> *) { return nb::cast(TSSInput{input, conversions, lifetime, generation}); },
        [&](const In<TSL<TS_T, SIZE>> *) { return nb::cast(TSLInput{input, conversions, lifetime, generation}); },
        [&](const In<TSB<SCHEMA>> *) { return nb::cast(TSBInput{input, conversions, lifetime, generation}); },
        [&](const In<TSD<SCALAR, TS_T>> *) { return nb::cast(TSDInput{input, conversions, lifetime, generation}); },
        [&](const In<REF<TS_T>> *) { return nb::cast(REFInput{input, conversions, lifetime, generation}); }, ddv::noop);
    if (!result) {
        throw std::logic_error{"the Python input has an unsupported time-series type"};
    }
    return std::move(*result);
}

bool InputView::valid() const {
    check_lifetime();
    return input().valid();
}

bool InputView::modified() const {
    check_lifetime();
    return input().modified();
}

nb::object InputView::last_modified_time() const {
    check_lifetime();
    return nb::cast(input().last_updated());
}

void InputView::check_lifetime() const {
    if (lifetime() == nullptr || !lifetime()->active(generation())) {
        throw std::runtime_error{"a Python time-series input view was used outside its callback"};
    }
}

nb::object TSInput::value() const {
    check_lifetime();
    if (!valid()) {
        throw std::runtime_error{"an invalid Python time-series input has no value"};
    }
    return scalar_value(as<In<TS<SCALAR>>>(), conversions());
}

nb::object TSInput::delta_value() const {
    check_lifetime();
    return modified() ? value() : nb::none();
}

nb::object REFInput::value() const {
    check_lifetime();
    if (!valid()) {
        throw std::runtime_error{"an invalid Python time-series input has no value"};
    }
    return nb::cast(ReferenceValue{as<In<REF<TS_T>>>().value(), lifetime(), generation()});
}

nb::object REFInput::delta_value() const {
    check_lifetime();
    return modified() ? value() : nb::none();
}

std::size_t TSSInput::size() const {
    check_lifetime();
    return as<In<TSS<SCALAR>>>().size();
}

bool TSSInput::contains(nb::handle item) const {
    check_lifetime();
    const auto &set = as<In<TSS<SCALAR>>>();
    const auto &type = type_cast<TimeSeriesSetType>(set.runtime_type().type());
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocation = allocator{memory};
    const auto scalar = conversions().from_python(type.item_type(), item, allocation);
    return set.contains(Scalar<SCALAR>::from(scalar));
}

nb::object TSSInput::value() const {
    check_lifetime();
    if (!valid()) {
        throw std::runtime_error{"an invalid Python time-series input has no value"};
    }
    return nb::cast(SetValueView{input(), conversions(), lifetime(), generation(), CollectionRange::current});
}

nb::object TSSInput::delta_value() const {
    check_lifetime();
    if (!modified()) {
        return nb::none();
    }
    return nb::cast(SetDeltaView{input(), conversions(), lifetime(), generation()});
}

nb::object TSSInput::iter() const {
    check_lifetime();
    return nb::cast(CollectionIterator{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                       CollectionProjection::value});
}

nb::object TSSInput::added() const {
    check_lifetime();
    return nb::cast(SetValueView{input(), conversions(), lifetime(), generation(), CollectionRange::added});
}

nb::object TSSInput::removed() const {
    check_lifetime();
    return nb::cast(SetValueView{input(), conversions(), lifetime(), generation(), CollectionRange::removed});
}

bool TSSInput::was_added(nb::handle item) const {
    check_lifetime();
    const auto &set = as<In<TSS<SCALAR>>>();
    const auto &type = type_cast<TimeSeriesSetType>(set.runtime_type().type());
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocation = allocator{memory};
    const auto scalar = conversions().from_python(type.item_type(), item, allocation);
    return set.was_added(Scalar<SCALAR>::from(scalar));
}

bool TSSInput::was_removed(nb::handle item) const {
    check_lifetime();
    const auto &set = as<In<TSS<SCALAR>>>();
    const auto &type = type_cast<TimeSeriesSetType>(set.runtime_type().type());
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocation = allocator{memory};
    const auto scalar = conversions().from_python(type.item_type(), item, allocation);
    return set.was_removed(Scalar<SCALAR>::from(scalar));
}

std::size_t TSLInput::size() const {
    check_lifetime();
    return as<In<TSL<TS_T, SIZE>>>().size();
}

nb::object TSLInput::getitem(nb::handle key) const {
    check_lifetime();
    const auto index = nb::cast<std::size_t>(key);
    const auto &list = as<In<TSL<TS_T, SIZE>>>();
    if (index >= list.size()) {
        throw nb::index_error();
    }
    return make_input_view(list[index], conversions(), lifetime(), generation());
}

nb::object TSLInput::value() const {
    check_lifetime();
    if (!valid()) {
        throw std::runtime_error{"an invalid Python time-series input has no value"};
    }
    return nb::cast(ListValueView{input(), conversions(), lifetime(), generation(), CollectionRange::current});
}

nb::object TSLInput::delta_value() const {
    check_lifetime();
    return modified()
               ? nb::cast(ChangeValueView{input(), conversions(), lifetime(), generation(), CollectionRange::updated})
               : nb::none();
}

nb::object TSLInput::iter() const {
    check_lifetime();
    return nb::cast(CollectionIterator{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                       CollectionProjection::input});
}

nb::object TSLInput::keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                      CollectionProjection::key});
}

nb::object TSLInput::values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                      CollectionProjection::input});
}

nb::object TSLInput::items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                      CollectionProjection::item});
}

nb::object TSLInput::modified_keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::updated,
                                      CollectionProjection::key});
}

nb::object TSLInput::modified_values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::updated,
                                      CollectionProjection::input});
}

nb::object TSLInput::modified_items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::updated,
                                      CollectionProjection::item});
}

nb::object TSLInput::valid_keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::valid,
                                      CollectionProjection::key});
}

nb::object TSLInput::valid_values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::valid,
                                      CollectionProjection::input});
}

nb::object TSLInput::valid_items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::valid,
                                      CollectionProjection::item});
}

std::size_t TSBInput::size() const {
    check_lifetime();
    return as<In<TSB<SCHEMA>>>().size();
}

nb::object TSBInput::getitem(nb::handle key) const {
    check_lifetime();
    const auto &bundle = as<In<TSB<SCHEMA>>>();
    auto index = std::size_t{};
    if (PyIndex_Check(key.ptr())) {
        index = nb::cast<std::size_t>(key);
    } else {
        const auto found = type_cast<TimeSeriesBundleType>(bundle.runtime_type().type())
                               .find_item_index(util::symbol{nb::cast<std::string>(key)});
        if (!found) {
            throw nb::key_error();
        }
        index = *found;
    }
    if (index >= bundle.size()) {
        throw nb::index_error();
    }
    return make_input_view(bundle[index], conversions(), lifetime(), generation());
}

nb::object TSBInput::getattr(nb::handle key) const {
    try {
        return getitem(key);
    } catch (const nb::builtin_exception &error) {
        if (error.type() == nb::exception_type::key_error) {
            throw nb::attribute_error();
        }
        throw;
    } catch (const nb::python_error &) {
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            throw nb::attribute_error();
        }
        throw;
    }
}

nb::object TSBInput::value() const {
    check_lifetime();
    if (!valid()) {
        throw std::runtime_error{"an invalid Python time-series input has no value"};
    }
    return nb::cast(BundleValueView{input(), conversions(), lifetime(), generation(), CollectionRange::current});
}

nb::object TSBInput::delta_value() const {
    check_lifetime();
    return modified()
               ? nb::cast(BundleValueView{input(), conversions(), lifetime(), generation(), CollectionRange::updated})
               : nb::none();
}

nb::object TSBInput::keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                      CollectionProjection::key});
}

nb::object TSBInput::values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                      CollectionProjection::input});
}

nb::object TSBInput::items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                      CollectionProjection::item});
}

nb::object TSBInput::modified_keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::updated,
                                      CollectionProjection::key});
}

nb::object TSBInput::modified_values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::updated,
                                      CollectionProjection::input});
}

nb::object TSBInput::modified_items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::updated,
                                      CollectionProjection::item});
}

nb::object TSBInput::valid_keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::valid,
                                      CollectionProjection::key});
}

nb::object TSBInput::valid_values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::valid,
                                      CollectionProjection::input});
}

nb::object TSBInput::valid_items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::valid,
                                      CollectionProjection::item});
}

std::size_t TSDInput::size() const {
    check_lifetime();
    return as<In<TSD<SCALAR, TS_T>>>().size();
}

nb::object TSDInput::getitem(nb::handle key) const {
    check_lifetime();
    const auto &dictionary = as<In<TSD<SCALAR, TS_T>>>();
    const auto &type = type_cast<TimeSeriesDictionaryType>(dictionary.runtime_type().type());
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocation = allocator{memory};
    const auto scalar = conversions().from_python(type.key_type(), key, allocation);
    const auto typed_key = Scalar<SCALAR>::from(scalar);
    const auto id = dictionary.find_item_id(typed_key);
    if (id == util::occupancy_index::npos) {
        PyErr_SetObject(PyExc_KeyError, key.ptr());
        return {};
    }
    return make_input_view(dictionary.item_at(id), conversions(), lifetime(), generation());
}

nb::object TSDInput::get(nb::handle key, nb::handle fallback) const {
    try {
        auto result = getitem(key);
        if (result) {
            return result;
        }
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            return nb::borrow<nb::object>(fallback);
        }
        throw nb::python_error();
    } catch (const nb::python_error &) {
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            return nb::borrow<nb::object>(fallback);
        }
        throw;
    }
}

bool TSDInput::contains(nb::handle key) const {
    check_lifetime();
    const auto &dictionary = as<In<TSD<SCALAR, TS_T>>>();
    const auto &type = type_cast<TimeSeriesDictionaryType>(dictionary.runtime_type().type());
    auto memory = std::pmr::unsynchronized_pool_resource{};
    auto allocation = allocator{memory};
    const auto scalar = conversions().from_python(type.key_type(), key, allocation);
    return dictionary.contains(Scalar<SCALAR>::from(scalar));
}

nb::object TSDInput::value() const {
    check_lifetime();
    if (!valid()) {
        throw std::runtime_error{"an invalid Python time-series input has no value"};
    }
    return nb::cast(DictionaryValueView{input(), conversions(), lifetime(), generation(), CollectionRange::current});
}

nb::object TSDInput::delta_value() const {
    check_lifetime();
    return modified() ? nb::cast(DictionaryValueView{input(), conversions(), lifetime(), generation(),
                                                     CollectionRange::changes})
                      : nb::none();
}

nb::object TSDInput::iter() const {
    check_lifetime();
    return nb::cast(CollectionIterator{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                       CollectionProjection::key});
}

nb::object TSDInput::key_set() const {
    check_lifetime();
    const auto keys = as<In<TSD<SCALAR, TS_T>>>().key_set();
    return make_input_view(In<TS_T>{keys.last_updated(), keys.data(), keys.runtime_type(), keys.modified()},
                           conversions(), lifetime(), generation());
}

nb::object TSDInput::keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                      CollectionProjection::key});
}

nb::object TSDInput::values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                      CollectionProjection::input});
}

nb::object TSDInput::items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::current,
                                      CollectionProjection::item});
}

nb::object TSDInput::added_keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::added,
                                      CollectionProjection::key});
}

nb::object TSDInput::added_values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::added,
                                      CollectionProjection::input});
}

nb::object TSDInput::added_items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::added,
                                      CollectionProjection::item});
}

nb::object TSDInput::removed_keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::removed,
                                      CollectionProjection::key});
}

nb::object TSDInput::removed_values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::removed,
                                      CollectionProjection::input});
}

nb::object TSDInput::removed_items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::removed,
                                      CollectionProjection::item});
}

nb::object TSDInput::modified_keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::modified,
                                      CollectionProjection::key});
}

nb::object TSDInput::modified_values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::modified,
                                      CollectionProjection::input});
}

nb::object TSDInput::modified_items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::modified,
                                      CollectionProjection::item});
}

nb::object TSDInput::valid_keys() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::valid,
                                      CollectionProjection::key});
}

nb::object TSDInput::valid_values() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::valid,
                                      CollectionProjection::input});
}

nb::object TSDInput::valid_items() const {
    check_lifetime();
    return nb::cast(CollectionEntries{input(), conversions(), lifetime(), generation(), CollectionRange::valid,
                                      CollectionProjection::item});
}

void bind_input_iterators(nb::module_ &module) {
    nb::class_<CollectionIterator>(module, "_CollectionIterator")
        .def("__iter__", &CollectionIterator::iter, nb::rv_policy::reference)
        .def("__next__", static_cast<nb::object (CollectionIterator::*)()>(&CollectionIterator::next));
    nb::class_<CollectionEntries>(module, "_CollectionEntries").def("__iter__", &CollectionEntries::iter);
    nb::class_<CollectionValue>(module, "_CollectionValue");
    nb::class_<SetValueView, CollectionValue>(module, "_SetValueView")
        .def("__len__", &CollectionValue::size)
        .def("__contains__", &CollectionValue::contains)
        .def("__iter__", &CollectionValue::iter)
        .def("copy", &CollectionValue::copy_set);
    nb::class_<ListValueView, CollectionValue>(module, "_ListValueView")
        .def("__len__", &CollectionValue::size)
        .def("__getitem__", &CollectionValue::getitem)
        .def("__iter__", &CollectionValue::iter)
        .def("copy", &CollectionValue::copy_list);
    nb::class_<ChangeValueView, CollectionValue>(module, "_ChangeValueView")
        .def("__len__", &CollectionValue::size)
        .def("__getitem__", &CollectionValue::getitem)
        .def("__iter__", &CollectionValue::iter)
        .def("get", &CollectionValue::get, "key"_a, "default"_a = nb::none())
        .def("keys", &CollectionValue::keys)
        .def("values", &CollectionValue::values)
        .def("items", &CollectionValue::items)
        .def("copy", &CollectionValue::copy_mapping);
    nb::class_<BundleValueView, CollectionValue>(module, "_BundleValueView")
        .def("__len__", &CollectionValue::size)
        .def("__getitem__", &CollectionValue::getitem)
        .def("__iter__", &CollectionValue::iter)
        .def("get", &CollectionValue::get, "key"_a, "default"_a = nb::none())
        .def("keys", &CollectionValue::keys)
        .def("values", &CollectionValue::values)
        .def("items", &CollectionValue::items)
        .def("copy", &CollectionValue::copy_mapping)
        .def("__getattr__", &CollectionValue::getattr);
    nb::class_<DictionaryValueView, CollectionValue>(module, "_DictionaryValueView")
        .def("__len__", &CollectionValue::size)
        .def("__getitem__", &CollectionValue::getitem)
        .def("__contains__", &CollectionValue::contains)
        .def("__iter__", &CollectionValue::iter)
        .def("get", &CollectionValue::get, "key"_a, "default"_a = nb::none())
        .def("keys", &CollectionValue::keys)
        .def("values", &CollectionValue::values)
        .def("items", &CollectionValue::items)
        .def("copy", &CollectionValue::copy_mapping);
    nb::class_<SetDeltaView>(module, "_SetDeltaView")
        .def_prop_ro("added", &SetDeltaView::added)
        .def_prop_ro("removed", &SetDeltaView::removed);
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
