#include <bg/python/scalar_conversion.hpp>

#include "dict_view.hpp"

#include <bg/python/object.hpp>

#include <bg/scalar/builtins.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/bundle/value.hpp>
#include <bg/scalar/details/string_storage.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/list/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/struct/scalar_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/scope_exit.hpp>

#include <datetime.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <typeinfo>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bg::python {
namespace {

using details::StringStorage;
using details::StringStorageFinalReleaseHook;

class PythonObjectScalarType final : public ScalarType {
  public:
    VISITOR_SUPPORT()

    PythonObjectScalarType(std::vector<const ScalarType *> bases, bool hashable)
        : bases_{std::move(bases)}, hashable_{hashable} {}

    bool is_resolved() const noexcept override { return true; }
    const Type &resolve(const TypeMap &) const override { return *this; }
    bool supports_equality() const noexcept override { return true; }
    bool supports_hash() const noexcept override { return hashable_; }

    bool accepts_type(const ScalarType &supplied) const noexcept {
        if (&supplied == this) {
            return true;
        }
        if (typeid(supplied) != typeid(PythonObjectScalarType)) {
            return false;
        }

        const auto &derived = static_cast<const PythonObjectScalarType &>(supplied);
        return std::ranges::find(derived.bases_, this) != derived.bases_.end();
    }

  protected:
    bool do_match(const Type &supplied, TypeMap &) const override {
        const auto *scalar = type_cast<ScalarType>(&supplied);
        return scalar != nullptr && accepts_type(*scalar);
    }

  private:
    std::vector<const ScalarType *> bases_;
    bool hashable_;
};

// Use the process heap because copied scalar handles can outlive any one graph allocator.
class PythonObjectStorage {
  public:
    explicit PythonObjectStorage(PyObject *value) : value_{value} {
        auto gil = nanobind::gil_scoped_acquire{};
        Py_INCREF(value_);
    }

    PythonObjectStorage(const PythonObjectStorage &) = delete;
    PythonObjectStorage &operator=(const PythonObjectStorage &) = delete;

    PyObject *value() const noexcept { return value_; }

    void retain() noexcept {
        if (references_.fetch_add(1, std::memory_order_relaxed) == 0) {
            std::terminate();
        }
    }

    void release() noexcept {
        if (references_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
            return;
        }

        auto *value = value_;
        delete this;
        Object::release_reference(value);
    }

  private:
    std::atomic_size_t references_{1};
    PyObject *value_;
};

class PythonObjectScalarRuntimeType final : public ScalarRuntimeType {
  public:
    explicit PythonObjectScalarRuntimeType(const PythonObjectScalarType &type)
        : ScalarRuntimeType{type, sizeof(PythonObjectStorage *), alignof(PythonObjectStorage *), false, true, true,
                            shared_handle_flag},
          object_type_{type} {}

    bool accepts_input(const ScalarRuntimeType &source) const noexcept override {
        return object_type_.accepts_type(source.type());
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<PythonObjectStorage **>(destination), nullptr);
    }

    void construct_from_value(void *destination, const void *value, allocator &) const override {
        std::construct_at(static_cast<PythonObjectStorage **>(destination),
                          new PythonObjectStorage{*static_cast<PyObject *const *>(value)});
    }

    void copy_construct(void *destination, const void *source, allocator &) const override {
        auto *value = *static_cast<PythonObjectStorage *const *>(source);
        if (value != nullptr) {
            value->retain();
        }
        std::construct_at(static_cast<PythonObjectStorage **>(destination), value);
    }

    void do_construct_from_input(void *destination, const details::ScalarData &source,
                                 allocator &) const override {
        auto *value = storage(source.mode(), source.data_word());
        if (value != nullptr) {
            value->retain();
        }
        std::construct_at(static_cast<PythonObjectStorage **>(destination), value);
    }

    void do_assign_from_input(void *destination, const details::ScalarData &source) const override {
        auto *replacement = storage(source.mode(), source.data_word());
        if (replacement != nullptr) {
            replacement->retain();
        }
        auto *previous = std::exchange(*static_cast<PythonObjectStorage **>(destination), replacement);
        if (previous != nullptr) {
            previous->release();
        }
    }

    std::size_t hash(ScalarDataMode mode, std::uintptr_t data) const override {
        auto *value = storage(mode, data);
        if (value == nullptr) {
            throw std::logic_error{"cannot hash an empty Python scalar"};
        }

        auto gil = nanobind::gil_scoped_acquire{};
        const auto hash = PyObject_Hash(value->value());
        if (hash == -1 && PyErr_Occurred() != nullptr) {
            nanobind::raise_python_error();
        }
        return static_cast<std::size_t>(hash);
    }

    bool equals(ScalarDataMode mode, std::uintptr_t data, const details::ScalarData &other) const override {
        if (typeid(other.runtime_type()) != typeid(PythonObjectScalarRuntimeType)) {
            return false;
        }

        auto *left = storage(mode, data);
        auto *right = storage(other.mode(), other.data_word());
        if (left == nullptr || right == nullptr) {
            return left == right;
        }

        auto gil = nanobind::gil_scoped_acquire{};
        const auto equal = PyObject_RichCompareBool(left->value(), right->value(), Py_EQ);
        if (equal < 0) {
            nanobind::raise_python_error();
        }
        return equal != 0;
    }

    std::size_t hash(const void *value) const override {
        return hash(ScalarDataMode::stored_view, reinterpret_cast<std::uintptr_t>(value));
    }

    bool equals(const void *value, const details::ScalarData &other) const override {
        return equals(ScalarDataMode::stored_view, reinterpret_cast<std::uintptr_t>(value), other);
    }

    void destroy(void *value, allocator &) const noexcept override {
        auto *storage_value = std::exchange(*static_cast<PythonObjectStorage **>(value), nullptr);
        if (storage_value != nullptr) {
            storage_value->release();
        }
    }

