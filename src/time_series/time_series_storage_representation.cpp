#include <bg/time_series/time_series_storage_representation.hpp>

#include <bg/scalar/storage_representation.hpp>
#include <bg/time_series/time_series_input_view.hpp>
#include <bg/time_series/time_series_output_view.hpp>
#include <bg/time_series/ref/ref_storage_representation.hpp>
#include <bg/time_series/tsb/tsb_storage_representation.hpp>
#include <bg/time_series/tsd/tsd_storage_representation.hpp>
#include <bg/time_series/tsl/tsl_storage_representation.hpp>
#include <bg/time_series/tss/tss_storage_representation.hpp>
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
namespace {

class ValueStorageRepresentation final : public TimeSeriesStorageRepresentation {
  public:
    ValueStorageRepresentation(const TimeSeriesType &type, const ScalarStorageRepresentation &scalar) noexcept
        : type_{&type}, scalar_{&scalar} {}

    [[nodiscard]] const TimeSeriesType &type() const noexcept override { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept override { return scalar_->size(); }
    [[nodiscard]] std::size_t value_alignment() const noexcept override { return scalar_->alignment(); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return scalar_->uses_variable_memory(); }
    [[nodiscard]] bool requires_finalization() const noexcept override { return scalar_->requires_finalization(); }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override {
        return scalar_->supports_byte_copy_initialization();
    }
    [[nodiscard]] const ScalarStorageRepresentation *scalar_storage_representation() const noexcept override {
        return scalar_;
    }
    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override {
        return *scalar_->input_view_prototype();
    }
    [[nodiscard]] const TimeSeriesOutputView *output_view_prototype() const noexcept override {
        return scalar_->output_view_prototype();
    }

    void construct_initial_value(void *destination) const noexcept override {
        scalar_->construct_initial_value(destination);
    }

    void construct_runtime_value(void *destination, std::pmr::memory_resource &memory) const override {
        scalar_->construct_runtime_value(destination, memory);
    }

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept override {
        scalar_->destroy(value, memory);
    }

  private:
    const TimeSeriesType *type_;
    const ScalarStorageRepresentation *scalar_;
};

struct ValueRepresentationKey {
    const TimeSeriesType *type;
    const ScalarStorageRepresentation *scalar;

    bool operator==(const ValueRepresentationKey &) const = default;
};

struct ValueRepresentationKeyHash {
    [[nodiscard]] std::size_t operator()(const ValueRepresentationKey &key) const noexcept {
        const auto type = std::hash<const TimeSeriesType *>{}(key.type);
        const auto scalar = std::hash<const ScalarStorageRepresentation *>{}(key.scalar);

        return type ^ (scalar << 1);
    }
};

} // namespace

TimeSeriesStorageRepresentation::FixedItem TimeSeriesStorageRepresentation::fixed_item(std::size_t) const {
    throw std::out_of_range{"time series has no fixed items"};
}

const TimeSeriesStorageRepresentation &
time_series_value_storage_representation(const TimeSeriesType &type,
                                         const ScalarStorageRepresentation &scalar_representation) {
    if (dynamic_cast<const TimeSeriesValueType *>(&type) == nullptr) {
        throw std::invalid_argument{"a value storage representation requires a time series value type"};
    }

    static auto mutex = std::mutex{};
    static auto representations =
        std::unordered_map<ValueRepresentationKey, std::unique_ptr<ValueStorageRepresentation>,
                           ValueRepresentationKeyHash>{};

    const auto lock = std::lock_guard{mutex};
    const auto key = ValueRepresentationKey{&type, &scalar_representation};
    auto [entry, inserted] = representations.try_emplace(key);
    if (inserted) {
        entry->second = std::make_unique<ValueStorageRepresentation>(type, scalar_representation);
    }

    return *entry->second;
}

const TimeSeriesStorageRepresentation &resolve_time_series_storage_representation(
    const TimeSeriesType &type,
    std::span<const TimeSeriesStorageRepresentation *const> available_time_series,
    std::span<const ScalarStorageRepresentation *const> available_scalars) {
    const auto supplied = std::ranges::find_if(available_time_series, [&](const auto *representation) {
        return representation != nullptr && &representation->type() == &type;
    });
    if (supplied != available_time_series.end()) {
        return **supplied;
    }

    auto scalar_representations = std::vector<const ScalarStorageRepresentation *>{available_scalars.begin(),
                                                                                   available_scalars.end()};
    for (const auto *representation : available_time_series) {
        if (representation != nullptr && representation->scalar_storage_representation() != nullptr) {
            scalar_representations.push_back(representation->scalar_storage_representation());
        }
    }

    if (const auto *value = dynamic_cast<const TimeSeriesValueType *>(&type)) {
        const auto &scalar = resolve_scalar_storage_representation(value->value_type(), scalar_representations);
        return time_series_value_storage_representation(type, scalar);
    }
    if (const auto *reference = dynamic_cast<const TimeSeriesReferenceType *>(&type)) {
        const auto &target = resolve_time_series_storage_representation(reference->target_type(),
                                                                        available_time_series,
                                                                        scalar_representations);
        return time_series_reference_storage_representation(*reference, target);
    }
    if (const auto *list = dynamic_cast<const TimeSeriesListType *>(&type)) {
        if (list->item_count() == std::dynamic_extent) {
            throw std::invalid_argument{"variable-size time series lists are not implemented"};
        }

        const auto &item = resolve_time_series_storage_representation(list->item_type(), available_time_series,
                                                                      scalar_representations);
        return details::fixed_time_series_list_storage_representation(*list, item);
    }
    if (const auto *set = dynamic_cast<const TimeSeriesSetType *>(&type)) {
        const auto &key = resolve_scalar_storage_representation(set->item_type(), scalar_representations);
        return details::time_series_set_storage_representation(*set, key);
    }
    if (const auto *dictionary = dynamic_cast<const TimeSeriesDictionaryType *>(&type)) {
        const auto &key = resolve_scalar_storage_representation(dictionary->key_type(), scalar_representations);
        const auto &item = resolve_time_series_storage_representation(dictionary->value_type(),
                                                                      available_time_series,
                                                                      scalar_representations);
        return details::time_series_dictionary_storage_representation(*dictionary, key, item);
    }
    if (const auto *bundle = dynamic_cast<const TimeSeriesBundleType *>(&type)) {
        auto items = std::vector<const TimeSeriesStorageRepresentation *>{};
        items.reserve(bundle->item_count());
        for (auto index = std::size_t{}; index < bundle->item_count(); ++index) {
            items.push_back(&resolve_time_series_storage_representation(bundle->item_type(index),
                                                                        available_time_series,
                                                                        scalar_representations));
        }
        return details::time_series_bundle_storage_representation(*bundle, items);
    }

    throw std::logic_error{"resolved time series type has no storage representation"};
}

} // namespace bg
