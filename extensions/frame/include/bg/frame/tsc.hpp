#pragma once

#include <bg/frame/column.hpp>
#include <bg/frame/literal.hpp>

#include <bg/scalar/declaration.hpp>
#include <bg/node/input_fwd.hpp>
#include <bg/node/input_properties.hpp>
#include <bg/time_series/cpp_runtime_type.hpp>
#include <bg/time_series/input_data.hpp>
#include <bg/time_series/output_binding.hpp>
#include <bg/time_series/time_series_type.hpp>
#include <bg/time_series/time_series_output.hpp>
#include <bg/type/declaration_variables.hpp>
#include <bg/type/type_map.hpp>
#include <bg/node/output_declaration_traits.hpp>

namespace bg {

namespace frame {

template <template <template <typename, auto...> typename> typename Schema, auto... Properties>
struct TSF;

// Extension descriptor for TSC<T>; node declarations use TSC<T> and do not access this type directly.
class ColumnSeriesType final : public TimeSeriesType {
  public:
    ColumnSeriesType(const ScalarType &cell_type, GuaranteeParameter guarantee) noexcept
        : TimeSeriesType{guarantee}, cell_type_{cell_type} {}

    void format_name(std::string &result) const override {
        result += "TSC<";
        cell_type().format_name(result);
        append_guarantee(result);
        result += '>';
    }
    const ScalarType &cell_type() const noexcept { return cell_type_; }
    bool is_resolved() const noexcept override { return cell_type_.is_resolved() && guarantee().is_resolved(); }
    const TimeSeriesType &with_guarantee(TimeSeriesGuarantee guarantee) const override;
    const Type &resolve(const TypeMap &type_map) const override;

  protected:
    bool do_match(const Type &supplied, TypeMap &type_map) const override;

  private:
    const ScalarType &cell_type_;
};

// Extension factory for semantic descriptors used during type resolution.
const ColumnSeriesType &column_series_type(const ScalarType &cell_type, GuaranteeParameter guarantee);

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
const TimeSeriesRuntimeType &column_time_series_runtime_type(const TimeSeriesType &type);
const TimeSeriesRuntimeType &column_view_time_series_runtime_type(const TimeSeriesType &type);
const TimeSeriesRuntimeType &column_binding_view_time_series_runtime_type(const TimeSeriesType &type);
template <cell_type T>
const TimeSeriesRuntimeType &column_computed_time_series_runtime_type(const TimeSeriesType &type);
const TimeSeriesRuntimeType &column_computed_time_series_runtime_type(const TimeSeriesType &type);

} // namespace frame

template <typename T, auto... Properties>
struct time_series_type_traits<frame::TSC<T, Properties...>> {
    using semantic_type = TimeSeriesType;
    static constexpr bool generic = details::scalar_variable<T>;
    static constexpr bool supported = frame::cell_type<T> || details::scalar_variable<T>;