    details::ScalarData borrow_shared(const void *storage) const noexcept override {
        const auto *value = static_cast<PythonObjectStorage *const *>(storage);
        return { *this, reinterpret_cast<std::uintptr_t>(*value), ScalarDataMode::shared_view };
    }

    const ScalarRuntimeType *retain_shared(const details::ScalarData &source,
                                           std::uintptr_t &data) const noexcept override {
        auto *value = storage(source.mode(), source.data_word());
        if (value == nullptr) {
            data = 0;
            return this;
        }
        value->retain();
        data = reinterpret_cast<std::uintptr_t>(value);
        return this;
    }

    void release_shared(std::uintptr_t data) const noexcept override {
        auto *value = reinterpret_cast<PythonObjectStorage *>(data);
        if (value != nullptr) {
            value->release();
        }
    }

    PyObject *python_object(const details::ScalarData &value) const noexcept {
        auto *storage_value = storage(value.mode(), value.data_word());
        return storage_value == nullptr ? nullptr : storage_value->value();
    }

  private:
    static PythonObjectStorage *storage(ScalarDataMode mode, std::uintptr_t data) noexcept {
        if (mode == ScalarDataMode::shared_view || mode == ScalarDataMode::shared_value) {
            return reinterpret_cast<PythonObjectStorage *>(data);
        }
        return *reinterpret_cast<PythonObjectStorage *const *>(data);
    }

    bool equals_same_type(const void *value, const details::ScalarData &other) const override {
        return equals(ScalarDataMode::stored_view, reinterpret_cast<std::uintptr_t>(value), other);
    }

    const PythonObjectScalarType &object_type_;
};

class PythonObjectRuntimeTypes {
  public:
    const PythonObjectScalarRuntimeType &get(const PythonObjectScalarType &type) {
        const auto lock = std::lock_guard{mutex_};
        if (const auto found = by_type_.find(&type); found != by_type_.end()) {
            return *found->second;
        }

        auto runtime_type = std::make_unique<PythonObjectScalarRuntimeType>(type);
        auto *result = runtime_type.get();
        runtime_types_.push_back(std::move(runtime_type));
        auto remove_runtime_type = util::scope_exit{[this]() noexcept { runtime_types_.pop_back(); }};

        const auto [entry, inserted] = by_type_.emplace(&type, result);
        if (!inserted) {
            throw std::logic_error{"a Python object runtime type was registered twice"};
        }
        auto remove_type_entry = util::scope_exit{[this, entry]() noexcept { by_type_.erase(entry); }};

        register_scalar_runtime_type(*result);
        remove_type_entry.release();
        remove_runtime_type.release();
        return *result;
    }

  private:
    std::mutex mutex_;
    std::unordered_map<const PythonObjectScalarType *, const PythonObjectScalarRuntimeType *> by_type_;
    std::vector<std::unique_ptr<PythonObjectScalarRuntimeType>> runtime_types_;
};

PythonObjectRuntimeTypes &python_object_runtime_types() {
    // Scalar runtime registration stores process-lifetime pointers to these runtime types.
    static auto *runtime_types = new PythonObjectRuntimeTypes{};
    return *runtime_types;
}

nanobind::object to_python_python_object(Scalar<> value, const PythonScalarConversions &) {
    const auto &runtime_type = static_cast<const PythonObjectScalarRuntimeType &>(value.runtime_type());
    auto *python_object = runtime_type.python_object(details::scalar_data(value));
    if (python_object == nullptr) {
        throw std::logic_error{"cannot convert an empty Python scalar"};
    }
    return nanobind::borrow<nanobind::object>(python_object);
}

template <typename T, typename Source> ScalarValue<> make_scalar(Source &&value, allocator &memory) {
    auto typed_value = ScalarValue<T>{std::forward<Source>(value), memory};
    return ScalarValue<>{typed_value, memory};
}

class StringIdentityCache {
  public:
    StringIdentityCache() : hook_{&StringIdentityCache::release_storage, this} {}

    StringStorage *retain_python_string(PyObject *python_string) {
        const std::lock_guard lock{mutex_};
        const auto found = by_python_string_.find(python_string);
        if (found == by_python_string_.end()) {
            return nullptr;
        }

        auto *storage = found->second;
        if (storage->try_retain()) {
            return storage;
        }

        by_python_string_.erase(found);
        return nullptr;
    }

    Object python_string(const StringStorage *storage) {
        const std::lock_guard lock{mutex_};
        const auto found = by_storage_.find(storage);
        return found == by_storage_.end() ? Object{} : found->second;
    }

    nanobind::object remember(StringStorage *storage, nanobind::handle python_string) {
        if (storage->install_final_release_hook(&hook_) != &hook_) {
            return {};
        }

        auto retained_python_string = Object{python_string};
        auto selected_python_string = Object{};
        {
            const std::lock_guard lock{mutex_};
            const auto [stored, inserted] = by_storage_.try_emplace(storage, std::move(retained_python_string));
            selected_python_string = stored->second;
            if (inserted) {
                by_python_string_.insert_or_assign(python_string.ptr(), storage);
            }
        }

        return nanobind::borrow<nanobind::object>(selected_python_string.get());
    }

  private:
    static void release_storage(void *context, const StringStorage *storage) noexcept {
        static_cast<StringIdentityCache *>(context)->release(storage);
    }

    void release(const StringStorage *storage) noexcept {
        auto released_python_string = Object{};
        {
            const std::lock_guard lock{mutex_};
            const auto found = by_storage_.find(storage);
            if (found == by_storage_.end()) {
                return;
            }

            auto *python_string = found->second.get().ptr();
            released_python_string = std::move(found->second);
            by_storage_.erase(found);

            const auto reverse = by_python_string_.find(python_string);
            if (reverse != by_python_string_.end() && reverse->second == storage) {
                by_python_string_.erase(reverse);
            }
        }
    }

