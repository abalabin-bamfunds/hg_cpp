#include <bg/logging.hpp>

#include <quill/Backend.h>
#include <quill/DeferredFormatCodec.h>
#include <quill/Frontend.h>
#include <quill/LogMacros.h>
#include <quill/sinks/ConsoleSink.h>
#include <quill/sinks/FileSink.h>

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace bg::logging::details {

struct DeferredMessage {
    mutable std::move_only_function<std::string()> format;
};

static_assert(std::is_nothrow_move_constructible_v<DeferredMessage>);
static_assert(std::is_nothrow_destructible_v<DeferredMessage>);

} // namespace bg::logging::details

namespace quill {

template <> struct Codec<bg::logging::details::DeferredMessage>
    : DeferredFormatCodec<bg::logging::details::DeferredMessage> {};

} // namespace quill

template <> struct fmtquill::formatter<bg::logging::details::DeferredMessage> {
    constexpr auto parse(fmtquill::format_parse_context &context) {
        const auto it = context.begin();
        if (it != context.end() && *it != '}') {
            throw fmtquill::format_error{"deferred log messages do not accept format specifiers"};
        }
        return it;
    }

    auto format(const bg::logging::details::DeferredMessage &value, fmtquill::format_context &context) const {
        const auto message = value.format();
        return fmtquill::format_to(context.out(), "{}", message);
    }
};

