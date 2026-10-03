#include <bg/time_series/time_series_runtime_type.hpp>

#include <bg/scalar/runtime_type.hpp>
#include <bg/time_series/details/output_state.hpp>
#include <bg/time_series/ref/ref_provider.hpp>
#include <bg/time_series/ref/ref_runtime_type.hpp>
#include <bg/time_series/tsb/tsb_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/time_series/tsl/tsl_runtime_type.hpp>
#include <bg/time_series/tss/tss_runtime_type.hpp>
#include <bg/type/time_series_bundle_type.hpp>
#include <bg/type/time_series_dict_type.hpp>
#include <bg/type/time_series_list_type.hpp>
#include <bg/type/time_series_reference_type.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace bg {

TimeSeriesRuntimeType::~TimeSeriesRuntimeType() = default;

namespace {

auto &registered_runtime_types() {
    static auto types = std::unordered_map<const TimeSeriesType *, const TimeSeriesRuntimeType *>{};
    return types;
}

auto &registered_runtime_types_mutex() {
    static auto mutex = std::mutex{};
    return mutex;
}

void validate_registered_runtime_type(const TimeSeriesRuntimeType &runtime_type) {
    auto valid = true;
    runtime_type.type().visit(
        [&](const TimeSeriesValueType *type) {
            const auto *value = dynamic_cast<const TimeSeriesValueRuntimeType *>(&runtime_type);
            valid = value != nullptr && &value->scalar_runtime_type().type() == &type->value_type();
        },
        [&](const TimeSeriesSetType *type) {
            const auto *set = dynamic_cast<const details::TimeSeriesSetRuntimeType *>(&runtime_type);
            valid = set != nullptr && &set->key_runtime_type().type() == &type->item_type();
        },
        [&](const TimeSeriesDictionaryType *type) {
            const auto *dictionary = dynamic_cast<const details::TimeSeriesDictionaryRuntimeType *>(&runtime_type);
            valid = dictionary != nullptr && &dictionary->key_runtime_type().type() == &type->key_type() &&
                    &dictionary->item_runtime_type().type() == &type->value_type();
        },
        ddv::noop);
    if (!valid) {
        throw std::invalid_argument{"the runtime type does not support its builtin time series semantic type"};
    }
}

bool has_matching_runtime_component(const TimeSeriesType &type,
                                    std::span<const TimeSeriesRuntimeType *const> available_time_series,
                                    std::span<const ScalarRuntimeType *const> available_scalars) {
    if (std::ranges::any_of(available_time_series, [&](const auto *runtime_type) {
            return runtime_type != nullptr && &runtime_type->type() == &type;
        })) {
        return true;
    }

    const auto has_scalar = [&](const ScalarType &scalar) {
        if (std::ranges::any_of(available_scalars, [&](const auto *runtime_type) {
                return runtime_type != nullptr && &runtime_type->type() == &scalar;
            })) {
            return true;
        }
        return std::ranges::any_of(available_time_series, [&](const auto *runtime_type) {
            const auto *value = runtime_type == nullptr ? nullptr : type_cast<TimeSeriesValueType>(&runtime_type->type());
            return value != nullptr && &value->value_type() == &scalar;
        });
    };

    auto result = false;
    type.visit(
        [&](const TimeSeriesValueType *value) { result = has_scalar(value->value_type()); },
        [&](const TimeSeriesListType *list) {
            result = has_matching_runtime_component(list->item_type(), available_time_series, available_scalars);
        },
        [&](const TimeSeriesSetType *set) { result = has_scalar(set->item_type()); },
        [&](const TimeSeriesDictionaryType *dictionary) {
            result = has_scalar(dictionary->key_type()) ||
                     has_matching_runtime_component(dictionary->value_type(), available_time_series, available_scalars);
        },
        [&](const TimeSeriesBundleType *bundle) {
            for (auto index = std::size_t{}; !result && index < bundle->item_count(); ++index) {
                result = has_matching_runtime_component(bundle->item_type(index), available_time_series, available_scalars);
            }
        },
        ddv::noop);
    return result;
}

class ValueRuntimeType final : public TimeSeriesValueRuntimeType {
  public:
    ValueRuntimeType(const TimeSeriesType &type, const ScalarRuntimeType &scalar) noexcept
        : TimeSeriesValueRuntimeType{type,
                                     scalar,
                                    scalar.size(),
                                    scalar.alignment(),
                                    scalar.uses_variable_memory(),
                                    scalar.requires_finalization(),
                                    scalar.supports_byte_copy_initialization()} {}