    std::mutex mutex_;
    std::unordered_map<const StringStorage *, Object> by_storage_;
    std::unordered_map<PyObject *, StringStorage *> by_python_string_;
    StringStorageFinalReleaseHook hook_;
};

StringIdentityCache &string_identity_cache() {
    // StringStorage hooks may outlive PythonScalarConversions, so their cache context must remain valid.
    static auto *cache = new StringIdentityCache{};
    return *cache;
}

ScalarValue<> scalar_from_string_storage(StringStorage *storage) {
    auto release_storage = util::scope_exit{[storage]() noexcept { storage->release(); }};
    auto *handle = storage;
    const auto value = Scalar<string>::from(cpp_scalar_runtime_type<string>(), &handle);
    return ScalarValue<>{ScalarValue<string>{value}};
}

nanobind::object decode_string(std::string_view text) {
    if (text.size() > static_cast<std::size_t>(PY_SSIZE_T_MAX)) {
        PyErr_SetString(PyExc_OverflowError, "BG string is too large for Python");
        nanobind::raise_python_error();
    }

    const auto *bytes = text.empty() ? "" : text.data();
    auto result = nanobind::steal<nanobind::object>(
        PyUnicode_DecodeUTF8(bytes, static_cast<Py_ssize_t>(text.size()), "strict"));
    if (!result) {
        nanobind::raise_python_error();
    }
    return result;
}

ScalarValue<> from_python_bool(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    if (!PyBool_Check(value.ptr())) {
        PyErr_SetString(PyExc_TypeError, "expected bool");
        nanobind::raise_python_error();
    }
    return make_scalar<bool>(value.ptr() == Py_True, memory);
}

ScalarValue<> from_python_byte(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    const auto converted = nanobind::cast<int>(value);
    if (converted < 0 || converted > 255) {
        throw std::out_of_range{"a Python integer for bg.byte must be in [0, 255]"};
    }
    return make_scalar<byte>(static_cast<byte>(converted), memory);
}

ScalarValue<> from_python_i64(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    if (!PyLong_Check(value.ptr())) {
        PyErr_SetString(PyExc_TypeError, "expected an integer for bg.i64");
        nanobind::raise_python_error();
    }

    const auto converted = PyLong_AsLongLong(value.ptr());
    if (converted == -1 && PyErr_Occurred() != nullptr) {
        nanobind::raise_python_error();
    }
    return make_scalar<i64>(static_cast<i64>(converted), memory);
}

ScalarValue<> from_python_double(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    return make_scalar<double>(nanobind::cast<double>(value), memory);
}

ScalarValue<> from_python_timedelta(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    if (!PyDelta_Check(value.ptr())) {
        PyErr_SetString(PyExc_TypeError, "expected datetime.timedelta for bg.timedelta_t");
        nanobind::raise_python_error();
    }

    constexpr auto microseconds_per_day = std::uint64_t{86'400'000'000};
    const auto days = static_cast<std::int64_t>(PyDateTime_DELTA_GET_DAYS(value.ptr()));
    const auto remainder = static_cast<std::uint64_t>(PyDateTime_DELTA_GET_SECONDS(value.ptr())) * 1'000'000 +
                           static_cast<std::uint64_t>(PyDateTime_DELTA_GET_MICROSECONDS(value.ptr()));

    const auto count = [&] {
        if (days >= 0) {
            const auto positive_days = static_cast<std::uint64_t>(days);
            const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<i64>::max());
            if (positive_days > (maximum - remainder) / microseconds_per_day) {
                throw std::overflow_error{"datetime.timedelta is outside the bg.timedelta_t range"};
            }
            return static_cast<i64>(positive_days * microseconds_per_day + remainder);
        }

        const auto negative_days = static_cast<std::uint64_t>(-days);
        const auto maximum_magnitude = std::uint64_t{1} << 63;
        const auto whole_days = negative_days - 1;
        if (whole_days > maximum_magnitude / microseconds_per_day) {
            throw std::overflow_error{"datetime.timedelta is outside the bg.timedelta_t range"};
        }
        const auto magnitude = whole_days * microseconds_per_day + (microseconds_per_day - remainder);
        if (magnitude > maximum_magnitude) {
            throw std::overflow_error{"datetime.timedelta is outside the bg.timedelta_t range"};
        }
        if (magnitude == maximum_magnitude) {
            return std::numeric_limits<i64>::min();
        }
        return -static_cast<i64>(magnitude);
    }();

    return make_scalar<timedelta_t>(timedelta_t{count}, memory);
}

ScalarValue<> from_python_string(nanobind::handle value, allocator &, const PythonScalarConversions &conversions) {
    auto string_value = nanobind::object{};
    auto *python_type = Py_TYPE(value.ptr());
    if (python_type == &PyUnicode_Type ||
        &conversions.type_from_python(nanobind::handle{python_type}) == &string_type()) {
        string_value = nanobind::borrow<nanobind::object>(value);
    } else {
        string_value = nanobind::steal<nanobind::object>(PyUnicode_FromObject(value.ptr()));
        if (!string_value) {
            nanobind::raise_python_error();
        }
    }

    Py_ssize_t length{};
    const auto *bytes = PyUnicode_AsUTF8AndSize(string_value.ptr(), &length);
    if (bytes == nullptr) {
        nanobind::raise_python_error();
    }

    auto &cache = string_identity_cache();
    if (auto *storage = cache.retain_python_string(string_value.ptr()); storage != nullptr) {
        return scalar_from_string_storage(storage);
    }

    // Wiring may retain shared string storage after the conversion allocator expires.
    auto result = ScalarValue<>{ScalarValue<string>{std::string_view{bytes, static_cast<std::size_t>(length)}}};
    if (auto *storage = StringStorage::from(details::scalar_data(result)); storage != nullptr) {
        cache.remember(storage, string_value);
    }
    return result;
}

