#include "pwp/files.hpp"
#include "pwp/protocol.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::cerr << "Check failed at " << __FILE__ << ':' << __LINE__ << ": " << #condition   \
                      << '\n';                                                                     \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

void test_endpoint_parsing() {
    const auto endpoint = pwp::parse_endpoint("127.0.0.1:8080");
    CHECK(endpoint.has_value());
    CHECK(endpoint->host == "127.0.0.1");
    CHECK(endpoint->port == 8080);
    CHECK(!pwp::parse_endpoint("localhost"));
    CHECK(!pwp::parse_endpoint("localhost:0"));
    CHECK(!pwp::parse_endpoint("localhost:70000"));
}

void test_validation() {
    CHECK(pwp::is_valid_username("alice-01"));
    CHECK(!pwp::is_valid_username("alice smith"));
    CHECK(pwp::is_safe_filename("notes from alice.txt"));
    CHECK(!pwp::is_safe_filename("../secret.txt"));
    CHECK(!pwp::is_safe_filename("folder/file.txt"));
}

void test_wire_protocol() {
    int descriptors[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    pwp::Socket sender(descriptors[0]);
    pwp::Socket receiver(descriptors[1]);

    CHECK(pwp::send_bool(sender.get(), true));
    CHECK(pwp::send_u32(sender.get(), 0x12345678U));
    CHECK(pwp::send_u64(sender.get(), 0x0123456789abcdefULL));
    CHECK(pwp::send_string(sender.get(), "hello, peer"));
    CHECK(pwp::send_endpoint(sender.get(), {"localhost", 9001}));

    bool boolean = false;
    std::uint32_t number32 = 0;
    std::uint64_t number64 = 0;
    std::string text;
    pwp::Endpoint endpoint;
    CHECK(pwp::receive_bool(receiver.get(), boolean) && boolean);
    CHECK(pwp::receive_u32(receiver.get(), number32) && number32 == 0x12345678U);
    CHECK(pwp::receive_u64(receiver.get(), number64) && number64 == 0x0123456789abcdefULL);
    CHECK(pwp::receive_string(receiver.get(), text) && text == "hello, peer");
    CHECK(pwp::receive_endpoint(receiver.get(), endpoint));
    CHECK(endpoint.host == "localhost" && endpoint.port == 9001);
}

void test_file_transfer() {
    const std::filesystem::path test_root = std::filesystem::temp_directory_path() /
                                            ("pwp-protocol-test-" + std::to_string(::getpid()));
    const std::filesystem::path source_directory = test_root / "source";
    const std::filesystem::path destination_directory = test_root / "destination";
    std::filesystem::create_directories(source_directory);
    std::filesystem::create_directories(destination_directory);

    const std::filesystem::path source = source_directory / "sample.bin";
    {
        std::ofstream output(source, std::ios::binary);
        for (int index = 0; index < 200'000; ++index) {
            output.put(static_cast<char>(index % 251));
        }
    }
    {
        std::ofstream existing(destination_directory / "sample.bin", std::ios::binary);
        existing << "keep this existing file";
    }

    int descriptors[2]{};
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0);
    pwp::Socket sender(descriptors[0]);
    pwp::Socket receiver(descriptors[1]);

    bool sent = false;
    std::thread send_thread([&] { sent = pwp::send_file(sender.get(), source); });
    const auto received = pwp::receive_file(receiver.get(), destination_directory, "sample.bin");
    send_thread.join();

    CHECK(sent);
    CHECK(received.has_value());
    CHECK(received->filename() == "sample (1).bin");
    std::ifstream original(source, std::ios::binary);
    std::ifstream copy(*received, std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(original), {}) ==
          std::string(std::istreambuf_iterator<char>(copy), {}));
    std::ifstream existing(destination_directory / "sample.bin", std::ios::binary);
    CHECK(std::string(std::istreambuf_iterator<char>(existing), {}) == "keep this existing file");

    std::filesystem::remove_all(test_root);
}

} // namespace

int main() {
    test_endpoint_parsing();
    test_validation();
    test_wire_protocol();
    test_file_transfer();
    std::cout << "All protocol tests passed.\n";
    return 0;
}
