#include <bg/time_series/time_series_storage_representation.hpp>

#include <bg/scalar/storage_representation.hpp>
#include <bg/time_series/time_series_input_view.hpp>
#include <bg/time_series/time_series_output_view.hpp>
#include <bg/type/time_series_value_type.hpp>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

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
    [[nodiscard]] const TimeSeriesOutputView &output_view_prototype() const noexcept override {
        return *scalar_->output_view_prototype();
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

} // namespace bg
