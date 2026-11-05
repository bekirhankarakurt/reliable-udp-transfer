# Contributing

Thanks for your interest. Bug reports, fixes and focused features are welcome.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DRUDP_WERROR=ON
cmake --build build -j 4
ctest --test-dir build --output-on-failure --timeout 300
./build/rudp selftest --size-mb 4 --sim-loss 0.05 --sim-delay 2
```

Before opening a pull request, also run the sanitizer build:

```bash
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DRUDP_WERROR=ON -DRUDP_SANITIZE=ON
cmake --build build-asan -j 4
ctest --test-dir build-asan --output-on-failure --timeout 300
```

## Style

- Modern C++17, RAII, zero warnings with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion`.
- No dependencies beyond the standard library and POSIX.
- New behavior comes with a test; protocol changes should be exercised through the impairment simulator.

## Commits and pull requests

- Small, logical commits with plain imperative messages, e.g. `Add resume state validation`.
- One topic per pull request; describe what changed and how you tested it.
- Update `README.md` and `CHANGELOG.md` (under "Unreleased") when behavior or options change.