    void construct_initial_value(void *destination) const noexcept override {
        scalar_runtime_type().construct_initial_value(destination);
    }

    void construct_runtime_value(void *destination, allocator &memory) const override {
        scalar_runtime_type().construct_runtime_value(destination, memory);
    }

    void destroy_value(void *value, allocator &memory) const noexcept override {
        scalar_runtime_type().destroy(value, memory);
    }
};

struct ValueRuntimeTypeKey {
    const TimeSeriesType *type;
    const ScalarRuntimeType *scalar;

    bool operator==(const ValueRuntimeTypeKey &) const = default;
};

struct ValueRuntimeTypeKeyHash {
    std::size_t operator()(const ValueRuntimeTypeKey &key) const noexcept {
        const auto type = std::hash<const TimeSeriesType *>{}(key.type);
        const auto scalar = std::hash<const ScalarRuntimeType *>{}(key.scalar);

        return type ^ (scalar << 1);
    }
};

} // namespace

void register_time_series_runtime_type(const TimeSeriesRuntimeType &runtime_type) {
    if (!runtime_type.type().is_resolved()) {
        throw std::invalid_argument{"a default time series runtime type requires a resolved semantic type"};
    }
    validate_registered_runtime_type(runtime_type);

    const auto lock = std::lock_guard{registered_runtime_types_mutex()};
    const auto [entry, inserted] = registered_runtime_types().try_emplace(&runtime_type.type(), &runtime_type);
    if (!inserted && entry->second != &runtime_type) {
        throw std::invalid_argument{"a semantic time series type already has a different default runtime type"};
    }
}

TimeSeriesRuntimeType::ItemLayout TimeSeriesRuntimeType::item_layout(item_id_t) const {
    throw std::out_of_range{"time series has no packed item layout"};
}

void TimeSeriesRuntimeType::invalidate_output(const details::OutputState &output) const {
    invalidate_output_value(output);
}

void TimeSeriesRuntimeType::invalidate_output_value(const details::OutputState &output) noexcept {
    output.invalidate_value();
}

void TimeSeriesRuntimeType::invalidate_output_items(const details::OutputState &output) {
    auto *context = const_cast<details::OutputState *>(&output);
    output.runtime_type().for_each_item_id(output.data(), context, [](void *context, item_id_t id) {
        const auto &output = *static_cast<const details::OutputState *>(context);
        auto *bytes = static_cast<std::byte *>(output.mutable_data());
        const auto field = output.runtime_type().item_layout(id);
        auto *path = output.add_path(output.runtime_type(), id);
        auto child = details::OutputState{field.runtime_type};
        child.bind(*reinterpret_cast<TimeSeriesState *>(bytes + field.last_updated_offset), bytes + field.value_offset,
                   path);
        child.invalidate();
        child.release_path();
    });
}

std::size_t TimeSeriesRuntimeType::item_count(const void *) const {
    return 0;
}

TimeSeriesRuntimeType::ConstItem TimeSeriesRuntimeType::item(const void *, item_id_t) const {
    throw std::out_of_range{"time series has no items"};
}

TimeSeriesRuntimeType::MutableItem TimeSeriesRuntimeType::item(void *, item_id_t) const {
    throw std::out_of_range{"time series has no mutable items"};
}

void TimeSeriesRuntimeType::for_each_item_id(const void *, void *, ItemIdFunction) const {}

bool TimeSeriesRuntimeType::item_updated(const TimeSeriesState &, const void *, item_id_t) const noexcept {
    return false;
}

void TimeSeriesRuntimeType::for_each_updated_item(const TimeSeriesState &, const void *, void *, ItemIdFunction) const {}

Reference TimeSeriesRuntimeType::reference(const void *) const {
    throw std::logic_error{"time series does not contain a reference address"};
}

