#include <bg/time_series/ref/ref_provider.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/scope_exit.hpp>

#include <ranges>

namespace bg::details {

namespace {

struct ScalarValueHash {
    [[nodiscard]] std::size_t operator()(const ScalarValue<> &value) const noexcept {
        return value.input_view().hash();
    }
    [[nodiscard]] std::size_t operator()(const ScalarData &value) const noexcept { return value.hash(); }
};

struct ScalarValueEqual {
    [[nodiscard]] bool operator()(const ScalarValue<> &stored, const ScalarData &value) const noexcept {
        return stored.input_view().equals(value);
    }
    [[nodiscard]] bool operator()(const ScalarValue<> &left, const ScalarValue<> &right) const noexcept {
        return left.input_view().equals(right.input_view());
    }
};

struct DictionaryKeySubscribers {
    DynamicSubscribers *subscribers{};
};

using KeySubscriberMap = util::hash_dictionary<ScalarValue<>, DictionaryKeySubscribers, ScalarValueHash,
                                               ScalarValueEqual>;

struct DictionarySourceObservers {
    explicit DictionarySourceObservers(ReferenceStorage &source) noexcept : source{&source} {}

    ReferenceStorage *source;
    KeySubscriberMap keys;
};

} // namespace

class DictionaryObservers {
  public:
    explicit DictionaryObservers(std::pmr::memory_resource &memory) : sources_{&memory}, memory_{&memory} {}

    ~DictionaryObservers() {
        for (auto *source : sources_) {
            release_source(*source);
        }
    }

    void add(ReferenceStorage &source, const ScalarData &key, RuntimeGraph &graph, std::uint32_t input_offset,
             std::uint32_t node_index) {
        auto &source_observers = require_source(source);
        auto [entry, inserted] = source_observers.keys.try_emplace_with(
            key, *memory_, [&](ScalarValue<> *destination) {
                std::construct_at(destination, Scalar<>::from(key), *memory_);
            }, [](ScalarValue<> *value) noexcept { std::destroy_at(value); },
            [](DictionaryKeySubscribers *destination) { std::construct_at(destination); });
        (void)inserted;

        auto &subscribers = DynamicSubscribers::ensure(entry->subscribers, *memory_);
        subscribers.add(graph, input_offset, 0, node_index, true, false);
    }

    void remove(ReferenceStorage &source, const ScalarData &key, RuntimeGraph &graph,
                std::uint32_t input_offset) noexcept {
        auto *source_observers = find_source(source);
        if (source_observers == nullptr) {
            return;
        }

        auto *entry = source_observers->keys.find(key);
        if (entry == nullptr || entry->subscribers == nullptr) {
            return;
        }
        entry->subscribers->remove(graph, input_offset);
        if (!entry->subscribers->empty()) {
            return;
        }

        source_observers->keys.erase_with(
            key, *memory_, [](ScalarValue<> *value) noexcept { std::destroy_at(value); },
            [](DictionaryKeySubscribers *value) noexcept {
                DynamicSubscribers::release(value->subscribers);
                std::destroy_at(value);
            });

        if (source_observers->keys.empty()) {
            const auto position = std::ranges::find(sources_, source_observers);
            *position = sources_.back();
            sources_.pop_back();
            release_source(*source_observers);
        }
    }

    void source_changed(OutputNotificationContext &context) {
        for (auto *source : sources_) {
            const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(
                source->source->target_runtime_type);
            if (runtime_type == nullptr || source->source->target_value == nullptr) {
                continue;
            }

            notify_changes(context, *source, *runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_added,
                           OutputChange::revalidation);
            notify_changes(context, *source, *runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_removed,
                           OutputChange::invalidation);
            notify_changes(context, *source, *runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_modified,
                           OutputChange::update);
        }
    }

  private:
    using ChangeFunction = void (TimeSeriesDictionaryRuntimeType::*)(
        const EvaluationTime &, const void *, void *, TimeSeriesRuntimeType::ItemIdFunction) const;

    [[nodiscard]] DictionarySourceObservers *find_source(const ReferenceStorage &source) const noexcept {
        const auto found = std::ranges::find_if(sources_,
                                                [&](const auto *candidate) { return candidate->source == &source; });
        return found == sources_.end() ? nullptr : *found;
    }

    [[nodiscard]] DictionarySourceObservers &require_source(ReferenceStorage &source) {
        if (auto *existing = find_source(source)) {
            return *existing;
        }

        auto *storage = memory_->allocate(sizeof(DictionarySourceObservers), alignof(DictionarySourceObservers));
        auto *result = std::construct_at(static_cast<DictionarySourceObservers *>(storage), source);
        auto release = util::scope_exit{[&]() noexcept { release_source(*result); }};
        sources_.push_back(result);
        release.release();
        return *result;
    }

