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

nb::object make_value(In<TS_T> input, const PythonScalarConversions &conversions,
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

struct SetEntry {
    nb::object key;
};

struct TimeSeriesEntry {
    nb::object key;
    In<TS_T> value;
    bool removed{};
};

struct LiveSetSelection {
    template <typename Set> static item_id_t next(const Set &set, item_id_t start) noexcept {
        return set.next_item_id(start);
    }

    template <typename Set> static std::size_t size(const Set &set) noexcept { return set.size(); }

    template <typename Set, typename Item> static bool contains(const Set &set, const Item &item) {
        return set.contains(item);
    }
};

struct AddedSetSelection {
    template <typename Set> static item_id_t next(const Set &set, item_id_t start) noexcept {
        return set.next_added_id(start);
    }

    template <typename Set> static std::size_t size(const Set &set) noexcept {
        auto count = std::size_t{};
        for (auto id = next(set, 0); id != util::occupancy_index::npos; id = next(set, id + 1)) {
            ++count;
        }
        return count;
    }

    template <typename Set, typename Item> static bool contains(const Set &set, const Item &item) {
        return set.was_added(item);
    }
};

struct RemovedSetSelection {
    template <typename Set> static item_id_t next(const Set &set, item_id_t start) noexcept {
        return set.next_removed_id(start);
    }

    template <typename Set> static std::size_t size(const Set &set) noexcept {
        auto count = std::size_t{};
        for (auto id = next(set, 0); id != util::occupancy_index::npos; id = next(set, id + 1)) {
            ++count;
        }
        return count;
    }

    template <typename Set, typename Item> static bool contains(const Set &set, const Item &item) {
        return set.was_removed(item);
    }
};

template <typename Selection> class SetCursor {
  public:
    SetEntry next(In<TS_T> input, const PythonScalarConversions &conversions) {
        const auto result = input.visit(
            [&](const In<TSS<SCALAR>> *set) {
                const auto id = Selection::next(*set, cursor_);
                if (id == util::occupancy_index::npos) {
                    throw nb::stop_iteration();
                }

                cursor_ = id + 1;
                const auto item = set->item_at(id);
                return SetEntry{.key = conversions.to_python(Scalar<>::from(item))};
            },
            ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input is not a time series set"};
        }
        return std::move(*result);
    }

  private:
    item_id_t cursor_{};
};

struct AllFixedSelection {
    static bool includes(bool, bool) noexcept { return true; }

    template <typename Collection> static std::size_t size(const Collection &collection) noexcept {
        return collection.size();
    }
};

struct ModifiedFixedSelection {
    static bool includes(bool modified, bool) noexcept { return modified; }

    template <typename Collection> static std::size_t size(const Collection &collection) {
        auto count = std::size_t{};
        for (auto index = item_id_t{}; index != collection.size(); ++index) {
            count += collection.item_updated(index);
        }
        return count;
    }
};

struct ValidFixedSelection {
    static bool includes(bool, bool valid) noexcept { return valid; }

    template <typename Collection> static std::size_t size(const Collection &collection) {
        auto count = std::size_t{};
        for (auto index = item_id_t{}; index != collection.size(); ++index) {
            count += collection[index].valid();
        }
        return count;
    }
};

template <typename Selection> class FixedCursor {
  public:
    TimeSeriesEntry next(In<TS_T> input, const PythonScalarConversions &) {
        const auto result = input.visit([&](const In<TSL<TS_T, SIZE>> *list) { return next(*list); },
                                        [&](const In<TSB<SCHEMA>> *bundle) { return next(*bundle); }, ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input is not a fixed collection"};
        }
        return std::move(*result);
    }

  private:
    template <typename Item, auto Size, auto... Properties>
    TimeSeriesEntry next(const In<TSL<Item, Size, Properties...>> &list) {
        while (cursor_ < list.size()) {
            const auto index = cursor_++;
            const auto value = list[index];
            if (Selection::includes(list.item_updated(index), value.valid())) {
                return {.key = nb::int_(index), .value = value};
            }
        }
        throw nb::stop_iteration();
    }

    template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
    TimeSeriesEntry next(const In<TSB<Schema, Properties...>> &bundle) {
        while (cursor_ < bundle.size()) {
            const auto index = cursor_++;
            const auto value = bundle[index];
            if (Selection::includes(bundle.item_updated(index), value.valid())) {
                return {.key = nb::cast(std::string{bundle.item_name(index).text()}), .value = value};
            }
        }
        throw nb::stop_iteration();
    }

    item_id_t cursor_{};
};

struct LiveDictionarySelection {
    template <typename Dictionary> item_id_t next(const Dictionary &dictionary) {
        const auto id = dictionary.next_item_id(cursor_);
        if (id != util::occupancy_index::npos) {
            cursor_ = id + 1;
        }
        return id;
    }

    bool removed() const noexcept { return false; }

    template <typename Dictionary, typename Key>
    static std::pair<item_id_t, bool> find(const Dictionary &dictionary, const Key &key) {
        return {dictionary.find_item_id(key), false};
    }

    template <typename Dictionary> static std::size_t size(const Dictionary &dictionary) noexcept {
        return dictionary.size();
    }

  private:
    item_id_t cursor_{};
};

struct AddedDictionarySelection {
    template <typename Dictionary> item_id_t next(const Dictionary &dictionary) {
        const auto id = dictionary.next_added_id(cursor_);
        if (id != util::occupancy_index::npos) {
            cursor_ = id + 1;
        }
        return id;
    }

    bool removed() const noexcept { return false; }

    template <typename Dictionary, typename Key>
    static std::pair<item_id_t, bool> find(const Dictionary &dictionary, const Key &key) {
        return {dictionary.find_added_id(key), false};
    }

    template <typename Dictionary> static std::size_t size(const Dictionary &dictionary) {
        auto selection = AddedDictionarySelection{};
        auto count = std::size_t{};
        while (selection.next(dictionary) != util::occupancy_index::npos) {
            ++count;
        }
        return count;
    }

  private:
    item_id_t cursor_{};
};

struct RemovedDictionarySelection {
    template <typename Dictionary> item_id_t next(const Dictionary &dictionary) {
        const auto id = dictionary.next_removed_id(cursor_);
        if (id != util::occupancy_index::npos) {
            cursor_ = id + 1;
        }
        return id;
    }

    bool removed() const noexcept { return true; }

    template <typename Dictionary, typename Key>
    static std::pair<item_id_t, bool> find(const Dictionary &dictionary, const Key &key) {
        return {dictionary.find_removed_id(key), true};
    }

    template <typename Dictionary> static std::size_t size(const Dictionary &dictionary) {
        auto selection = RemovedDictionarySelection{};
        auto count = std::size_t{};
        while (selection.next(dictionary) != util::occupancy_index::npos) {
            ++count;
        }
        return count;
    }

  private:
    item_id_t cursor_{};
};

struct ModifiedDictionarySelection {
    template <typename Dictionary> item_id_t next(const Dictionary &dictionary) {
        const auto id = dictionary.next_modified_id(cursor_);
        if (id != util::occupancy_index::npos) {
            cursor_ = id + 1;
        }
        return id;
    }

    bool removed() const noexcept { return false; }

    template <typename Dictionary, typename Key>
    static std::pair<item_id_t, bool> find(const Dictionary &dictionary, const Key &key) {
        return {dictionary.find_modified_id(key), false};
    }

    template <typename Dictionary> static std::size_t size(const Dictionary &dictionary) {
        auto selection = ModifiedDictionarySelection{};
        auto count = std::size_t{};
        while (selection.next(dictionary) != util::occupancy_index::npos) {
            ++count;
        }
        return count;
    }

  private:
    item_id_t cursor_{};
};

struct ValidDictionarySelection {
    template <typename Dictionary> item_id_t next(const Dictionary &dictionary) {
        const auto id = dictionary.next_valid_id(cursor_);
        if (id != util::occupancy_index::npos) {
            cursor_ = id + 1;
        }
        return id;
    }

    bool removed() const noexcept { return false; }

    template <typename Dictionary, typename Key>
    static std::pair<item_id_t, bool> find(const Dictionary &dictionary, const Key &key) {
        const auto id = dictionary.find_item_id(key);
        if (id == util::occupancy_index::npos || !dictionary.item_at(id).valid()) {
            return {util::occupancy_index::npos, false};
        }
        return {id, false};
    }

    template <typename Dictionary> static std::size_t size(const Dictionary &dictionary) {
        auto selection = ValidDictionarySelection{};
        auto count = std::size_t{};
        while (selection.next(dictionary) != util::occupancy_index::npos) {
            ++count;
        }
        return count;
    }

  private:
    item_id_t cursor_{};
};

struct ChangedDictionarySelection {
    template <typename Dictionary> item_id_t next(const Dictionary &dictionary) {
        while (true) {
            auto id = util::occupancy_index::npos;
            if (phase_ == 0) {
                id = dictionary.next_added_id(cursor_);
            } else if (phase_ == 1) {
                id = dictionary.next_modified_id(cursor_);
            } else {
                id = dictionary.next_removed_id(cursor_);
            }
            if (id != util::occupancy_index::npos) {
                cursor_ = id + 1;
                return id;
            }
            if (phase_ == 2) {
                return id;
            }

            ++phase_;
            cursor_ = 0;
        }
    }

    bool removed() const noexcept { return phase_ == 2; }

    template <typename Dictionary, typename Key>
    static std::pair<item_id_t, bool> find(const Dictionary &dictionary, const Key &key) {
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

    template <typename Dictionary> static std::size_t size(const Dictionary &dictionary) {
        auto selection = ChangedDictionarySelection{};
        auto count = std::size_t{};
        while (selection.next(dictionary) != util::occupancy_index::npos) {
            ++count;
        }
        return count;
    }

  private:
    item_id_t cursor_{};
    unsigned phase_{};
};

template <typename Selection> class DictionaryCursor {
  public:
    TimeSeriesEntry next(In<TS_T> input, const PythonScalarConversions &conversions) {
        const auto result = input.visit(
            [&](const In<TSD<SCALAR, TS_T>> *dictionary) { return next(*dictionary, conversions); }, ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input is not a time series dictionary"};
        }
        return std::move(*result);
    }

  private:
    template <typename Key, typename Item, auto... Properties>
    TimeSeriesEntry next(const In<TSD<Key, Item, Properties...>> &dictionary,
                         const PythonScalarConversions &conversions) {
        const auto id = selection_.next(dictionary);
        if (id == util::occupancy_index::npos) {
            throw nb::stop_iteration();
        }

        const auto native_key = dictionary.key_at(id);
        return {.key = conversions.to_python(Scalar<>::from(native_key)),
                .value = dictionary.item_at(id),
                .removed = selection_.removed()};
    }

    Selection selection_;
};

struct SetEntryValue {
    static nb::object get(SetEntry &entry, const PythonScalarConversions &, const std::shared_ptr<CallbackLifetime> &,
                          std::uint64_t) {
        return entry.key;
    }
};

struct InputEntryValue {
    static nb::object get(TimeSeriesEntry &entry, const PythonScalarConversions &conversions,
                          const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
        return make_input_view(entry.value, conversions, lifetime, generation);
    }
};

struct ValueEntryValue {
    static nb::object get(TimeSeriesEntry &entry, const PythonScalarConversions &conversions,
                          const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
        return make_value(entry.value, conversions, lifetime, generation);
    }
};

struct DeltaEntryValue {
    static nb::object get(TimeSeriesEntry &entry, const PythonScalarConversions &conversions,
                          const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
        return entry.removed ? nb::borrow<nb::object>(details::remove_sentinel())
                             : make_delta_value(entry.value, conversions, lifetime, generation);
    }
};

struct KeyProjection {
    template <typename EntryValue, typename Entry>
    static nb::object project(Entry &entry, const PythonScalarConversions &, const std::shared_ptr<CallbackLifetime> &,
                              std::uint64_t) {
        return std::move(entry.key);
    }
};

struct ValueProjection {
    template <typename EntryValue, typename Entry>
    static nb::object project(Entry &entry, const PythonScalarConversions &conversions,
                              const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
        return EntryValue::get(entry, conversions, lifetime, generation);
    }
};

struct ItemProjection {
    template <typename EntryValue, typename Entry>
    static nb::object project(Entry &entry, const PythonScalarConversions &conversions,
                              const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
        auto value = EntryValue::get(entry, conversions, lifetime, generation);
        return nb::make_tuple(std::move(entry.key), value);
    }
};

template <typename Cursor, typename EntryValue, typename Projection>
class CollectionIterator final : public CollectionViewState {
  public:
    CollectionIterator(In<TS_T> input, const PythonScalarConversions &conversions,
                       std::shared_ptr<CallbackLifetime> lifetime, std::uint64_t generation) noexcept
        : CollectionViewState{input, conversions, std::move(lifetime), generation} {}

    CollectionIterator &iter() noexcept { return *this; }

    nb::object next() {
        check_lifetime();
        auto entry = cursor_.next(input(), conversions());
        return Projection::template project<EntryValue>(entry, conversions(), lifetime(), generation());
    }

  private:
    Cursor cursor_;
};

template <typename Iterator> class CollectionEntries final : public CollectionViewState {
  public:
    using CollectionViewState::CollectionViewState;

    nb::object iter() const {
        check_lifetime();
        return nb::cast(Iterator{input(), conversions(), lifetime(), generation()});
    }
};

template <typename Selection>
using SetIterator = CollectionIterator<SetCursor<Selection>, SetEntryValue, ValueProjection>;

template <typename Selection, typename EntryValue, typename Projection>
using FixedIterator = CollectionIterator<FixedCursor<Selection>, EntryValue, Projection>;

template <typename Selection, typename EntryValue, typename Projection>
using DictionaryIterator = CollectionIterator<DictionaryCursor<Selection>, EntryValue, Projection>;

template <typename Selection, typename EntryValue, typename Projection>
using FixedEntries = CollectionEntries<FixedIterator<Selection, EntryValue, Projection>>;

template <typename Selection, typename EntryValue, typename Projection>
using DictionaryEntries = CollectionEntries<DictionaryIterator<Selection, EntryValue, Projection>>;

template <typename Selection> class SetValueView final : public CollectionViewState {
  public:
    using CollectionViewState::CollectionViewState;

    std::size_t size() const {
        check_lifetime();
        const auto result = input().visit([](const In<TSS<SCALAR>> *set) { return Selection::size(*set); }, ddv::noop);
        return result.value_or(0);
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
                return Selection::contains(*set, item);
            },
            ddv::noop);
        return result.value_or(false);
    }

    nb::object iter() const {
        check_lifetime();
        return nb::cast(SetIterator<Selection>{input(), conversions(), lifetime(), generation()});
    }

    nb::object copy() const {
        check_lifetime();
        return nb::module_::import_("builtins").attr("set")(iter());
    }
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
                return make_value((*list)[index], conversions(), lifetime(), generation());
            },
            ddv::noop);
        if (!result) {
            throw std::logic_error{"the Python input is not a time series list"};
        }
        return std::move(*result);
    }

    nb::object iter() const {
        check_lifetime();
        return nb::cast(FixedIterator<AllFixedSelection, ValueEntryValue, ValueProjection>{input(), conversions(),
                                                                                           lifetime(), generation()});
    }

    nb::object copy() const {
        check_lifetime();
        return nb::module_::import_("builtins").attr("list")(iter());
    }
};

