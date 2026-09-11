#pragma once

#include <bg/time_series/item_id.hpp>
#include <bg/time_series/ref/ref_provider.hpp>

#include <cstdint>

namespace bg::details {

class ScalarData;
class TsdKeySubscriptions;

class TsdKeySubscription {
  public:
    TsdKeySubscription() = default;

    explicit operator bool() const noexcept { return tsd_ != nullptr; }

  private:
    friend class TsdKeySubscriptions;
    friend bool tsd_key_subscription_matches(TsdKeySubscription, ReferenceAddress, const ScalarData &);
    friend ReferenceAddress tsd_key_subscription_target(TsdKeySubscription);
    friend void remove_tsd_key_subscriber(TsdKeySubscription, RuntimeGraph &, std::uint32_t) noexcept;

    TsdKeySubscription(void *tsd, TimeSeriesItemId item) noexcept : tsd_{tsd}, item_{item} {}

    void *tsd_{};
    TimeSeriesItemId item_{TimeSeriesItemId(-1)};
};

static_assert(sizeof(TsdKeySubscription) == 2 * sizeof(void *));

[[nodiscard]] TsdKeySubscription add_tsd_key_subscriber(ReferenceAddress address, const ScalarData &key,
                                                        RuntimeGraph &graph, std::uint32_t node_index);
bool tsd_key_subscription_matches(TsdKeySubscription subscription, ReferenceAddress tsd, const ScalarData &key);
ReferenceAddress tsd_key_subscription_target(TsdKeySubscription subscription);
void remove_tsd_key_subscriber(TsdKeySubscription subscription, RuntimeGraph &graph,
                               std::uint32_t node_index) noexcept;

} // namespace bg::details
