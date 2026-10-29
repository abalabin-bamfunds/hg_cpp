#pragma once

#include <bg/export.hpp>
#include <bg/type/type.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <stdexcept>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bg {

// Owns semantic types whose references can outlive the operation that created them.
// Static registration accepts only objects with process-static storage duration.
class BG_EXPORT TypeRegistry {
  private:
    class CacheBase {
      public:
        virtual ~CacheBase() = default;
    };

    template <typename Descriptor, typename Key, typename Hash> class KeyedCache final : public CacheBase {
      public:
        template <typename Factory>
        const Descriptor &get_or_create(TypeRegistry &registry, Key key, Factory &&factory) {
            if (const auto found = descriptors_.find(key); found != descriptors_.end()) {
                return *found->second;
            }

            auto candidate = std::unique_ptr<Descriptor>{std::invoke(std::forward<Factory>(factory))};
            if (candidate == nullptr) {
                throw std::invalid_argument{"a type registry factory returned no descriptor"};
            }

            const auto &descriptor = registry.register_owned(std::move(candidate));
            descriptors_.try_emplace(std::move(key), &descriptor);
            return descriptor;
        }

      private:
        std::unordered_map<Key, const Descriptor *, Hash> descriptors_;
    };

  public:
    static TypeRegistry &instance() noexcept;

    // Registration runs on the setup thread. Factories must return a new descriptor and must not request their own
    // key recursively. Completed descriptors can be read concurrently after registration finishes.
    template <typename Descriptor, typename Hash, typename Key, typename Factory>
        requires std::derived_from<Descriptor, Type> &&
                 requires(Factory &candidate_factory) {
                     { std::invoke(candidate_factory) } -> std::convertible_to<std::unique_ptr<Descriptor>>;
                 }
    const Descriptor &get_or_create(Key key, Factory &&factory) {
        return keyed_cache<Descriptor, Key, Hash>().get_or_create(*this, std::move(key),
                                                                  std::forward<Factory>(factory));
    }

    template <typename T>
        requires std::derived_from<T, Type>
    static const T &register_static(const T &type) noexcept {
        register_static_type(type);
        return type;
    }

    template <typename T>
        requires std::derived_from<T, Type>
    const T &register_owned(std::unique_ptr<T> type) {
        const auto *result = type.get();
        register_owned_type(std::move(type));
        return *result;
    }

    bool contains(const Type &type) const;

  private:
    template <typename Descriptor, typename Key, typename Hash>
    KeyedCache<Descriptor, Key, Hash> &keyed_cache() {
        using cache_type = KeyedCache<Descriptor, Key, Hash>;
        const auto key = std::type_index{typeid(cache_type)};
        if (const auto found = caches_.find(key); found != caches_.end()) {
            return static_cast<cache_type &>(*found->second);
        }

        auto cache = std::make_unique<cache_type>();
        auto *result = cache.get();
        caches_.try_emplace(key, std::move(cache));
        return *result;
    }

    TypeRegistry() = default;
    static void register_static_type(const Type &type) noexcept;
    void register_owned_type(std::unique_ptr<Type> type);

    std::unordered_map<std::type_index, std::unique_ptr<CacheBase>> caches_;
    details::StaticRegistryEntry<Type> *static_entries_{};
    std::vector<std::unique_ptr<Type>> owned_types_;
};

} // namespace bg
