#include "dict_view.hpp"

#include <bg/scalar/dict/runtime_type.hpp>

#include <nanobind/stl/tuple.h>

#include <stdexcept>
#include <utility>

namespace nb = nanobind;
using namespace nanobind::literals;

namespace bg::python {

namespace {

nb::object mapping_view(const PythonDictView &mapping, const char *name) {
    auto abc = nb::module_::import_("collections.abc");
    return abc.attr(name)(nb::cast(mapping));
}

} // namespace

void PythonDictCursorIterator::check_lifetime() const {
    if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
        throw std::runtime_error{"a Python dictionary view was used outside its callback"};
    }
}

nb::object PythonDictCursorIterator::next() {
    check_lifetime();

    if (batch_index_ == batch_size_) {
        batch_size_ = cursor_->next_batch(batch_);
        batch_index_ = 0;
        if (batch_size_ == 0) {
            throw nb::stop_iteration();
        }
    }

    const auto &entry = batch_[batch_index_++];
    const auto &key_runtime_type = runtime_type_->key_runtime_type();
    const auto &mapped_runtime_type = runtime_type_->mapped_runtime_type();
    if (projection_ == Projection::keys) {
        return key_to_python_(Scalar<>::from(key_runtime_type, entry.key), *conversions_);
    }

    if (projection_ == Projection::values) {
        return mapped_to_python_(Scalar<>::from(mapped_runtime_type, entry.mapped), *conversions_);
    }

    const auto key = Scalar<>::from(key_runtime_type, entry.key);
    const auto value = Scalar<>::from(mapped_runtime_type, entry.mapped);
    return nb::make_tuple(key_to_python_(key, *conversions_), mapped_to_python_(value, *conversions_));
}

void PythonDictEntryView::check_lifetime() const {
    if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
        throw std::runtime_error{"a Python dictionary view was used outside its callback"};
    }
}

std::size_t PythonDictEntryView::size() const {
    check_lifetime();
    return dictionary_.size();
}

nb::object PythonDictEntryView::iter() const {
    check_lifetime();
    return nb::cast(PythonDictCursorIterator{dictionary_, *conversions_, lifetime_, generation_, projection_});
}

void PythonDictView::check_lifetime() const {
    if (lifetime_ == nullptr || !lifetime_->active(generation_)) {
        throw std::runtime_error{"a Python dictionary view was used outside its callback"};
    }
}

ScalarValue<> PythonDictView::key(nb::handle value) const {
    auto memory = allocator{};
    if (key_conversion_ != nullptr) {
        return key_conversion_(value, memory, *conversions_);
    }
    return conversions_->from_python(dictionary_.runtime_type().key_runtime_type().type(), value, memory);
}

std::size_t PythonDictView::size() const {
    check_lifetime();
    return dictionary_.size();
}

nb::object PythonDictView::getitem(nb::handle value) const {
    check_lifetime();
    auto key_value = key(value);
    const auto mapped = dictionary_.find(Scalar<SCALAR>::from(key_value));
    if (!mapped) {
        PyErr_SetObject(PyExc_KeyError, value.ptr());
        return {};
    }

    return value_conversion_(Scalar<>::from(*mapped), *conversions_);
}

bool PythonDictView::contains(nb::handle value) const {
    check_lifetime();
    auto key_value = key(value);
    return dictionary_.contains(Scalar<SCALAR>::from(key_value));
}

nb::object PythonDictView::get(nb::handle value, nb::handle fallback) const {
    check_lifetime();
    auto key_value = key(value);
    const auto mapped = dictionary_.find(Scalar<SCALAR>::from(key_value));
    if (!mapped) {
        return nb::borrow<nb::object>(fallback);
    }

    return value_conversion_(Scalar<>::from(*mapped), *conversions_);
}

PythonDictCursorIterator PythonDictView::iter() const {
    check_lifetime();
    return {dictionary_, *conversions_, lifetime_, generation_, PythonDictCursorIterator::Projection::keys};
}

nb::object PythonDictView::keys() const {
    check_lifetime();
    return mapping_view(*this, "KeysView");
}

nb::object PythonDictView::items() const {
    check_lifetime();
    return nb::cast(PythonDictEntryView{dictionary_, *conversions_, lifetime_, generation_,
                                        PythonDictCursorIterator::Projection::items});
}

nb::object PythonDictView::values() const {
    check_lifetime();
    return nb::cast(PythonDictEntryView{dictionary_, *conversions_, lifetime_, generation_,
                                        PythonDictCursorIterator::Projection::values});
}

nb::object PythonDictView::copy() const {
    check_lifetime();
    return conversions_->to_python(Scalar<>::from(dictionary_));
}

void bind_dict_view(nb::module_ &module) {
    module.def("_dict_view_layout", [] {
        return std::tuple{sizeof(PythonDictView),
                          sizeof(PythonDictCursorIterator),
                          sizeof(PythonDictEntryView),
                          sizeof(Scalar<>),
                          sizeof(ScalarDictRuntimeType::BatchEntry),
                          PythonDictCursorIterator::batch_capacity,
                          sizeof(const void *)};
    });

    nb::class_<PythonDictCursorIterator>(module, "_DictCursorIterator")
        .def("__iter__", [](PythonDictCursorIterator &iterator) -> PythonDictCursorIterator & { return iterator; },
             nb::rv_policy::reference)
        .def("__next__", &PythonDictCursorIterator::next);

    nb::class_<PythonDictEntryView>(module, "_DictEntryView")
        .def("__len__", &PythonDictEntryView::size)
        .def("__iter__", &PythonDictEntryView::iter);

    nb::class_<PythonDictView>(module, "Dict")
        .def("__len__", &PythonDictView::size)
        .def("__getitem__", &PythonDictView::getitem)
        .def("__contains__", &PythonDictView::contains)
        .def("__iter__", &PythonDictView::iter)
        .def("get", &PythonDictView::get, "key"_a, "default"_a = nb::none())
        .def("keys", &PythonDictView::keys)
        .def("items", &PythonDictView::items)
        .def("values", &PythonDictView::values)
        .def("copy", &PythonDictView::copy);

    auto abc = nb::module_::import_("collections.abc");
    abc.attr("Mapping").attr("register")(module.attr("Dict"));
}

} // namespace bg::python
