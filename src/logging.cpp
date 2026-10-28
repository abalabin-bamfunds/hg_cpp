#include <bg/logging.hpp>

#include <quill/Backend.h>
#include <quill/Frontend.h>
#include <quill/sinks/ConsoleSink.h>

#include <utility>

namespace bg::logging {
quill::Logger *logger() {
    static auto *const bg_logger = [] {
        quill::Backend::start();

        if (auto *existing = quill::Frontend::get_logger("bg"); existing != nullptr) {
            return existing;
        }

        auto sink_configuration = quill::ConsoleSinkConfig{};
        sink_configuration.set_stream("stdout");
        auto sink = quill::Frontend::create_or_get_sink<quill::ConsoleSink>("bg", sink_configuration);
        return quill::Frontend::create_or_get_logger("bg", std::move(sink));
    }();
    return bg_logger;
}

void flush() { logger()->flush_log(); }

} // namespace bg::logging