ScalarValue<> from_python_symbol(nanobind::handle value, allocator &memory,
                                 const PythonScalarConversions &conversions) {
    if (!PyUnicode_Check(value.ptr())) {
        PyErr_SetString(PyExc_TypeError, "expected text for a BG symbol");
        nanobind::raise_python_error();
    }

    const auto &python_type = conversions.type_from_python(
        nanobind::handle{reinterpret_cast<PyObject *>(Py_TYPE(value.ptr()))});
    const auto is_null = &python_type == &symbol_type() &&
                         nanobind::cast<bool>(nanobind::borrow<nanobind::object>(value).attr("_is_null"));
    if (is_null) {
        return make_scalar<symbol>(symbol{}, memory);
    }

    Py_ssize_t length{};
    const auto *text = PyUnicode_AsUTF8AndSize(value.ptr(), &length);
    if (text == nullptr) {
        nanobind::raise_python_error();
    }
    return make_scalar<symbol>(symbol{std::string_view{text, static_cast<std::size_t>(length)}}, memory);
}

ScalarValue<> from_python_dict(const DictScalarType &type, nanobind::handle value, allocator &memory,
                               const PythonScalarConversions &conversions) {
    if (PyMapping_Check(value.ptr()) == 0) {
        PyErr_SetString(PyExc_TypeError, "expected a mapping for a scalar dictionary");
        nanobind::raise_python_error();
    }

    auto items = nanobind::steal<nanobind::object>(PyMapping_Items(value.ptr()));
    if (!items) {
        nanobind::raise_python_error();
    }

    const auto &runtime_type = static_cast<const ScalarDictRuntimeType &>(resolve_scalar_runtime_type(type, {}));
    auto *storage = memory.allocate_bytes(runtime_type.size(), runtime_type.alignment());
    auto release_storage = util::scope_exit{[&]() noexcept {
        memory.deallocate_bytes(storage, runtime_type.size(), runtime_type.alignment());
    }};
    runtime_type.construct_runtime_value(storage, memory);
    auto release_value = util::scope_exit{[&]() noexcept { runtime_type.destroy(storage, memory); }};

    for (const auto item : nanobind::borrow<nanobind::iterable>(items)) {
        const auto pair = nanobind::cast<nanobind::tuple>(item);
        auto key = conversions.from_python(type.key_type(), pair[0], memory);
        auto mapped = conversions.from_python(type.mapped_type(), pair[1], memory);
        runtime_type.insert_or_assign(storage, details::scalar_data(key), details::scalar_data(mapped), memory);
    }

    auto value_view = Scalar<Dict<SCALAR, SCALAR>>::from(runtime_type, storage);
    return ScalarValue<>{value_view, memory};
}

ScalarValue<> from_python_list(const ListScalarType &type, nanobind::handle value, allocator &memory,
                               const PythonScalarConversions &conversions) {
    if (PyTuple_Check(value.ptr()) == 0 && PyList_Check(value.ptr()) == 0) {
        PyErr_SetString(PyExc_TypeError, "expected a tuple or list for a scalar list");
        nanobind::raise_python_error();
    }

    const auto &runtime_type = static_cast<const ScalarListRuntimeType &>(resolve_scalar_runtime_type(type, {}));
    struct Construction {
        nanobind::handle value;
        const PythonScalarConversions *conversions;
    } construction{value, &conversions};
    const auto construct = [](void *context, const ScalarRuntimeType &runtime_type, void *destination,
                              allocator &memory) {
        const auto &input = *static_cast<const Construction *>(context);
        const auto &list_runtime = static_cast<const ScalarListRuntimeType &>(runtime_type);
        runtime_type.construct_runtime_value(destination, memory);
        auto rollback = util::scope_exit{[&]() noexcept { runtime_type.destroy(destination, memory); }};

        for (const auto item : nanobind::borrow<nanobind::iterable>(input.value)) {
            auto converted = input.conversions->from_python(list_runtime.item_runtime_type().type(), item, memory);
            list_runtime.append(destination, details::scalar_data(converted), memory);
        }
        rollback.release();
    };
    return ScalarValue<>::from_construction(runtime_type, {&construction, construct}, memory);
}

nanobind::object to_python_list(const Scalar<List<SCALAR>> &value, const PythonScalarConversions &conversions) {
    auto result = nanobind::tuple_builder{value.size()};
    const auto item_to_python = conversions.to_python_handler(value.runtime_type().item_runtime_type());
    for (auto index = std::size_t{}; index < value.size(); ++index) {
        const auto item = value[index];
        result.put(item_to_python(Scalar<>::from(item), conversions));
    }
    return result.commit();
}

nanobind::object to_python_set(const Scalar<Set<SCALAR>> &value, const PythonScalarConversions &conversions) {
    auto result = nanobind::steal<nanobind::object>(PySet_New(nullptr));
    if (!result) {
        nanobind::raise_python_error();
    }

    const auto item_to_python = conversions.to_python_handler(value.runtime_type().item_runtime_type());
    value.for_each([&](Scalar<SCALAR> item) {
        const auto converted = item_to_python(Scalar<>::from(item), conversions);
        if (PySet_Add(result.ptr(), converted.ptr()) < 0) {
            nanobind::raise_python_error();
        }
    });
    return result;
}

nanobind::object to_python_dict(const Scalar<Dict<SCALAR, SCALAR>> &value,
                               const PythonScalarConversions &conversions) {
    auto result = nanobind::dict{};
    const auto &runtime_type = value.runtime_type();
    const auto key_to_python = conversions.to_python_handler(runtime_type.key_runtime_type());
    const auto mapped_to_python = conversions.to_python_handler(runtime_type.mapped_runtime_type());
    value.for_each([&](Scalar<SCALAR> key, Scalar<SCALAR> item) {
        const auto converted_key = key_to_python(Scalar<>::from(key), conversions);
        const auto converted_value = mapped_to_python(Scalar<>::from(item), conversions);
        if (PyDict_SetItem(result.ptr(), converted_key.ptr(), converted_value.ptr()) < 0) {
            nanobind::raise_python_error();
        }
    });
    return result;
}

