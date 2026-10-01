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
#include <utility>

namespace nb = nanobind;
using namespace nanobind::literals;

namespace bg::python {
namespace {

nb::object scalar_value(const In<TS<SCALAR>> &input, const PythonScalarConversions &conversions) {
    const auto value = input.value_view();
    return conversions.to_python(Scalar<>::from(value));
}

enum class SetSelection : std::uint8_t { current, added, removed };
enum class FixedSelection : std::uint8_t { all, modified, valid };
enum class DictionarySelection : std::uint8_t { current, added, removed, modified, valid, changes };
enum class EntryProjection : std::uint8_t { key, value, item };
enum class EntryValue : std::uint8_t { input, current, delta };

nb::object make_current_value(In<TS_T> input, const PythonScalarConversions &conversions,
                              const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation);
nb::object make_delta_value(In<TS_T> input, const PythonScalarConversions &conversions,
                            const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation);

class CollectionViewState {
  public:
    CollectionViewState(In<TS_T> input, const PythonScalarConversions &conversions,
                        std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation) noexcept
        : input_{input}, conversions_{&conversions}, lifetime_{std::move(lifetime)}, generation_{generation} {}

  protected:
    void check_lifetime() const {
        if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
            throw std::runtime_error{"a Python collection view was used outside its callback"};
        }
    }

    const In<TS_T> &input() const noexcept { return input_; }
    const PythonScalarConversions &conversions() const noexcept { return *conversions_; }
    const std::shared_ptr<CallbackLifetime> &lifetime() const noexcept { return lifetime_; }
    std::uint64_t generation() const noexcept { return generation_; }

  private:
    In<TS_T> input_;
    const PythonScalarConversions *conversions_;
    std::shared_ptr<CallbackLifetime> lifetime_;
    std::uint64_t generation_;
};

class SetIterator final : public CollectionViewState {
  public:
    SetIterator(In<TS_T> input, const PythonScalarConversions &conversions, std::shared_ptr<CallbackLifetime> lifetime,
                std::uint64_t generation, SetSelection selection) noexcept
        : CollectionViewState{input, conversions, std::move(lifetime), generation}, selection_{selection} {}

    SetIterator &iter() noexcept { return *this; }

    nb::object next() {
        check_lifetime();
        const auto result = input().visit([&](const In<TSS<SCALAR>> *set) { return next(*set); }, ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input is not a time series set"};
        }
        return std::move(*result);
    }

  private:
    nb::object next(const In<TSS<SCALAR>> &set) {
        auto id = util::occupancy_index::npos;
        switch (selection_) {
        case SetSelection::current:
            id = set.next_item_id(cursor_);
            break;
        case SetSelection::added:
            id = set.next_added_id(cursor_);
            break;
        case SetSelection::removed:
            id = set.next_removed_id(cursor_);
            break;
        }
        if (id == util::occupancy_index::npos) {
            throw nb::stop_iteration();
        }

        cursor_ = id + 1;
        const auto item = set.item_at(id);
        return conversions().to_python(Scalar<>::from(item));
    }

    SetSelection selection_;
    item_id_t cursor_{};
};

class FixedIterator final : public CollectionViewState {
  public:
    FixedIterator(In<TS_T> input, const PythonScalarConversions &conversions,
                  std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation, FixedSelection selection,
                  EntryValue entry_value, EntryProjection projection) noexcept
        : CollectionViewState{input, conversions, std::move(lifetime), generation}, selection_{selection},
          entry_value_{entry_value}, projection_{projection} {}

    FixedIterator &iter() noexcept { return *this; }

    nb::object next() {
        check_lifetime();
        const auto result = input().visit([&](const In<TSL<TS_T, SIZE>> *list) { return next(*list); },
                                          [&](const In<TSB<SCHEMA>> *bundle) { return next(*bundle); }, ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input is not a fixed collection"};
        }
        return std::move(*result);
    }

  private:
    bool selected(bool modified, bool valid) const noexcept {
        switch (selection_) {
        case FixedSelection::all:
            return true;
        case FixedSelection::modified:
            return modified;
        case FixedSelection::valid:
            return valid;
        }
        std::unreachable();
    }