template <typename Selection, typename EntryValue> class FixedMappingView final : public CollectionViewState {
  public:
    using CollectionViewState::CollectionViewState;

    std::size_t size() const {
        check_lifetime();
        const auto result =
            input().visit([](const In<TSL<TS_T, SIZE>> *list) { return Selection::size(*list); },
                          [](const In<TSB<SCHEMA>> *bundle) { return Selection::size(*bundle); }, ddv::noop);
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
        return nb::cast(
            FixedIterator<Selection, EntryValue, KeyProjection>{input(), conversions(), lifetime(), generation()});
    }

    nb::object keys() const { return entries<KeyProjection>(); }
    nb::object values() const { return entries<ValueProjection>(); }
    nb::object items() const { return entries<ItemProjection>(); }

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
        return nb::module_::import_("builtins").attr("dict")(entries<ItemProjection>());
    }

  private:
    nb::object selected_value(In<TS_T> value, bool modified) const {
        if (!Selection::includes(modified, value.valid())) {
            throw nb::key_error();
        }

        auto entry = TimeSeriesEntry{.value = value};
        return EntryValue::get(entry, conversions(), lifetime(), generation());
    }

    template <typename Projection> nb::object entries() const {
        check_lifetime();
        return nb::cast(
            FixedEntries<Selection, EntryValue, Projection>{input(), conversions(), lifetime(), generation()});
    }
};

