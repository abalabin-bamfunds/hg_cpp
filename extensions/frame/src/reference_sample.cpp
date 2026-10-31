#include <bg/frame/frame.hpp>
#include <bg/frame/tsc.hpp>
#include <bg/time_series/ref/ref_input_sample.hpp>
#include <bg/util/scope_exit.hpp>
#include <bg/util/static_storage.hpp>

#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <variant>
#include <vector>

namespace bg::frame::details {
using bg::details::ResolvedTimeSeries;

namespace {

struct BaselineRows {
    enum class BaselineForm { empty, current, normalized, removed };

    RowSet rows{};
    const util::index<> *added{};
    const util::index<> *removed{};
    const void *domain{};
    BaselineForm form{BaselineForm::empty};

    bool has(row_id_t id) const noexcept {
        switch (form) {
        case BaselineForm::empty:
            return false;
        case BaselineForm::current:
            return rows.has(id);
        case BaselineForm::normalized:
            return (rows.has(id) && (added == nullptr || !added->contains(id))) ||
                   (removed != nullptr && removed->contains(id));
        case BaselineForm::removed:
            return removed != nullptr && removed->contains(id);
        }
        std::terminate();
    }

    template <typename Function> void each(Function &&function) const {
        switch (form) {
        case BaselineForm::empty:
            return;
        case BaselineForm::current:
            rows.each(std::forward<Function>(function));
            return;
        case BaselineForm::normalized:
            rows.each([&](row_id_t id) {
                if (added == nullptr || !added->contains(id))
                    std::invoke(function, id);
            });
            if (removed != nullptr)
                for (const auto id : *removed)
                    if (!rows.has(id) || (added != nullptr && added->contains(id)))
                        std::invoke(function, id);
            return;
        case BaselineForm::removed:
            if (removed != nullptr)
                for (const auto id : *removed)
                    std::invoke(function, id);
            return;
        }
        std::terminate();
    }
};

BaselineRows baseline_rows(RowSet rows, const util::index<> *added, const util::index<> *removed,
                           const void *cycle_start_domain, EvaluationTime change_time,
                           bool valid_before, EvaluationTime evaluation_time) noexcept {
    if (!valid_before)
        return {};
    if (change_time != evaluation_time)
        return {rows, nullptr, nullptr, rows.identity, BaselineRows::BaselineForm::current};
    if (cycle_start_domain == nullptr)
        return {};
    if (rows.identity == cycle_start_domain)
        return {rows, added, removed, cycle_start_domain, BaselineRows::BaselineForm::normalized};
    return {{}, nullptr, removed, cycle_start_domain, BaselineRows::BaselineForm::removed};
}

void compare_rows(const BaselineRows &baseline, RowSet current, util::index<> &added,
                  util::index<> &removed, util::index<> &common_modified, allocator memory) {
    if (baseline.domain == nullptr || baseline.domain != current.identity) {
        current.each([&](row_id_t id) { added.set(id, memory.memory_resource()); });
        baseline.each([&](row_id_t id) { removed.set(id, memory.memory_resource()); });
        return;
    }

    current.each([&](row_id_t id) {
        if (baseline.has(id))
            common_modified.set(id, memory.memory_resource());
        else
            added.set(id, memory.memory_resource());
    });
    baseline.each([&](row_id_t id) {
        if (!current.has(id))
            removed.set(id, memory.memory_resource());
    });
}

FrameReadStorage frame_storage(ResolvedTimeSeries source, bool valid) noexcept {
    if (source.runtime_type == nullptr || source.storage == nullptr)
        return {};

    const auto &runtime = static_cast<const FrameSeriesRuntimeType &>(*source.runtime_type);
    auto result = runtime.input_storage(source.storage);
    result.valid = valid;
    if (!valid) {
        result.rows = {};
        result.size = 0;
    }
    result.last_updated = source.last_updated.time();
    return result;
}

template <cell_type Cell> struct SampledColumn final : bg::details::ReferenceSample {
    SampledColumn(const TimeSeriesType &type, ResolvedTimeSeries previous, ResolvedTimeSeries current,
                  EvaluationTime evaluation_time, allocator &memory)
        : runtime_type{&column_view_time_series_runtime_type<Cell>(type)}, previous_source{previous},
          sampled_last_updated{TimeSeriesState::from_validity_transition(
              evaluation_time, previous.last_updated.valid_before(evaluation_time), current.last_updated.valid())},
          memory{memory} {
        auto rollback = util::scope_exit{[&]() noexcept {
            added.release(memory.memory_resource());
            modified.release(memory.memory_resource());
            removed.release(memory.memory_resource());
        }};

        const auto *current_runtime = current.runtime_type == nullptr
                                          ? nullptr
                                          : static_cast<const ColumnSeriesRuntimeType<Cell> *>(current.runtime_type);
        const auto current_view = current_runtime == nullptr || current.storage == nullptr
                                      ? std::optional<ColumnSeriesView<Cell>>{}
                                      : std::optional{current_runtime->input_view(current.storage)};
        const auto *previous_runtime = previous.runtime_type == nullptr
                                           ? nullptr
                                           : static_cast<const ColumnSeriesRuntimeType<Cell> *>(previous.runtime_type);
        const auto previous_view = previous_runtime == nullptr || previous.storage == nullptr
                                       ? std::optional<ColumnSeriesView<Cell>>{}
                                       : std::optional{previous_runtime->input_view(previous.storage)};

        if (current_view)
            storage = current_view->scalar_storage();
        else if (previous_view)
            storage = previous_view->scalar_storage();

        const auto previous_valid = previous.last_updated.valid_before(evaluation_time);
        const auto current_valid = current.last_updated.valid();
        const auto previous_storage = previous_view ? std::addressof(previous_view->scalar_storage()) : nullptr;
        const auto baseline = previous_storage == nullptr
                                  ? BaselineRows{}
                                  : baseline_rows(previous_storage->visible_rows,
                                                 std::addressof(previous_view->added()),
                                                 std::addressof(previous_view->removed()),
                                                 previous_storage->cycle_start_domain, previous_view->change_time(),
                                                 previous_valid, evaluation_time);
        const auto current_rows = current_view && current_valid ? current_view->scalar_storage().visible_rows : RowSet{};
        compare_rows(baseline, current_rows, added, removed, modified, memory);

        storage.visible_rows = current_rows;
        storage.added = std::addressof(added);
        storage.modified = std::addressof(modified);
        storage.removed = std::addressof(removed);
        storage.change_time = evaluation_time;
        storage.cycle_start_domain = baseline.domain;
        rollback.release();
    }

    ~SampledColumn() override {
        added.release(memory.memory_resource());
        modified.release(memory.memory_resource());
        removed.release(memory.memory_resource());
    }

    void retain() noexcept override { ++reference_count; }
    void release() noexcept override {
        if (--reference_count == 0)
            memory.delete_object(this);
    }
    TimeSeriesRuntimeType::ConstItem input() const noexcept override {
        return {*runtime_type, sampled_last_updated, &storage};
    }
    ResolvedTimeSeries previous() const noexcept override { return previous_source; }

    const TimeSeriesRuntimeType *runtime_type;
    std::size_t reference_count{1};
    ResolvedTimeSeries previous_source;
    TimeSeriesState sampled_last_updated;
    ColumnStorage<Cell> storage;
    util::index<> added;
    util::index<> modified;
    util::index<> removed;
    allocator memory;
};

class SampledFrameRuntimeType;

using SampledFrameColumnStorage = FrameViewColumnInput::Storage;

struct SampledFrame final : bg::details::ReferenceSample {
    SampledFrame(const TimeSeriesType &type, ResolvedTimeSeries previous, ResolvedTimeSeries current,
                 EvaluationTime evaluation_time, allocator &memory);
    ~SampledFrame() override;