    nb::object entry(In<TS_T> value) const {
        switch (entry_value_) {
        case EntryValue::input:
            return make_input_view(value, conversions(), lifetime(), generation());
        case EntryValue::current:
            return make_current_value(value, conversions(), lifetime(), generation());
        case EntryValue::delta:
            return make_delta_value(value, conversions(), lifetime(), generation());
        }
        std::unreachable();
    }

    nb::object project(nb::object key, In<TS_T> value) const {
        if (projection_ == EntryProjection::key) {
            return key;
        }

        auto result = entry(value);
        return projection_ == EntryProjection::item ? nb::make_tuple(std::move(key), result) : result;
    }

    template <typename Item, auto Size, auto... Properties>
    nb::object next(const In<TSL<Item, Size, Properties...>> &list) {
        while (cursor_ < list.size()) {
            const auto index = cursor_++;
            const auto value = list[index];
            if (selected(list.item_updated(index), value.valid())) {
                return project(nb::int_(index), value);
            }
        }
        throw nb::stop_iteration();
    }

    template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
    nb::object next(const In<TSB<Schema, Properties...>> &bundle) {
        while (cursor_ < bundle.size()) {
            const auto index = cursor_++;
            const auto value = bundle[index];
            if (selected(bundle.item_updated(index), value.valid())) {
                return project(nb::cast(std::string{bundle.item_name(index).text()}), value);
            }
        }
        throw nb::stop_iteration();
    }

    FixedSelection selection_;
    EntryValue entry_value_;
    EntryProjection projection_;
    item_id_t cursor_{};
};

class FixedEntries final : public CollectionViewState {
  public:
    FixedEntries(In<TS_T> input, const PythonScalarConversions &conversions, std::shared_ptr<CallbackLifetime> lifetime,
                 std::uint64_t generation, FixedSelection selection, EntryValue entry_value,
                 EntryProjection projection) noexcept
        : CollectionViewState{input, conversions, std::move(lifetime), generation}, selection_{selection},
          entry_value_{entry_value}, projection_{projection} {}

    nb::object iter() const {
        check_lifetime();
        return nb::cast(
            FixedIterator{input(), conversions(), lifetime(), generation(), selection_, entry_value_, projection_});
    }

  private:
    FixedSelection selection_;
    EntryValue entry_value_;
    EntryProjection projection_;
};

class DictionaryIterator final : public CollectionViewState {
  public:
    DictionaryIterator(In<TS_T> input, const PythonScalarConversions &conversions,
                       std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation,
                       DictionarySelection selection, EntryValue entry_value, EntryProjection projection) noexcept
        : CollectionViewState{input, conversions, std::move(lifetime), generation}, selection_{selection},
          entry_value_{entry_value}, projection_{projection} {}

    DictionaryIterator &iter() noexcept { return *this; }

    nb::object next() {
        check_lifetime();
        const auto result =
            input().visit([&](const In<TSD<SCALAR, TS_T>> *dictionary) { return next(*dictionary); }, ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input is not a time series dictionary"};
        }
        return std::move(*result);
    }

  private:
    DictionarySelection active_selection() const noexcept {
        if (selection_ != DictionarySelection::changes) {
            return selection_;
        }
        if (phase_ == 0) {
            return DictionarySelection::added;
        }
        if (phase_ == 1) {
            return DictionarySelection::modified;
        }
        return DictionarySelection::removed;
    }

    template <typename Dictionary> item_id_t next_id(const Dictionary &dictionary, DictionarySelection selection) {
        switch (selection) {
        case DictionarySelection::current:
        case DictionarySelection::valid:
            return dictionary.next_item_id(cursor_);
        case DictionarySelection::added:
            return dictionary.next_added_id(cursor_);
        case DictionarySelection::removed:
            return dictionary.next_removed_id(cursor_);
        case DictionarySelection::modified:
            return dictionary.next_modified_id(cursor_);
        case DictionarySelection::changes:
            break;
        }
        std::unreachable();
    }

    nb::object entry(In<TS_T> value, bool removed) const {
        switch (entry_value_) {
        case EntryValue::input:
            return make_input_view(value, conversions(), lifetime(), generation());
        case EntryValue::current:
            return make_current_value(value, conversions(), lifetime(), generation());
        case EntryValue::delta:
            return removed ? nb::borrow<nb::object>(details::remove_sentinel())
                           : make_delta_value(value, conversions(), lifetime(), generation());
        }
        std::unreachable();
    }