nanobind::object to_python_struct(const Scalar<STRUCT> &value, const PythonScalarConversions &conversions) {
    auto result = nanobind::dict{};
    for (auto index = std::size_t{}; index < value.field_count(); ++index) {
        const auto name = value.field_name(index);
        const auto key = nanobind::str{name.data(), name.size()};
        const auto item = value[index];
        const auto field = conversions.to_python(Scalar<>::from(item));
        if (PyDict_SetItem(result.ptr(), key.ptr(), field.ptr()) < 0) {
            nanobind::raise_python_error();
        }
    }
    return result;
}

nanobind::object to_python_hierarchical(const Scalar<HIERARCHICAL> &value,
                                       const PythonScalarConversions &conversions) {
    auto result = nanobind::dict{};
    for (auto index = std::size_t{}; index < value.field_count(); ++index) {
        const auto name = value.field_name(index);
        const auto key = nanobind::str{name.data(), name.size()};
        auto field = nanobind::object{nanobind::none()};
        if (value.present(index)) {
            const auto item = value[index];
            field = conversions.to_python(Scalar<>::from(item));
        }
        if (PyDict_SetItem(result.ptr(), key.ptr(), field.ptr()) < 0) {
            nanobind::raise_python_error();
        }
    }

    auto python_type = conversions.python_schema_class(value.type().schema());
    if (python_type) {
        auto *instance_ptr = PyType_GenericAlloc(reinterpret_cast<PyTypeObject *>(python_type.ptr()), 0);
        auto instance = nanobind::steal<nanobind::object>(instance_ptr);
        if (!instance) {
            nanobind::raise_python_error();
        }
        for (auto index = std::size_t{}; index < value.field_count(); ++index) {
            const auto name = value.field_name(index);
            const auto key = nanobind::str{name.data(), name.size()};
            auto *field = PyDict_GetItem(result.ptr(), key.ptr());
            if (field == nullptr || PyObject_SetAttr(instance.ptr(), key.ptr(), field) < 0) {
                nanobind::raise_python_error();
            }
        }
        return instance;
    }
    return result;
}

ScalarValue<> from_python_hierarchical(const ScalarType &expected, nanobind::handle value, allocator &memory,
                                      const PythonScalarConversions &conversions) {
    const auto &actual = conversions.type_from_python(
        nanobind::handle{reinterpret_cast<PyObject *>(Py_TYPE(value.ptr()))});
    if (!expected.accepts(actual)) {
        PyErr_SetString(PyExc_TypeError, "Python schema value is not compatible with the annotated bundle type");
        nanobind::raise_python_error();
    }

    const auto *actual_type = type_cast<HierarchicalScalarType>(&actual);
    if (actual_type == nullptr) {
        throw std::invalid_argument{"Python schema value has no registered hierarchical scalar schema"};
    }
    const auto &actual_schema = static_cast<const Schema &>(actual_type->schema_type());

    auto field_values = std::vector<ScalarValue<>>{};
    field_values.reserve(actual_schema.field_count());
    auto fields = std::vector<std::optional<Scalar<>>>(actual_schema.field_count());
    for (auto index = std::size_t{}; index < actual_schema.field_count(); ++index) {
        const auto &field = actual_schema.field(index);
        auto field_value = nanobind::getattr(value, field.name.text().data());
        if (field_value.is_none()) {
            if (!field.optional) {
                PyErr_Format(PyExc_TypeError, "required schema field '%s' cannot be None", field.name.text().data());
                nanobind::raise_python_error();
            }
            continue;
        }

        const auto *field_type = type_cast<ScalarType>(field.type);
        if (field_type == nullptr) {
            throw std::invalid_argument{"Python bundle schemas require scalar fields"};
        }
        field_values.push_back(conversions.from_python(*field_type, field_value, memory));
        fields[index] = Scalar<>::from(field_values.back());
    }

    auto bundle_value = bundle(memory, actual_schema, fields);
    return ScalarValue<>{bundle_value};
}

nanobind::object to_python_byte(Scalar<> value, const PythonScalarConversions &) {
    return nanobind::int_{std::to_integer<unsigned int>(unchecked_scalar_cast<byte>(value).value())};
}

nanobind::object to_python_bool(Scalar<> value, const PythonScalarConversions &) {
    return nanobind::bool_{unchecked_scalar_cast<bool>(value).value()};
}

nanobind::object to_python_i64(Scalar<> value, const PythonScalarConversions &) {
    return nanobind::int_{unchecked_scalar_cast<i64>(value).value()};
}

nanobind::object to_python_double(Scalar<> value, const PythonScalarConversions &) {
    return nanobind::float_{unchecked_scalar_cast<double>(value).value()};
}

