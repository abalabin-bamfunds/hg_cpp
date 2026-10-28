#include <bg/logging.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>

#include <mutex>
#include <stdexcept>
#include <utility>

namespace bg::logging {
namespace {

struct InitializationState {
    std::mutex mutex;
    quill::Logger *logger{};
};

InitializationState &initialization_state() {
    static auto state = InitializationState{};
    return state;
}

std::shared_ptr<quill::Sink> stdout_sink() {
    auto sink_configuration = quill::ConsoleSinkConfig{};
    sink_configuration.set_stream("stdout");
    return quill::Frontend::create_or_get_sink<quill::ConsoleSink>("bg", sink_configuration);
}

quill::Logger *initialize_lazily() {
    auto &state = initialization_state();
    const auto lock = std::scoped_lock{state.mutex};

    if (state.logger != nullptr) {
        return state.logger;
    }

    auto *existing = quill::Frontend::get_logger("bg");
    if (!quill::Backend::is_running()) {
        quill::Backend::start();
    }

    if (existing != nullptr) {
        state.logger = existing;
        return state.logger;
    }

    state.logger = quill::Frontend::create_or_get_logger("bg", stdout_sink(), default_pattern_options());
    return state.logger;
}

} // namespace

quill::PatternFormatterOptions default_pattern_options() {
    return quill::PatternFormatterOptions{"%(time) LOG_%(log_level:<9) %(logger:<12) %(message)%(mdc)",
                                         "%H:%M:%S.%Qus"};
}

quill::Logger *initialize(std::vector<std::shared_ptr<quill::Sink>> sinks,
                          const quill::PatternFormatterOptions &pattern,
                          const quill::BackendOptions &backend) {
    auto &state = initialization_state();
    const auto lock = std::scoped_lock{state.mutex};

    if (state.logger != nullptr || quill::Frontend::get_logger("bg") != nullptr) {
        throw std::logic_error{"BG logging has already been initialized or accessed"};
    }
    if (quill::Backend::is_running()) {
        throw std::logic_error{"Quill's backend is already running"};
    }

    if (sinks.empty()) {
        sinks.push_back(stdout_sink());
    }

    auto *logger = quill::Frontend::create_logger("bg", std::move(sinks), pattern);
    quill::Backend::start(backend);
    state.logger = logger;
    return logger;
}

quill::Logger *logger() {
    static auto *const bg_logger = initialize_lazily();
    return bg_logger;
}

void flush() { logger()->flush_log(); }

} // namespace bg::logging
