#include <bg/python/scalar_conversion.hpp>

#include "dict_view.hpp"

#include <bg/python/object.hpp>

#include <bg/scalar/builtins.hpp>
#include <bg/scalar/bundle/scalar_type.hpp>
#include <bg/scalar/details/string_storage.hpp>
#include <bg/scalar/dict/scalar_type.hpp>
#include <bg/scalar/dict/runtime_type.hpp>
#include <bg/scalar/list/scalar_type.hpp>
#include <bg/scalar/set/scalar_type.hpp>
#include <bg/scalar/struct/scalar_type.hpp>
#include <bg/scalar/value.hpp>
#include <bg/util/scope_exit.hpp>

#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bg::python {
namespace {

using details::StringStorage;
using details::StringStorageFinalReleaseHook;

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

ScalarValue<> from_python_string(nanobind::handle value, allocator &, const PythonScalarConversions &) {
    Py_ssize_t length{};
    const auto *bytes = PyUnicode_AsUTF8AndSize(value.ptr(), &length);
    if (bytes == nullptr) {
        nanobind::raise_python_error();
    }

    auto &cache = string_identity_cache();
    if (auto *storage = cache.retain_python_string(value.ptr()); storage != nullptr) {
        return scalar_from_string_storage(storage);
    }

    // Wiring may retain shared string storage after the conversion allocator expires.
    auto result = ScalarValue<>{ScalarValue<string>{std::string_view{bytes, static_cast<std::size_t>(length)}}};
    if (auto *storage = StringStorage::from(details::scalar_data(result)); storage != nullptr) {
        cache.remember(storage, value);
    }
    return result;
}

ScalarValue<> from_python_symbol(nanobind::handle value, allocator &memory, const PythonScalarConversions &) {
    return make_scalar<symbol>(nanobind::cast<symbol>(value), memory);
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

nanobind::object to_python_list(const Scalar<List<SCALAR>> &value, const PythonScalarConversions &conversions) {
    auto result = nanobind::list{};
    const auto item_to_python = conversions.to_python_handler(value.runtime_type().item_runtime_type());
    for (auto index = std::size_t{}; index < value.size(); ++index) {
        const auto item = value[index];
        result.append(item_to_python(Scalar<>::from(item), conversions));
    }
    return result;
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
    return result;
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

nanobind::object to_python_symbol(Scalar<> value, const PythonScalarConversions &) {
    return nanobind::cast(unchecked_scalar_cast<symbol>(value).value());
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
    };

    ScalarDispatch<PythonToScalar> from_python;
    ScalarDispatch<ScalarToPython> to_python;
    ScalarDispatch<ScalarToPythonView> to_python_view;
    std::unordered_map<PyTypeObject *, PythonType> python_types;
};

PythonScalarConversions::PythonScalarConversions() : impl_{std::make_unique<Impl>()} {
    const auto add_from_python = [this](const ScalarType &type, PythonToScalar conversion) {
        if (!impl_->from_python.add(type, conversion)) {
            throw std::logic_error{"a built-in Python scalar conversion was registered twice"};
        }
    };

    add_from_python(byte_type(), &from_python_byte);
    add_from_python(bool_type(), &from_python_bool);
    add_from_python(int64_type(), &from_python_i64);
    add_from_python(double_type(), &from_python_double);
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
    add_to_python(string_type(), &to_python_string);
    add_to_python(symbol_type(), &to_python_symbol);

    const auto add_python_builtin = [this](PyTypeObject &python_type, const ScalarType &scalar_type) {
        auto type = nanobind::handle{reinterpret_cast<PyObject *>(&python_type)};
        impl_->python_types.emplace(&python_type, Impl::PythonType{Object{type}, &scalar_type});
    };

    add_python_builtin(PyBool_Type, bool_type());
    add_python_builtin(PyLong_Type, int64_type());
    add_python_builtin(PyFloat_Type, double_type());
    add_python_builtin(PyUnicode_Type, string_type());
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
    return impl_->python_types.emplace(type_key, Impl::PythonType{Object{python_type}, &type}).second;
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

    const auto count = PyTuple_GET_SIZE(method_resolution_order.ptr());
    for (auto index = Py_ssize_t{}; index < count; ++index) {
        auto *type = reinterpret_cast<PyTypeObject *>(PyTuple_GET_ITEM(method_resolution_order.ptr(), index));
        const auto found = impl_->python_types.find(type);
        if (found != impl_->python_types.end()) {
            return *found->second.scalar_type;
        }
    }

    throw std::invalid_argument{"no scalar type is registered for this Python type"};
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
    const auto *conversion = impl_->from_python.find(type);
    if (conversion != nullptr) {
        return (*conversion)(value, memory, *this);
    }

    if (const auto *dictionary = type_cast<DictScalarType>(&type)) {
        return from_python_dict(*dictionary, value, memory, *this);
    }

    throw std::invalid_argument{"no Python-to-scalar conversion is registered for this scalar type"};
}

bool PythonScalarConversions::can_from_python(const ScalarType &type) const noexcept {
    if (impl_->from_python.find(type) != nullptr) {
        return true;
    }
    if (const auto *dictionary = type_cast<DictScalarType>(&type)) {
        return can_from_python(dictionary->key_type()) && can_from_python(dictionary->mapped_type());
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
