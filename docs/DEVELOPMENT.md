# Developing NovaCache

## Requirements

- CMake 3.24 or newer
- Ninja
- A C++20 compiler (Apple Clang, Clang, or GCC)
- Git and network access during the first configure for GoogleTest
- Optional: clang-format and clang-tidy

On macOS, install the command-line tools and dependencies:

```bash
xcode-select --install
brew install cmake ninja llvm
```

On Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build clang clang-format clang-tidy
```

## Build and test

```bash
cmake --preset debug
cmake --build --preset debug -j
ctest --preset debug --output-on-failure
./scripts/smoke-test.sh debug
```

For an optimized build:

```bash
cmake --preset release
cmake --build --preset release -j
ctest --preset release --output-on-failure
```

The Phase 1 server can be exercised locally with:

```bash
./build/debug/novacache-server --host 127.0.0.1 --port 6379
./build/debug/novacache-cli -p 6379 PING
redis-cli -p 6379 PING
```

It is intentionally blocking and single-threaded until the Phase 2 reactor.

## Sanitizers

```bash
cmake --preset asan
cmake --build --preset asan -j
ctest --preset asan --output-on-failure

cmake --preset tsan
cmake --build --preset tsan -j
ctest --preset tsan --output-on-failure
```

ASan includes UndefinedBehaviorSanitizer. TSan is separate because it is
incompatible with ASan and is primarily validated in Linux CI. The convenience
script `./scripts/run-sanitizers.sh` runs both supported suites.

## Formatting and static analysis

Run the formatting check with:

```bash
./scripts/check-format.sh
```

To apply formatting:

```bash
clang-format -i \
  apps/*.cpp \
  src/**/*.cpp \
  include/novacache/**/*.hpp \
  tests/unit/*.cpp \
  tests/integration/*.cpp
```

Configure a Debug build before running clang-tidy so
`build/debug/compile_commands.json` exists.

## Source conventions

- Public APIs belong under `include/novacache/<component>/`.
- Implementations mirror them under `src/<component>/`.
- Executable entry points stay thin and live under `apps/`.
- Tests are split into `unit`, `integration`, `concurrency`, `fuzz`, and
  reusable `support` as those suites are introduced.
- Use RAII for resources, `std::string_view` only for non-owning lifetimes, and
  explicit error results for expected failures.
- Add source files to the narrowest CMake target and add tests with the change.
- Do not commit build output, runtime data, logs, secrets, or benchmark results.

## Troubleshooting

- `Could not create named generator Ninja`: install Ninja and reconfigure.
- Preset schema errors: verify `cmake --version` reports at least 3.24.
- GoogleTest download failures: verify GitHub access and retry configuration;
  the dependency is pinned rather than read from a moving branch.
- Stale configuration: remove only the relevant generated `build/<preset>`
  directory, then configure that preset again.