nanobind::object to_python_timedelta(Scalar<> value, const PythonScalarConversions &) {
    const auto count = unchecked_scalar_cast<timedelta_t>(value).value().count();
    constexpr auto microseconds_per_day = std::int64_t{86'400'000'000};
    const auto days = count / microseconds_per_day;
    const auto within_day = count % microseconds_per_day;
    const auto seconds = within_day / 1'000'000;
    const auto microseconds = within_day % 1'000'000;
    auto *result = PyDelta_FromDSU(static_cast<int>(days), static_cast<int>(seconds), static_cast<int>(microseconds));
    if (result == nullptr) {
        nanobind::raise_python_error();
    }
    return nanobind::steal<nanobind::object>(result);
}

nanobind::object to_python_string(Scalar<> value, const PythonScalarConversions &) {
    const auto text = unchecked_scalar_cast<string>(value).value();
    auto *storage = StringStorage::from(details::scalar_data(value));
    if (storage == nullptr || !storage->try_retain()) {
        return decode_string(text);
    }

    auto release_storage = util::scope_exit{[storage]() noexcept { storage->release(); }};
    auto &cache = string_identity_cache();
    auto cached_python_string = cache.python_string(storage);
    if (cached_python_string) {
        return nanobind::borrow<nanobind::object>(cached_python_string.get());
    }

    auto decoded = decode_string(text);
    auto retained_python_string = cache.remember(storage, decoded);
    return retained_python_string ? std::move(retained_python_string) : std::move(decoded);
}

nanobind::object to_python_symbol(Scalar<> value, const PythonScalarConversions &conversions) {
    return conversions.symbol_to_python(unchecked_scalar_cast<symbol>(value).value());
}

nanobind::object to_python_structural(Scalar<> value, const PythonScalarConversions &conversions) {
    auto result = value.visit(
        [&conversions](const Scalar<List<SCALAR>> *scalar) -> nanobind::object {
            return to_python_list(*scalar, conversions);
        },
        [&conversions](const Scalar<Set<SCALAR>> *scalar) -> nanobind::object {
            return to_python_set(*scalar, conversions);
        },
        [&conversions](const Scalar<Dict<SCALAR, SCALAR>> *scalar) -> nanobind::object {
            return to_python_dict(*scalar, conversions);
        },
        [&conversions](const Scalar<STRUCT> *scalar) -> nanobind::object {
            return to_python_struct(*scalar, conversions);
        },
        [&conversions](const Scalar<HIERARCHICAL> *scalar) -> nanobind::object {
            return to_python_hierarchical(*scalar, conversions);
        },
        [](const Scalar<> *) -> nanobind::object {
            throw std::invalid_argument{"no Python conversion is registered for this scalar runtime type"};
        },
        ddv::noop);
    if (!result) {
        throw std::invalid_argument{"no Python conversion is registered for this scalar value"};
    }
    return std::move(*result);
}

nanobind::object to_python_view_default(Scalar<> value, const PythonScalarConversions &conversions,
                                        const std::shared_ptr<CallbackLifetime> &lifetime,
                                        std::uint64_t generation) {
    auto result = value.visit(
        [&](const Scalar<Dict<SCALAR, SCALAR>> *dictionary) -> nanobind::object {
            return nanobind::cast(PythonDictView{*dictionary, conversions, lifetime, generation});
        },
        [&]<typename T>(const Scalar<T> *scalar) -> nanobind::object {
            return conversions.to_python(Scalar<>::from(*scalar));
        },
        ddv::noop);
    if (!result) {
        throw std::invalid_argument{"no Python view conversion is registered for this scalar value"};
    }

    return std::move(*result);
}

} // namespace

struct NB_HIDDEN PythonScalarConversions::Impl {
    struct PythonType {
        Object type;
        const ScalarType *scalar_type;
        bool explicit_mapping;
    };

    ScalarDispatch<PythonToScalar> from_python;
    ScalarDispatch<ScalarToPython> to_python;
    ScalarDispatch<ScalarToPythonView> to_python_view;
    std::unordered_map<PyTypeObject *, PythonType> python_types;
    std::unordered_map<const ScalarType *, const PythonObjectScalarRuntimeType *> python_object_types;
    std::unordered_map<const ScalarType *, Object> python_object_classes;
    std::unordered_map<const ScalarType *, Object> python_schemas;
    Object symbol_python_type;
};

PythonScalarConversions::PythonScalarConversions() : impl_{std::make_unique<Impl>()} {
    if (PyDateTimeAPI == nullptr) {
        PyDateTime_IMPORT;
        if (PyDateTimeAPI == nullptr) {
            nanobind::raise_python_error();
        }
    }

    const auto add_from_python = [this](const ScalarType &type, PythonToScalar conversion) {
        if (!impl_->from_python.add(type, conversion)) {
            throw std::logic_error{"a built-in Python scalar conversion was registered twice"};
        }
    };

    add_from_python(byte_type(), &from_python_byte);
    add_from_python(bool_type(), &from_python_bool);
    add_from_python(int64_type(), &from_python_i64);
    add_from_python(double_type(), &from_python_double);
    add_from_python(timedelta_type(), &from_python_timedelta);
    add_from_python(string_type(), &from_python_string);
    add_from_python(symbol_type(), &from_python_symbol);

    const auto add_to_python = [this](const ScalarType &type, ScalarToPython conversion) {
        if (!impl_->to_python.add(type, conversion)) {
            throw std::logic_error{"a built-in scalar-to-Python conversion was registered twice"};
        }
    };

    add_to_python(byte_type(), &to_python_byte);
    add_to_python(bool_type(), &to_python_bool);
    add_to_python(int64_type(), &to_python_i64);
    add_to_python(double_type(), &to_python_double);
    add_to_python(timedelta_type(), &to_python_timedelta);
    add_to_python(string_type(), &to_python_string);
    add_to_python(symbol_type(), &to_python_symbol);

    const auto add_python_builtin = [this](PyTypeObject &python_type, const ScalarType &scalar_type) {
        auto type = nanobind::handle{reinterpret_cast<PyObject *>(&python_type)};
        impl_->python_types.emplace(&python_type, Impl::PythonType{Object{type}, &scalar_type, true});
    };

    add_python_builtin(PyBool_Type, bool_type());
    add_python_builtin(PyLong_Type, int64_type());
    add_python_builtin(PyFloat_Type, double_type());
    add_python_builtin(PyUnicode_Type, string_type());
    add_python_builtin(*PyDateTimeAPI->DeltaType, timedelta_type());
}

PythonScalarConversions::~PythonScalarConversions() = default;

bool PythonScalarConversions::add(const ScalarType &type, PythonToScalar conversion) {
    if (conversion == nullptr) {
        throw std::invalid_argument{"a Python-to-scalar conversion cannot be null"};
    }
    return impl_->from_python.add(type, conversion);
}

