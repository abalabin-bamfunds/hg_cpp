#pragma once

#include <bg/time_series/list/storage_representation.hpp>
#include <bg/time_series/set/input_view.hpp>
#include <bg/time_series/set/output_view.hpp>
#include <bg/time_series/storage_representation.hpp>
#include <bg/type/time_series_set_type.hpp>
#include <bg/util/polymorphic_value.hpp>

#include <memory>
#include <mutex>
#include <unordered_map>

namespace bg::details {

template <typename TimeSeries> class CppTimeSeriesSetStorageRepresentation;

template <typename K, auto... Properties>
class CppTimeSeriesSetStorageRepresentation<TSS<K, Properties...>> final : public TimeSeriesStorageRepresentation {
    using Storage = time_series_set_storage<K>;

  public:
    explicit CppTimeSeriesSetStorageRepresentation(const TimeSeriesSetType &type) noexcept : type_{&type} {}

    [[nodiscard]] const TimeSeriesType &type() const noexcept override { return *type_; }
    [[nodiscard]] std::size_t value_size() const noexcept override { return sizeof(Storage); }
    [[nodiscard]] std::size_t value_alignment() const noexcept override { return alignof(Storage); }
    [[nodiscard]] bool uses_variable_memory() const noexcept override { return true; }
    [[nodiscard]] bool requires_destruction() const noexcept override { return true; }
    [[nodiscard]] bool supports_byte_copy_initialization() const noexcept override { return true; }

    [[nodiscard]] const TimeSeriesInputView &input_view_prototype() const noexcept override {
        static const auto prototype =
            util::polymorphic_value<TimeSeriesInputView>::template make<TypedTimeSeriesSetInputView<K>>();
        return prototype.get();
    }

    [[nodiscard]] const TimeSeriesOutputView &output_view_prototype() const noexcept override {
        static const auto prototype =
            util::polymorphic_value<TimeSeriesOutputView>::template make<TypedTimeSeriesSetOutputView<K>>();
        return prototype.get();
    }

    void construct_initial_value(void *destination) const noexcept override {
        std::construct_at(static_cast<Storage *>(destination));
    }

    void destroy_value(void *value, std::pmr::memory_resource &memory) const noexcept override {
        auto *storage = static_cast<Storage *>(value);
        storage->release(memory, [](std::uint32_t) {});
        std::destroy_at(storage);
    }

  private:
    const TimeSeriesSetType *type_;
};

template <typename K, auto... Properties> struct CppTimeSeriesStorageRepresentation<TSS<K, Properties...>> {
    [[nodiscard]] static const TimeSeriesStorageRepresentation &get(const TimeSeriesType &type) noexcept {
        using TimeSeries = TSS<K, Properties...>;
        using Representation = CppTimeSeriesSetStorageRepresentation<TimeSeries>;

        static auto mutex = std::mutex{};
        static auto representations = std::unordered_map<const TimeSeriesType *, std::unique_ptr<Representation>>{};

        const auto lock = std::lock_guard{mutex};
        auto [entry, inserted] = representations.try_emplace(&type);
        if (inserted) {
            entry->second = std::make_unique<Representation>(static_cast<const TimeSeriesSetType &>(type));
        }
        return *entry->second;
    }
};

} // namespace bg::details
