#include "pwp/protocol.hpp"

#include <atomic>
#include <csignal>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

using pwp::Endpoint;
using pwp::PeerRequest;
using pwp::Socket;
using pwp::TrackerRequest;

struct PeerRecord {
    std::string username;
    Endpoint endpoint;
};

struct FileProbe {
    PeerRecord peer;
    bool available = false;
};

struct PeerListing {
    PeerRecord peer;
    std::vector<std::string> files;
    bool valid = false;
};

constexpr std::size_t kMaxRegisteredPeers = 128;

class Tracker {
  public:
    explicit Tracker(Endpoint listen_endpoint) : listen_endpoint_(std::move(listen_endpoint)) {}

    int run() {
        Socket listener = pwp::create_listener(listen_endpoint_);
        if (!listener) {
            std::cerr << "error: could not listen on " << pwp::endpoint_to_string(listen_endpoint_)
                      << '\n';
            return 1;
        }

        std::cout << "Pair with Peer tracker\n"
                  << "Listening on " << pwp::endpoint_to_string(listen_endpoint_) << '\n'
                  << std::flush;

        while (true) {
            sockaddr_storage address{};
            socklen_t address_size = sizeof(address);
            const int client_fd =
                ::accept(listener.get(), reinterpret_cast<sockaddr*>(&address), &address_size);
            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                std::cerr << "error: accept failed: " << std::strerror(errno) << '\n';
                return 1;
            }

            pwp::configure_socket(client_fd);
            pwp::set_socket_timeout(client_fd, std::chrono::seconds(10));
            try {
                std::thread(&Tracker::handle_connection, this, Socket(client_fd)).detach();
            } catch (const std::system_error& error) {
                log(std::string("warning: could not start request worker: ") + error.what());
            }
        }
    }

  private:
    void handle_connection(Socket socket) {
        std::uint32_t request_value = 0;
        if (!pwp::receive_u32(socket.get(), request_value)) {
            return;
        }

        try {
            switch (static_cast<TrackerRequest>(request_value)) {
            case TrackerRequest::kRegister:
                handle_register(socket.get());
                break;
            case TrackerRequest::kFindFile:
                handle_find_file(socket.get());
                break;
            case TrackerRequest::kListFiles:
                handle_list_files(socket.get());
                break;
            case TrackerRequest::kPeerInfo:
                handle_peer_info(socket.get());
                break;
            case TrackerRequest::kUnregister:
                handle_unregister(socket.get());
                break;
            default:
                break;
            }
        } catch (const std::exception& error) {
            log(std::string("warning: request failed: ") + error.what());
        }
    }

    void handle_register(int socket_fd) {
        std::string username;
        Endpoint endpoint;
        if (!pwp::receive_string(socket_fd, username) ||
            !pwp::receive_endpoint(socket_fd, endpoint) || !pwp::is_valid_username(username)) {
            pwp::send_bool(socket_fd, false);
            return;
        }

        bool registered = false;
        bool replaced_stale_peer = false;
        std::optional<Endpoint> existing_endpoint;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            const auto existing = peers_by_username_.find(username);
            if (existing == peers_by_username_.end()) {
                if (peers_by_username_.size() < kMaxRegisteredPeers) {
                    peers_by_username_.emplace(username, endpoint);
                    registered = true;
                }
            } else {
                existing_endpoint = existing->second;
            }
        }

        if (existing_endpoint && !pwp::connect_to(*existing_endpoint, std::chrono::seconds(1))) {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            const auto existing = peers_by_username_.find(username);
            if (existing != peers_by_username_.end() &&
                endpoints_match(existing->second, *existing_endpoint)) {
                existing->second = endpoint;
                registered = true;
                replaced_stale_peer = true;
            }
        }

        pwp::send_bool(socket_fd, registered);
        if (registered) {
            log(std::string(replaced_stale_peer ? "~ " : "+ ") + username + " at " +
                pwp::endpoint_to_string(endpoint));
        }
    }

    void handle_unregister(int socket_fd) {
        std::string username;
        Endpoint endpoint;
        if (!pwp::receive_string(socket_fd, username) ||
            !pwp::receive_endpoint(socket_fd, endpoint)) {
            return;
        }

        bool removed = false;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            const auto peer = peers_by_username_.find(username);
            if (peer != peers_by_username_.end() && peer->second.host == endpoint.host &&
                peer->second.port == endpoint.port) {
                peers_by_username_.erase(peer);
                removed = true;
            }
        }
        if (removed) {
            log("- " + username);
        }
    }

    void handle_find_file(int socket_fd) {
        std::string requester;
        std::string filename;
        if (!pwp::receive_string(socket_fd, requester) ||
            !pwp::receive_string(socket_fd, filename)) {
            return;
        }

        std::vector<std::future<FileProbe>> probes;
        for (const PeerRecord& peer : snapshot_peers(requester)) {
            probes.push_back(std::async(std::launch::async, &Tracker::probe_file, peer, filename));
        }

        for (std::future<FileProbe>& future : probes) {
            const FileProbe result = future.get();
            if (result.available) {
                pwp::send_bool(socket_fd, true);
                pwp::send_string(socket_fd, result.peer.username);
                pwp::send_endpoint(socket_fd, result.peer.endpoint);
                return;
            }
        }

        pwp::send_bool(socket_fd, false);
    }

    void handle_list_files(int socket_fd) {
        std::string requester;
        if (!pwp::receive_string(socket_fd, requester)) {
            return;
        }

        std::vector<std::future<PeerListing>> probes;
        for (const PeerRecord& peer : snapshot_peers(requester)) {
            probes.push_back(std::async(std::launch::async, &Tracker::fetch_listing, peer));
        }

        std::vector<PeerListing> listings;
        for (std::future<PeerListing>& future : probes) {
            PeerListing listing = future.get();
            if (listing.valid) {
                listings.push_back(std::move(listing));
            }
        }

        if (!pwp::send_u32(socket_fd, static_cast<std::uint32_t>(listings.size()))) {
            return;
        }
        for (const PeerListing& listing : listings) {
            if (!pwp::send_string(socket_fd, listing.peer.username) ||
                !pwp::send_u32(socket_fd, static_cast<std::uint32_t>(listing.files.size()))) {
                return;
            }
            for (const std::string& filename : listing.files) {
                if (!pwp::send_string(socket_fd, filename)) {
                    return;
                }
            }
        }
    }

    void handle_peer_info(int socket_fd) {
        std::string username;
        if (!pwp::receive_string(socket_fd, username)) {
            return;
        }

        std::optional<Endpoint> endpoint;
        {
            std::lock_guard<std::mutex> lock(peers_mutex_);
            const auto peer = peers_by_username_.find(username);
            if (peer != peers_by_username_.end()) {
                endpoint = peer->second;
            }
        }

        pwp::send_bool(socket_fd, endpoint.has_value());
        if (endpoint) {
            pwp::send_endpoint(socket_fd, *endpoint);
        }
    }

    std::vector<PeerRecord> snapshot_peers(const std::string& excluded_username) {
        std::vector<PeerRecord> peers;
        std::lock_guard<std::mutex> lock(peers_mutex_);
        peers.reserve(peers_by_username_.size());
        for (const auto& [username, endpoint] : peers_by_username_) {
            if (username != excluded_username) {
                peers.push_back(PeerRecord{username, endpoint});
            }
        }
        return peers;
    }

    static bool endpoints_match(const Endpoint& left, const Endpoint& right) {
        return left.host == right.host && left.port == right.port;
    }

    static FileProbe probe_file(PeerRecord peer, const std::string& filename) {
        Socket connection = pwp::connect_to(peer.endpoint, std::chrono::seconds(2));
        if (!connection ||
            !pwp::send_u32(connection.get(), static_cast<std::uint32_t>(PeerRequest::kHasFile)) ||
            !pwp::send_string(connection.get(), filename)) {
            return FileProbe{std::move(peer), false};
        }

        bool available = false;
        if (!pwp::receive_bool(connection.get(), available)) {
            available = false;
        }
        return FileProbe{std::move(peer), available};
    }

    static PeerListing fetch_listing(PeerRecord peer) {
        PeerListing listing{std::move(peer), {}, false};
        Socket connection = pwp::connect_to(listing.peer.endpoint, std::chrono::seconds(2));
        if (!connection ||
            !pwp::send_u32(connection.get(), static_cast<std::uint32_t>(PeerRequest::kListFiles))) {
            return listing;
        }

        std::uint32_t file_count = 0;
        if (!pwp::receive_u32(connection.get(), file_count) || file_count > pwp::kMaxListedFiles) {
            return listing;
        }
        listing.files.reserve(file_count);
        for (std::uint32_t index = 0; index < file_count; ++index) {
            std::string filename;
            if (!pwp::receive_string(connection.get(), filename)) {
                listing.files.clear();
                return listing;
            }
            listing.files.push_back(std::move(filename));
        }
        listing.valid = true;
        return listing;
    }

    void log(const std::string& message) {
        std::lock_guard<std::mutex> lock(log_mutex_);
        std::cout << message << '\n' << std::flush;
    }

    Endpoint listen_endpoint_;
    std::map<std::string, Endpoint> peers_by_username_;
    std::mutex peers_mutex_;
    std::mutex log_mutex_;
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [--listen HOST:PORT]\n\n"
              << "Options:\n"
              << "  --listen HOST:PORT  Address to listen on (default: 0.0.0.0:8080)\n"
              << "  -h, --help          Show this help\n";
}

} // namespace

int main(int argc, char* argv[]) {
    Endpoint listen_endpoint{"0.0.0.0", pwp::kDefaultTrackerPort};

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "-h" || argument == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (argument == "--listen" && index + 1 < argc) {
            const auto parsed = pwp::parse_endpoint(argv[++index]);
            if (!parsed) {
                std::cerr << "error: --listen must be HOST:PORT\n";
                return 2;
            }
            listen_endpoint = *parsed;
            continue;
        }

        std::cerr << "error: unknown or incomplete option: " << argument << '\n';
        print_usage(argv[0]);
        return 2;
    }

    std::signal(SIGPIPE, SIG_IGN);
    return Tracker(std::move(listen_endpoint)).run();
}
