#pragma once

#include <hg/evaluation_time.hpp>

#include <chrono>

namespace hg {

class WallClock {
  public:
    virtual ~WallClock() = default;

    virtual EvaluationTime now() const noexcept = 0;
};

class SystemWallClock final : public WallClock {
  public:
    static const SystemWallClock &instance() noexcept {
        static const SystemWallClock clock;
        return clock;
    }

    EvaluationTime now() const noexcept override {
        return std::chrono::time_point_cast<EvaluationTime::duration>(std::chrono::system_clock::now());
    }
};

} // namespace hg