    template <typename Scope, std::size_t Index, TimeSeriesTypeRole Role>
    static const TimeSeriesType &get(GuaranteeParameter guarantee) {
        if constexpr (frame::cell_type<T>) {
            if (guarantee.is_resolved())
                return frame::column_time_series_type<T>(*guarantee.value());
            return frame::column_series_type(cpp_scalar_type<T>(), guarantee);
        } else {
            return frame::column_series_type(scalar_type_traits<T>::type(), guarantee);
        }
    }
};

template <typename T, auto... Properties>
struct declaration_variable_traits<frame::TSC<T, Properties...>> {
    template <typename Visitor> static void for_each(Visitor &&visitor) {
        declaration_variable_traits<T>::for_each(visitor);
        TimeSeriesDeclarationProperties<Properties...>::for_each_variable(std::forward<Visitor>(visitor));
    }
};

template <typename T, auto... Properties>
struct time_series_runtime_traits<frame::TSC<T, Properties...>> {
    static const TimeSeriesRuntimeType &get(const TimeSeriesType &type) {
        if constexpr (frame::cell_type<T>)
            return frame::column_time_series_runtime_type<T>(type);
        else
            return frame::column_time_series_runtime_type(type);
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
    bool all_valid() const noexcept { return valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const TimeSeriesInputData &input_data() const noexcept { return data_; }
    std::size_t size() const noexcept { return valid() ? storage().size() : 0; }
    bool contains(frame::row_id_t id) const noexcept { return valid() && storage().contains(id); }
    std::optional<frame::cell_value_t<T>> cell(frame::row_id_t id) const { return storage().cell(id); }
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

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_}.visit(std::forward<Functions>(functions)...);
    }

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
    bool changes_current() const noexcept {
        return valid() && modified() && storage().change_time() == last_updated();
    }
    TimeSeriesInputData data_;
};

template <details::scalar_variable T, auto... SeriesProperties, auto... Properties>
class In<frame::TSC<T, SeriesProperties...>, Properties...>
    : public InputActivityControl<InputProperties<Properties...>::dynamic_activity> {
    using ParsedProperties = InputProperties<Properties...>;
    using ActivityControl = InputActivityControl<ParsedProperties::dynamic_activity>;

  public:
    explicit In(const TimeSeriesInputData &data, bool *activity = nullptr) noexcept
        : ActivityControl{activity}, data_{data} {}

    bool valid() const noexcept { return data_.valid(); }
    bool all_valid() const noexcept { return valid(); }
    bool became_valid() const noexcept { return data_.became_valid(); }
    bool became_invalid() const noexcept { return data_.became_invalid(); }
    bool modified() const noexcept { return data_.modified(); }
    EvaluationTime last_updated() const noexcept { return data_.last_updated(); }
    const TimeSeriesInputData &input_data() const noexcept { return data_; }
    std::size_t size() const noexcept { return valid() ? cell_count() : 0; }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return data_.runtime_type(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        return In<TS_T>{data_}.visit(std::forward<Functions>(functions)...);
    }

    template <frame::cell_type Cell>
    auto input() const {
        check_cell_type<Cell>();
        auto *activity = ActivityControl::activity();
        return In<frame::TSC<Cell, SeriesProperties...>, Properties...>{data_, activity};
    }

    template <frame::cell_type Cell>
    frame::ColumnView<Cell> view() const {
        return input<Cell>().value_view().value();
    }

    Scalar<frame::Column<T>> value_view() const {
        const auto &type = column_type();
        return frame::visit_cell_type(type.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
            const auto value = input<Cell>().value_view();
            return Scalar<frame::Column<T>>::from(value);
        });
    }

    frame::ColumnView<> column_view() const {
        const auto &type = column_type();
        return frame::visit_cell_type(type.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
            return frame::ColumnView<>{view<Cell>()};
        });
    }

    static constexpr auto name = ParsedProperties::name;
    static constexpr auto activity = ParsedProperties::activity;
    static constexpr auto validity = ParsedProperties::validity;
    static constexpr auto dynamic_binding = ParsedProperties::dynamic_binding;
    static constexpr auto dynamic_activity = ParsedProperties::dynamic_activity;
    using time_series_type = frame::TSC<T, SeriesProperties...>;

  private:
    const frame::ColumnSeriesType &column_type() const noexcept {
        return static_cast<const frame::ColumnSeriesType &>(data_.runtime_type().type());
    }

    std::size_t cell_count() const noexcept {
        const auto &type = column_type();
        return frame::visit_cell_type(type.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
            return static_cast<const frame::ColumnSeriesRuntimeType<Cell> &>(data_.runtime_type())
                .input_view(data_.data())
                .size();
        });
    }

    template <frame::cell_type Cell> void check_cell_type() const {
        if (std::addressof(column_type().cell_type()) != std::addressof(cpp_scalar_type<Cell>()))
            throw std::invalid_argument{"frame TSC input cell type does not match the requested type"};
    }

    TimeSeriesInputData data_;
};