    nb::object project(nb::object key, In<TS_T> value, bool removed) const {
        if (projection_ == EntryProjection::key) {
            return key;
        }

        auto result = entry(value, removed);
        return projection_ == EntryProjection::item ? nb::make_tuple(std::move(key), result) : result;
    }

    template <typename Key, typename Item, auto... Properties>
    nb::object next(const In<TSD<Key, Item, Properties...>> &dictionary) {
        while (true) {
            const auto selection = active_selection();
            const auto id = next_id(dictionary, selection);
            if (id == util::occupancy_index::npos) {
                if (selection_ == DictionarySelection::changes && phase_ < 2) {
                    ++phase_;
                    cursor_ = 0;
                    continue;
                }
                throw nb::stop_iteration();
            }

            cursor_ = id + 1;
            const auto value = dictionary.item_at(id);
            if (selection != DictionarySelection::valid || value.valid()) {
                const auto native_key = dictionary.key_at(id);
                const auto key = conversions().to_python(Scalar<>::from(native_key));
                return project(key, value, selection == DictionarySelection::removed);
            }
        }
    }

    DictionarySelection selection_;
    EntryValue entry_value_;
    EntryProjection projection_;
    item_id_t cursor_{};
    unsigned phase_{};
};

class DictionaryEntries final : public CollectionViewState {
  public:
    DictionaryEntries(In<TS_T> input, const PythonScalarConversions &conversions,
                      std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation,
                      DictionarySelection selection, EntryValue entry_value, EntryProjection projection) noexcept
        : CollectionViewState{input, conversions, std::move(lifetime), generation}, selection_{selection},
          entry_value_{entry_value}, projection_{projection} {}

    nb::object iter() const {
        check_lifetime();
        return nb::cast(DictionaryIterator{input(), conversions(), lifetime(), generation(), selection_, entry_value_,
                                           projection_});
    }

  private:
    DictionarySelection selection_;
    EntryValue entry_value_;
    EntryProjection projection_;
};

class SetValueView final : public CollectionViewState {
  public:
    SetValueView(In<TS_T> input, const PythonScalarConversions &conversions, std::shared_ptr<CallbackLifetime> lifetime,
                 std::uint64_t generation, SetSelection selection) noexcept
        : CollectionViewState{input, conversions, std::move(lifetime), generation}, selection_{selection} {}

    std::size_t size() const {
        check_lifetime();
        if (selection_ == SetSelection::current) {
            const auto result = input().visit([](const In<TSS<SCALAR>> *set) { return set->size(); }, ddv::noop);
            return result.value_or(0);
        }

        auto iterator = SetIterator{input(), conversions(), lifetime(), generation(), selection_};
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

    bool contains(nb::handle key) const {
        check_lifetime();
        const auto result = input().visit(
            [&](const In<TSS<SCALAR>> *set) {
                const auto &type = type_cast<TimeSeriesSetType>(set->runtime_type().type());
                auto memory = std::pmr::unsynchronized_pool_resource{};
                auto allocation = allocator{memory};
                const auto scalar = conversions().from_python(type.item_type(), key, allocation);
                const auto item = Scalar<SCALAR>::from(scalar);
                switch (selection_) {
                case SetSelection::current:
                    return set->contains(item);
                case SetSelection::added:
                    return set->was_added(item);
                case SetSelection::removed:
                    return set->was_removed(item);
                }
                std::unreachable();
            },
            ddv::noop);
        return result.value_or(false);
    }

    nb::object iter() const {
        check_lifetime();
        return nb::cast(SetIterator{input(), conversions(), lifetime(), generation(), selection_});
    }

    nb::object copy() const {
        check_lifetime();
        return nb::module_::import_("builtins").attr("set")(iter());
    }

  private:
    SetSelection selection_;
};

class ListValueView final : public CollectionViewState {
  public:
    using CollectionViewState::CollectionViewState;

    std::size_t size() const {
        check_lifetime();
        const auto result = input().visit([](const In<TSL<TS_T, SIZE>> *list) { return list->size(); }, ddv::noop);
        return result.value_or(0);
    }

