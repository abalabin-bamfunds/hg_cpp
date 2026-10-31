#pragma once

#include <cstdint>

namespace bg {

struct GraphInstanceId {
    std::uint64_t value{};

    friend bool operator==(GraphInstanceId, GraphInstanceId) = default;
};

class GraphInstanceIdRange {
  public:
    GraphInstanceId at(std::uint32_t index) const noexcept { return {first_.value + index}; }

  private:
    explicit GraphInstanceIdRange(GraphInstanceId first) noexcept : first_{first} {}

    GraphInstanceId first_;

    friend class GraphInstanceIdAllocator;
};

class GraphInstanceIdAllocator {
  public:
    static GraphInstanceId allocate();
    static GraphInstanceIdRange reserve(std::uint64_t count);

  private:
    static GraphInstanceId reserve_ids(std::uint64_t count);
};

} // namespace bg
