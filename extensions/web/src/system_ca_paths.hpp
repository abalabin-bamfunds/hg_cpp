#pragma once

#include <array>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>

namespace hg::web::details {

struct SystemCaPaths {
    std::optional<std::string> file;
    std::optional<std::string> directory;
};

inline SystemCaPaths system_ca_paths() {
    auto paths = SystemCaPaths{};
#if defined(__linux__)
    if (const auto *file = std::getenv("SSL_CERT_FILE")) {
        paths.file = file;
    }
    if (const auto *directory = std::getenv("SSL_CERT_DIR")) {
        paths.directory = directory;
    }

    if (!paths.file) {
        static constexpr std::array files{
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/cert.pem",
        };
        for (const auto *file : files) {
            std::error_code error;
            if (std::filesystem::is_regular_file(file, error)) {
                paths.file = file;
                break;
            }
        }
    }
    if (!paths.directory) {
        static constexpr std::array directories{"/etc/ssl/certs", "/etc/pki/tls/certs"};
        for (const auto *directory : directories) {
            std::error_code error;
            if (std::filesystem::is_directory(directory, error)) {
                paths.directory = directory;
                break;
            }
        }
    }
#endif

    return paths;
}

} // namespace hg::web::details