    nb::object getitem(nb::handle key) const {
        check_lifetime();
        const auto result = input().visit(
            [&](const In<TSL<TS_T, SIZE>> *list) -> nb::object {
                const auto index = nb::cast<std::size_t>(key);
                if (index >= list->size()) {
                    throw nb::index_error();
                }
                return make_current_value((*list)[index], conversions(), lifetime(), generation());
            },
            ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input is not a time series list"};
        }
        return std::move(*result);
    }

    nb::object iter() const {
        check_lifetime();
        return nb::cast(FixedIterator{input(), conversions(), lifetime(), generation(), FixedSelection::all,
                                      EntryValue::current, EntryProjection::value});
    }

    nb::object copy() const {
        check_lifetime();
        return nb::module_::import_("builtins").attr("list")(iter());
    }
};

class FixedMappingView : public CollectionViewState {
  public:
    FixedMappingView(In<TS_T> input, const PythonScalarConversions &conversions,
                     std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation, FixedSelection selection,
                     EntryValue entry_value) noexcept
        : CollectionViewState{input, conversions, std::move(lifetime), generation}, selection_{selection},
          entry_value_{entry_value} {}

    std::size_t size() const {
        check_lifetime();
        if (selection_ == FixedSelection::all) {
            const auto result = input().visit([](const In<TSL<TS_T, SIZE>> *list) { return list->size(); },
                                              [](const In<TSB<SCHEMA>> *bundle) { return bundle->size(); }, ddv::noop);
            return result.value_or(0);
        }

        auto iterator = FixedIterator{input(),    conversions(), lifetime(),          generation(),
                                      selection_, entry_value_,  EntryProjection::key};
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

    nb::object getitem(nb::handle key) const {
        check_lifetime();
        const auto result = input().visit(
            [&](const In<TSL<TS_T, SIZE>> *list) -> nb::object {
                const auto index = nb::cast<std::size_t>(key);
                if (index >= list->size()) {
                    throw nb::index_error();
                }
                return selected_value((*list)[index], list->item_updated(index));
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
                return selected_value((*bundle)[index], bundle->item_updated(index));
            },
            ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input is not a fixed collection"};
        }
        return std::move(*result);
    }

    nb::object get(nb::handle key, nb::handle fallback) const {
        try {
            return getitem(key);
        } catch (const nb::builtin_exception &error) {
            if (error.type() == nb::exception_type::key_error) {
                return nb::borrow<nb::object>(fallback);
            }
            throw;
        }
    }

    nb::object iter() const {
        check_lifetime();
        return nb::cast(FixedIterator{input(), conversions(), lifetime(), generation(), selection_, entry_value_,
                                      EntryProjection::key});
    }

    nb::object keys() const { return entries(EntryProjection::key); }
    nb::object values() const { return entries(EntryProjection::value); }
    nb::object items() const { return entries(EntryProjection::item); }

    nb::object getattr(nb::handle key) const {
        try {
            return getitem(key);
        } catch (const nb::builtin_exception &error) {
            if (error.type() == nb::exception_type::key_error) {
                throw nb::attribute_error();
            }
            throw;
        }
    }

    nb::object copy() const {
        check_lifetime();
        return nb::module_::import_("builtins").attr("dict")(entries(EntryProjection::item));
    }

  private:
    nb::object selected_value(In<TS_T> value, bool modified) const {
        if (selection_ == FixedSelection::modified && !modified) {
            throw nb::key_error();
        }
        if (selection_ == FixedSelection::valid && !value.valid()) {
            throw nb::key_error();
        }

        switch (entry_value_) {
        case EntryValue::input:
            return make_input_view(value, conversions(), lifetime(), generation());
        case EntryValue::current:
            return make_current_value(value, conversions(), lifetime(), generation());
        case EntryValue::delta:
            return make_delta_value(value, conversions(), lifetime(), generation());
        }
        std::unreachable();
    }

    nb::object entries(EntryProjection projection) const {
        check_lifetime();
        return nb::cast(
            FixedEntries{input(), conversions(), lifetime(), generation(), selection_, entry_value_, projection});
    }

    FixedSelection selection_;
    EntryValue entry_value_;
};

class ListDeltaView final : public FixedMappingView {
  public:
    using FixedMappingView::FixedMappingView;
};

class BundleValueView final : public FixedMappingView {
  public:
    using FixedMappingView::FixedMappingView;
};

class DictionaryValueView final : public CollectionViewState {
  public:
    DictionaryValueView(In<TS_T> input, const PythonScalarConversions &conversions,
                        std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation,
                        DictionarySelection selection, EntryValue entry_value) noexcept
        : CollectionViewState{input, conversions, std::move(lifetime), generation}, selection_{selection},
          entry_value_{entry_value} {}

