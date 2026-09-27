#include "type_registration.hpp"

#include <utility>

namespace bg::details {
namespace {

thread_local SemanticTypeRegistration *current_registration = nullptr;

} // namespace

std::recursive_mutex &semantic_type_registration_mutex() {
    static auto mutex = std::recursive_mutex{};
    return mutex;
}

SemanticTypeRegistration::SemanticTypeRegistration() noexcept {
    current_registration = this;
}

SemanticTypeRegistration::~SemanticTypeRegistration() {
    if (!committed_) {
        for (auto rollback = rollbacks_.rbegin(); rollback != rollbacks_.rend(); ++rollback) {
            (*rollback)();
        }
    }
    current_registration = nullptr;
}

SemanticTypeRegistration *SemanticTypeRegistration::current() noexcept {
    return current_registration;
}

void SemanticTypeRegistration::on_rollback(std::function<void()> rollback) {
    rollbacks_.push_back(std::move(rollback));
}

void SemanticTypeRegistration::on_finalize(std::function<bool()> finalize) {
    finalizers_.push_back(std::move(finalize));
}

void SemanticTypeRegistration::on_validate(std::function<void()> validate) {
    validators_.push_back(std::move(validate));
}

void SemanticTypeRegistration::commit() {
    auto changed = true;
    while (changed) {
        changed = false;
        for (const auto &finalize : finalizers_) {
            changed = finalize() || changed;
        }
    }
    for (const auto &validate : validators_) {
        validate();
    }
    committed_ = true;
}

} // namespace bg::details