using FixedDeltaView = FixedMappingView<ModifiedFixedSelection, DeltaEntryValue>;
using BundleValueView = FixedMappingView<AllFixedSelection, ValueEntryValue>;

template <typename Selection, typename EntryValue> class DictionaryMappingView final : public CollectionViewState {
  public:
    using CollectionViewState::CollectionViewState;

    std::size_t size() const {
        check_lifetime();
        const auto result = input().visit(
            [](const In<TSD<SCALAR, TS_T>> *dictionary) { return Selection::size(*dictionary); }, ddv::noop);
        return result.value_or(0);
    }

    nb::object getitem(nb::handle key) const {
        check_lifetime();
        const auto result = input().visit(
            [&](const In<TSD<SCALAR, TS_T>> *dictionary) -> nb::object {
                const auto &type = type_cast<TimeSeriesDictionaryType>(dictionary->runtime_type().type());
                auto memory = std::pmr::unsynchronized_pool_resource{};
                auto allocation = allocator{memory};
                const auto scalar = conversions().from_python(type.key_type(), key, allocation);
                const auto [id, removed] = Selection::find(*dictionary, Scalar<SCALAR>::from(scalar));
                if (id == util::occupancy_index::npos) {
                    PyErr_SetObject(PyExc_KeyError, key.ptr());
                    return {};
                }

                auto entry = TimeSeriesEntry{.value = dictionary->item_at(id), .removed = removed};
                return EntryValue::get(entry, conversions(), lifetime(), generation());
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
                return Selection::find(*dictionary, Scalar<SCALAR>::from(scalar)).first != util::occupancy_index::npos;
            },
            ddv::noop);
        return result.value_or(false);
    }

    nb::object iter() const {
        check_lifetime();
        return nb::cast(
            DictionaryIterator<Selection, EntryValue, KeyProjection>{input(), conversions(), lifetime(), generation()});
    }

    nb::object keys() const { return entries<KeyProjection>(); }
    nb::object values() const { return entries<ValueProjection>(); }
    nb::object items() const { return entries<ItemProjection>(); }

    nb::object copy() const {
        check_lifetime();
        return nb::module_::import_("builtins").attr("dict")(entries<ItemProjection>());
    }

  private:
    template <typename Projection> nb::object entries() const {
        check_lifetime();
        return nb::cast(
            DictionaryEntries<Selection, EntryValue, Projection>{input(), conversions(), lifetime(), generation()});
    }
};