    std::size_t size() const {
        check_lifetime();
        if (selection_ == DictionarySelection::current) {
            const auto result =
                input().visit([](const In<TSD<SCALAR, TS_T>> *dictionary) { return dictionary->size(); }, ddv::noop);
            return result.value_or(0);
        }

        auto iterator = DictionaryIterator{input(),    conversions(), lifetime(),          generation(),
                                           selection_, entry_value_,  EntryProjection::key};
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

    nb::object getitem(nb::handle key) const {
        check_lifetime();
        const auto result = input().visit(
            [&](const In<TSD<SCALAR, TS_T>> *dictionary) -> nb::object {
                const auto &type = type_cast<TimeSeriesDictionaryType>(dictionary->runtime_type().type());
                auto memory = std::pmr::unsynchronized_pool_resource{};
                auto allocation = allocator{memory};
                const auto scalar = conversions().from_python(type.key_type(), key, allocation);
                const auto [id, removed] = find(*dictionary, Scalar<SCALAR>::from(scalar));
                if (id == util::occupancy_index::npos) {
                    PyErr_SetObject(PyExc_KeyError, key.ptr());
                    return {};
                }
                if (entry_value_ == EntryValue::delta && removed) {
                    return nb::borrow<nb::object>(details::remove_sentinel());
                }

                const auto value = dictionary->item_at(id);
                switch (entry_value_) {
                case EntryValue::input:
                    return make_input_view(value, conversions(), lifetime(), generation());
                case EntryValue::current:
                    return make_current_value(value, conversions(), lifetime(), generation());
                case EntryValue::delta:
                    return make_delta_value(value, conversions(), lifetime(), generation());
                }
                std::unreachable();
            },
            ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input is not a time series dictionary"};
        }
        return std::move(*result);
    }

    nb::object get(nb::handle key, nb::handle fallback) const {
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

    bool contains(nb::handle key) const {
        check_lifetime();
        const auto result = input().visit(
            [&](const In<TSD<SCALAR, TS_T>> *dictionary) {
                const auto &type = type_cast<TimeSeriesDictionaryType>(dictionary->runtime_type().type());
                auto memory = std::pmr::unsynchronized_pool_resource{};
                auto allocation = allocator{memory};
                const auto scalar = conversions().from_python(type.key_type(), key, allocation);
                return find(*dictionary, Scalar<SCALAR>::from(scalar)).first != util::occupancy_index::npos;
            },
            ddv::noop);
        return result.value_or(false);
    }

    nb::object iter() const {
        check_lifetime();
        return nb::cast(DictionaryIterator{input(), conversions(), lifetime(), generation(), selection_, entry_value_,
                                           EntryProjection::key});
    }

    nb::object keys() const { return entries(EntryProjection::key); }
    nb::object values() const { return entries(EntryProjection::value); }
    nb::object items() const { return entries(EntryProjection::item); }

    nb::object copy() const {
        check_lifetime();
        return nb::module_::import_("builtins").attr("dict")(entries(EntryProjection::item));
    }

  private:
    template <typename Dictionary, typename Key>
    std::pair<item_id_t, bool> find(const Dictionary &dictionary, const Key &key) const {
        switch (selection_) {
        case DictionarySelection::current:
        case DictionarySelection::valid:
            return {dictionary.find_item_id(key), false};
        case DictionarySelection::added:
            return {dictionary.find_added_id(key), false};
        case DictionarySelection::removed:
            return {dictionary.find_removed_id(key), true};
        case DictionarySelection::modified:
            return {dictionary.find_modified_id(key), false};
        case DictionarySelection::changes:
            auto id = dictionary.find_added_id(key);
            if (id != util::occupancy_index::npos) {
                return {id, false};
            }
            id = dictionary.find_modified_id(key);
            if (id != util::occupancy_index::npos) {
                return {id, false};
            }
            return {dictionary.find_removed_id(key), true};
        }
        std::unreachable();
    }

    nb::object entries(EntryProjection projection) const {
        check_lifetime();
        return nb::cast(
            DictionaryEntries{input(), conversions(), lifetime(), generation(), selection_, entry_value_, projection});
    }

    DictionarySelection selection_;
    EntryValue entry_value_;
};

class SetDeltaView final : public CollectionViewState {
  public:
    using CollectionViewState::CollectionViewState;

    nb::object added() const {
        check_lifetime();
        return nb::cast(SetValueView{input(), conversions(), lifetime(), generation(), SetSelection::added});
    }

    nb::object removed() const {
        check_lifetime();
        return nb::cast(SetValueView{input(), conversions(), lifetime(), generation(), SetSelection::removed});
    }
};

nb::object make_current_value(In<TS_T> input, const PythonScalarConversions &conversions,
                              const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
    if (!input.valid()) {
        return nb::none();
    }

    const auto result = input.visit(
        [&](const In<TS<SCALAR>> *value) { return scalar_value(*value, conversions); },
        [&](const In<TSS<SCALAR>> *) {
            return nb::cast(SetValueView{input, conversions, lifetime, generation, SetSelection::current});
        },
        [&](const In<TSL<TS_T, SIZE>> *) { return nb::cast(ListValueView{input, conversions, lifetime, generation}); },
        [&](const In<TSB<SCHEMA>> *) {
            return nb::cast(
                BundleValueView{input, conversions, lifetime, generation, FixedSelection::all, EntryValue::current});
        },
        [&](const In<TSD<SCALAR, TS_T>> *) {
            return nb::cast(DictionaryValueView{input, conversions, lifetime, generation, DictionarySelection::current,
                                                EntryValue::current});
        },
        [&](const In<REF<TS_T>> *value) { return nb::cast(ReferenceValue{value->value(), lifetime, generation}); },
        ddv::noop);
    if (!result) {
        throw std::logic_error{"the Python input has an unsupported time-series type"};
    }
    return std::move(*result);
}

nb::object make_delta_value(In<TS_T> input, const PythonScalarConversions &conversions,
                            const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
    if (!input.modified()) {
        return nb::none();
    }

    const auto result = input.visit(
        [&](const In<TS<SCALAR>> *value) { return scalar_value(*value, conversions); },
        [&](const In<TSS<SCALAR>> *) { return nb::cast(SetDeltaView{input, conversions, lifetime, generation}); },
        [&](const In<TSL<TS_T, SIZE>> *) {
            return nb::cast(
                ListDeltaView{input, conversions, lifetime, generation, FixedSelection::modified, EntryValue::delta});
        },
        [&](const In<TSB<SCHEMA>> *) {
            return nb::cast(
                BundleValueView{input, conversions, lifetime, generation, FixedSelection::modified, EntryValue::delta});
        },
        [&](const In<TSD<SCALAR, TS_T>> *) {
            return nb::cast(DictionaryValueView{input, conversions, lifetime, generation, DictionarySelection::changes,
                                                EntryValue::delta});
        },
        [&](const In<REF<TS_T>> *value) { return nb::cast(ReferenceValue{value->value(), lifetime, generation}); },
        ddv::noop);
    if (!result) {
        throw std::logic_error{"the Python input has an unsupported time-series type"};
    }
    return std::move(*result);
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
    return nb::cast(SetValueView{input(), conversions(), lifetime(), generation(), SetSelection::current});
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
    return nb::cast(SetIterator{input(), conversions(), lifetime(), generation(), SetSelection::current});
}

nb::object TSSInput::added() const {
    check_lifetime();
    return nb::cast(SetValueView{input(), conversions(), lifetime(), generation(), SetSelection::added});
}

nb::object TSSInput::removed() const {
    check_lifetime();
    return nb::cast(SetValueView{input(), conversions(), lifetime(), generation(), SetSelection::removed});
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
    return nb::cast(ListValueView{input(), conversions(), lifetime(), generation()});
}

nb::object TSLInput::delta_value() const {
    check_lifetime();
    return modified() ? nb::cast(ListDeltaView{input(), conversions(), lifetime(), generation(),
                                               FixedSelection::modified, EntryValue::delta})
                      : nb::none();
}

nb::object TSLInput::iter() const {
    check_lifetime();
    return nb::cast(FixedIterator{input(), conversions(), lifetime(), generation(), FixedSelection::all,
                                  EntryValue::input, EntryProjection::value});
}

nb::object TSLInput::keys() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::all,
                                 EntryValue::input, EntryProjection::key});
}

nb::object TSLInput::values() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::all,
                                 EntryValue::input, EntryProjection::value});
}

