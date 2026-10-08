#include <bg/frame/tsc.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_registry.hpp>

#include <cstdint>
#include <memory>

namespace {

template <bg::frame::cell_type T> class ColumnScalarType final : public bg::ScalarType {
  public:
    bool is_resolved() const noexcept override { return true; }
    const bg::Type &resolve(const bg::TypeMap &) const override { return *this; }
};

template <bg::frame::cell_type T>
class NativeColumnSeriesRuntimeType final : public bg::frame::ColumnSeriesRuntimeType<T> {
  public:
    explicit NativeColumnSeriesRuntimeType(const bg::TimeSeriesType &type)
        : bg::frame::ColumnSeriesRuntimeType<T>{type, sizeof(bg::frame::NativeColumn<T>),
                                                alignof(bg::frame::NativeColumn<T>), true, true, true} {}
    bg::frame::ColumnSeriesView<T> input_view(const void *value) const noexcept override {
        return static_cast<const bg::frame::NativeColumn<T> *>(value)->series_view();
    }
    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<bg::frame::NativeColumn<T> *>(destination));
    }
    void destroy_value(void *value, bg::allocator &memory) const noexcept override {
        auto *column = static_cast<bg::frame::NativeColumn<T> *>(value);
        column->release(memory);
        std::destroy_at(column);
    }
    void invalidate_output(const bg::OutputState &output) const override {
        auto *value = static_cast<bg::frame::NativeColumn<T> *>(output.mutable_data());
        value->clear(output.context().evaluation_time(), output.allocator());
        bg::TimeSeriesRuntimeType::invalidate_output_value(output);
    }
};

template <bg::frame::cell_type T>
class ColumnViewSeriesRuntimeType final : public bg::frame::ColumnSeriesRuntimeType<T> {
  public:
    explicit ColumnViewSeriesRuntimeType(const bg::TimeSeriesType &type)
        : bg::frame::ColumnSeriesRuntimeType<T>{type, sizeof(bg::frame::ColumnStorage<T>),
                                                alignof(bg::frame::ColumnStorage<T>), false, false, true} {}
    bg::frame::ColumnSeriesView<T> input_view(const void *value) const noexcept override {
        return bg::frame::ColumnSeriesView<T>{*static_cast<const bg::frame::ColumnStorage<T> *>(value)};
    }
    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<bg::frame::ColumnStorage<T> *>(destination));
    }
    void destroy_value(void *value, bg::allocator &) const noexcept override {
        std::destroy_at(static_cast<bg::frame::ColumnStorage<T> *>(value));
    }
};

template <bg::frame::cell_type T>
class ColumnBindingViewSeriesRuntimeType final : public bg::frame::ColumnSeriesRuntimeType<T> {
  public:
    explicit ColumnBindingViewSeriesRuntimeType(const bg::TimeSeriesType &type)
        : bg::frame::ColumnSeriesRuntimeType<T>{type, sizeof(bg::frame::ColumnViewOutputStorage<T>),
                                                alignof(bg::frame::ColumnViewOutputStorage<T>), true, true, true} {}
    bg::frame::ColumnSeriesView<T> input_view(const void *value) const noexcept override {
        return bg::frame::ColumnSeriesView<T>{static_cast<const bg::frame::ColumnViewOutputStorage<T> *>(value)->scalar};
    }
    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<bg::frame::ColumnViewOutputStorage<T> *>(destination));
    }
    void destroy_value(void *value, bg::allocator &memory) const noexcept override {
        auto *storage = static_cast<bg::frame::ColumnViewOutputStorage<T> *>(value);
        storage->release(memory);
        std::destroy_at(storage);
    }
    void invalidate_output(const bg::OutputState &output) const override {
        auto *storage = static_cast<bg::frame::ColumnViewOutputStorage<T> *>(output.mutable_data());
        storage->invalidate();
        bg::TimeSeriesRuntimeType::invalidate_output_value(output);
    }
};

template <bg::frame::cell_type T>
const bg::frame::ColumnSeriesType<T> &ordinary_column_series_type() {
    static const auto type = bg::frame::ColumnSeriesType<T>{bg::TimeSeriesGuarantee::ordinary};
    [[maybe_unused]] static const auto &registered_type = bg::TypeRegistry::register_static(type);
    return registered_type;
}

template <bg::frame::cell_type T>
const NativeColumnSeriesRuntimeType<T> &ordinary_column_series_runtime_type() {
    static const auto runtime = NativeColumnSeriesRuntimeType<T>{ordinary_column_series_type<T>()};
    return runtime;
}

