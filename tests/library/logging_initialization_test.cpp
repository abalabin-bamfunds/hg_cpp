#include <bg/logging.hpp>
#include <bg/util/scope_exit.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

std::filesystem::path log_path() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / ("bg_logging_" + std::to_string(nonce) + ".log");
}

std::string read_file(const std::filesystem::path &path) {
    auto file = std::ifstream{path};
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

auto remove_file_on_exit(const std::filesystem::path &path) {
    return bg::util::scope_exit{[path]() noexcept {
        auto error = std::error_code{};
        std::filesystem::remove(path, error);
    }};
}

int stream_descriptor(std::FILE *stream) {
#if defined(_WIN32)
    return _fileno(stream);
#else
    return fileno(stream);
#endif
}

int duplicate_descriptor(int descriptor) {
#if defined(_WIN32)
    return _dup(descriptor);
#else
    return dup(descriptor);
#endif
}

int replace_descriptor(int destination, int source) {
#if defined(_WIN32)
    return _dup2(source, destination);
#else
    return dup2(source, destination);
#endif
}

void close_descriptor(int descriptor) {
#if defined(_WIN32)
    _close(descriptor);
#else
    close(descriptor);
#endif
}

class ConsoleCapture {
  public:
    ConsoleCapture(std::FILE *stream, const std::filesystem::path &path) : stream_{stream} {
        file_ = std::fopen(path.string().c_str(), "w");
        if (file_ == nullptr) {
            throw std::runtime_error{"could not create console capture file"};
        }

        const auto descriptor = stream_descriptor(stream_);
        original_descriptor_ = duplicate_descriptor(descriptor);
        if (original_descriptor_ < 0 || replace_descriptor(descriptor, stream_descriptor(file_)) < 0) {
            if (original_descriptor_ >= 0) {
                close_descriptor(original_descriptor_);
                original_descriptor_ = -1;
            }
            close();
            throw std::runtime_error{"could not redirect console stream"};
        }
        redirected_ = true;
    }

    ConsoleCapture(const ConsoleCapture &) = delete;
    ConsoleCapture &operator=(const ConsoleCapture &) = delete;

    ~ConsoleCapture() { restore(); }

    void restore() noexcept {
        if (!redirected_) {
            close();
            return;
        }

        std::fflush(stream_);
        (void)replace_descriptor(stream_descriptor(stream_), original_descriptor_);
        close_descriptor(original_descriptor_);
        original_descriptor_ = -1;
        redirected_ = false;
        close();
    }

  private:
    void close() noexcept {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    std::FILE *stream_{};
    std::FILE *file_{};
    int original_descriptor_{-1};
    bool redirected_{};
};

} // namespace

TEST_CASE("BG logging initializes lazily and rejects later configuration", "[library][logging]") {
    bg::log::info("lazy initialization");
    bg::logging::flush();

    CHECK_THROWS_AS(bg::logging::initialize(), std::logic_error);
}

TEST_CASE("BG logging initializes to an append-only file sink", "[library][logging]") {
    const auto path = log_path();
    const auto cleanup = remove_file_on_exit(path);
    {
        auto file = std::ofstream{path};
        REQUIRE(file.is_open());
        file << "previous\n";
    }

    bg::logging::initialize({.file = path.string(), .std_err = true, .pattern = "{message}"});
    CHECK(bg::logging::level() == bg::logging::Level::info);
    CHECK(bg::logging::enabled(bg::logging::Level::info));
    CHECK_FALSE(bg::logging::enabled(bg::logging::Level::debug));

    bg::log::info("configured output");
    bg::logging::flush();

    CHECK(read_file(path) == "previous\nconfigured output\n");
    CHECK_THROWS_AS(bg::logging::initialize(), std::logic_error);
}

TEST_CASE("BG logging validates patterns before initialization", "[library][logging]") {
    CHECK_THROWS_AS(bg::logging::initialize({.pattern = "{unknown}"}), std::invalid_argument);
    CHECK_THROWS_AS(bg::logging::initialize({.pattern = "{message} {message}"}), std::invalid_argument);
    CHECK_THROWS_AS(bg::logging::initialize({.pattern = "%(message)"}), std::invalid_argument);

    const auto path = log_path();
    const auto cleanup = remove_file_on_exit(path);
    bg::logging::initialize({.file = path.string(), .pattern = "{{{level:>8}}}|{logger}|{message}"});
    bg::log::warning("ready");
    bg::logging::flush();

    CHECK(read_file(path).find("{ WARNING}|bg|ready\n") != std::string::npos);
}

TEST_CASE("BG logging level changes control enabled calls", "[library][logging]") {
    CHECK(bg::logging::level() == bg::logging::Level::info);
    bg::logging::set_level(bg::logging::Level::debug);
    CHECK(bg::logging::level() == bg::logging::Level::debug);
    CHECK(bg::logging::enabled(bg::logging::Level::debug));
    CHECK(bg::logging::enabled(bg::logging::Level::critical));
    CHECK_FALSE(bg::logging::enabled(bg::logging::Level::trace));

    bg::logging::set_level(bg::logging::Level::off);
    CHECK_FALSE(bg::logging::enabled(bg::logging::Level::critical));
    CHECK_FALSE(bg::logging::enabled(bg::logging::Level::off));
}

TEST_CASE("BG logging defaults to stdout with microsecond timestamps", "[library][logging]") {
    const auto path = log_path();
    const auto cleanup = remove_file_on_exit(path);
    {
        auto capture = ConsoleCapture{stdout, path};
        bg::logging::initialize();
        bg::log::info("default output");
        bg::logging::flush();
        capture.restore();
    }

    const auto output = read_file(path);
    CHECK(std::regex_search(output, std::regex{R"(^\d{2}:\d{2}:\d{2}\.\d{6} LOG_INFO\s+bg\s+default output\n$)"}));
}

TEST_CASE("BG logging can select stderr and UTC timestamps", "[library][logging]") {
    const auto path = log_path();
    const auto cleanup = remove_file_on_exit(path);
    {
        auto capture = ConsoleCapture{stderr, path};
        bg::logging::initialize({.std_err = true, .pattern = "{time}|{message}", .time_pattern = "%z", .utc = true});
        bg::log::info("UTC output");
        bg::logging::flush();
        capture.restore();
    }

    const auto output = read_file(path);
    CHECK(std::regex_search(output, std::regex{R"(^\+0000\|UTC output\n$)"}));
}