template <frame::cell_type T, auto... Properties>
class Out<frame::TSC<T, Properties...>> {
  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}
    void apply(const literal::TimeSeriesLiteral &literal) const {
        frame::details::apply_series_literal(literal, binding_.get());
    }
    frame::row_id_t add(std::optional<frame::cell_value_t<T>> value) const {
        const auto id = storage().add(value, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
        return id;
    }
    void set(frame::row_id_t id, std::optional<frame::cell_value_t<T>> value) const {
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
    EvaluationTime last_updated() const noexcept { return binding_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    template <typename Input, auto... InputProperties>
    void copy(const In<Input, InputProperties...> &input) const {
        binding_->runtime_type().copy(input.input_data(), binding_.get());
    }

    template <typename Input, auto... InputProperties>
    void copy_tick(const In<Input, InputProperties...> &input) const {
        binding_->runtime_type().copy_tick(input.input_data(), binding_.get());
    }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(binding_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = frame::TSC<T, Properties...>;

  private:
    frame::NativeColumn<T> &storage() const noexcept {
        return *static_cast<frame::NativeColumn<T> *>(binding_->mutable_data());
    }
    TimeSeriesOutputBinding binding_;
};

template <details::scalar_variable T, auto... Properties>
class Out<frame::TSC<T, Properties...>> {
  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    void apply(const literal::TimeSeriesLiteral &literal) const {
        frame::details::apply_series_literal(literal, binding_.get());
    }

    template <frame::cell_type Cell>
        requires std::same_as<Cell, frame::cell_value_t<Cell>>
    frame::row_id_t add(std::optional<Cell> value) const {
        const auto id = storage<Cell>().add(value, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
        return id;
    }

    template <frame::cell_type Cell>
        requires(!std::same_as<Cell, frame::cell_value_t<Cell>>)
    frame::row_id_t add(std::optional<frame::cell_value_t<Cell>> value) const {
        const auto id = storage<Cell>().add(value, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
        return id;
    }

    template <frame::cell_type Cell>
        requires std::same_as<Cell, frame::cell_value_t<Cell>>
    frame::row_id_t add(Cell value) const {
        return add<Cell>(std::optional<Cell>{value});
    }

    template <frame::cell_type Cell>
        requires(!std::same_as<Cell, frame::cell_value_t<Cell>>)
    frame::row_id_t add(frame::cell_value_t<Cell> value) const {
        return add<Cell>(std::optional<frame::cell_value_t<Cell>>{value});
    }

    template <frame::cell_type Cell>
        requires std::same_as<Cell, frame::cell_value_t<Cell>>
    void set(frame::row_id_t id, std::optional<Cell> value) const {
        storage<Cell>().set(id, value, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
    }

    template <frame::cell_type Cell>
        requires(!std::same_as<Cell, frame::cell_value_t<Cell>>)
    void set(frame::row_id_t id, std::optional<frame::cell_value_t<Cell>> value) const {
        storage<Cell>().set(id, value, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
    }

    template <frame::cell_type Cell>
        requires std::same_as<Cell, frame::cell_value_t<Cell>>
    void set(frame::row_id_t id, Cell value) const {
        set<Cell>(id, std::optional<Cell>{value});
    }

    template <frame::cell_type Cell>
        requires(!std::same_as<Cell, frame::cell_value_t<Cell>>)
    void set(frame::row_id_t id, frame::cell_value_t<Cell> value) const {
        set<Cell>(id, std::optional<frame::cell_value_t<Cell>>{value});
    }

    bool erase(frame::row_id_t id) const {
        return frame::visit_cell_type(column_type().cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
            return erase<Cell>(id);
        });
    }

    void clear() const {
        frame::visit_cell_type(column_type().cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
            clear<Cell>();
        });
    }

    void invalidate() const { binding_->invalidate(); }
    bool valid() const noexcept { return binding_->valid(); }
    EvaluationTime last_updated() const noexcept { return binding_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    template <typename Input, auto... InputProperties>
    void copy(const In<Input, InputProperties...> &input) const {
        binding_->runtime_type().copy(input.input_data(), binding_.get());
    }

    template <typename Input, auto... InputProperties>
    void copy_tick(const In<Input, InputProperties...> &input) const {
        binding_->runtime_type().copy_tick(input.input_data(), binding_.get());
    }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(binding_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = frame::TSC<T, Properties...>;

  private:
    const frame::ColumnSeriesType &column_type() const noexcept {
        return static_cast<const frame::ColumnSeriesType &>(binding_->runtime_type().type());
    }

    template <frame::cell_type Cell> bool has_cell_type() const noexcept {
        return std::addressof(column_type().cell_type()) == std::addressof(cpp_scalar_type<Cell>());
    }

    template <frame::cell_type Cell> frame::NativeColumn<Cell> &storage() const {
        if (!has_cell_type<Cell>())
            throw std::invalid_argument{"frame TSC output cell type does not match the requested type"};
        return *static_cast<frame::NativeColumn<Cell> *>(binding_->mutable_data());
    }

    template <frame::cell_type Cell> bool erase(frame::row_id_t id) const {
        const auto erased = storage<Cell>().erase(id, binding_->context().evaluation_time(), binding_->allocator());
        if (erased)
            binding_->publish();
        return erased;
    }

    template <frame::cell_type Cell> void clear() const {
        auto &column = storage<Cell>();
        if (!binding_->valid() || column.size() != 0) {
            column.clear(binding_->context().evaluation_time(), binding_->allocator());
            binding_->publish();
        }
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
            if (output.membership.valid)
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
    EvaluationTime last_updated() const noexcept { return binding_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(binding_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = frame::TSC<T, Properties...>;

  private:
    frame::ColumnViewOutputStorage<T> &storage() const noexcept {
        return *static_cast<frame::ColumnViewOutputStorage<T> *>(binding_->mutable_data());
    }
    TimeSeriesOutputBinding binding_;
};

template <details::scalar_variable T, auto... Properties>
class Out<frame::TSC<T, Properties...>, frame::view> {
  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    template <typename SourceCell, auto... SourceProperties, auto... InputProperties>
        requires(frame::cell_type<SourceCell> || details::scalar_variable<SourceCell>)
    bool bind(const In<frame::TSC<SourceCell, SourceProperties...>, InputProperties...> &source) const {
        if (!source.valid()) {
            if (binding_->valid())
                binding_->invalidate();
            return false;
        }

        const auto &source_type = static_cast<const frame::ColumnSeriesType &>(source.runtime_type().type());
        return frame::visit_cell_type(source_type.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
            return bind_cell<Cell>(concrete_input<Cell, SourceCell>(source));
        });
    }

    void invalidate() const { binding_->invalidate(); }
    bool valid() const noexcept { return binding_->valid(); }
    EvaluationTime last_updated() const noexcept { return binding_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(binding_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = frame::TSC<T, Properties...>;

  private:
    template <frame::cell_type Cell, typename SourceCell, typename SourceInput>
    static auto concrete_input(const SourceInput &source) {
        if constexpr (frame::cell_type<SourceCell>) {
            return source;
        } else {
            return source.template input<Cell>();
        }
    }

    template <frame::cell_type Cell, typename SourceInput>
    bool bind_cell(const SourceInput &source) const {
        if (std::addressof(static_cast<const frame::ColumnSeriesType &>(binding_->runtime_type().type()).cell_type()) !=
            std::addressof(cpp_scalar_type<Cell>()))
            throw std::invalid_argument{"frame TSC view cell type does not match its source"};
        const auto input = source.extension_descriptor();
        auto &output = *static_cast<frame::ColumnViewOutputStorage<Cell> *>(binding_->mutable_data());
        const auto changed = output.bind(input.storage, input.modified, input.last_updated,
                                         binding_->context().evaluation_time(), binding_->allocator());
        if (changed)
            binding_->publish();
        return changed;
    }

    TimeSeriesOutputBinding binding_;
};

template <typename T, auto... Properties>
class Out<frame::TSC<T, Properties...>, frame::computed<>> {
    static_assert(frame::cell_type<T> || details::scalar_variable<T>);

  public:
    explicit Out(TimeSeriesOutputBinding binding) noexcept : binding_{std::move(binding)} {}

    template <typename SourceCell, auto... SourceProperties, auto... InputProperties>
        requires(frame::cell_type<SourceCell> || details::scalar_variable<SourceCell>)
    bool bind(const In<frame::TSC<SourceCell, SourceProperties...>, InputProperties...> &source) const {
        if (!source.valid()) {
            if (binding_->valid())
                binding_->invalidate();
            return false;
        }
        const auto &source_type = static_cast<const frame::ColumnSeriesType &>(source.runtime_type().type());
        return frame::visit_cell_type(source_type.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
            if constexpr (frame::cell_type<SourceCell>)
                return bind_source(source);
            else
                return bind_source(source.template input<Cell>());
        });
    }

    template <template <template <typename, auto...> typename> typename Schema, auto... SourceProperties,
              auto... InputProperties>
    bool bind(const In<frame::TSF<Schema, SourceProperties...>, InputProperties...> &source) const {
        if (!source.valid()) {
            if (binding_->valid())
                binding_->invalidate();
            return false;
        }
        const auto input = source.extension_descriptor();
        if constexpr (frame::cell_type<T>) {
            return bind_frame_rows<T>(input);
        } else {
            return frame::visit_cell_type(column_type().cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
                return bind_frame_rows<Cell>(input);
            });
        }
    }

    template <typename Function> void for_each_added(Function &&function) const {
        if constexpr (frame::cell_type<T>) {
            visit_added<T>(std::forward<Function>(function));
        } else {
            frame::visit_cell_type(column_type().cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
                visit_added<Cell>(std::forward<Function>(function));
            });
        }
    }

    void set(frame::row_id_t id, std::optional<frame::cell_value_t<T>> value) const
        requires frame::cell_type<T>
    {
        auto &output = storage<T>();
        output.set(id, value, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
    }

    template <frame::cell_type Cell>
        requires details::scalar_variable<T>
    void set(frame::row_id_t id, std::optional<frame::cell_value_t<Cell>> value) const {
        auto &output = storage<Cell>();
        output.set(id, value, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
    }

    template <frame::cell_type Cell>
        requires(details::scalar_variable<T> && std::same_as<Cell, frame::cell_value_t<Cell>>)
    void set(frame::row_id_t id, std::optional<Cell> value) const {
        auto &output = storage<Cell>();
        output.set(id, value, binding_->context().evaluation_time(), binding_->allocator());
        binding_->publish();
    }

    template <frame::cell_type Cell>
        requires details::scalar_variable<T>
    void set(frame::row_id_t id, frame::cell_value_t<Cell> value) const {
        set<Cell>(id, std::optional<frame::cell_value_t<Cell>>{value});
    }

    template <frame::cell_type Cell>
        requires(details::scalar_variable<T> && std::same_as<Cell, frame::cell_value_t<Cell>>)
    void set(frame::row_id_t id, Cell value) const {
        set<Cell>(id, std::optional<Cell>{value});
    }

    void set(frame::row_id_t id, frame::cell_value_t<T> value) const
        requires frame::cell_type<T>
    {
        set(id, std::optional<frame::cell_value_t<T>>{std::move(value)});
    }

    void set(frame::row_id_t id, std::nullopt_t) const {
        if constexpr (frame::cell_type<T>) {
            set(id, std::optional<frame::cell_value_t<T>>{});
        } else {
            frame::visit_cell_type(column_type().cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
                set<Cell>(id, std::optional<frame::cell_value_t<Cell>>{});
            });
        }
    }

    template <frame::cell_type Cell>
        requires(details::scalar_variable<T> || std::same_as<T, Cell>)
    void set_batch(std::span<const frame::row_id_t> ids, const frame::ColumnBatch<Cell> &batch) const {
        auto &output = storage<Cell>();
        if (output.set_batch(ids, batch, binding_->context().evaluation_time(), binding_->allocator()))
            binding_->publish();
    }

    void invalidate() const { binding_->invalidate(); }
    bool valid() const noexcept { return binding_->valid(); }
    EvaluationTime last_updated() const noexcept { return binding_->last_updated(); }
    const TimeSeriesRuntimeType &runtime_type() const noexcept { return binding_->runtime_type(); }

    template <typename... Functions> decltype(auto) visit(Functions &&...functions) const {
        auto output = details::output_factory::make<Out<TS_T>>(binding_.get());
        return output.visit(std::forward<Functions>(functions)...);
    }

    using time_series_type = frame::TSC<T, Properties...>;

  private:
    template <frame::cell_type Cell>
    bool has_cell_type() const noexcept {
        return std::addressof(column_type().cell_type()) == std::addressof(cpp_scalar_type<Cell>());
    }

    const frame::ColumnSeriesType &column_type() const noexcept {
        return static_cast<const frame::ColumnSeriesType &>(binding_->runtime_type().type());
    }

    template <frame::cell_type Cell>
    frame::ColumnComputedOutputStorage<Cell> &storage() const {
        if constexpr (frame::cell_type<T>) {
            static_assert(std::same_as<T, Cell>);
        } else if (!has_cell_type<Cell>()) {
            throw std::invalid_argument{"computed TSC cell type does not match its output type"};
        }
        return *static_cast<frame::ColumnComputedOutputStorage<Cell> *>(binding_->mutable_data());
    }

    template <frame::cell_type Cell, typename Function>
    void visit_added(Function &&function) const {
        const auto &output = storage<Cell>();
        if (!output.membership.valid || output.membership.delta_time != binding_->context().evaluation_time())
            return;
        for (auto id : output.membership.added_rows)
            std::invoke(function, id);
    }

    template <typename SourceInput>
    bool bind_source(const SourceInput &source) const {
        if constexpr (frame::cell_type<T>) {
            return bind_result<T>(source);
        } else {
            return frame::visit_cell_type(column_type().cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
                return bind_result<Cell>(source);
            });
        }
    }

    template <frame::cell_type Cell, typename SourceInput>
    bool bind_result(const SourceInput &source) const {
        const auto input = source.extension_descriptor();
        auto &output = storage<Cell>();
        const auto changed = output.bind(input.storage, input.modified, input.last_updated,
                                         binding_->context().evaluation_time(), binding_->allocator());
        if (changed)
            binding_->publish();
        return changed;
    }

    template <frame::cell_type Cell, typename SourceInput>
    bool bind_frame_rows(const SourceInput &source) const {
        auto &output = storage<Cell>();
        const auto changed = output.bind_rows(source.rows, source.modified,
                                              source.modified && source.change_time == source.last_updated,
                                              source.added, source.removed, binding_->context().evaluation_time(),
                                              binding_->allocator());
        if (changed)
            binding_->publish();
        return changed;
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

template <details::scalar_variable T, auto... Properties>
struct output_declaration_traits<Out<frame::TSC<T, Properties...>, frame::view>> {
    using time_series_type = frame::TSC<T, Properties...>;

    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &type) {
        return frame::column_binding_view_time_series_runtime_type(type);
    }
};

template <frame::cell_type T, auto... Properties>
struct output_declaration_traits<Out<frame::TSC<T, Properties...>, frame::computed<>>> {
    using time_series_type = frame::TSC<T, Properties...>;

    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &type) {
        return frame::column_computed_time_series_runtime_type<T>(type);
    }
};

template <details::scalar_variable T, auto... Properties>
struct output_declaration_traits<Out<frame::TSC<T, Properties...>, frame::computed<>>> {
    using time_series_type = frame::TSC<T, Properties...>;

    static const TimeSeriesRuntimeType &runtime(const TimeSeriesType &type) {
        return frame::column_computed_time_series_runtime_type(type);
    }
};

} // namespace bg