    void retain() noexcept override { ++reference_count; }
    void release() noexcept override {
        if (--reference_count == 0)
            memory.delete_object(this);
    }
    TimeSeriesRuntimeType::ConstItem input() const noexcept override;
    ResolvedTimeSeries previous() const noexcept override { return previous_source; }
    TimeSeriesRuntimeType::ConstItem item(item_id_t id) const;
    FrameReadStorage input_storage() const noexcept;

    static bool column_changed(const void *context, std::size_t index) noexcept;
    static void for_each_changed_column(const void *context, void *target,
                                        FrameReadStorage::changed_column_visitor visit);
    static const util::index<> *modified_column(const void *context, std::size_t index) noexcept;
    static FrameColumnReadBinding column_binding(const void *context, std::size_t index);

    const SampledFrameRuntimeType *runtime_type;
    std::size_t reference_count{1};
    ResolvedTimeSeries previous_source;
    ResolvedTimeSeries current_source;
    EvaluationTime evaluation_time{MIN_DT};
    TimeSeriesState sampled_last_updated;
    FrameReadStorage source_frame;
    RowSet sampled_rows{};
    std::pmr::vector<SampledFrameColumnStorage> columns;
    util::index<> added;
    util::index<> removed;
    util::index<> common_modified;
    util::index<> changed;
    const void *cycle_start_domain{};
    allocator memory;
};

class SampledFrameRuntimeType final : public FrameSeriesRuntimeType {
  public:
    SampledFrameRuntimeType(const TimeSeriesType &type, const SchemaType &actual_schema)
        : FrameSeriesRuntimeType{type, 0, 1, false, false, false} {
        const auto &sampled_actual_type = frame_series_type(actual_schema, type.guarantee());
        item_runtimes_.reserve(sampled_actual_type.item_count());
        for (auto id = item_id_t{}; id < sampled_actual_type.item_count(); ++id)
            item_runtimes_.push_back(
                std::addressof(column_view_time_series_runtime_type(sampled_actual_type.item_type(id))));
    }

