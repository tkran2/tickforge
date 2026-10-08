CXX ?= c++
CXXFLAGS ?= -O3 -DNDEBUG -std=c++20 -Wall -Wextra -Wpedantic -Werror
CPPFLAGS = -Iinclude
LDLIBS = -pthread
ASAN_OPTIONS ?= detect_leaks=1
.PHONY: all test sanitize tsan benchmark clean
all: build/tickforge build/benchmark
build:
	mkdir -p build
build/tickforge: src/main.cpp include/tickforge.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDLIBS) -o $@
build/benchmark: src/benchmark.cpp include/tickforge.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDLIBS) -o $@
build/tests: tests/test.cpp include/tickforge.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< $(LDLIBS) -o $@
test: build/tests
	./build/tests
sanitize: | build
	$(CXX) $(CPPFLAGS) -O1 -g -std=c++20 -Wall -Wextra -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/test.cpp -o build/tests-sanitized
	ASAN_OPTIONS=$(ASAN_OPTIONS) ./build/tests-sanitized
benchmark: build/benchmark
	./build/benchmark 100000 5 256
tsan: | build
	$(CXX) $(CPPFLAGS) -O1 -g -std=c++20 -pthread -fsanitize=thread tests/test.cpp -o build/tests-tsan
	./build/tests-tsan
clean:
	rm -rf build
