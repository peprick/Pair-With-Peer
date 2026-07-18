#include "pwp/files.hpp"
#include "pwp/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
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

struct PeerConfig {
    std::string username;
    Endpoint tracker{"127.0.0.1", pwp::kDefaultTrackerPort};
    Endpoint listen;
    Endpoint advertise;
    std::filesystem::path data_directory;
    bool accept_direct_sends = false;
};

constexpr std::size_t kMaxConcurrentPeerRequests = 32;

std::string trim(std::string value) {
    const auto is_not_space = [](unsigned char character) { return !std::isspace(character); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), is_not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), is_not_space).base(), value.end());
    return value;
}

class Peer {
  public:
    explicit Peer(PeerConfig config)
        : config_(std::move(config)), public_directory_(config_.data_directory / "public"),
          private_directory_(config_.data_directory / "private"),
          downloads_directory_(config_.data_directory / "downloads"),
          inbox_directory_(config_.data_directory / "inbox") {}

    int run() {
        if (!prepare_directories()) {
            return 1;
        }

        listener_ = pwp::create_listener(config_.listen);
        if (!listener_) {
            std::cerr << "error: could not listen on " << pwp::endpoint_to_string(config_.listen)
                      << '\n';
            return 1;
        }

        if (!register_with_tracker()) {
            return 1;
        }

        running_ = true;
        const int listener_fd = listener_.get();
        listener_thread_ = std::thread(&Peer::accept_connections, this, listener_fd);

        print_welcome();
        command_loop();

        unregister_from_tracker();
        running_ = false;
        ::shutdown(listener_fd, SHUT_RDWR);
        listener_.reset();

        if (listener_thread_.joinable()) {
            listener_thread_.join();
        }

        std::vector<std::future<void>> workers;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers.swap(workers_);
        }
        for (std::future<void>& worker : workers) {
            if (worker.valid()) {
                try {
                    worker.get();
                } catch (const std::exception& error) {
                    print_line(std::string("warning: peer request failed: ") + error.what());
                }
            }
        }

