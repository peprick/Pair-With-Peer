#pragma once

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <netdb.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace pwp {

constexpr std::uint16_t kDefaultTrackerPort = 8080;
constexpr std::size_t kMaxStringSize = 4 * 1024;
constexpr std::uint32_t kMaxListedFiles = 100'000;

enum class TrackerRequest : std::uint32_t {
    kRegister = 1,
    kFindFile = 2,
    kListFiles = 3,
    kPeerInfo = 4,
    kUnregister = 5,
};

enum class PeerRequest : std::uint32_t {
    kHasFile = 1,
    kFetchFile = 2,
    kListFiles = 3,
    kOfferFile = 4,
};

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
};

class Socket {
  public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    ~Socket() { reset(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    Socket(Socket&& other) noexcept : fd_(other.release()) {}

    Socket& operator=(Socket&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] int get() const { return fd_; }
    [[nodiscard]] explicit operator bool() const { return fd_ >= 0; }

    int release() {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

  private:
    int fd_ = -1;
};

inline void configure_socket(int socket_fd) {
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    ::setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
}

inline void set_socket_timeout(int socket_fd, std::chrono::seconds timeout) {
    timeval value{};
    value.tv_sec = timeout.count();
    ::setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
    ::setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
}

inline bool send_all(int socket_fd, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t sent = 0;
    while (sent < size) {
#ifdef MSG_NOSIGNAL
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        const ssize_t result = ::send(socket_fd, bytes + sent, size - sent, flags);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

inline bool receive_all(int socket_fd, void* data, std::size_t size) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    std::size_t received = 0;
    while (received < size) {
        const ssize_t result = ::recv(socket_fd, bytes + received, size - received, 0);
        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result <= 0) {
            return false;
        }
        received += static_cast<std::size_t>(result);
    }
    return true;
}

inline bool send_u8(int socket_fd, std::uint8_t value) {
    return send_all(socket_fd, &value, sizeof(value));
}

inline bool receive_u8(int socket_fd, std::uint8_t& value) {
    return receive_all(socket_fd, &value, sizeof(value));
}

inline bool send_bool(int socket_fd, bool value) { return send_u8(socket_fd, value ? 1 : 0); }

inline bool receive_bool(int socket_fd, bool& value) {
    std::uint8_t wire_value = 0;
    if (!receive_u8(socket_fd, wire_value) || wire_value > 1) {
        return false;
    }
    value = wire_value == 1;
    return true;
}

inline bool send_u32(int socket_fd, std::uint32_t value) {
    const std::uint32_t wire_value = htonl(value);
    return send_all(socket_fd, &wire_value, sizeof(wire_value));
}

inline bool receive_u32(int socket_fd, std::uint32_t& value) {
    std::uint32_t wire_value = 0;
    if (!receive_all(socket_fd, &wire_value, sizeof(wire_value))) {
        return false;
    }
    value = ntohl(wire_value);
    return true;
}

inline bool send_u64(int socket_fd, std::uint64_t value) {
    std::uint8_t bytes[8]{};
    for (int index = 7; index >= 0; --index) {
        bytes[index] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
    return send_all(socket_fd, bytes, sizeof(bytes));
}

inline bool receive_u64(int socket_fd, std::uint64_t& value) {
    std::uint8_t bytes[8]{};
    if (!receive_all(socket_fd, bytes, sizeof(bytes))) {
        return false;
    }
    value = 0;
    for (const std::uint8_t byte : bytes) {
        value = (value << 8U) | byte;
    }
    return true;
}

inline bool send_string(int socket_fd, std::string_view value) {
    if (value.size() > kMaxStringSize || value.size() > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    return send_u32(socket_fd, static_cast<std::uint32_t>(value.size())) &&
           send_all(socket_fd, value.data(), value.size());
}

inline bool receive_string(int socket_fd, std::string& value) {
    std::uint32_t size = 0;
    if (!receive_u32(socket_fd, size) || size > kMaxStringSize) {
        return false;
    }
    value.assign(size, '\0');
    return receive_all(socket_fd, value.data(), value.size());
}

inline bool send_endpoint(int socket_fd, const Endpoint& endpoint) {
    return send_string(socket_fd, endpoint.host) && send_u32(socket_fd, endpoint.port);
}

inline bool receive_endpoint(int socket_fd, Endpoint& endpoint) {
    std::uint32_t port = 0;
    if (!receive_string(socket_fd, endpoint.host) || !receive_u32(socket_fd, port) || port == 0 ||
        port > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }
    endpoint.port = static_cast<std::uint16_t>(port);
    return true;
}

inline std::optional<Endpoint> parse_endpoint(std::string_view value) {
    const std::size_t separator = value.rfind(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 >= value.size()) {
        return std::nullopt;
    }

    const std::string host(value.substr(0, separator));
    const std::string port_text(value.substr(separator + 1));
    if (port_text.find_first_not_of("0123456789") != std::string::npos) {
        return std::nullopt;
    }

    try {
        const unsigned long port = std::stoul(port_text);
        if (port == 0 || port > std::numeric_limits<std::uint16_t>::max()) {
            return std::nullopt;
        }
        return Endpoint{host, static_cast<std::uint16_t>(port)};
    } catch (...) {
        return std::nullopt;
    }
}

inline std::string endpoint_to_string(const Endpoint& endpoint) {
    return endpoint.host + ":" + std::to_string(endpoint.port);
}

inline bool is_valid_username(std::string_view username) {
    if (username.empty() || username.size() > 32) {
        return false;
    }
    for (const char character : username) {
        const bool accepted =
            (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '-' || character == '_';
        if (!accepted) {
            return false;
        }
    }
    return true;
}

inline bool connect_with_timeout(int socket_fd, const sockaddr* address, socklen_t address_size,
                                 std::chrono::seconds timeout) {
    const int original_flags = ::fcntl(socket_fd, F_GETFL, 0);
    if (original_flags < 0 || ::fcntl(socket_fd, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        return false;
    }

    const int result = ::connect(socket_fd, address, address_size);
    if (result == 0) {
        ::fcntl(socket_fd, F_SETFL, original_flags);
        return true;
    }
    if (errno != EINPROGRESS) {
        ::fcntl(socket_fd, F_SETFL, original_flags);
        return false;
    }

    fd_set writable;
    FD_ZERO(&writable);
    FD_SET(socket_fd, &writable);
    timeval value{};
    value.tv_sec = timeout.count();

    const int selected = ::select(socket_fd + 1, nullptr, &writable, nullptr, &value);
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    const bool connected =
        selected > 0 &&
        ::getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_size) == 0 &&
        socket_error == 0;
    ::fcntl(socket_fd, F_SETFL, original_flags);
    return connected;
}

inline Socket connect_to(const Endpoint& endpoint,
                         std::chrono::seconds timeout = std::chrono::seconds(5)) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const std::string port = std::to_string(endpoint.port);
    if (::getaddrinfo(endpoint.host.c_str(), port.c_str(), &hints, &results) != 0) {
        return {};
    }

    Socket socket;
    for (addrinfo* address = results; address != nullptr; address = address->ai_next) {
        Socket candidate(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (!candidate) {
            continue;
        }
        configure_socket(candidate.get());
        if (connect_with_timeout(candidate.get(), address->ai_addr, address->ai_addrlen, timeout)) {
            set_socket_timeout(candidate.get(), timeout);
            socket = std::move(candidate);
            break;
        }
    }

    ::freeaddrinfo(results);
    return socket;
}

inline Socket create_listener(const Endpoint& endpoint, int backlog = 16) {
    addrinfo hints{};
    hints.ai_family = endpoint.host == "0.0.0.0" || endpoint.host == "*" ? AF_INET : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    const std::string port = std::to_string(endpoint.port);
    const char* host =
        endpoint.host == "0.0.0.0" || endpoint.host == "*" ? nullptr : endpoint.host.c_str();
    if (::getaddrinfo(host, port.c_str(), &hints, &results) != 0) {
        return {};
    }

    Socket listener;
    for (addrinfo* address = results; address != nullptr; address = address->ai_next) {
        Socket candidate(::socket(address->ai_family, address->ai_socktype, address->ai_protocol));
        if (!candidate) {
            continue;
        }
        configure_socket(candidate.get());
        const int reuse_address = 1;
        ::setsockopt(candidate.get(), SOL_SOCKET, SO_REUSEADDR, &reuse_address,
                     sizeof(reuse_address));
        if (::bind(candidate.get(), address->ai_addr, address->ai_addrlen) == 0 &&
            ::listen(candidate.get(), backlog) == 0) {
            listener = std::move(candidate);
            break;
        }
    }

    ::freeaddrinfo(results);
    return listener;
}

} // namespace pwp
