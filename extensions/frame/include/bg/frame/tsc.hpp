#pragma once

#include <bg/frame/column.hpp>

#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/input_data.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/type/declaration_variables.hpp>
#include <bg/type/type_registry.hpp>
#include <bg/type/type_map.hpp>
#include <bg/node/output_declaration_traits.hpp>

namespace bg {

namespace frame {

template <cell_type T> class ColumnSeriesType final : public TimeSeriesType {
  public:
    explicit ColumnSeriesType(GuaranteeParameter guarantee) : TimeSeriesType{guarantee} {}

    bool is_resolved() const noexcept override { return guarantee().is_resolved(); }
    const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;
    const Type &resolve(const TypeMap &type_map) const override;

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override;
};

template <cell_type T> class ColumnSeriesRuntimeType : public TimeSeriesRuntimeType {
  public:
    virtual ColumnSeriesView<T> input_view(const void *value) const noexcept = 0;

  protected:
    ColumnSeriesRuntimeType(const TimeSeriesType &type, std::size_t size, std::size_t alignment,
                            bool uses_variable_memory, bool requires_finalization,
                            bool supports_byte_copy_initialization) noexcept
        : TimeSeriesRuntimeType{type, size, alignment, uses_variable_memory, requires_finalization,
                                supports_byte_copy_initialization} {}
};

template <cell_type T>
const TimeSeriesRuntimeType &column_binding_view_time_series_runtime_type(const TimeSeriesType &type);

} // namespace frame

template <frame::cell_type T, auto... Properties>
struct time_series_type_traits<frame::TSC<T, Properties...>> {
    using semantic_type = TimeSeriesType;
    static constexpr bool generic = false;
    static constexpr bool supported = true;

    template <typename Scope, std::size_t Index, bool Input>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        if (!guarantee.is_resolved()) {
            static const auto type = frame::ColumnSeriesType<T>{guarantee};
            [[maybe_unused]] static const auto &registered_type = TypeRegistry::register_static(type);
            return registered_type;
        }
        return frame::column_time_series_type<T>(*guarantee.value());
    }
};

template <frame::cell_type T, auto... Properties>
struct declaration_variable_traits<frame::TSC<T, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        TimeSeriesDeclarationProperties<Properties...>::for_each_variable(std::forward<Visitor>(visitor));
    }
};

template <frame::cell_type T, auto... Properties>
struct time_series_runtime_traits<frame::TSC<T, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        return frame::column_time_series_runtime_type<T>(type);
    }
};

namespace frame::details {
template <cell_type T> struct ColumnInputDescriptor {
    ColumnStorage<T> storage;
    bool valid{};
    bool modified{};
    EvaluationTime last_updated{MIN_DT};
};
} // namespace frame::details

