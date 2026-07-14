# Building hades

hades is plain **CMake + Ninja + C++20**. The Nix dev shell is the reference environment
(pinned deps, what CI-equivalent testing runs on), but nothing about the build requires Nix —
you just need the five libraries below discoverable by `find_package`/`pkg-config`.

| dependency | found via | license |
|---|---|---|
| [libcpr](https://github.com/libcpr/cpr) (HTTP client, wraps libcurl) | `find_package(cpr)` | MIT |
| [nlohmann_json](https://github.com/nlohmann/json) | `find_package(nlohmann_json)` | MIT |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) (HTTP server) | `find_package(httplib)` | MIT |
| [libedit](https://thrysoee.dk/editline/) (REPL line editing) | `pkg-config libedit` | BSD-3 |
| [GoogleTest](https://github.com/google/googletest) | `find_package(GTest)` | BSD-3 |

Plus a C++20 compiler (GCC 12+ / Clang 15+ / AppleClang 15+), CMake ≥ 3.20, Ninja, pkg-config.

## Build (all platforms, after installing deps)

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure   # full suite, ~7 s
./build/hades manifests/dev.hades            # needs HADES_API_KEY exported
```

## Nix (any Linux or macOS with Nix installed — the reference path)

```bash
nix develop --command cmake -S . -B build -G Ninja
nix develop --command cmake --build build
nix develop --command ctest --test-dir build
```

## Debian 12+ / Ubuntu 24.04+

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
  libcpr-dev nlohmann-json3-dev libcpp-httplib-dev libedit-dev libgtest-dev
```

Note: `libcpr-dev` entered Debian at 12 (bookworm) and Ubuntu at 23.10 — on older releases
build [cpr from source](https://github.com/libcpr/cpr#usage) (standard CMake install) or use Nix.

## Fedora

```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
  cpr-devel json-devel cpp-httplib-devel libedit-devel gtest-devel
```

## Arch

```bash
sudo pacman -S --needed base-devel cmake ninja cpr nlohmann-json cpp-httplib libedit gtest
```

## macOS (Homebrew)

```bash
xcode-select --install    # compiler toolchain, if not already present
brew install cmake ninja pkg-config cpr nlohmann-json cpp-httplib googletest libedit
# brew's libedit is keg-only — point pkg-config at it:
export PKG_CONFIG_PATH="$(brew --prefix libedit)/lib/pkgconfig:$PKG_CONFIG_PATH"
```

Then the standard build block above. The code is POSIX (fork/exec subprocesses, plain sockets)
and has no Linux-only dependencies, but day-to-day development happens on Linux — if you hit a
macOS issue, please open one.

## Sanitizer lanes (development)

The default `build/` above is a plain build. The project's test lanes:

```bash
cmake -S . -B build -G Ninja -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined"   # ASan+UBSan
cmake -S . -B build-tsan -G Ninja -DCMAKE_CXX_FLAGS="-fsanitize=thread"        # TSan
```

## Raspberry Pi / aarch64 (static, Nix only)

```bash
nix build .#hades-aarch64-static    # fully static musl aarch64 deploy dir in ./result
scp -rL result/ pi:hades/           # zero dependencies on the target
```

The cross build uses Nix's `pkgsCross` static toolchain; there is no non-Nix recipe for it —
on a Pi you can alternatively just build natively with the Debian instructions above.