namespace bg::logging {
namespace {

struct State {
    std::mutex mutex;
    std::atomic<Level> level{Level::info};
    quill::Logger *logger{};
};

State &state() {
    static auto value = State{};
    return value;
}

quill::LogLevel to_quill(Level value) {
    switch (value) {
    case Level::trace:
        return quill::LogLevel::TraceL1;
    case Level::debug:
        return quill::LogLevel::Debug;
    case Level::info:
        return quill::LogLevel::Info;
    case Level::warning:
        return quill::LogLevel::Warning;
    case Level::error:
        return quill::LogLevel::Error;
    case Level::critical:
        return quill::LogLevel::Critical;
    case Level::off:
        return quill::LogLevel::None;
    }
    throw std::invalid_argument{"unknown BG logging level"};
}

std::string translate_pattern(std::string_view pattern) {
    static const auto quill_attributes = std::map<std::string_view, std::string_view>{
        {"time", "time"}, {"level", "log_level"}, {"logger", "logger"}, {"message", "message"}};
    auto used_attributes = std::set<std::string_view>{};
    auto result = std::string{};

    for (auto index = std::size_t{}; index < pattern.size();) {
        const auto character = pattern[index];
        if (character == '{') {
            if (index + 1 < pattern.size() && pattern[index + 1] == '{') {
                result.push_back('{');
                index += 2;
                continue;
            }

            const auto end = pattern.find('}', index + 1);
            if (end == std::string_view::npos) {
                throw std::invalid_argument{"logging pattern contains an unclosed field"};
            }

            const auto field = pattern.substr(index + 1, end - index - 1);
            const auto separator = field.find(':');
            const auto name = field.substr(0, separator);
            const auto spec = separator == std::string_view::npos ? std::string_view{} : field.substr(separator + 1);
            if (!quill_attributes.contains(name)) {
                throw std::invalid_argument{"logging pattern contains an unknown field"};
            }
            if (!used_attributes.insert(name).second) {
                throw std::invalid_argument{"logging pattern repeats a field"};
            }
            if (separator != std::string_view::npos && spec.empty()) {
                throw std::invalid_argument{"logging pattern field width must include a number"};
            }
            if (!spec.empty()) {
                auto width = spec;
                if (width.front() == '<' || width.front() == '>' || width.front() == '^') {
                    width.remove_prefix(1);
                }
                if (width.empty()) {
                    throw std::invalid_argument{"logging pattern field width must include a number"};
                }
                for (const auto digit : width) {
                    if (digit < '0' || digit > '9') {
                        throw std::invalid_argument{"logging pattern field width must be numeric"};
                    }
                }
            }

            result += "%(";
            result += quill_attributes.at(name);
            if (!spec.empty()) {
                result.push_back(':');
                result.append(spec);
            }
            result.push_back(')');
            index = end + 1;
            continue;
        }

        if (character == '}') {
            if (index + 1 < pattern.size() && pattern[index + 1] == '}') {
                result.push_back('}');
                index += 2;
                continue;
            }
            throw std::invalid_argument{"logging pattern contains an unmatched closing brace"};
        }

        if (character == '%' && index + 1 < pattern.size() && pattern[index + 1] == '(') {
            throw std::invalid_argument{"logging pattern uses unsupported backend syntax"};
        }

        result.push_back(character);
        ++index;
    }

    return result;
}

std::shared_ptr<quill::Sink> create_sink(const Config &config) {
    if (!config.file.empty()) {
        auto sink_config = quill::FileSinkConfig{};
        sink_config.set_open_mode('a');
        return quill::Frontend::create_or_get_sink<quill::FileSink>(config.file, sink_config);
    }

    auto sink_config = quill::ConsoleSinkConfig{};
    sink_config.set_stream(config.std_err ? "stderr" : "stdout");
    return quill::Frontend::create_or_get_sink<quill::ConsoleSink>("bg", sink_config);
}

quill::PatternFormatterOptions pattern_options(const Config &config) {
    return quill::PatternFormatterOptions{
        translate_pattern(config.pattern), config.time_pattern,
        config.utc ? quill::Timezone::GmtTime : quill::Timezone::LocalTime};
}

quill::Logger *initialize_lazily() {
    auto &current = state();
    const auto lock = std::scoped_lock{current.mutex};
    if (current.logger != nullptr) {
        return current.logger;
    }
    if (quill::Backend::is_running()) {
        throw std::logic_error{"BG logging must be initialized before the logging backend starts"};
    }
    if (quill::Frontend::get_logger("bg") != nullptr) {
        throw std::logic_error{"the BG logger name is already in use"};
    }

    auto config = Config{.level = current.level.load(std::memory_order_relaxed)};
    auto *logger = quill::Frontend::create_logger("bg", create_sink(config), pattern_options(config));
    logger->set_log_level(to_quill(config.level));
    quill::Backend::start();
    current.logger = logger;
    return logger;
}

quill::Logger *logger() {
    static auto *const value = initialize_lazily();
    return value;
}

} // namespace

void initialize(Config config) {
    const auto quill_level = to_quill(config.level);
    const auto options = pattern_options(config);
    auto &current = state();
    const auto lock = std::scoped_lock{current.mutex};
    if (current.logger != nullptr) {
        throw std::logic_error{"BG logging has already been initialized or used"};
    }
    if (quill::Backend::is_running()) {
        throw std::logic_error{"BG logging must be initialized before the logging backend starts"};
    }
    if (quill::Frontend::get_logger("bg") != nullptr) {
        throw std::logic_error{"the BG logger name is already in use"};
    }

    auto *created = quill::Frontend::create_logger("bg", create_sink(config), options);
    created->set_log_level(quill_level);
    quill::Backend::start();
    current.level.store(config.level, std::memory_order_relaxed);
    current.logger = created;
}

void set_level(Level value) {
    const auto quill_level = to_quill(value);
    auto &current = state();
    const auto lock = std::scoped_lock{current.mutex};
    if (current.logger != nullptr) {
        current.logger->set_log_level(quill_level);
    }
    current.level.store(value, std::memory_order_relaxed);
}

Level level() noexcept { return state().level.load(std::memory_order_relaxed); }

bool enabled(Level value) noexcept {
    const auto configured_level = level();
    return value != Level::off && configured_level != Level::off && value >= configured_level;
}

void flush() { logger()->flush_log(); }

void details::submit(Level value, std::move_only_function<std::string()> format) {
    if (!enabled(value)) {
        return;
    }
    QUILL_LOG_DYNAMIC(logger(), to_quill(value), "{}", details::DeferredMessage{std::move(format)});
}

} // namespace bg::logging