template <frame::cell_type T, auto... SeriesProperties, auto... Properties>
class In<frame::TSC<T, SeriesProperties...>, Properties...>
    : public InputActivityControl<InputProperties<Properties...>::dynamic_activity> {
    using ParsedProperties = InputProperties<Properties...>;
    using ActivityControl = InputActivityControl<ParsedProperties::dynamic_activity>;

  public:
    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr) noexcept
        : ActivityControl{activity}, data_{data} {}

    bool valid() const noexcept { return data_.valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    std::size_t size() const noexcept { return valid() ? storage().size() : 0; }
    bool contains(frame::row_id_t id) const noexcept { return valid() && storage().contains(id); }
    std::optional<T> cell(frame::row_id_t id) const { return storage().cell(id); }
    const void *row_domain() const noexcept { return valid() ? storage().row_domain() : nullptr; }
    Scalar<frame::Column<T>> value_view() const noexcept {
        return Scalar<frame::Column<T>>::from(storage().scalar_storage());
    }
    template <typename Function> void for_each_row(Function &&function) const {
        if (!valid())
            return;
        auto position = std::size_t{};
        storage().for_each_batch([&](const auto &batch) {
            for (const auto id : batch.ids)
                std::invoke(function, id, position++);
        });
    }
    template <typename Function> void for_each_batch(Function &&function) const {
        if (valid())
            storage().for_each_batch(std::forward<Function>(function));
    }
    template <typename Function> void for_each_changed(Function &&function) const {
        if (!changes_current())
            return;
        for (auto id : storage().added())
            std::invoke(function, id);
        for (auto id : storage().modified())
            std::invoke(function, id);
    }
    template <typename Function> void for_each_removed(Function &&function) const {
        if (!changes_current())
            return;
        for (auto id : storage().removed())
            std::invoke(function, id);
    }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }

    frame::details::ColumnInputDescriptor<T> extension_descriptor() const noexcept {
        if (!valid())
            return {};
        return {storage().scalar_storage(), true, modified(), last_updated()};
    }

    static constexpr auto name = ParsedProperties::name;
    static constexpr auto activity = ParsedProperties::activity;
    static constexpr auto validity = ParsedProperties::validity;
    static constexpr auto dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr auto dynamic_activity = ParsedProperties::dynamic_activity;

    using time_series_type = frame::TSC<T, SeriesProperties...>;

  private:
    frame::ColumnSeriesView<T> storage() const noexcept {
        return static_cast<const frame::ColumnSeriesRuntimeType<T> &>(data_.runtime_type()).input_view(data_.data());
    }
    bool changes_current() const noexcept { return modified() && storage().change_time() == last_updated(); }
    TimeSeriesInputData data_;
};

template <frame::cell_type T, auto... Properties>
class Out<frame::TSC<T, Properties...>> {
  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}
    frame::row_id_t add(std::optional<T> value) const {
        const auto id = storage().add(value, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
        return id;
    }
    void set(frame::row_id_t id, std::optional<T> value) const {
        storage().set(id, value, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
    }
    bool erase(frame::row_id_t id) const {
        const auto erased = storage().erase(id, binding_->context().evaluation_time(), binding_->allocator());
        if (erased)
            binding_->publish();
        return erased;
    }
    void clear() const {
        if (!binding_->valid() || storage().size() != 0) {
            storage().clear(binding_->context().evaluation_time(), binding_->allocator());
            binding_->publish();
        }
    }
    void invalidate() const { binding_->invalidate(); }
    bool valid() const noexcept { return binding_->valid(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    using time_series_type = frame::TSC<T, Properties...>;

  private:
    frame::NativeColumn<T> &storage() const noexcept {
        return *static_cast<frame::NativeColumn<T> *>(binding_->mutable_data());
    }
    TimeSeriesOutputBinding binding_;
};

template <frame::cell_type T, auto... Properties>
class Out<frame::TSC<T, Properties...>, frame::view> {
  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    template <auto... SourceSeriesProperties, auto... InputProperties>
    bool bind(const In<frame::TSC<T, SourceSeriesProperties...>, InputProperties...> &source) const {
        const auto input = source.extension_descriptor();
        auto &output = storage();
        if (!input.valid) {
            if (output.valid)
                binding_->invalidate();
            return false;
        }

        const auto changed = output.bind(input.storage, input.modified, input.last_updated,
                                         binding_->context().evaluation_time(), binding_->allocator());
        if (changed)
            binding_->publish();
        return changed;
    }

    void invalidate() const { binding_->invalidate(); }
    bool valid() const noexcept { return binding_->valid(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    using time_series_type = frame::TSC<T, Properties...>;

  private:
    frame::ColumnViewOutputStorage<T> &storage() const noexcept {
        return *static_cast<frame::ColumnViewOutputStorage<T> *>(binding_->mutable_data());
    }
    TimeSeriesOutputBinding binding_;
};

} // namespace bg

namespace bg {

template <frame::cell_type T, auto... Properties>
struct output_declaration_traits<Out<frame::TSC<T, Properties...>, frame::view>> {
    using time_series_type = frame::TSC<T, Properties...>;

    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &type) {
        return frame::column_binding_view_time_series_runtime_type<T>(type);
    }
};

} // namespace bg
