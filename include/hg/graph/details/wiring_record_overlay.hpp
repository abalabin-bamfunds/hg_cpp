#pragma once

#include <memory>
#include <vector>

namespace bg {

class GraphWiring;
class WiringPoint;

namespace details {

class WiringRecord;

// Owns wiring records needed only while resolving a call against one GraphWiring.
class WiringRecordOverlay {
  public:
    explicit WiringRecordOverlay(GraphWiring &wiring) noexcept;
    ~WiringRecordOverlay();

    WiringRecordOverlay(const WiringRecordOverlay &) = delete;
    WiringRecordOverlay &operator=(const WiringRecordOverlay &) = delete;
    WiringRecordOverlay(WiringRecordOverlay &&) = delete;
    WiringRecordOverlay &operator=(WiringRecordOverlay &&) = delete;

    // Returned wiring points support resolution-time inspection only and must not be materialized.
    [[nodiscard]] WiringPoint add(std::unique_ptr<WiringRecord> record);
    // Invalidates every wiring point returned by add().
    void clear() noexcept;

  private:
    GraphWiring *wiring_;
    std::vector<std::unique_ptr<WiringRecord>> records_;
};

} // namespace details
} // namespace bg
