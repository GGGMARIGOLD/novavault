CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wno-misleading-indentation -pthread
LDLIBS = -lssl -lcrypto -lz

.PHONY: img run restart deps

vespera: main.cpp httplib.h json.hpp
	$(CXX) $(CXXFLAGS) main.cpp -o vespera $(LDLIBS)

deps:
	curl -L -o httplib.h https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.7/httplib.h
	curl -L -o json.hpp https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp

# The Rust image shrinker is optional: without Rust the server still works, it just keeps uploads as they are.
img:
	@command -v cargo >/dev/null && cargo build --release -q --manifest-path img/Cargo.toml || echo "(Rust not found: uploaded images won't be shrunk)"

run: vespera img
	./vespera

# Stops any Vespera that's already running, then starts the new one.
restart: vespera img
	-pkill -x vespera; sleep 1
	./vespera
