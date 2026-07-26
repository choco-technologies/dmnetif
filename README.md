# dmnetif

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

dmnetif DMOD library module.

## Description

TODO: describe what this module does.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Usage

This library module provides functions that can be used by other modules:

```c
#include "dmnetif.h"
```

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmnetif`.
## Project Structure

```
dmnetif/
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   └── dmnetif.h
├── src/
│   └── dmnetif.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmnetif_test.c
├── CMakeLists.txt
├── Makefile
├── dmnetif.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