nb::object TSLInput::items() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::all,
                                 EntryValue::input, EntryProjection::item});
}

nb::object TSLInput::modified_keys() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::modified,
                                 EntryValue::input, EntryProjection::key});
}

nb::object TSLInput::modified_values() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::modified,
                                 EntryValue::input, EntryProjection::value});
}

nb::object TSLInput::modified_items() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::modified,
                                 EntryValue::input, EntryProjection::item});
}

nb::object TSLInput::valid_keys() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::valid,
                                 EntryValue::input, EntryProjection::key});
}

nb::object TSLInput::valid_values() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::valid,
                                 EntryValue::input, EntryProjection::value});
}

nb::object TSLInput::valid_items() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::valid,
                                 EntryValue::input, EntryProjection::item});
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
    return nb::cast(
        BundleValueView{input(), conversions(), lifetime(), generation(), FixedSelection::all, EntryValue::current});
}

nb::object TSBInput::delta_value() const {
    check_lifetime();
    return modified() ? nb::cast(BundleValueView{input(), conversions(), lifetime(), generation(),
                                                 FixedSelection::modified, EntryValue::delta})
                      : nb::none();
}

nb::object TSBInput::keys() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::all,
                                 EntryValue::input, EntryProjection::key});
}

nb::object TSBInput::values() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::all,
                                 EntryValue::input, EntryProjection::value});
}

