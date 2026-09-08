#include <bg/time_series/ref/ref_provider.hpp>

#include <bg/graph/dynamic_subscribers.hpp>
#include <bg/graph/node.hpp>
#include <bg/graph/runtime_graph.hpp>
#include <bg/scalar/value.hpp>
#include <bg/time_series/ref/ref_storage.hpp>
#include <bg/time_series/time_series_runtime_type.hpp>
#include <bg/time_series/tsd/tsd_runtime_type.hpp>
#include <bg/util/hash_dictionary.hpp>
#include <bg/util/scope_exit.hpp>

#include <memory>
#include <ranges>

namespace bg::details {

ReferenceRepresentation::~ReferenceRepresentation() = default;

namespace {

struct ScalarValueHash {
    std::size_t operator()(const ScalarValue<> &value) const noexcept { return value.hash(); }
    std::size_t operator()(const ScalarData &value) const noexcept { return value.hash(); }
};

struct ScalarValueEqual {
    bool operator()(const ScalarValue<> &stored, const ScalarData &value) const noexcept {
        return stored.equals(value);
    }
    bool operator()(const ScalarValue<> &left, const ScalarValue<> &right) const noexcept { return left.equals(right); }
};

struct DictionaryKeySubscribers {
    DynamicSubscribers *subscribers{};
};

using KeySubscriberMap =
    util::hash_dictionary<ScalarValue<>, DictionaryKeySubscribers, ScalarValueHash, ScalarValueEqual>;

struct DictionarySourceObservers {
    explicit DictionarySourceObservers(ReferenceAddress source) noexcept : source{source} {}

    ReferenceAddress source;
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

    void add(ReferenceAddress source, const ScalarData &key, RuntimeGraph &graph, std::uint32_t input_offset,
             std::uint32_t node_index) {
        auto &source_observers = require_source(source);
        auto [entry, inserted] = source_observers.keys.try_emplace_with(
            key, *memory_,
            [&](ScalarValue<> *destination) { std::construct_at(destination, Scalar<>::from(key), *memory_); },
            [](ScalarValue<> *value) noexcept { std::destroy_at(value); },
            [](DictionaryKeySubscribers *destination) { std::construct_at(destination); });
        (void)inserted;

        auto &subscribers = DynamicSubscribers::ensure(entry->subscribers, *memory_);
        subscribers.add(graph, input_offset, 0, node_index, true, false);
    }