    void release_source(DictionarySourceObservers &source) noexcept {
        source.keys.release_with(
            *memory_, [](ScalarValue<> *key) noexcept { std::destroy_at(key); },
            [](DictionaryKeySubscribers *entry) noexcept {
                DynamicSubscribers::release(entry->subscribers);
                std::destroy_at(entry);
            });
        std::destroy_at(&source);
        memory_->deallocate(&source, sizeof(DictionarySourceObservers), alignof(DictionarySourceObservers));
    }

    void notify_changes(OutputNotificationContext &context, DictionarySourceObservers &source,
                        const TimeSeriesDictionaryRuntimeType &runtime_type, ChangeFunction changes,
                        OutputChange change) {
        struct Callback {
            DictionarySourceObservers *source;
            const TimeSeriesDictionaryRuntimeType *runtime_type;
            OutputNotificationContext *context;
            OutputChange change;
        } callback{&source, &runtime_type, &context, change};

        (runtime_type.*changes)(
            *source.source->target_last_updated, source.source->target_value, &callback,
            [](void *value, TimeSeriesItemId id) {
                auto &callback = *static_cast<Callback *>(value);
                const auto key = ScalarData{callback.runtime_type->key_runtime_type(),
                                            callback.runtime_type->key(callback.source->source->target_value, id)};
                if (auto *entry = callback.source->keys.find(key)) {
                    callback.context->notify(entry->subscribers, callback.change);
                }
            });
    }

    std::pmr::vector<DictionarySourceObservers *> sources_;
    std::pmr::memory_resource *memory_;
};

ReferenceProvider::ReferenceProvider(RuntimeGraph &owner, std::pmr::memory_resource &memory, ReferenceStorage &root)
    : owner_{&owner}, memory_{&memory}, root_{&root}, representations_{&memory} {}

ReferenceProvider::~ReferenceProvider() {
    if (dictionary_observers_ != nullptr) {
        std::destroy_at(dictionary_observers_);
        memory_->deallocate(dictionary_observers_, sizeof(DictionaryObservers), alignof(DictionaryObservers));
    }
    for (auto *representation : representations_ | std::views::reverse) {
        DynamicSubscribers::release(representation->reference().subscribers);
        representation->destroy(*memory_);
    }
}

void ReferenceProvider::add_dictionary_key_subscriber(ReferenceStorage &source, const ScalarData &key,
                                                      RuntimeGraph &graph, std::uint32_t input_offset,
                                                      std::uint32_t node_index) {
    if (dictionary_observers_ == nullptr) {
        auto *storage = memory_->allocate(sizeof(DictionaryObservers), alignof(DictionaryObservers));
        auto release = util::scope_exit{[&]() noexcept {
            memory_->deallocate(storage, sizeof(DictionaryObservers), alignof(DictionaryObservers));
        }};
        dictionary_observers_ = std::construct_at(static_cast<DictionaryObservers *>(storage), *memory_);
        release.release();
    }
    dictionary_observers_->add(source, key, graph, input_offset, node_index);
}

void ReferenceProvider::remove_dictionary_key_subscriber(ReferenceStorage &source, const ScalarData &key,
                                                         RuntimeGraph &graph,
                                                         std::uint32_t input_offset) noexcept {
    if (dictionary_observers_ != nullptr) {
        dictionary_observers_->remove(source, key, graph, input_offset);
    }
}

ReferenceStorage &ReferenceProvider::representation(ReferenceStorage &source,
                                                     const TimeSeriesType &requested_type) {
    if (&source.target_runtime_type->type() == &requested_type) {
        return source;
    }
    if (&root_->target_runtime_type->type() == &requested_type) {
        return *root_;
    }

    for (auto *representation : representations_) {
        auto &reference = representation->reference();
        if (&representation->source() == &source && reference.target_runtime_type != nullptr &&
            &reference.target_runtime_type->type() == &requested_type) {
            return reference;
        }
    }

    auto *result = source.target_runtime_type->create_reference_representation(*this, source, requested_type, *memory_);
    auto release = util::scope_exit{[&]() noexcept { result->destroy(*memory_); }};
    representations_.push_back(result);
    release.release();
    return result->reference();
}

void ReferenceProvider::source_changed(OutputNotificationContext &context, OutputChange change) {
    const auto count = representations_.size();
    for (auto index = std::size_t{}; index < count; ++index) {
        auto *representation = representations_[index];
        representation->source_changed(context, change);
    }
    if (dictionary_observers_ != nullptr) {
        dictionary_observers_->source_changed(context);
    }
}

} // namespace bg::details