    static const SampledFrameRuntimeType &for_type(const TimeSeriesType &type, const SchemaType &actual_schema) {
        struct Entry {
            const TimeSeriesType *type;
            const SchemaType *actual_schema;
            std::unique_ptr<SampledFrameRuntimeType> runtime_type;
        };
        static auto entries = util::static_storage<std::vector<Entry>>{};
        auto &cache = entries.get();
        const auto matches = [&](const Entry &entry) {
            return entry.type == &type && entry.actual_schema == std::addressof(actual_schema);
        };
        const auto found = std::ranges::find_if(cache, matches);
        if (found != cache.end())
            return *found->runtime_type;
        cache.push_back({std::addressof(type), std::addressof(actual_schema),
                         std::make_unique<SampledFrameRuntimeType>(type, actual_schema)});
        return *cache.back().runtime_type;
    }

    ItemLayout item_layout(item_id_t) const override {
        throw std::logic_error{"a sampled frame has no packed item layout"};
    }
    const TimeSeriesRuntimeType &item_runtime_type(item_id_t id) const override {
        return *item_runtimes_.at(id);
    }
    std::size_t item_count(const void *value) const override {
        return static_cast<const SampledFrame *>(value)->columns.size();
    }
    ConstItem item(const void *value, item_id_t id) const override {
        return static_cast<const SampledFrame *>(value)->item(id);
    }
    MutableItem item(void *, item_id_t) const override {
        throw std::logic_error{"a sampled frame is immutable"};
    }
    void for_each_item_id(const void *value, void *context, ItemIdFunction function) const override {
        for (auto id = item_id_t{}; id < item_count(value); ++id)
            function(context, id);
    }
    bool item_updated(const TimeSeriesState &last_updated, const void *value, item_id_t id) const noexcept override {
        const auto &sample = *static_cast<const SampledFrame *>(value);
        return last_updated.valid() && last_updated.time() == sample.sampled_last_updated.time() &&
               id < sample.columns.size();
    }
    void for_each_updated_item(const TimeSeriesState &last_updated, const void *value, void *context,
                               ItemIdFunction function) const override {
        const auto &sample = *static_cast<const SampledFrame *>(value);
        if (!last_updated.valid() || last_updated.time() != sample.sampled_last_updated.time())
            return;
        for (auto id = item_id_t{}; id < sample.columns.size(); ++id)
            function(context, id);
    }
    void for_each_modified_item(const TimeSeriesState &last_updated, const void *value,
                                EvaluationTime evaluation_time, void *context,
                                ModifiedItemFunction function) const override {
        if (last_updated.time() != evaluation_time)
            return;
        const auto &sample = *static_cast<const SampledFrame *>(value);
        for (auto id = item_id_t{}; id < sample.columns.size(); ++id) {
            const auto item = sample.item(id);
            function(context, {id, &item_runtime_type(id), &sample.sampled_last_updated, item.value});
        }
    }
    bool collection_valid(const void *value) const noexcept override {
        return static_cast<const SampledFrame *>(value)->current_source.last_updated.valid();
    }
    FrameReadStorage input_storage(const void *value) const noexcept override {
        auto result = static_cast<const SampledFrame *>(value)->input_storage();
        result.source_runtime = this;
        return result;
    }
    void construct_initial_value(void *) const noexcept override { std::terminate(); }
    void destroy_value(void *, allocator &) const noexcept override { std::terminate(); }

