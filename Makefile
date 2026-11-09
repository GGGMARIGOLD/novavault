CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wno-misleading-indentation -pthread
LDLIBS = -lz

.PHONY: img run restart deps windows

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

# Windows build: one self-contained Vespera.exe (website included), cross-compiled from Linux.
# Needs: sudo apt install g++-mingw-w64-x86-64-posix libz-mingw-w64-dev
windows:
	mkdir -p dist
	{ printf 'static const char* VESPERA_PAGE = R"VSPHTML('; cat index.html; printf ')VSPHTML";\n'; } > index_embed.h
	x86_64-w64-mingw32-g++ -std=c++17 -O2 -D_WIN32_WINNT=0x0A00 -static -pthread main.cpp -o dist/Vespera.exe -lws2_32 -lbcrypt -lz -lshell32
	rm -f index_embed.h
	-@command -v cargo >/dev/null && cargo build --release -q --manifest-path img/Cargo.toml --target x86_64-pc-windows-gnu && cp img/target/x86_64-pc-windows-gnu/release/vespera-img.exe dist/ || echo "(Rust Windows target not installed: Vespera.exe will keep images as uploaded)"