const TimeSeriesRuntimeType &time_series_value_runtime_type(const TimeSeriesType &type,
                                                            const ScalarRuntimeType &scalar_runtime_type) {
    auto value_type = false;
    type.visit([&](const TimeSeriesValueType *) { value_type = true; }, ddv::noop);
    if (!value_type) {
        throw std::invalid_argument{"a value runtime type requires a time series value type"};
    }

    static auto mutex = std::mutex{};
    static auto runtime_types =
        std::unordered_map<ValueRuntimeTypeKey, std::unique_ptr<ValueRuntimeType>, ValueRuntimeTypeKeyHash>{};

    const auto lock = std::lock_guard{mutex};
    const auto key = ValueRuntimeTypeKey{&type, &scalar_runtime_type};
    auto [entry, inserted] = runtime_types.try_emplace(key);
    if (inserted) {
        entry->second = std::make_unique<ValueRuntimeType>(type, scalar_runtime_type);
    }

    return *entry->second;
}

const TimeSeriesRuntimeType &
resolve_time_series_runtime_type(const TimeSeriesType &type,
                                 std::span<const TimeSeriesRuntimeType *const> available_time_series,
                                 std::span<const ScalarRuntimeType *const> available_scalars) {
    const auto supplied = std::ranges::find_if(available_time_series, [&](const auto *runtime_type) {
        return runtime_type != nullptr && &runtime_type->type() == &type;
    });
    if (supplied != available_time_series.end()) {
        return **supplied;
    }

    if (!has_matching_runtime_component(type, available_time_series, available_scalars)) {
        const auto lock = std::lock_guard{registered_runtime_types_mutex()};
        if (const auto registered = registered_runtime_types().find(&type);
            registered != registered_runtime_types().end()) {
            return *registered->second;
        }
    }

    auto scalar_runtime_types =
        std::vector<const ScalarRuntimeType *>{available_scalars.begin(), available_scalars.end()};
    for (const auto *runtime_type : available_time_series) {
        if (runtime_type == nullptr) {
            continue;
        }
        runtime_type->type().visit(
            [&](const TimeSeriesValueType *) {
                scalar_runtime_types.push_back(
                    &static_cast<const TimeSeriesValueRuntimeType &>(*runtime_type).scalar_runtime_type());
            },
            ddv::noop);
    }

    const TimeSeriesRuntimeType *result{};
    type.visit(
        [&](const TimeSeriesValueType *value) {
            const auto &scalar = resolve_scalar_runtime_type(value->value_type(), scalar_runtime_types);
            result = &time_series_value_runtime_type(type, scalar);
        },
        [&](const TimeSeriesReferenceType *reference) {
            result = &time_series_reference_runtime_type(*reference);
        },
        [&](const TimeSeriesListType *list) {
            if (list->item_count() == std::dynamic_extent) {
                throw std::invalid_argument{"variable-size time series lists are not implemented"};
            }

            const auto &item =
                resolve_time_series_runtime_type(list->item_type(), available_time_series, scalar_runtime_types);
            result = &details::fixed_time_series_list_runtime_type(*list, item);
        },
        [&](const TimeSeriesSetType *set) {
            const auto &key = resolve_scalar_runtime_type(set->item_type(), scalar_runtime_types);
            result = &details::time_series_set_runtime_type(*set, key);
        },
        [&](const TimeSeriesDictionaryType *dictionary) {
            const auto &key = resolve_scalar_runtime_type(dictionary->key_type(), scalar_runtime_types);
            const auto &item =
                resolve_time_series_runtime_type(dictionary->value_type(), available_time_series, scalar_runtime_types);
            result = &details::time_series_dictionary_runtime_type(*dictionary, key, item);
        },
        [&](const TimeSeriesBundleType *bundle) {
            auto items = std::vector<const TimeSeriesRuntimeType *>{};
            items.reserve(bundle->item_count());
            for (auto index = std::size_t{}; index < bundle->item_count(); ++index) {
                items.push_back(&resolve_time_series_runtime_type(bundle->item_type(index), available_time_series,
                                                                  scalar_runtime_types));
            }
            result = &details::time_series_bundle_runtime_type(*bundle, items);
        },
        ddv::noop);
    if (result != nullptr) {
        return *result;
    }

    const auto lock = std::lock_guard{registered_runtime_types_mutex()};
    if (const auto registered = registered_runtime_types().find(&type); registered != registered_runtime_types().end()) {
        return *registered->second;
    }
    throw std::logic_error{"resolved time series type has no runtime type"};
}

} // namespace bg