bool PythonScalarConversions::add(const ScalarType &type, ScalarToPython conversion) {
    if (conversion == nullptr) {
        throw std::invalid_argument{"a scalar-to-Python conversion cannot be null"};
    }
    return impl_->to_python.add(type, conversion);
}

bool PythonScalarConversions::add(const ScalarRuntimeType &runtime_type, ScalarToPython conversion) {
    if (conversion == nullptr) {
        throw std::invalid_argument{"a scalar-to-Python conversion cannot be null"};
    }
    return impl_->to_python.add(runtime_type, conversion);
}

bool PythonScalarConversions::add(const ScalarType &type, ScalarToPythonView conversion) {
    if (conversion == nullptr) {
        throw std::invalid_argument{"a scalar-to-Python-view conversion cannot be null"};
    }
    return impl_->to_python_view.add(type, conversion);
}

bool PythonScalarConversions::add(const ScalarRuntimeType &runtime_type, ScalarToPythonView conversion) {
    if (conversion == nullptr) {
        throw std::invalid_argument{"a scalar-to-Python-view conversion cannot be null"};
    }
    return impl_->to_python_view.add(runtime_type, conversion);
}

bool PythonScalarConversions::add_python_type(nanobind::handle python_type, const ScalarType &type) {
    if (!PyType_Check(python_type.ptr())) {
        throw std::invalid_argument{"a Python scalar type registration requires a Python type"};
    }

    auto *type_key = reinterpret_cast<PyTypeObject *>(python_type.ptr());
    const auto added =
        impl_->python_types.emplace(type_key, Impl::PythonType{Object{python_type}, &type, true}).second;
    if (added && &type == &symbol_type()) {
        impl_->symbol_python_type = Object{python_type};
    }
    return added;
}

const ScalarType &PythonScalarConversions::add_python_schema(nanobind::handle python_type, const bg::Schema &schema) {
    if (!PyType_Check(python_type.ptr())) {
        throw std::invalid_argument{"a Python schema registration requires a Python type"};
    }

    const auto &scalar_type = hierarchical_scalar_type(schema);
    auto *type_key = reinterpret_cast<PyTypeObject *>(python_type.ptr());
    if (const auto found = impl_->python_types.find(type_key); found != impl_->python_types.end()) {
        if (found->second.scalar_type != &scalar_type) {
            throw std::invalid_argument{"a Python schema class already has a different scalar type"};
        }
        return scalar_type;
    }

    impl_->python_types.emplace(type_key, Impl::PythonType{Object{python_type}, &scalar_type, true});
    impl_->python_schemas.emplace(&scalar_type, Object{python_type});
    return scalar_type;
}

nanobind::object PythonScalarConversions::python_schema_class(const bg::Schema &schema) const {
    const auto &scalar_type = hierarchical_scalar_type(schema);
    const auto found = impl_->python_schemas.find(&scalar_type);
    if (found == impl_->python_schemas.end()) {
        return {};
    }
    return nanobind::borrow<nanobind::object>(found->second.get());
}

nanobind::object PythonScalarConversions::python_object_class(const ScalarType &type) const {
    const auto found = impl_->python_object_classes.find(&type);
    if (found == impl_->python_object_classes.end()) {
        return {};
    }
    return nanobind::borrow<nanobind::object>(found->second.get());
}

const ScalarType &PythonScalarConversions::add_python_type(nanobind::handle python_type) {
    if (!PyType_Check(python_type.ptr())) {
        throw std::invalid_argument{"a Python scalar type registration requires a Python type"};
    }

    auto *python_type_key = reinterpret_cast<PyTypeObject *>(python_type.ptr());
    if (const auto found = impl_->python_types.find(python_type_key); found != impl_->python_types.end()) {
        return *found->second.scalar_type;
    }

    auto method_resolution_order =
        nanobind::steal<nanobind::object>(PyObject_GetAttrString(python_type.ptr(), "__mro__"));
    if (!method_resolution_order) {
        nanobind::raise_python_error();
    }

    auto bases = std::vector<const ScalarType *>{};
    const auto count = PyTuple_GET_SIZE(method_resolution_order.ptr());
    for (auto index = Py_ssize_t{1}; index < count; ++index) {
        auto *base = reinterpret_cast<PyTypeObject *>(PyTuple_GET_ITEM(method_resolution_order.ptr(), index));
        const auto found = impl_->python_types.find(base);
        if (found != impl_->python_types.end()) {
            bases.push_back(found->second.scalar_type);
        } else {
            bases.push_back(&add_python_type(nanobind::handle{reinterpret_cast<PyObject *>(base)}));
        }
    }

    auto scalar_type = std::make_unique<PythonObjectScalarType>(std::move(bases),
                                                                 python_type_key->tp_hash != PyObject_HashNotImplemented);
    const auto &registered_type = TypeRegistry::instance().register_owned(std::move(scalar_type));
    const auto &runtime_type = python_object_runtime_types().get(static_cast<const PythonObjectScalarType &>(registered_type));
    impl_->python_object_types.emplace(&registered_type, &runtime_type);
    if (!impl_->to_python.add(runtime_type, &to_python_python_object)) {
        throw std::logic_error{"a Python object runtime type was registered twice"};
    }
    impl_->python_types.emplace(python_type_key, Impl::PythonType{Object{python_type}, &registered_type, false});
    impl_->python_object_classes.emplace(&registered_type, Object{python_type});
    return registered_type;
}

ScalarToPython PythonScalarConversions::to_python_handler(const ScalarRuntimeType &runtime_type) const noexcept {
    if (const auto *conversion = impl_->to_python.find(runtime_type)) {
        return *conversion;
    }
    return &to_python_structural;
}

nanobind::object PythonScalarConversions::to_python(Scalar<> value) const {
    return to_python_handler(value.runtime_type())(value, *this);
}