  private:
    std::vector<const TimeSeriesRuntimeType *> item_runtimes_;
};

const SchemaType &sampled_actual_schema(ResolvedTimeSeries current, ResolvedTimeSeries previous) {
    auto storage = frame_storage(current, current.last_updated.valid());
    if (storage.schema == nullptr)
        storage = frame_storage(previous, previous.last_updated.valid());
    if (storage.schema == nullptr)
        throw std::logic_error{"sampled frame has no source schema"};
    return *storage.schema;
}

template <cell_type Cell>
void bind_sampled_column(SampledFrameColumnStorage &destination, FrameColumnReadBinding binding, RowSet rows,
                         const util::index<> &added, const util::index<> &modified,
                         const util::index<> &removed, EvaluationTime evaluation_time,
                         const void *cycle_start_domain) {
    auto &storage = destination.template emplace<ColumnStorage<Cell>>();
    if (binding.payload != nullptr && binding.copy_column_storage != nullptr)
        binding.copy_column_storage(std::addressof(storage), binding.payload);
    storage.visible_rows = rows;
    storage.added = std::addressof(added);
    storage.modified = std::addressof(modified);
    storage.removed = std::addressof(removed);
    storage.change_time = evaluation_time;
    storage.cycle_start_domain = cycle_start_domain;
}

void bind_sampled_column(SampledFrameColumnStorage &destination, FrameColumnReadBinding binding, RowSet rows,
                        const util::index<> &added, const util::index<> &modified,
                        const util::index<> &removed, EvaluationTime evaluation_time,
                        const void *cycle_start_domain, const TimeSeriesRuntimeType &item_runtime) {
    const auto &type = static_cast<const ColumnSeriesType &>(item_runtime.type());
    visit_cell_type(type.cell_type(), [&]<typename Cell>(std::type_identity<Cell>) {
        bind_sampled_column<Cell>(destination, binding, rows, added, modified, removed, evaluation_time,
                                  cycle_start_domain);
    });
}

bg::details::ReferenceSample *create_sampled_frame(const TimeSeriesType &type, ResolvedTimeSeries previous,
                                                   ResolvedTimeSeries current, EvaluationTime evaluation_time,
                                                   allocator &memory) {
    if (previous.runtime_type == nullptr && current.runtime_type == nullptr)
        return nullptr;
    if (!previous.last_updated.valid_before(evaluation_time) && !current.last_updated.valid())
        return nullptr;
    return memory.new_object<SampledFrame>(type, previous, current, evaluation_time, memory);
}

} // namespace

template <cell_type Cell>
bg::details::ReferenceSample *create_sampled_column(const TimeSeriesType &type, ResolvedTimeSeries previous,
                                                    ResolvedTimeSeries current, EvaluationTime evaluation_time,
                                                    allocator &memory) {
    if (!previous.last_updated.valid_before(evaluation_time) && !current.last_updated.valid())
        return nullptr;
    return memory.new_object<SampledColumn<Cell>>(type, previous, current, evaluation_time, memory);
}

template bg::details::ReferenceSample *create_sampled_column<bool>(const TimeSeriesType &, ResolvedTimeSeries,
                                                                    ResolvedTimeSeries, EvaluationTime, allocator &);
template bg::details::ReferenceSample *create_sampled_column<i64>(const TimeSeriesType &, ResolvedTimeSeries,
                                                                   ResolvedTimeSeries, EvaluationTime, allocator &);
template bg::details::ReferenceSample *create_sampled_column<double>(const TimeSeriesType &, ResolvedTimeSeries,
                                                                      ResolvedTimeSeries, EvaluationTime,
                                                                      allocator &);
template bg::details::ReferenceSample *create_sampled_column<string>(const TimeSeriesType &, ResolvedTimeSeries,
                                                                      ResolvedTimeSeries, EvaluationTime,
                                                                      allocator &);
template bg::details::ReferenceSample *create_sampled_column<symbol>(const TimeSeriesType &, ResolvedTimeSeries,
                                                                      ResolvedTimeSeries, EvaluationTime,
                                                                      allocator &);

SampledFrame::SampledFrame(const TimeSeriesType &type, ResolvedTimeSeries previous, ResolvedTimeSeries current,
                           EvaluationTime time, allocator &memory)
    : runtime_type{std::addressof(SampledFrameRuntimeType::for_type(type, sampled_actual_schema(current, previous)))},
      previous_source{previous}, current_source{current},
      evaluation_time{time},
      sampled_last_updated{TimeSeriesState::from_validity_transition(
          time, previous.last_updated.valid_before(time), current.last_updated.valid())},
      columns{&memory.memory_resource()}, memory{memory} {
    auto rollback = util::scope_exit{[&]() noexcept {
        added.release(memory.memory_resource());
        removed.release(memory.memory_resource());
        common_modified.release(memory.memory_resource());
        changed.release(memory.memory_resource());
    }};

    auto previous_frame = frame_storage(previous, previous.last_updated.valid_before(time));
    const auto current_frame = frame_storage(current, current.last_updated.valid());
    source_frame = current_frame.schema == nullptr ? previous_frame : current_frame;
    sampled_rows = current.last_updated.valid() ? current_frame.rows : RowSet{};
    if (source_frame.schema != nullptr)
        columns.resize(source_frame.schema->field_count());

    const auto baseline = baseline_rows(previous_frame.rows, previous_frame.added, previous_frame.removed,
                                        previous_frame.cycle_start_domain, previous_frame.change_time,
                                        previous.last_updated.valid_before(time), time);
    cycle_start_domain = baseline.domain;
    const auto current_rows = sampled_rows;
    compare_rows(baseline, current_rows, added, removed, common_modified, memory);
    for (const auto id : added)
        changed.set(id, memory.memory_resource());
    for (const auto id : common_modified)
        changed.set(id, memory.memory_resource());

    for (auto id = item_id_t{}; id < columns.size(); ++id) {
        const auto &field_runtime = runtime_type->item_runtime_type(id);
        const auto binding = current_frame.schema != nullptr && current_frame.column_binding != nullptr &&
                                     id < current_frame.schema->field_count()
                                 ? current_frame.column(id)
                                 : FrameColumnReadBinding{};
        bind_sampled_column(columns[id], binding, current_rows, added, common_modified, removed, time,
                            cycle_start_domain, field_runtime);
    }
    rollback.release();
}

SampledFrame::~SampledFrame() {
    added.release(memory.memory_resource());
    removed.release(memory.memory_resource());
    common_modified.release(memory.memory_resource());
    changed.release(memory.memory_resource());
}

TimeSeriesRuntimeType::ConstItem SampledFrame::input() const noexcept {
    return {*runtime_type, sampled_last_updated, this};
}

bool SampledFrame::column_changed(const void *context, std::size_t index) noexcept {
    return index < static_cast<const SampledFrame *>(context)->columns.size();
}

void SampledFrame::for_each_changed_column(const void *context, void *target,
                                          FrameReadStorage::changed_column_visitor visit) {
    const auto &sample = *static_cast<const SampledFrame *>(context);
    for (auto id = item_id_t{}; id < sample.columns.size(); ++id)
        visit(target, id);
}

TimeSeriesRuntimeType::ConstItem SampledFrame::item(item_id_t id) const {
    if (id >= columns.size())
        throw std::out_of_range{"sampled frame column index"};
    const auto &storage = columns[id];
    const auto *payload = std::visit([]<typename Cell>(const ColumnStorage<Cell> &column) -> const void * {
        return std::addressof(column);
    }, storage);
    return {runtime_type->item_runtime_type(id), sampled_last_updated, payload};
}

const util::index<> *SampledFrame::modified_column(const void *context, std::size_t index) noexcept {
    const auto &sample = *static_cast<const SampledFrame *>(context);
    return index < sample.columns.size() ? std::addressof(sample.common_modified) : nullptr;
}

FrameColumnReadBinding SampledFrame::column_binding(const void *context, std::size_t index) {
    const auto &sample = *static_cast<const SampledFrame *>(context);
    if (index >= sample.columns.size())
        return {};
    return std::visit(
        []<typename Cell>(const ColumnStorage<Cell> &column) {
            return frame_column_read_binding<Cell>(std::addressof(column));
        },
        sample.columns[index]);
}

FrameReadStorage SampledFrame::input_storage() const noexcept {
    auto result = source_frame;
    result.valid = current_source.last_updated.valid();
    result.modified = true;
    result.rows = result.valid ? sampled_rows : RowSet{};
    result.cycle_start_domain = cycle_start_domain;
    result.size = result.rows.count();
    result.added = std::addressof(added);
    result.removed = std::addressof(removed);
    result.changed = std::addressof(changed);
    result.column_modified = {};
    result.column_modified_context = this;
    result.modified_column = &modified_column;
    result.change_time = evaluation_time;
    result.last_updated = evaluation_time;
    result.columns_context = this;
    result.column_binding = &column_binding;
    result.changed_columns_context = this;
    result.column_changed = &column_changed;
    result.for_each_changed_column = &for_each_changed_column;
    result.source_owner = this;
    return result;
}

} // namespace bg::frame::details

namespace bg::frame {

bg::details::ReferenceSampleFactory FrameSeriesRuntimeType::reference_sample_factory() const noexcept {
    return &details::create_sampled_frame;
}

} // namespace bg::frame
