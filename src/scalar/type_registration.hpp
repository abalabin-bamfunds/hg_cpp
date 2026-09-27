#pragma once

#include <functional>
#include <mutex>
#include <vector>

namespace bg::details {

std::recursive_mutex &semantic_type_registration_mutex();

class SemanticTypeRegistration {
  public:
    SemanticTypeRegistration() noexcept;
    ~SemanticTypeRegistration();

    SemanticTypeRegistration(const SemanticTypeRegistration &) = delete;
    SemanticTypeRegistration &operator=(const SemanticTypeRegistration &) = delete;

    static SemanticTypeRegistration *current() noexcept;

    void on_rollback(std::function<void()> rollback);
    void on_finalize(std::function<bool()> finalize);
    void on_validate(std::function<void()> validate);
    void commit();

  private:
    std::vector<std::function<void()>> rollbacks_;
    std::vector<std::function<bool()>> finalizers_;
    std::vector<std::function<void()>> validators_;
    bool committed_{false};
};

} // namespace bg::details
