#include <bg/frame/tsc.hpp>

#include <bg/type/type_map.hpp>
#include <bg/type/type_resolution_error.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/util/symbol.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace {

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

struct ColumnSeriesTypeKey {
    const bg::ScalarType *cell_type;
    std::variant<bg::TimeSeriesGuarantee, const bg::GuaranteeVariable *> guarantee;

    bool operator==(const ColumnSeriesTypeKey &) const = default;
};

struct ColumnSeriesTypeKeyHash {
    std::size_t operator()(const ColumnSeriesTypeKey &key) const noexcept {
        const auto guarantee_hash = std::visit(
            [](const auto &guarantee) {
                using Guarantee = std::remove_cvref_t<decltype(guarantee)>;
                if constexpr (std::same_as<Guarantee, bg::TimeSeriesGuarantee>)
                    return std::hash<int>{}(static_cast<int>(guarantee));
                else
                    return std::hash<const bg::GuaranteeVariable *>{}(guarantee);
            },
            key.guarantee);
        return std::hash<const bg::ScalarType *>{}(key.cell_type) ^ (guarantee_hash << 1U);
    }
};

ColumnSeriesTypeKey column_series_type_key(const bg::ScalarType &cell_type, bg::GuaranteeParameter guarantee) {
    if (const auto value = guarantee.value())
        return {&cell_type, *value};
    if (const auto *variable = guarantee.variable())
        return {&cell_type, variable};
    throw std::invalid_argument{"a frame column time series requires a guarantee"};
}

template <bg::frame::cell_type T>
const bg::frame::ColumnSeriesType &ordinary_column_series_type() {
    return bg::frame::column_series_type(bg::cpp_scalar_type<T>(), bg::TimeSeriesGuarantee::ordinary);
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

const bg::frame::ColumnType &bg::frame::column_type(const bg::ScalarType &cell_type) {
    return bg::TypeRegistry::instance().get_or_create<bg::frame::ColumnType, std::hash<const bg::ScalarType *>>(
        &cell_type, [&] { return std::make_unique<bg::frame::ColumnType>(cell_type); });
}

const bg::Type &bg::frame::ColumnType::resolve(const bg::TypeMap &type_map) const {
    const auto &resolved_cell_type = cell_type().resolve(type_map);
    const auto *scalar_type = bg::type_cast<bg::ScalarType>(&resolved_cell_type);
    if (scalar_type == nullptr) {
        throw bg::TypeResolutionError{bg::TypeResolutionErrorCode::invalid_type_map, bg::util::symbol{"Column"},
                                      "column cell type did not resolve to a scalar type"};
    }
    return bg::frame::column_type(*scalar_type);
}

bool bg::frame::ColumnType::do_match(const bg::Type &supplied, bg::TypeMap &type_map) const {
    const auto *column = dynamic_cast<const ColumnType *>(&supplied);
    if (column == nullptr || !column->is_resolved())
        return false;

    auto trial = type_map.overlay();
    if (!cell_type().match(column->cell_type(), trial))
        return false;
    type_map.apply(std::move(trial));
    return true;
}

const bg::frame::ColumnSeriesType &bg::frame::column_series_type(const bg::ScalarType &cell_type,
                                                                 bg::GuaranteeParameter guarantee) {
    const auto key = column_series_type_key(cell_type, guarantee);
    return bg::TypeRegistry::instance().get_or_create<bg::frame::ColumnSeriesType, ColumnSeriesTypeKeyHash>(
        key, [&] { return std::make_unique<bg::frame::ColumnSeriesType>(cell_type, guarantee); });
}

const bg::TimeSeriesType &bg::frame::ColumnSeriesType::with_guarantee(bg::TimeSeriesGuarantee guarantee) const {
    if (guarantee != bg::TimeSeriesGuarantee::ordinary)
        throw std::invalid_argument{"frame TSC requires ordinary"};
    return column_series_type(cell_type(), guarantee);
}

const bg::Type &bg::frame::ColumnSeriesType::resolve(const bg::TypeMap &type_map) const {
    const auto &resolved_cell_type = cell_type().resolve(type_map);
    const auto *scalar_type = bg::type_cast<bg::ScalarType>(&resolved_cell_type);
    if (scalar_type == nullptr) {
        throw bg::TypeResolutionError{bg::TypeResolutionErrorCode::invalid_type_map, bg::util::symbol{"TSC"},
                                      "column time series cell type did not resolve to a scalar type"};
    }
    const auto resolved_guarantee = guarantee().resolve(type_map);
    if (resolved_guarantee != bg::TimeSeriesGuarantee::ordinary)
        throw std::invalid_argument{"frame TSC requires ordinary"};
    return column_series_type(*scalar_type, resolved_guarantee);
}

bool bg::frame::ColumnSeriesType::do_match(const bg::Type &supplied, bg::TypeMap &type_map) const {
    const auto *column = dynamic_cast<const ColumnSeriesType *>(&supplied);
    if (column == nullptr || !column->is_resolved())
        return false;

    auto trial = type_map.overlay();
    if (!cell_type().match(column->cell_type(), trial) || !guarantee().match(*column->guarantee().value(), trial))
        return false;

    type_map.apply(std::move(trial));
    return true;
}

namespace bg {

template <frame::cell_type T> const ScalarType &scalar_type_traits<frame::Column<T>>::type() {
    return frame::column_type(cpp_scalar_type<T>());
}

template const ScalarType &scalar_type_traits<frame::Column<i64>>::type();
template const ScalarType &scalar_type_traits<frame::Column<double>>::type();
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

template const TimeSeriesType &frame::column_time_series_type<i64>(TimeSeriesGuarantee);
template const TimeSeriesType &frame::column_time_series_type<double>(TimeSeriesGuarantee);
template const TimeSeriesRuntimeType &frame::column_time_series_runtime_type<i64>(const TimeSeriesType &);
template const TimeSeriesRuntimeType &frame::column_time_series_runtime_type<double>(const TimeSeriesType &);
template const TimeSeriesRuntimeType &frame::column_view_time_series_runtime_type<i64>(const TimeSeriesType &);
template const TimeSeriesRuntimeType &frame::column_view_time_series_runtime_type<double>(const TimeSeriesType &);
template const TimeSeriesRuntimeType &frame::column_binding_view_time_series_runtime_type<i64>(const TimeSeriesType &);
template const TimeSeriesRuntimeType &frame::column_binding_view_time_series_runtime_type<double>(const TimeSeriesType &);

} // namespace bg