nanobind::object PythonScalarConversions::symbol_to_python(util::symbol value) const {
    if (!impl_->symbol_python_type) {
        throw std::logic_error{"bg.Symbol must be registered before converting a symbol"};
    }

    auto symbol_class = nanobind::borrow<nanobind::object>(impl_->symbol_python_type.get());
    auto cache = nanobind::borrow<nanobind::object>(symbol_class.attr("_instances"));
    const auto text = value.text();
    auto key = nanobind::object{};
    if (value.has_value()) {
        key = nanobind::steal<nanobind::object>(
            PyBytes_FromStringAndSize(text.data(), static_cast<Py_ssize_t>(text.size())));
        if (!key) {
            nanobind::raise_python_error();
        }
    } else {
        key = nanobind::none();
    }

    auto cached = cache.attr("get")(key);
    if (!cached.is_none()) {
        return cached;
    }

    auto result = value.has_value() ? symbol_class(decode_string(text)) : symbol_class();
    return result;
}

nanobind::object PythonScalarConversions::to_python_view(
    Scalar<> value, const std::shared_ptr<CallbackLifetime> &lifetime, std::uint64_t generation) const {
    if (const auto *conversion = impl_->to_python_view.find(value.runtime_type())) {
        return (*conversion)(value, *this, lifetime, generation);
    }
    if (const auto *conversion = impl_->to_python.find(value.runtime_type())) {
        return (*conversion)(value, *this);
    }

    return to_python_view_default(value, *this, lifetime, generation);
}

const ScalarType &PythonScalarConversions::type_from_python(nanobind::handle python_type) const {
    if (!PyType_Check(python_type.ptr())) {
        throw std::invalid_argument{"Python scalar type inference requires a Python type"};
    }

    auto method_resolution_order =
        nanobind::steal<nanobind::object>(PyObject_GetAttrString(python_type.ptr(), "__mro__"));
    if (!method_resolution_order) {
        nanobind::raise_python_error();
    }

    auto *python_type_key = reinterpret_cast<PyTypeObject *>(python_type.ptr());
    if (const auto found = impl_->python_types.find(python_type_key); found != impl_->python_types.end()) {
        return *found->second.scalar_type;
    }

    const auto count = PyTuple_GET_SIZE(method_resolution_order.ptr());
    for (auto index = Py_ssize_t{1}; index < count; ++index) {
        auto *type = reinterpret_cast<PyTypeObject *>(PyTuple_GET_ITEM(method_resolution_order.ptr(), index));
        const auto found = impl_->python_types.find(type);
        if (found != impl_->python_types.end() && found->second.explicit_mapping) {
            return *found->second.scalar_type;
        }
    }

    return const_cast<PythonScalarConversions *>(this)->add_python_type(python_type);
}

ScalarValue<> PythonScalarConversions::from_python(nanobind::handle value, allocator &memory) const {
    if (value.ptr() == nullptr) {
        throw std::invalid_argument{"a Python scalar value cannot be null"};
    }

    const auto &type = type_from_python(nanobind::handle{reinterpret_cast<PyObject *>(Py_TYPE(value.ptr()))});
    return from_python(type, value, memory);
}

ScalarValue<> PythonScalarConversions::from_python(const ScalarType &type, nanobind::handle value,
                                                   allocator &memory) const {
    if (impl_->python_schemas.contains(&type)) {
        return from_python_hierarchical(type, value, memory, *this);
    }

    if (const auto found = impl_->python_object_types.find(&type); found != impl_->python_object_types.end()) {
        const auto &actual_type = type_from_python(
            nanobind::handle{reinterpret_cast<PyObject *>(Py_TYPE(value.ptr()))});
        if (!type.accepts(actual_type)) {
            PyErr_SetString(PyExc_TypeError, "Python object is not an instance of the annotated scalar class");
            nanobind::raise_python_error();
        }

        const auto runtime = impl_->python_object_types.find(&actual_type);
        if (runtime == impl_->python_object_types.end()) {
            PyErr_SetString(PyExc_TypeError, "Python object has no compatible scalar storage");
            nanobind::raise_python_error();
        }

        auto *storage = new PythonObjectStorage{value.ptr()};
        auto release_storage = util::scope_exit{[storage]() noexcept { storage->release(); }};
        auto *handle = storage;
        const auto scalar = Scalar<>::from(*runtime->second, &handle);
        return ScalarValue<>{scalar};
    }

    const auto *conversion = impl_->from_python.find(type);
    if (conversion != nullptr) {
        return (*conversion)(value, memory, *this);
    }

    if (const auto *dictionary = type_cast<DictScalarType>(&type)) {
        return from_python_dict(*dictionary, value, memory, *this);
    }
    if (const auto *list = type_cast<ListScalarType>(&type)) {
        return from_python_list(*list, value, memory, *this);
    }

    throw std::invalid_argument{"no Python-to-scalar conversion is registered for this scalar type"};
}

bool PythonScalarConversions::can_from_python(const ScalarType &type) const noexcept {
    if (impl_->python_schemas.contains(&type)) {
        return true;
    }
    if (impl_->python_object_types.contains(&type)) {
        return true;
    }
    if (impl_->from_python.find(type) != nullptr) {
        return true;
    }
    if (const auto *dictionary = type_cast<DictScalarType>(&type)) {
        return can_from_python(dictionary->key_type()) && can_from_python(dictionary->mapped_type());
    }
    if (const auto *list = type_cast<ListScalarType>(&type)) {
        return can_from_python(list->item_type());
    }
    return false;
}

PythonToScalar PythonScalarConversions::from_python_handler(const ScalarType &type) const noexcept {
    const auto *conversion = impl_->from_python.find(type);
    return conversion == nullptr ? nullptr : *conversion;
}

PythonScalarConversions &scalar_conversions() {
    static auto conversions = PythonScalarConversions{};
    return conversions;
}

} // namespace bg::python