nb::object TSBInput::items() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::all,
                                 EntryValue::input, EntryProjection::item});
}

nb::object TSBInput::modified_keys() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::modified,
                                 EntryValue::input, EntryProjection::key});
}

nb::object TSBInput::modified_values() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::modified,
                                 EntryValue::input, EntryProjection::value});
}

nb::object TSBInput::modified_items() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::modified,
                                 EntryValue::input, EntryProjection::item});
}

nb::object TSBInput::valid_keys() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::valid,
                                 EntryValue::input, EntryProjection::key});
}

nb::object TSBInput::valid_values() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::valid,
                                 EntryValue::input, EntryProjection::value});
}

nb::object TSBInput::valid_items() const {
    check_lifetime();
    return nb::cast(FixedEntries{input(), conversions(), lifetime(), generation(), FixedSelection::valid,
                                 EntryValue::input, EntryProjection::item});
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
    return nb::cast(DictionaryValueView{input(), conversions(), lifetime(), generation(), DictionarySelection::current,
                                        EntryValue::current});
}

nb::object TSDInput::delta_value() const {
    check_lifetime();
    return modified() ? nb::cast(DictionaryValueView{input(), conversions(), lifetime(), generation(),
                                                     DictionarySelection::changes, EntryValue::delta})
                      : nb::none();
}

nb::object TSDInput::iter() const {
    check_lifetime();
    return nb::cast(DictionaryIterator{input(), conversions(), lifetime(), generation(), DictionarySelection::current,
                                       EntryValue::input, EntryProjection::key});
}

nb::object TSDInput::key_set() const {
    check_lifetime();
    const auto keys = as<In<TSD<SCALAR, TS_T>>>().key_set();
    return make_input_view(In<TS_T>{keys.last_updated(), keys.data(), keys.runtime_type(), keys.modified()},
                           conversions(), lifetime(), generation());
}

nb::object TSDInput::keys() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::current,
                                      EntryValue::input, EntryProjection::key});
}

nb::object TSDInput::values() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::current,
                                      EntryValue::input, EntryProjection::value});
}

nb::object TSDInput::items() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::current,
                                      EntryValue::input, EntryProjection::item});
}

nb::object TSDInput::added_keys() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::added,
                                      EntryValue::input, EntryProjection::key});
}

nb::object TSDInput::added_values() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::added,
                                      EntryValue::input, EntryProjection::value});
}