template <bg::frame::cell_type T>
const ColumnViewSeriesRuntimeType<T> &column_view_series_runtime_type() {
    static const auto runtime = ColumnViewSeriesRuntimeType<T>{ordinary_column_series_type<T>()};
    return runtime;
}

template <bg::frame::cell_type T>
const ColumnBindingViewSeriesRuntimeType<T> &column_binding_view_series_runtime_type() {
    static const auto runtime = ColumnBindingViewSeriesRuntimeType<T>{ordinary_column_series_type<T>()};
    return runtime;
}

} // namespace

template <bg::frame::cell_type T>
const bg::TimeSeriesType &bg::frame::ColumnSeriesType<T>::with_guarantee(bg::TimeSeriesGuarantee guarantee) const {
    return column_time_series_type<T>(guarantee);
}

template <bg::frame::cell_type T>
const bg::Type &bg::frame::ColumnSeriesType<T>::resolve(const bg::TypeMap &type_map) const {
    return column_time_series_type<T>(guarantee().resolve(type_map));
}

template <bg::frame::cell_type T>
bool bg::frame::ColumnSeriesType<T>::do_match(const bg::Type &supplied, bg::TypeMap &type_map) const {
    const auto *column = dynamic_cast<const ColumnSeriesType<T> *>(&supplied);
    if (column == nullptr || !column->is_resolved())
        return false;

    auto trial = type_map.overlay();
    if (!guarantee().match(*column->guarantee().value(), trial))
        return false;
    type_map.apply(std::move(trial));
    return true;
}

namespace bg {

template <frame::cell_type T> const ScalarType &scalar_type_traits<frame::Column<T>>::type() noexcept {
    static const ColumnScalarType<T> type;
    [[maybe_unused]] static const auto &registered_type = TypeRegistry::register_static(type);
    return registered_type;
}

template const ScalarType &scalar_type_traits<frame::Column<i64>>::type() noexcept;
template const ScalarType &scalar_type_traits<frame::Column<double>>::type() noexcept;
template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::Column<i64>>();
template const ScalarRuntimeType &cpp_scalar_runtime_type<frame::Column<double>>();

template <frame::cell_type T> const TimeSeriesType &frame::column_time_series_type(TimeSeriesGuarantee guarantee) {
    if (guarantee != TimeSeriesGuarantee::ordinary)
        throw std::invalid_argument{"frame TSC requires ordinary"};
    const auto &type = ordinary_column_series_type<T>();
    register_time_series_runtime_type(ordinary_column_series_runtime_type<T>());
    return type;
}

template <frame::cell_type T>
const TimeSeriesRuntimeType &frame::column_time_series_runtime_type(const TimeSeriesType &type) {
    if (&type != &ordinary_column_series_type<T>())
        throw std::invalid_argument{"frame TSC runtime requires resolved ordinary type"};

    return ordinary_column_series_runtime_type<T>();
}

template <frame::cell_type T>
const TimeSeriesRuntimeType &frame::column_view_time_series_runtime_type(const TimeSeriesType &type) {
    if (&type != &ordinary_column_series_type<T>())
        throw std::invalid_argument{"frame TSC view runtime requires resolved ordinary type"};

    return column_view_series_runtime_type<T>();
}

template <frame::cell_type T>
const TimeSeriesRuntimeType &frame::column_binding_view_time_series_runtime_type(const TimeSeriesType &type) {
    if (&type != &ordinary_column_series_type<T>())
        throw std::invalid_argument{"frame TSC binding view runtime requires resolved ordinary type"};

    return column_binding_view_series_runtime_type<T>();
}

template class frame::ColumnSeriesType<i64>;
template class frame::ColumnSeriesType<double>;
template const TimeSeriesType &frame::column_time_series_type<i64>(TimeSeriesGuarantee);
template const TimeSeriesType &frame::column_time_series_type<double>(TimeSeriesGuarantee);
template const TimeSeriesRuntimeType &frame::column_time_series_runtime_type<i64>(const TimeSeriesType &);
template const TimeSeriesRuntimeType &frame::column_time_series_runtime_type<double>(const TimeSeriesType &);
template const TimeSeriesRuntimeType &frame::column_view_time_series_runtime_type<i64>(const TimeSeriesType &);
template const TimeSeriesRuntimeType &frame::column_view_time_series_runtime_type<double>(const TimeSeriesType &);
template const TimeSeriesRuntimeType &frame::column_binding_view_time_series_runtime_type<i64>(const TimeSeriesType &);
template const TimeSeriesRuntimeType &frame::column_binding_view_time_series_runtime_type<double>(const TimeSeriesType &);

} // namespace bg