    void remove(ReferenceAddress source, const ScalarData &key, RuntimeGraph &graph,
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

    void source_changed(OutputChangeContext &context) {
        for (auto *source : sources_) {
            const auto target = reference_target(source->source);
            const auto *runtime_type = dynamic_cast<const TimeSeriesDictionaryRuntimeType *>(target.runtime_type);
            if (runtime_type == nullptr || target.value == nullptr) {
                continue;
            }

            notify_changes(context, *source, target, *runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_added,
                           OutputChange::revalidation);
            notify_changes(context, *source, target, *runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_removed,
                           OutputChange::invalidation);
            notify_changes(context, *source, target, *runtime_type, &TimeSeriesDictionaryRuntimeType::for_each_modified,
                           OutputChange::update);
        }
    }

  private:
    using ChangeFunction = void (TimeSeriesDictionaryRuntimeType::*)(const EvaluationTime &, const void *, void *,
                                                                     TimeSeriesRuntimeType::ItemIdFunction) const;

    DictionarySourceObservers *find_source(ReferenceAddress source) const noexcept {
        const auto found =
            std::ranges::find_if(sources_, [&](const auto *candidate) { return candidate->source == source; });
        return found == sources_.end() ? nullptr : *found;
    }

    DictionarySourceObservers &require_source(ReferenceAddress source) {
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

    void notify_changes(OutputChangeContext &context, DictionarySourceObservers &source, ReferenceTarget target,
                        const TimeSeriesDictionaryRuntimeType &runtime_type, ChangeFunction changes,
                        OutputChange change) {
        struct Callback {
            DictionarySourceObservers *source;
            const TimeSeriesDictionaryRuntimeType *runtime_type;
            ReferenceTarget target;
            OutputChangeContext *context;
            OutputChange change;
        } callback{&source, &runtime_type, target, &context, change};

        (runtime_type.*changes)(*target.last_updated, target.value, &callback, [](void *value, TimeSeriesItemId id) {
            auto &callback = *static_cast<Callback *>(value);
            const auto key = ScalarData{callback.runtime_type->key_runtime_type(),
                                        callback.runtime_type->key(callback.target.value, id)};
            if (auto *entry = callback.source->keys.find(key)) {
                callback.context->notify(entry->subscribers, callback.change);
            }
        });
    }

    std::pmr::vector<DictionarySourceObservers *> sources_;
    std::pmr::memory_resource *memory_;
};

ReferenceProvider::ReferenceProvider(std::pmr::memory_resource &memory, ReferenceRoot &root,
                                     DynamicSubscribers *root_subscribers)
    : memory_{&memory}, root_{&root}, representations_{&memory}, subscriber_groups_{&memory} {
    subscriber_groups_.push_back(root_subscribers);
}

ReferenceProvider::~ReferenceProvider() {
    if (dictionary_observers_ != nullptr) {
        std::destroy_at(dictionary_observers_);
        memory_->deallocate(dictionary_observers_, sizeof(DictionaryObservers), alignof(DictionaryObservers));
    }
    for (auto *representation : representations_ | std::views::reverse) {
        representation->destroy(*memory_);
    }
    for (auto *subscribers : subscriber_groups_) {
        DynamicSubscribers::release(subscribers);
    }
}

ReferenceTarget ReferenceProvider::representation_target(std::uint32_t index) const {
    return representations_[index]->target();
}

void ReferenceProvider::add_root_subscriber(RuntimeGraph &graph, std::uint32_t input_offset, std::size_t data,
                                            std::uint32_t node_index, bool active, bool required,
                                            InputChangeFunction callback) {
    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_.front(), *memory_);
    subscribers.add(graph, input_offset, data, node_index, active, required, callback);
}

void ReferenceProvider::add_root_observer(RuntimeGraph &graph, void *target, DynamicObserverFunction function) {
    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_.front(), *memory_);
    subscribers.add_observer(graph, target, function);
}

void ReferenceProvider::remove_root_subscriber(RuntimeGraph &graph, std::size_t input_offset) noexcept {
    if (subscriber_groups_.front() != nullptr) {
        subscriber_groups_.front()->remove(graph, input_offset);
    }
}

void ReferenceProvider::remove_root_observer(RuntimeGraph &graph, const void *target) noexcept {
    if (subscriber_groups_.front() != nullptr) {
        subscriber_groups_.front()->remove_observer(graph, target);
    }
}

void ReferenceProvider::notify_root(OutputChangeContext &context, OutputChange change) {
    context.notify(subscriber_groups_.front(), change);
}

void ReferenceProvider::add_subscriber(ReferenceRoute &route, RuntimeGraph &graph, std::uint32_t input_offset,
                                       std::size_t data, std::uint32_t node_index, bool active, bool required,
                                       InputChangeFunction callback) {
    if (route.subscriber_group == 0) {
        subscriber_groups_.push_back(nullptr);
        route.subscriber_group = static_cast<std::uint32_t>(subscriber_groups_.size());
    }

    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_[route.subscriber_group - 1], *memory_);
    subscribers.add(graph, input_offset, data, node_index, active, required, callback);
}

void ReferenceProvider::add_observer(ReferenceRoute &route, RuntimeGraph &graph, void *target,
                                     DynamicObserverFunction function) {
    if (route.subscriber_group == 0) {
        subscriber_groups_.push_back(nullptr);
        route.subscriber_group = static_cast<std::uint32_t>(subscriber_groups_.size());
    }

    auto &subscribers = DynamicSubscribers::ensure(subscriber_groups_[route.subscriber_group - 1], *memory_);
    subscribers.add_observer(graph, target, function);
}

void ReferenceProvider::remove_subscriber(ReferenceRoute &route, RuntimeGraph &graph,
                                          std::size_t input_offset) noexcept {
    if (route.subscriber_group != 0) {
        subscriber_groups_[route.subscriber_group - 1]->remove(graph, input_offset);
    }
}

void ReferenceProvider::remove_observer(ReferenceRoute &route, RuntimeGraph &graph, const void *target) noexcept {
    if (route.subscriber_group != 0) {
        subscriber_groups_[route.subscriber_group - 1]->remove_observer(graph, target);
    }
}

void ReferenceProvider::notify(ReferenceRoute &route, OutputChangeContext &context, OutputChange change) {
    if (route.subscriber_group != 0) {
        context.notify(subscriber_groups_[route.subscriber_group - 1], change);
    }
}

void ReferenceProvider::add_dictionary_key_subscriber(ReferenceAddress source, const ScalarData &key,
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

void ReferenceProvider::remove_dictionary_key_subscriber(ReferenceAddress source, const ScalarData &key,
                                                         RuntimeGraph &graph, std::uint32_t input_offset) noexcept {
    if (dictionary_observers_ != nullptr) {
        dictionary_observers_->remove(source, key, graph, input_offset);
    }
}

ReferenceAddress ReferenceProvider::representation(ReferenceAddress source, const TimeSeriesType &requested_type) {
    const auto source_target = reference_target(source);
    if (&source_target.runtime_type->type() == &requested_type) {
        return source;
    }
    const auto root_target = reference_target(ReferenceAddress{root_});
    if (&root_target.runtime_type->type() == &requested_type) {
        return ReferenceAddress{root_};
    }

    for (auto *representation : representations_) {
        if (representation->source() == source && &representation->target().runtime_type->type() == &requested_type) {
            return representation->reference();
        }
    }

    auto *result = source_target.runtime_type->create_reference_representation(*this, source, requested_type, *memory_);
    auto release = util::scope_exit{[&]() noexcept { result->destroy(*memory_); }};
    result->bind(static_cast<std::uint32_t>(representations_.size()));
    representations_.push_back(result);
    release.release();
    return result->reference();
}

void ReferenceProvider::source_changed(OutputChangeContext &context, OutputChange change) {
    const auto count = representations_.size();
    for (auto index = std::size_t{}; index < count; ++index) {
        auto *representation = representations_[index];
        representation->source_changed(context, change);
    }
    if (dictionary_observers_ != nullptr) {
        dictionary_observers_->source_changed(context);
    }
    notify_root(context, change);
}

namespace {

ReferenceRoot &reference_root(ReferenceAddress address) noexcept {
    while (auto *route = address.get<ReferenceRoute>()) {
        address = route->parent;
    }
    return *address.get<ReferenceRoot>();
}

ReferenceProvider &require_provider(ReferenceRoot &root) {
    if (auto *provider = root.data.get<ReferenceProvider>()) {
        return *provider;
    }

    auto *subscribers = root.data.get<DynamicSubscribers>();
    auto &memory = root.graph->memory_resource();
    auto *storage = memory.allocate(sizeof(ReferenceProvider), alignof(ReferenceProvider));
    auto release = util::scope_exit{
        [&]() noexcept { memory.deallocate(storage, sizeof(ReferenceProvider), alignof(ReferenceProvider)); }};
    auto *provider = std::construct_at(static_cast<ReferenceProvider *>(storage), memory, root, subscribers);
    root.data = provider;
    release.release();
    return *provider;
}

ReferenceProvider &require_provider(ReferenceAddress address) {
    return require_provider(reference_root(address));
}

ReferenceProvider &route_provider(ReferenceAddress address) noexcept {
    return *reference_root(address).data.get<ReferenceProvider>();
}

} // namespace

ReferenceTarget reference_target(ReferenceAddress address) {
    if (auto *root = address.get<ReferenceRoot>()) {
        const auto source = root->graph->node(*root->source);
        return {&source.input_last_updated(0), source.input_value(0), root->runtime_type};
    }
    const auto &route = *address.get<ReferenceRoute>();
    if (route.is_representation()) {
        return route_provider(address).representation_target(route.representation());
    }

    const auto parent = reference_target(route.parent);
    const auto item = parent.runtime_type->item(parent.value, route.item());
    return {&item.last_updated, item.value, &item.runtime_type};
}

ReferenceAddress reference_representation(ReferenceAddress source, const TimeSeriesType &requested_type) {
    const auto target = reference_target(source);
    if (&target.runtime_type->type() == &requested_type) {
        return source;
    }
    return require_provider(source).representation(source, requested_type);
}

void add_reference_subscriber(ReferenceAddress address, RuntimeGraph &graph, std::uint32_t input_offset,
                              std::size_t data, std::uint32_t node_index, bool active, bool required,
                              InputChangeFunction callback) {
    if (auto *root = address.get<ReferenceRoot>()) {
        if (auto *provider = root->data.get<ReferenceProvider>()) {
            provider->add_root_subscriber(graph, input_offset, data, node_index, active, required, callback);
            return;
        }
        auto *subscribers = root->data.get<DynamicSubscribers>();
        auto &result = DynamicSubscribers::ensure(subscribers, root->graph->memory_resource());
        root->data = subscribers;
        result.add(graph, input_offset, data, node_index, active, required, callback);
        return;
    }
    auto &route = *address.get<ReferenceRoute>();
    route_provider(address).add_subscriber(route, graph, input_offset, data, node_index, active, required, callback);
}

void add_reference_observer(ReferenceAddress address, RuntimeGraph &graph, void *target,
                            DynamicObserverFunction function) {
    if (auto *root = address.get<ReferenceRoot>()) {
        if (auto *provider = root->data.get<ReferenceProvider>()) {
            provider->add_root_observer(graph, target, function);
            return;
        }
        auto *subscribers = root->data.get<DynamicSubscribers>();
        auto &result = DynamicSubscribers::ensure(subscribers, root->graph->memory_resource());
        root->data = subscribers;
        result.add_observer(graph, target, function);
        return;
    }
    auto &route = *address.get<ReferenceRoute>();
    route_provider(address).add_observer(route, graph, target, function);
}

void remove_reference_subscriber(ReferenceAddress address, RuntimeGraph &graph, std::size_t input_offset) noexcept {
    if (auto *root = address.get<ReferenceRoot>()) {
        if (auto *provider = root->data.get<ReferenceProvider>()) {
            provider->remove_root_subscriber(graph, input_offset);
        } else if (auto *subscribers = root->data.get<DynamicSubscribers>()) {
            subscribers->remove(graph, input_offset);
        }
        return;
    }
    auto &route = *address.get<ReferenceRoute>();
    route_provider(address).remove_subscriber(route, graph, input_offset);
}

void remove_reference_observer(ReferenceAddress address, RuntimeGraph &graph, const void *target) noexcept {
    if (auto *root = address.get<ReferenceRoot>()) {
        if (auto *provider = root->data.get<ReferenceProvider>()) {
            provider->remove_root_observer(graph, target);
        } else if (auto *subscribers = root->data.get<DynamicSubscribers>()) {
            subscribers->remove_observer(graph, target);
        }
        return;
    }
    auto &route = *address.get<ReferenceRoute>();
    route_provider(address).remove_observer(route, graph, target);
}

void notify_reference(ReferenceAddress address, OutputChangeContext &context, OutputChange change) {
    if (auto *root = address.get<ReferenceRoot>()) {
        if (auto *provider = root->data.get<ReferenceProvider>()) {
            provider->notify_root(context, change);
        } else {
            context.notify(root->data.get<DynamicSubscribers>(), change);
        }
        return;
    }
    auto &route = *address.get<ReferenceRoute>();
    route_provider(address).notify(route, context, change);
}

void add_reference_dictionary_key_subscriber(ReferenceAddress address, const ScalarData &key, RuntimeGraph &graph,
                                             std::uint32_t input_offset, std::uint32_t node_index) {
    require_provider(address).add_dictionary_key_subscriber(address, key, graph, input_offset, node_index);
}

void remove_reference_dictionary_key_subscriber(ReferenceAddress address, const ScalarData &key, RuntimeGraph &graph,
                                                std::uint32_t input_offset) noexcept {
    if (auto *provider = reference_root(address).data.get<ReferenceProvider>()) {
        provider->remove_dictionary_key_subscriber(address, key, graph, input_offset);
    }
}

void notify_reference_source(ReferenceRoot &root, OutputChangeContext &context, OutputChange change) {
    if (auto *provider = root.data.get<ReferenceProvider>()) {
        provider->source_changed(context, change);
    } else {
        context.notify(root.data.get<DynamicSubscribers>(), change);
    }
}

void destroy_reference_root(ReferenceRoot &root, std::pmr::memory_resource &memory) noexcept {
    if (auto *provider = root.data.get<ReferenceProvider>()) {
        std::destroy_at(provider);
        memory.deallocate(provider, sizeof(ReferenceProvider), alignof(ReferenceProvider));
    } else {
        DynamicSubscribers::release(root.data.get<DynamicSubscribers>());
    }
    std::destroy_at(&root);
}

} // namespace bg::details
