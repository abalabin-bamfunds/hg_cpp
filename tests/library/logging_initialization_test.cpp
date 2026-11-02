#include <hg/logging.hpp>
#include <hg/util/scope_exit.hpp>

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
    return std::filesystem::temp_directory_path() / ("hg_logging_" + std::to_string(nonce) + ".log");
}

std::string read_file(const std::filesystem::path &path) {
    auto file = std::ifstream{path};
    REQUIRE(file.is_open());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

auto remove_file_on_exit(const std::filesystem::path &path) {
    return hg::util::scope_exit{[path]() noexcept {
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

TEST_CASE("HG logging initializes lazily and rejects later configuration", "[library][logging]") {
    hg::log::info("lazy initialization");
    hg::logging::flush();

    CHECK_THROWS_AS(hg::logging::initialize(), std::logic_error);
}

TEST_CASE("HG logging initializes to an append-only file sink", "[library][logging]") {
    const auto path = log_path();
    const auto cleanup = remove_file_on_exit(path);
    {
        auto file = std::ofstream{path};
        REQUIRE(file.is_open());
        file << "previous\n";
    }

    hg::logging::initialize({.file = path.string(), .std_err = true, .pattern = "{message}"});
    CHECK(hg::logging::level() == hg::logging::Level::info);
    CHECK(hg::logging::enabled(hg::logging::Level::info));
    CHECK_FALSE(hg::logging::enabled(hg::logging::Level::debug));

    hg::log::info("configured output");
    hg::logging::flush();

    CHECK(read_file(path) == "previous\nconfigured output\n");
    CHECK_THROWS_AS(hg::logging::initialize(), std::logic_error);
}

TEST_CASE("HG logging validates patterns before initialization", "[library][logging]") {
    CHECK_THROWS_AS(hg::logging::initialize({.pattern = "{unknown}"}), std::invalid_argument);
    CHECK_THROWS_AS(hg::logging::initialize({.pattern = "{message} {message}"}), std::invalid_argument);
    CHECK_THROWS_AS(hg::logging::initialize({.pattern = "%(message)"}), std::invalid_argument);

    const auto path = log_path();
    const auto cleanup = remove_file_on_exit(path);
    hg::logging::initialize({.file = path.string(), .pattern = "{{{level:>8}}}|{logger}|{message}"});
    hg::log::warning("ready");
    hg::logging::flush();

    CHECK(read_file(path).find("{ WARNING}|hg|ready\n") != std::string::npos);
}

TEST_CASE("HG logging level changes control enabled calls", "[library][logging]") {
    CHECK(hg::logging::level() == hg::logging::Level::info);
    hg::logging::set_level(hg::logging::Level::debug);
    CHECK(hg::logging::level() == hg::logging::Level::debug);
    CHECK(hg::logging::enabled(hg::logging::Level::debug));
    CHECK(hg::logging::enabled(hg::logging::Level::critical));
    CHECK_FALSE(hg::logging::enabled(hg::logging::Level::trace));

    hg::logging::set_level(hg::logging::Level::off);
    CHECK_FALSE(hg::logging::enabled(hg::logging::Level::critical));
    CHECK_FALSE(hg::logging::enabled(hg::logging::Level::off));
}

TEST_CASE("HG logging defaults to stdout with microsecond timestamps", "[library][logging]") {
    const auto path = log_path();
    const auto cleanup = remove_file_on_exit(path);
    {
        auto capture = ConsoleCapture{stdout, path};
        hg::logging::initialize();
        hg::log::info("default output");
        hg::logging::flush();
        capture.restore();
    }

    const auto output = read_file(path);
    CHECK(std::regex_search(output, std::regex{R"(^\d{2}:\d{2}:\d{2}\.\d{6} LOG_INFO\s+hg\s+default output\n$)"}));
}

TEST_CASE("HG logging can select stderr and UTC timestamps", "[library][logging]") {
    const auto path = log_path();
    const auto cleanup = remove_file_on_exit(path);
    {
        auto capture = ConsoleCapture{stderr, path};
        hg::logging::initialize({.std_err = true, .pattern = "{time}|{message}", .time_pattern = "%z", .utc = true});
        hg::log::info("UTC output");
        hg::logging::flush();
        capture.restore();
    }

    const auto output = read_file(path);
    CHECK(std::regex_search(output, std::regex{R"(^\+0000\|UTC output\n$)"}));
}
