#pragma once

#include "pwp/protocol.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace pwp {

constexpr std::size_t kTransferBufferSize = 64 * 1024;
constexpr std::uint64_t kMaxAcceptedFileSize = 1ULL * 1024ULL * 1024ULL * 1024ULL;

inline bool is_safe_filename(std::string_view filename) {
    return !filename.empty() && filename.size() <= 255 && filename != "." && filename != ".." &&
           filename.find('/') == std::string_view::npos &&
           filename.find('\\') == std::string_view::npos &&
           filename.find('\0') == std::string_view::npos;
}

inline std::vector<std::string> list_regular_files(const std::filesystem::path& directory) {
    std::vector<std::string> files;
    std::error_code error;
    for (std::filesystem::directory_iterator iterator(directory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        if (iterator->is_regular_file(error) && !error &&
            iterator->file_size(error) <= kMaxAcceptedFileSize && !error) {
            files.push_back(iterator->path().filename().string());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

inline bool write_all(int file_descriptor, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t written = 0;
    while (written < size) {
        const ssize_t result = ::write(file_descriptor, bytes + written, size - written);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

inline std::filesystem::path numbered_path(const std::filesystem::path& directory,
                                           const std::string& filename, std::uint32_t copy) {
    if (copy == 0) {
        return directory / filename;
    }
    const std::filesystem::path leaf(filename);
    return directory /
           (leaf.stem().string() + " (" + std::to_string(copy) + ")" + leaf.extension().string());
}

inline std::optional<std::filesystem::path>
commit_without_overwrite(const std::filesystem::path& partial,
                         const std::filesystem::path& directory, const std::string& filename) {
    for (std::uint32_t copy = 0; copy < 100'000; ++copy) {
        const std::filesystem::path destination = numbered_path(directory, filename, copy);
        if (::link(partial.c_str(), destination.c_str()) == 0) {
            ::unlink(partial.c_str());
            return destination;
        }
        if (errno != EEXIST) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

inline bool send_file(int socket_fd, const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return false;
    }

    const std::uint64_t size = std::filesystem::file_size(path, error);
    if (error || size > kMaxAcceptedFileSize) {
        return false;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input || !send_u64(socket_fd, size)) {
        return false;
    }

    std::array<char, kTransferBufferSize> buffer{};
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        input.read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (input.gcount() != static_cast<std::streamsize>(chunk) ||
            !send_all(socket_fd, buffer.data(), chunk)) {
            return false;
        }
        remaining -= chunk;
    }
    return true;
}

inline std::optional<std::filesystem::path>
receive_file(int socket_fd, const std::filesystem::path& directory, const std::string& filename) {
    if (!is_safe_filename(filename)) {
        return std::nullopt;
    }

    std::uint64_t size = 0;
    if (!receive_u64(socket_fd, size) || size > kMaxAcceptedFileSize) {
        return std::nullopt;
    }

    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return std::nullopt;
    }

    std::string partial_template = (directory / ".pwp-transfer-XXXXXX").string();
    std::vector<char> partial_buffer(partial_template.begin(), partial_template.end());
    partial_buffer.push_back('\0');
    const int output_fd = ::mkstemp(partial_buffer.data());
    if (output_fd < 0) {
        return std::nullopt;
    }
    const std::filesystem::path partial(partial_buffer.data());

    std::array<char, kTransferBufferSize> buffer{};
    std::uint64_t remaining = size;
    while (remaining > 0) {
        const std::size_t chunk =
            static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
        if (!receive_all(socket_fd, buffer.data(), chunk) ||
            !write_all(output_fd, buffer.data(), chunk)) {
            ::close(output_fd);
            std::filesystem::remove(partial, error);
            return std::nullopt;
        }
        remaining -= chunk;
    }

    if (::close(output_fd) != 0) {
        std::filesystem::remove(partial, error);
        return std::nullopt;
    }
    const auto destination = commit_without_overwrite(partial, directory, filename);
    if (!destination) {
        std::filesystem::remove(partial, error);
    }
    return destination;
}

} // namespace pwp
