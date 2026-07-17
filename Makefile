CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -pthread

.PHONY: all setup clean

all: server client

server: server.cpp server_handler.cpp
	$(CXX) $(CXXFLAGS) server.cpp -o $@

client: client.cpp client_handler.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o $@

setup:
	mkdir -p shared/public shared/private

clean:
	rm -f server client