        print_line("Goodbye.");
        return 0;
    }

  private:
    bool prepare_directories() {
        std::error_code error;
        for (const auto& directory :
             {public_directory_, private_directory_, downloads_directory_, inbox_directory_}) {
            std::filesystem::create_directories(directory, error);
            if (error) {
                std::cerr << "error: could not create " << directory << ": " << error.message()
                          << '\n';
                return false;
            }
        }
        return true;
    }

    bool register_with_tracker() {
        Socket tracker = connect_to_tracker();
        if (!tracker) {
            std::cerr << "error: could not connect to tracker at "
                      << pwp::endpoint_to_string(config_.tracker) << '\n';
            return false;
        }

        if (!pwp::send_u32(tracker.get(), static_cast<std::uint32_t>(TrackerRequest::kRegister)) ||
            !pwp::send_string(tracker.get(), config_.username) ||
            !pwp::send_endpoint(tracker.get(), config_.advertise)) {
            std::cerr << "error: registration request failed\n";
            return false;
        }

        bool registered = false;
        if (!pwp::receive_bool(tracker.get(), registered)) {
            std::cerr << "error: tracker closed the registration request\n";
            return false;
        }
        if (!registered) {
            std::cerr << "error: username '" << config_.username << "' is already registered\n";
            return false;
        }
        return true;
    }

    void unregister_from_tracker() {
        Socket tracker = connect_to_tracker();
        if (!tracker) {
            return;
        }
        pwp::send_u32(tracker.get(), static_cast<std::uint32_t>(TrackerRequest::kUnregister));
        pwp::send_string(tracker.get(), config_.username);
        pwp::send_endpoint(tracker.get(), config_.advertise);
    }

    Socket connect_to_tracker() const {
        Socket tracker = pwp::connect_to(config_.tracker);
        if (tracker) {
            pwp::set_socket_timeout(tracker.get(), std::chrono::seconds(30));
        }
        return tracker;
    }

    void accept_connections(int listener_fd) {
        while (running_) {
            sockaddr_storage address{};
            socklen_t address_size = sizeof(address);
            const int connection_fd =
                ::accept(listener_fd, reinterpret_cast<sockaddr*>(&address), &address_size);
            if (connection_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (running_) {
                    print_line("warning: incoming connection failed");
                }
                break;
            }

            pwp::configure_socket(connection_fd);
            pwp::set_socket_timeout(connection_fd, std::chrono::seconds(30));
            Socket connection(connection_fd);
            std::lock_guard<std::mutex> lock(workers_mutex_);
            reap_completed_workers();
            if (workers_.size() >= kMaxConcurrentPeerRequests) {
                print_line("warning: too many simultaneous peer requests");
                continue;
            }
            try {
                workers_.push_back(std::async(std::launch::async, &Peer::handle_peer_request, this,
                                              std::move(connection)));
            } catch (const std::system_error& error) {
                print_line(std::string("warning: could not start request worker: ") + error.what());
            }
        }
    }

    void reap_completed_workers() {
        workers_.erase(
            std::remove_if(
                workers_.begin(), workers_.end(),
                [this](std::future<void>& worker) {
                    if (worker.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                        return false;
                    }
                    try {
                        worker.get();
                    } catch (const std::exception& error) {
                        print_line(std::string("warning: peer request failed: ") + error.what());
                    }
                    return true;
                }),
            workers_.end());
    }

    void handle_peer_request(Socket connection) {
        std::uint32_t request_value = 0;
        if (!pwp::receive_u32(connection.get(), request_value)) {
            return;
        }

        switch (static_cast<PeerRequest>(request_value)) {
        case PeerRequest::kHasFile:
            handle_has_file(connection.get());
            break;
        case PeerRequest::kFetchFile:
            handle_fetch_file(connection.get());
            break;
        case PeerRequest::kListFiles:
            handle_public_file_list(connection.get());
            break;
        case PeerRequest::kOfferFile:
            handle_offered_file(connection.get());
            break;
        default:
            break;
        }
    }

    void handle_has_file(int socket_fd) const {
        std::string filename;
        if (!pwp::receive_string(socket_fd, filename)) {
            return;
        }
        pwp::send_bool(socket_fd, public_file_exists(filename));
    }

    void handle_fetch_file(int socket_fd) const {
        std::string filename;
        if (!pwp::receive_string(socket_fd, filename)) {
            return;
        }

        const bool available = public_file_exists(filename);
        if (!pwp::send_bool(socket_fd, available) || !available) {
            return;
        }
        pwp::send_file(socket_fd, public_directory_ / filename);
    }

    void handle_public_file_list(int socket_fd) const {
        std::vector<std::string> files = pwp::list_regular_files(public_directory_);
        if (files.size() > pwp::kMaxListedFiles) {
            files.resize(pwp::kMaxListedFiles);
        }
        if (!pwp::send_u32(socket_fd, static_cast<std::uint32_t>(files.size()))) {
            return;
        }
        for (const std::string& filename : files) {
            if (!pwp::send_string(socket_fd, filename)) {
                return;
            }
        }
    }

    void handle_offered_file(int socket_fd) {
        std::string sender;
        std::string filename;
        if (!pwp::receive_string(socket_fd, sender) || !pwp::receive_string(socket_fd, filename)) {
            return;
        }

        const bool accepted = config_.accept_direct_sends && pwp::is_valid_username(sender) &&
                              pwp::is_safe_filename(filename);
        if (!pwp::send_bool(socket_fd, accepted) || !accepted) {
            return;
        }

        const auto saved_path = pwp::receive_file(socket_fd, inbox_directory_, filename);
        pwp::send_bool(socket_fd, saved_path.has_value());
        if (saved_path) {
            print_line("Received '" + saved_path->filename().string() + "' from " + sender +
                       " -> " + saved_path->string());
        }
    }

    bool public_file_exists(const std::string& filename) const {
        if (!pwp::is_safe_filename(filename)) {
            return false;
        }
        std::error_code error;
        const std::filesystem::path path = public_directory_ / filename;
        return std::filesystem::is_regular_file(path, error) && !error &&
               std::filesystem::file_size(path, error) <= pwp::kMaxAcceptedFileSize && !error;
    }

    void command_loop() {
        std::string line;
        while (running_) {
            {
                std::lock_guard<std::mutex> lock(output_mutex_);
                std::cout << "pwp> " << std::flush;
            }
            if (!std::getline(std::cin, line)) {
                break;
            }

            line = trim(std::move(line));
            if (line.empty()) {
                continue;
            }

            const std::size_t separator = line.find_first_of(" \t");
            const std::string command = line.substr(0, separator);
            const std::string arguments =
                separator == std::string::npos ? "" : trim(line.substr(separator + 1));

            if (command == "help") {
                print_commands();
            } else if (command == "status") {
                print_status();
            } else if (command == "list") {
                list_available_files();
            } else if (command == "get") {
                if (arguments.empty()) {
                    print_line("Usage: get <filename>");
                } else {
                    download_file(arguments);
                }
            } else if (command == "send") {
                const std::size_t target_end = arguments.find_first_of(" \t");
                if (target_end == std::string::npos) {
                    print_line("Usage: send <peer> <filename>");
                } else {
                    const std::string target = arguments.substr(0, target_end);
                    const std::string filename = trim(arguments.substr(target_end + 1));
                    if (filename.empty()) {
                        print_line("Usage: send <peer> <filename>");
                    } else {
                        send_direct_file(target, filename);
                    }
                }
            } else if (command == "quit" || command == "exit") {
                break;
            } else {
                print_line("Unknown command. Type 'help' to see available commands.");
            }
        }
    }

    void list_available_files() {
        Socket tracker = connect_to_tracker();
        if (!tracker) {
            print_line("Could not reach the tracker.");
            return;
        }
        if (!pwp::send_u32(tracker.get(), static_cast<std::uint32_t>(TrackerRequest::kListFiles)) ||
            !pwp::send_string(tracker.get(), config_.username)) {
            print_line("Could not request the file list.");
            return;
        }

        std::uint32_t peer_count = 0;
        if (!pwp::receive_u32(tracker.get(), peer_count) || peer_count > pwp::kMaxListedFiles) {
            print_line("The tracker returned an invalid file list.");
            return;
        }

        if (peer_count == 0) {
            print_line("No other peers are currently reachable.");
            return;
        }

        std::ostringstream output;
        output << "Available public files:\n";
        bool found_file = false;
        for (std::uint32_t peer_index = 0; peer_index < peer_count; ++peer_index) {
            std::string username;
            std::uint32_t file_count = 0;
            if (!pwp::receive_string(tracker.get(), username) ||
                !pwp::receive_u32(tracker.get(), file_count) || file_count > pwp::kMaxListedFiles) {
                print_line("The tracker returned an incomplete file list.");
                return;
            }
            output << "  " << username << " (" << file_count << ")\n";
            for (std::uint32_t file_index = 0; file_index < file_count; ++file_index) {
                std::string filename;
                if (!pwp::receive_string(tracker.get(), filename)) {
                    print_line("The tracker returned an incomplete file list.");
                    return;
                }
                output << "    - " << filename << '\n';
                found_file = true;
            }
        }
        if (!found_file) {
            output << "  (connected peers are not sharing files yet)\n";
        }
        print_line(output.str(), false);
    }

    void download_file(const std::string& filename) {
        if (!pwp::is_safe_filename(filename)) {
            print_line("Filename must be a single file name without path separators.");
            return;
        }

        Socket tracker = connect_to_tracker();
        if (!tracker ||
            !pwp::send_u32(tracker.get(), static_cast<std::uint32_t>(TrackerRequest::kFindFile)) ||
            !pwp::send_string(tracker.get(), config_.username) ||
            !pwp::send_string(tracker.get(), filename)) {
            print_line("Could not search for the file.");
            return;
        }

        bool found = false;
        if (!pwp::receive_bool(tracker.get(), found) || !found) {
            print_line("File not found: " + filename);
            return;
        }

        std::string source_peer;
        Endpoint source_endpoint;
        if (!pwp::receive_string(tracker.get(), source_peer) ||
            !pwp::receive_endpoint(tracker.get(), source_endpoint)) {
            print_line("The tracker returned an invalid peer address.");
            return;
        }
        tracker.reset();

        Socket source = pwp::connect_to(source_endpoint, std::chrono::seconds(30));
        if (!source ||
            !pwp::send_u32(source.get(), static_cast<std::uint32_t>(PeerRequest::kFetchFile)) ||
            !pwp::send_string(source.get(), filename)) {
            print_line("Could not connect to " + source_peer + ".");
            return;
        }

        bool available = false;
        if (!pwp::receive_bool(source.get(), available) || !available) {
            print_line(source_peer + " no longer has that file.");
            return;
        }

        const auto saved_path = pwp::receive_file(source.get(), downloads_directory_, filename);
        if (!saved_path) {
            print_line("Download failed before the file was complete.");
            return;
        }
        print_line("Downloaded from " + source_peer + " -> " + saved_path->string());
    }

    void send_direct_file(const std::string& target, const std::string& filename) {
        if (!pwp::is_valid_username(target)) {
            print_line("Peer names may contain letters, numbers, '-' and '_'.");
            return;
        }
        if (!pwp::is_safe_filename(filename)) {
            print_line("Filename must be a single file name without path separators.");
            return;
        }

        const std::filesystem::path source_path = private_directory_ / filename;
        std::error_code error;
        if (!std::filesystem::is_regular_file(source_path, error) || error ||
            std::filesystem::file_size(source_path, error) > pwp::kMaxAcceptedFileSize || error) {
            print_line("Direct-share file is missing or exceeds the size limit: " +
                       source_path.string());
            return;
        }

        Socket tracker = connect_to_tracker();
        if (!tracker ||
            !pwp::send_u32(tracker.get(), static_cast<std::uint32_t>(TrackerRequest::kPeerInfo)) ||
            !pwp::send_string(tracker.get(), target)) {
            print_line("Could not look up " + target + ".");
            return;
        }

        bool found = false;
        Endpoint target_endpoint;
        if (!pwp::receive_bool(tracker.get(), found) || !found ||
            !pwp::receive_endpoint(tracker.get(), target_endpoint)) {
            print_line("Peer is not registered: " + target);
            return;
        }
        tracker.reset();

        Socket receiver = pwp::connect_to(target_endpoint, std::chrono::seconds(30));
        if (!receiver ||
            !pwp::send_u32(receiver.get(), static_cast<std::uint32_t>(PeerRequest::kOfferFile)) ||
            !pwp::send_string(receiver.get(), config_.username) ||
            !pwp::send_string(receiver.get(), filename)) {
            print_line("Could not connect to " + target + ".");
            return;
        }

        bool ready = false;
        if (!pwp::receive_bool(receiver.get(), ready) || !ready ||
            !pwp::send_file(receiver.get(), source_path)) {
            print_line(target + " could not accept the file.");
            return;
        }

        bool completed = false;
        if (!pwp::receive_bool(receiver.get(), completed) || !completed) {
            print_line("The transfer to " + target + " did not complete.");
            return;
        }
        print_line("Sent '" + filename + "' to " + target + ".");
    }

    void print_welcome() {
        std::ostringstream output;
        output << "Pair with Peer\n"
               << "Connected as " << config_.username << "\n"
               << "Tracker:  " << pwp::endpoint_to_string(config_.tracker) << "\n"
               << "Listening: " << pwp::endpoint_to_string(config_.advertise) << "\n"
               << "Direct:    "
               << (config_.accept_direct_sends ? "accepting incoming sends" : "disabled") << "\n"
               << "Data:      " << config_.data_directory << "\n"
               << "Type 'help' to see available commands.";
        print_line(output.str());
    }

    void print_commands() {
        print_line("Commands:\n"
                   "  list                    List public files shared by reachable peers\n"
                   "  get <filename>          Download a public file\n"
                   "  send <peer> <filename> Send a file from your private directory\n"
                   "  status                  Show connection and directory details\n"
                   "  help                    Show this command list\n"
                   "  quit                    Disconnect cleanly");
    }

    void print_status() {
        std::ostringstream output;
        output << "Peer:      " << config_.username << '\n'
               << "Tracker:   " << pwp::endpoint_to_string(config_.tracker) << '\n'
               << "Listening: " << pwp::endpoint_to_string(config_.advertise) << '\n'
               << "Direct:    "
               << (config_.accept_direct_sends ? "accepting incoming sends" : "disabled") << '\n'
               << "Public:    " << public_directory_ << '\n'
               << "Private:   " << private_directory_ << '\n'
               << "Downloads: " << downloads_directory_ << '\n'
               << "Inbox:     " << inbox_directory_;
        print_line(output.str());
    }

    void print_line(const std::string& message, bool append_newline = true) const {
        std::lock_guard<std::mutex> lock(output_mutex_);
        std::cout << message;
        if (append_newline && (message.empty() || message.back() != '\n')) {
            std::cout << '\n';
        }
        std::cout << std::flush;
    }

    PeerConfig config_;
    std::filesystem::path public_directory_;
    std::filesystem::path private_directory_;
    std::filesystem::path downloads_directory_;
    std::filesystem::path inbox_directory_;
    Socket listener_;
    std::atomic<bool> running_{false};
    std::thread listener_thread_;
    std::vector<std::future<void>> workers_;
    std::mutex workers_mutex_;
    mutable std::mutex output_mutex_;
};

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " --name NAME --listen HOST:PORT [options]\n\n"
              << "Required:\n"
              << "  --name NAME            Unique peer name (letters, numbers, '-' and '_')\n"
              << "  --listen HOST:PORT     Local address for incoming peer connections\n\n"
              << "Options:\n"
              << "  --tracker HOST:PORT    Tracker address (default: 127.0.0.1:8080)\n"
              << "  --advertise HOST:PORT  Reachable address announced to other peers\n"
              << "                         (default: the --listen address)\n"
              << "  --data-dir PATH        Peer data directory (default: peers/NAME)\n"
              << "  --accept-direct        Accept direct sends into inbox/ (off by default)\n"
              << "  -h, --help             Show this help\n";
}