nb::object TSDInput::added_items() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::added,
                                      EntryValue::input, EntryProjection::item});
}

nb::object TSDInput::removed_keys() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::removed,
                                      EntryValue::input, EntryProjection::key});
}

nb::object TSDInput::removed_values() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::removed,
                                      EntryValue::input, EntryProjection::value});
}

nb::object TSDInput::removed_items() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::removed,
                                      EntryValue::input, EntryProjection::item});
}

nb::object TSDInput::modified_keys() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::modified,
                                      EntryValue::input, EntryProjection::key});
}

nb::object TSDInput::modified_values() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::modified,
                                      EntryValue::input, EntryProjection::value});
}

nb::object TSDInput::modified_items() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::modified,
                                      EntryValue::input, EntryProjection::item});
}

nb::object TSDInput::valid_keys() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::valid,
                                      EntryValue::input, EntryProjection::key});
}

nb::object TSDInput::valid_values() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::valid,
                                      EntryValue::input, EntryProjection::value});
}

nb::object TSDInput::valid_items() const {
    check_lifetime();
    return nb::cast(DictionaryEntries{input(), conversions(), lifetime(), generation(), DictionarySelection::valid,
                                      EntryValue::input, EntryProjection::item});
}

void bind_input_iterators(nb::module_ &module) {
    nb::class_<SetIterator>(module, "_SetIterator")
        .def("__iter__", &SetIterator::iter, nb::rv_policy::reference)
        .def("__next__", static_cast<nb::object (SetIterator::*)()>(&SetIterator::next));
    nb::class_<FixedIterator>(module, "_FixedIterator")
        .def("__iter__", &FixedIterator::iter, nb::rv_policy::reference)
        .def("__next__", static_cast<nb::object (FixedIterator::*)()>(&FixedIterator::next));
    nb::class_<DictionaryIterator>(module, "_DictionaryIterator")
        .def("__iter__", &DictionaryIterator::iter, nb::rv_policy::reference)
        .def("__next__", static_cast<nb::object (DictionaryIterator::*)()>(&DictionaryIterator::next));

    nb::class_<FixedEntries>(module, "_FixedEntries").def("__iter__", &FixedEntries::iter);
    nb::class_<DictionaryEntries>(module, "_DictionaryEntries").def("__iter__", &DictionaryEntries::iter);

    nb::class_<SetValueView>(module, "_SetValueView")
        .def("__len__", &SetValueView::size)
        .def("__contains__", &SetValueView::contains)
        .def("__iter__", &SetValueView::iter)
        .def("copy", &SetValueView::copy);
    nb::class_<ListValueView>(module, "_ListValueView")
        .def("__len__", &ListValueView::size)
        .def("__getitem__", &ListValueView::getitem)
        .def("__iter__", &ListValueView::iter)
        .def("copy", &ListValueView::copy);

    nb::class_<FixedMappingView>(module, "_FixedMappingView")
        .def("__len__", &FixedMappingView::size)
        .def("__getitem__", &FixedMappingView::getitem)
        .def("__iter__", &FixedMappingView::iter)
        .def("get", &FixedMappingView::get, "key"_a, "default"_a = nb::none())
        .def("keys", &FixedMappingView::keys)
        .def("values", &FixedMappingView::values)
        .def("items", &FixedMappingView::items)
        .def("copy", &FixedMappingView::copy);
    nb::class_<ListDeltaView, FixedMappingView>(module, "_ChangeValueView");
    nb::class_<BundleValueView, FixedMappingView>(module, "_BundleValueView")
        .def("__getattr__", &BundleValueView::getattr);

    nb::class_<DictionaryValueView>(module, "_DictionaryValueView")
        .def("__len__", &DictionaryValueView::size)
        .def("__getitem__", &DictionaryValueView::getitem)
        .def("__contains__", &DictionaryValueView::contains)
        .def("__iter__", &DictionaryValueView::iter)
        .def("get", &DictionaryValueView::get, "key"_a, "default"_a = nb::none())
        .def("keys", &DictionaryValueView::keys)
        .def("values", &DictionaryValueView::values)
        .def("items", &DictionaryValueView::items)
        .def("copy", &DictionaryValueView::copy);

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