using DictionaryValueView = DictionaryMappingView<LiveDictionarySelection, ValueEntryValue>;
using DictionaryDeltaView = DictionaryMappingView<ChangedDictionarySelection, DeltaEntryValue>;

class SetDeltaView final : public CollectionViewState {
  public:
    using CollectionViewState::CollectionViewState;

    nb::object added() const {
        check_lifetime();
        return nb::cast(SetValueView<AddedSetSelection>{input(), conversions(), lifetime(), generation()});
    }

    nb::object removed() const {
        check_lifetime();
        return nb::cast(SetValueView<RemovedSetSelection>{input(), conversions(), lifetime(), generation()});
    }
};

nb::object make_value(In<TS_T> input, const PythonScalarConversions &conversions,
                      const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) {
    if (!input.valid()) {
        return nb::none();
    }

    const auto result = input.visit(
        [&](const In<TS<SCALAR>> *value) { return scalar_value(*value, conversions); },
        [&](const In<TSS<SCALAR>> *) {
            return nb::cast(SetValueView<LiveSetSelection>{input, conversions, lifetime, generation});
        },
        [&](const In<TSL<TS_T, SIZE>> *) { return nb::cast(ListValueView{input, conversions, lifetime, generation}); },
        [&](const In<TSB<SCHEMA>> *) { return nb::cast(BundleValueView{input, conversions, lifetime, generation}); },
        [&](const In<TSD<SCALAR, TS_T>> *) {
            return nb::cast(DictionaryValueView{input, conversions, lifetime, generation});
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
        [&](const In<TSL<TS_T, SIZE>> *) { return nb::cast(FixedDeltaView{input, conversions, lifetime, generation}); },
        [&](const In<TSB<SCHEMA>> *) { return nb::cast(FixedDeltaView{input, conversions, lifetime, generation}); },
        [&](const In<TSD<SCALAR, TS_T>> *) {
            return nb::cast(DictionaryDeltaView{input, conversions, lifetime, generation});
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
    return nb::cast(SetValueView<LiveSetSelection>{input(), conversions(), lifetime(), generation()});
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
    return nb::cast(SetIterator<LiveSetSelection>{input(), conversions(), lifetime(), generation()});
}

nb::object TSSInput::added() const {
    check_lifetime();
    return nb::cast(SetValueView<AddedSetSelection>{input(), conversions(), lifetime(), generation()});
}

nb::object TSSInput::removed() const {
    check_lifetime();
    return nb::cast(SetValueView<RemovedSetSelection>{input(), conversions(), lifetime(), generation()});
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
    return modified() ? nb::cast(FixedDeltaView{input(), conversions(), lifetime(), generation()}) : nb::none();
}

nb::object TSLInput::iter() const {
    check_lifetime();
    return nb::cast(FixedIterator<AllFixedSelection, InputEntryValue, ValueProjection>{input(), conversions(),
                                                                                       lifetime(), generation()});
}

nb::object TSLInput::keys() const {
    check_lifetime();
    return nb::cast(FixedEntries<AllFixedSelection, InputEntryValue, KeyProjection>{input(), conversions(), lifetime(),
                                                                                    generation()});
}

nb::object TSLInput::values() const {
    check_lifetime();
    return nb::cast(FixedEntries<AllFixedSelection, InputEntryValue, ValueProjection>{input(), conversions(),
                                                                                      lifetime(), generation()});
}

nb::object TSLInput::items() const {
    check_lifetime();
    return nb::cast(FixedEntries<AllFixedSelection, InputEntryValue, ItemProjection>{input(), conversions(), lifetime(),
                                                                                     generation()});
}

nb::object TSLInput::modified_keys() const {
    check_lifetime();
    return nb::cast(FixedEntries<ModifiedFixedSelection, InputEntryValue, KeyProjection>{input(), conversions(),
                                                                                         lifetime(), generation()});
}

nb::object TSLInput::modified_values() const {
    check_lifetime();
    return nb::cast(FixedEntries<ModifiedFixedSelection, InputEntryValue, ValueProjection>{input(), conversions(),
                                                                                           lifetime(), generation()});
}

nb::object TSLInput::modified_items() const {
    check_lifetime();
    return nb::cast(FixedEntries<ModifiedFixedSelection, InputEntryValue, ItemProjection>{input(), conversions(),
                                                                                          lifetime(), generation()});
}

nb::object TSLInput::valid_keys() const {
    check_lifetime();
    return nb::cast(FixedEntries<ValidFixedSelection, InputEntryValue, KeyProjection>{input(), conversions(),
                                                                                      lifetime(), generation()});
}

nb::object TSLInput::valid_values() const {
    check_lifetime();
    return nb::cast(FixedEntries<ValidFixedSelection, InputEntryValue, ValueProjection>{input(), conversions(),
                                                                                        lifetime(), generation()});
}

nb::object TSLInput::valid_items() const {
    check_lifetime();
    return nb::cast(FixedEntries<ValidFixedSelection, InputEntryValue, ItemProjection>{input(), conversions(),
                                                                                       lifetime(), generation()});
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
    return nb::cast(BundleValueView{input(), conversions(), lifetime(), generation()});
}

nb::object TSBInput::delta_value() const {
    check_lifetime();
    return modified() ? nb::cast(FixedDeltaView{input(), conversions(), lifetime(), generation()}) : nb::none();
}

nb::object TSBInput::keys() const {
    check_lifetime();
    return nb::cast(FixedEntries<AllFixedSelection, InputEntryValue, KeyProjection>{input(), conversions(), lifetime(),
                                                                                    generation()});
}

nb::object TSBInput::values() const {
    check_lifetime();
    return nb::cast(FixedEntries<AllFixedSelection, InputEntryValue, ValueProjection>{input(), conversions(),
                                                                                      lifetime(), generation()});
}

nb::object TSBInput::items() const {
    check_lifetime();
    return nb::cast(FixedEntries<AllFixedSelection, InputEntryValue, ItemProjection>{input(), conversions(), lifetime(),
                                                                                     generation()});
}

nb::object TSBInput::modified_keys() const {
    check_lifetime();
    return nb::cast(FixedEntries<ModifiedFixedSelection, InputEntryValue, KeyProjection>{input(), conversions(),
                                                                                         lifetime(), generation()});
}

nb::object TSBInput::modified_values() const {
    check_lifetime();
    return nb::cast(FixedEntries<ModifiedFixedSelection, InputEntryValue, ValueProjection>{input(), conversions(),
                                                                                           lifetime(), generation()});
}

nb::object TSBInput::modified_items() const {
    check_lifetime();
    return nb::cast(FixedEntries<ModifiedFixedSelection, InputEntryValue, ItemProjection>{input(), conversions(),
                                                                                          lifetime(), generation()});
}

nb::object TSBInput::valid_keys() const {
    check_lifetime();
    return nb::cast(FixedEntries<ValidFixedSelection, InputEntryValue, KeyProjection>{input(), conversions(),
                                                                                      lifetime(), generation()});
}

nb::object TSBInput::valid_values() const {
    check_lifetime();
    return nb::cast(FixedEntries<ValidFixedSelection, InputEntryValue, ValueProjection>{input(), conversions(),
                                                                                        lifetime(), generation()});
}

nb::object TSBInput::valid_items() const {
    check_lifetime();
    return nb::cast(FixedEntries<ValidFixedSelection, InputEntryValue, ItemProjection>{input(), conversions(),
                                                                                       lifetime(), generation()});
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
    return nb::cast(DictionaryValueView{input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::delta_value() const {
    check_lifetime();
    return modified() ? nb::cast(DictionaryDeltaView{input(), conversions(), lifetime(), generation()}) : nb::none();
}

nb::object TSDInput::iter() const {
    check_lifetime();
    return nb::cast(DictionaryIterator<LiveDictionarySelection, InputEntryValue, KeyProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::key_set() const {
    check_lifetime();
    const auto keys = as<In<TSD<SCALAR, TS_T>>>().key_set();
    return make_input_view(In<TS_T>{keys.last_updated(), keys.data(), keys.runtime_type(), keys.modified()},
                           conversions(), lifetime(), generation());
}

nb::object TSDInput::keys() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<LiveDictionarySelection, InputEntryValue, KeyProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::values() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<LiveDictionarySelection, InputEntryValue, ValueProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::items() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<LiveDictionarySelection, InputEntryValue, ItemProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::added_keys() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<AddedDictionarySelection, InputEntryValue, KeyProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::added_values() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<AddedDictionarySelection, InputEntryValue, ValueProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::added_items() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<AddedDictionarySelection, InputEntryValue, ItemProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::removed_keys() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<RemovedDictionarySelection, InputEntryValue, KeyProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::removed_values() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<RemovedDictionarySelection, InputEntryValue, ValueProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::removed_items() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<RemovedDictionarySelection, InputEntryValue, ItemProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::modified_keys() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<ModifiedDictionarySelection, InputEntryValue, KeyProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::modified_values() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<ModifiedDictionarySelection, InputEntryValue, ValueProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::modified_items() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<ModifiedDictionarySelection, InputEntryValue, ItemProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::valid_keys() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<ValidDictionarySelection, InputEntryValue, KeyProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::valid_values() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<ValidDictionarySelection, InputEntryValue, ValueProjection>{
        input(), conversions(), lifetime(), generation()});
}

nb::object TSDInput::valid_items() const {
    check_lifetime();
    return nb::cast(DictionaryEntries<ValidDictionarySelection, InputEntryValue, ItemProjection>{
        input(), conversions(), lifetime(), generation()});
}

namespace {

template <typename Iterator> void bind_iterator(nb::module_ &module, const char *name) {
    nb::class_<Iterator>(module, name)
        .def("__iter__", &Iterator::iter, nb::rv_policy::reference)
        .def("__next__", &Iterator::next);
}

template <typename Entries> void bind_entries(nb::module_ &module, const char *name) {
    nb::class_<Entries>(module, name).def("__iter__", &Entries::iter);
}

template <typename Selection, typename EntryValue> void bind_fixed_iteration(nb::module_ &module, const char *name) {
    const auto prefix = std::string{"_"} + name;
    bind_iterator<FixedIterator<Selection, EntryValue, KeyProjection>>(module, (prefix + "KeyIterator").c_str());
    bind_iterator<FixedIterator<Selection, EntryValue, ValueProjection>>(module, (prefix + "ValueIterator").c_str());
    bind_iterator<FixedIterator<Selection, EntryValue, ItemProjection>>(module, (prefix + "ItemIterator").c_str());

    bind_entries<FixedEntries<Selection, EntryValue, KeyProjection>>(module, (prefix + "KeyEntries").c_str());
    bind_entries<FixedEntries<Selection, EntryValue, ValueProjection>>(module, (prefix + "ValueEntries").c_str());
    bind_entries<FixedEntries<Selection, EntryValue, ItemProjection>>(module, (prefix + "ItemEntries").c_str());
}

template <typename Selection, typename EntryValue>
void bind_dictionary_iteration(nb::module_ &module, const char *name) {
    const auto prefix = std::string{"_"} + name;
    bind_iterator<DictionaryIterator<Selection, EntryValue, KeyProjection>>(module, (prefix + "KeyIterator").c_str());
    bind_iterator<DictionaryIterator<Selection, EntryValue, ValueProjection>>(module,
                                                                              (prefix + "ValueIterator").c_str());
    bind_iterator<DictionaryIterator<Selection, EntryValue, ItemProjection>>(module, (prefix + "ItemIterator").c_str());

    bind_entries<DictionaryEntries<Selection, EntryValue, KeyProjection>>(module, (prefix + "KeyEntries").c_str());
    bind_entries<DictionaryEntries<Selection, EntryValue, ValueProjection>>(module, (prefix + "ValueEntries").c_str());
    bind_entries<DictionaryEntries<Selection, EntryValue, ItemProjection>>(module, (prefix + "ItemEntries").c_str());
}

template <typename Selection> void bind_set_value(nb::module_ &module, const char *name) {
    using View = SetValueView<Selection>;
    nb::class_<View>(module, name)
        .def("__len__", &View::size)
        .def("__contains__", &View::contains)
        .def("__iter__", &View::iter)
        .def("copy", &View::copy);
}

template <typename View> nb::class_<View> bind_fixed_mapping(nb::module_ &module, const char *name) {
    auto binding = nb::class_<View>(module, name);
    binding.def("__len__", &View::size)
        .def("__getitem__", &View::getitem)
        .def("__iter__", &View::iter)
        .def("get", &View::get, "key"_a, "default"_a = nb::none())
        .def("keys", &View::keys)
        .def("values", &View::values)
        .def("items", &View::items)
        .def("copy", &View::copy);
    return binding;
}

template <typename View> void bind_dictionary_value(nb::module_ &module, const char *name) {
    nb::class_<View>(module, name)
        .def("__len__", &View::size)
        .def("__getitem__", &View::getitem)
        .def("__contains__", &View::contains)
        .def("__iter__", &View::iter)
        .def("get", &View::get, "key"_a, "default"_a = nb::none())
        .def("keys", &View::keys)
        .def("values", &View::values)
        .def("items", &View::items)
        .def("copy", &View::copy);
}

} // namespace

void bind_input_iterators(nb::module_ &module) {
    bind_iterator<SetIterator<LiveSetSelection>>(module, "_LiveSetIterator");
    bind_iterator<SetIterator<AddedSetSelection>>(module, "_AddedSetIterator");
    bind_iterator<SetIterator<RemovedSetSelection>>(module, "_RemovedSetIterator");

    bind_fixed_iteration<AllFixedSelection, ValueEntryValue>(module, "FixedValue");
    bind_fixed_iteration<ModifiedFixedSelection, DeltaEntryValue>(module, "FixedDelta");
    bind_fixed_iteration<AllFixedSelection, InputEntryValue>(module, "FixedInput");
    bind_fixed_iteration<ModifiedFixedSelection, InputEntryValue>(module, "ModifiedFixedInput");
    bind_fixed_iteration<ValidFixedSelection, InputEntryValue>(module, "ValidFixedInput");

    bind_dictionary_iteration<LiveDictionarySelection, ValueEntryValue>(module, "DictionaryValue");
    bind_dictionary_iteration<ChangedDictionarySelection, DeltaEntryValue>(module, "DictionaryDelta");
    bind_dictionary_iteration<LiveDictionarySelection, InputEntryValue>(module, "DictionaryInput");
    bind_dictionary_iteration<AddedDictionarySelection, InputEntryValue>(module, "AddedDictionaryInput");
    bind_dictionary_iteration<RemovedDictionarySelection, InputEntryValue>(module, "RemovedDictionaryInput");
    bind_dictionary_iteration<ModifiedDictionarySelection, InputEntryValue>(module, "ModifiedDictionaryInput");
    bind_dictionary_iteration<ValidDictionarySelection, InputEntryValue>(module, "ValidDictionaryInput");

    bind_set_value<LiveSetSelection>(module, "_SetValueView");
    bind_set_value<AddedSetSelection>(module, "_AddedSetValueView");
    bind_set_value<RemovedSetSelection>(module, "_RemovedSetValueView");

    nb::class_<ListValueView>(module, "_ListValueView")
        .def("__len__", &ListValueView::size)
        .def("__getitem__", &ListValueView::getitem)
        .def("__iter__", &ListValueView::iter)
        .def("copy", &ListValueView::copy);

    bind_fixed_mapping<FixedDeltaView>(module, "_FixedDeltaView");
    bind_fixed_mapping<BundleValueView>(module, "_BundleValueView").def("__getattr__", &BundleValueView::getattr);

    bind_dictionary_value<DictionaryValueView>(module, "_DictionaryValueView");
    bind_dictionary_value<DictionaryDeltaView>(module, "_DictionaryDeltaView");

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