std::optional<PeerConfig> parse_config(int argc, char* argv[], int& exit_code) {
    PeerConfig config;
    bool has_listen = false;
    bool has_advertise = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "-h" || argument == "--help") {
            print_usage(argv[0]);
            exit_code = 0;
            return std::nullopt;
        }
        if (argument == "--accept-direct") {
            config.accept_direct_sends = true;
            continue;
        }
        if (index + 1 >= argc) {
            std::cerr << "error: missing value for " << argument << '\n';
            exit_code = 2;
            return std::nullopt;
        }

        const std::string value = argv[++index];
        if (argument == "--name") {
            config.username = value;
        } else if (argument == "--listen") {
            const auto endpoint = pwp::parse_endpoint(value);
            if (!endpoint) {
                std::cerr << "error: --listen must be HOST:PORT\n";
                exit_code = 2;
                return std::nullopt;
            }
            config.listen = *endpoint;
            has_listen = true;
        } else if (argument == "--advertise") {
            const auto endpoint = pwp::parse_endpoint(value);
            if (!endpoint) {
                std::cerr << "error: --advertise must be HOST:PORT\n";
                exit_code = 2;
                return std::nullopt;
            }
            config.advertise = *endpoint;
            has_advertise = true;
        } else if (argument == "--tracker") {
            const auto endpoint = pwp::parse_endpoint(value);
            if (!endpoint) {
                std::cerr << "error: --tracker must be HOST:PORT\n";
                exit_code = 2;
                return std::nullopt;
            }
            config.tracker = *endpoint;
        } else if (argument == "--data-dir") {
            config.data_directory = value;
        } else {
            std::cerr << "error: unknown option: " << argument << '\n';
            exit_code = 2;
            return std::nullopt;
        }
    }

    if (!pwp::is_valid_username(config.username)) {
        std::cerr
            << "error: --name is required and may contain only letters, numbers, '-' and '_'\n";
        exit_code = 2;
        return std::nullopt;
    }
    if (!has_listen) {
        std::cerr << "error: --listen is required\n";
        exit_code = 2;
        return std::nullopt;
    }
    if (!has_advertise) {
        config.advertise = config.listen;
    }
    if (config.advertise.host == "0.0.0.0" || config.advertise.host == "*") {
        std::cerr << "error: use --advertise with an address other peers can reach\n";
        exit_code = 2;
        return std::nullopt;
    }
    if (config.data_directory.empty()) {
        config.data_directory = std::filesystem::path("peers") / config.username;
    }
    return config;
}

} // namespace

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    int exit_code = 0;
    auto config = parse_config(argc, argv, exit_code);
    if (!config) {
        return exit_code;
    }
    return Peer(std::move(*config)).run();
}
