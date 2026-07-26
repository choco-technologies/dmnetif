# dmnetif

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

dmnetif is a [DMOD](https://github.com/choco-technologies/dmod) library
module - the network interface manager. It is the boundary between devfs
(`dmdevfs`/`dmdrvi` device files, e.g. `/dev/dmeth0`) and the network: a
driver registers the devfs path it was assigned as a named interface
(`"eth0"`); everything above that line (a TCP/IP stack, `netctl`/`ifconfig`)
only ever talks to dmnetif by interface name and never has to open a device
file, know a `dmdrvi` ioctl command, or depend on `dmdrvi`/`dmdevfs` headers
at all.

```
┌──────────────────────────────────────────────┐
│  TCP/IP stack (networkd) / netctl / ifconfig  │
├──────────────────────────────────────────────┤
│                  DMNETIF                      │
│   register/unregister, up/down, link status,  │
│   MAC/IP address, MTU, send/receive one frame,│
│   packet stats, name <-> handle lookup        │
├──────────────────────────────────────────────┤
│         DMDRVI (open/read/write/ioctl)        │
├──────────────────────────────────────────────┤
│   Network driver (dmeth, ...) + DMDEVFS        │
└──────────────────────────────────────────────┘
```

## Key design points

- **Registration is driver-facing, everything else is consumer-facing.** A
  driver calls `dmnetif_register(name, device_path)` once its device file's
  path is known - typically from its `dmdrvi_path_ready()` DIF, *not* from
  its own `dmod_init()`, since `dmdevfs` has not necessarily created the
  device node yet at that point. `ifconfig`/`netctl`/a TCP/IP stack then only
  ever look the interface up by name.
- **IP address, netmask, broadcast and MTU are pure local bookkeeping.**
  Unlike the MAC address, none of these are pushed down to the driver via a
  `dmdrvi` ioctl - the driver/hardware has no notion of them. Whatever
  assigns them (a DHCP client, static config in `networkd`, `ifconfig`)
  calls the corresponding `dmnetif_set_*()`, and anything else reads them
  back with `dmnetif_get_*()`.
- **One address type for IPv4 and IPv6, borrowed from dmroute.**
  `dmroute_addr_t` (not a dmnetif-specific type) backs the IP address,
  netmask, and broadcast fields - a netmask/broadcast is shaped exactly like
  an address (4 or 16 raw bytes discriminated by `family`), and reusing
  dmroute's type keeps the two representations from ever drifting apart.
- **Setting the IP address keeps dmroute's connected route in sync.**
  `dmnetif_set_ip_address()` calls `dmroute_add()`/`_remove()` directly (see
  `update_connected_route()` in `src/dmnetif.c`), replacing only the route it
  previously added for that interface. Set the netmask *before* the address
  for it to be picked up - a netmask set afterward does not retroactively
  refresh the route. With no netmask on record yet, it falls back to an
  all-ones host mask so the interface is at least reachable by its own
  address.
- **`dmnetif_receive()` is non-blocking only as far as the driver behind it
  is.** It's documented to return 0 immediately when nothing is pending, but
  a driver without its own non-blocking read (dmdrvi has no `O_NONBLOCK`/
  timeout concept) can still block inside it - see
  [docs/dmnetif.md](docs/dmnetif.md#frame-io).
- **`dmnetif_is_present()` and `dmnetif_is_up()` track different things.**
  `_is_up()` is administrative state (has `dmnetif_up()` been called);
  `_is_present()` checks whether the backing devfs file still exists at all
  (e.g. a hot-unplugged driver).
- **Thread-safe registry.** Register/unregister/lookup/enumeration are
  guarded by a single mutex (`dmosi`) around the interface list (`dmlist`).

See [docs/dmnetif.md](docs/dmnetif.md) for the full rationale (including why
there is deliberately no `rx_errors` counter) and
[docs/api-reference.md](docs/api-reference.md) for the complete API.

## Building

### Using CMake

dmnetif is a standalone DMOD module: its `CMakeLists.txt` fetches `dmod`
itself via CMake's `FetchContent` (defaulting to the `develop` branch), so no
other repository needs to be checked out first.

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

This builds both the `dmnetif` library module and its `test_dmnetif` test
binary (see [Testing](#testing) below). Pass `-DDMOD_DIR=/path/to/local/dmod`
to build against a local dmod checkout instead of fetching it from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

The Makefile requires an existing `dmod` checkout (there is no FetchContent
equivalent for Make) and builds the `dmnetif` module itself, not the test
suite.

## Testing

Tests are plain DMOD test modules built with `dmod_add_test()`:
`tests/dmnetif_test.c` registers each test case with `DMOD_TEST_STEP()` and
runs against `"/dev/null"` as a stand-in device file - real enough for the
underlying file open to succeed without needing an actual `dmdrvi`-backed
network driver.

After building (see above), run the resulting binary directly from the build
directory:

```bash
./tests/test_dmnetif
```

It discovers and runs every `DMOD_TEST_STEP()` automatically and exits with a
code equal to the number of failed steps (`0` means everything passed).
`tests/dmnetif_test.c` covers:

- **Registration** - handle validity, NULL/duplicate-name/nonexistent-device
  rejection, unregistration.
- **Lookup / enumeration** - `find_by_name`, `count`, `for_each` (including
  early-stop).
- **State control** - `is_up`/`is_present` on a fresh, never-started
  interface.
- **IP address / netmask / broadcast** - assign, read back, clear, and
  invalid-family rejection, for both IPv4 and IPv6.
- **MTU and statistics** - defaults, round-trips, zero-MTU rejection.
- **Connected route integration** - `dmnetif_set_ip_address()`'s side effect
  on `dmroute` is checked against dmroute's own API directly (not a mock):
  host-route fallback without a netmask, subnet route once one is set, route
  removal on clear/unregister, and replacement (not duplication) on
  reassignment.
- **Invalid-handle error paths** - every accessor rejects `NULL`/a stale
  handle.

`DMDRVI_IOCTL_NET_*`-driven behavior (actual up/down, MAC address, link
status, send/receive) is only exercised here for its `NULL`-handle error
paths - real coverage needs a real network driver behind
`dmnetif_register()`.

## Usage

`dmnetif_register()` is normally called by a driver once its device file's
path is known, not from the driver's own `dmod_init()`:

```c
dmod_dmdrvi_dif_api_declaration(1.0, dmeth, void, _path_ready,
    ( dmdrvi_context_t context, const dmdrvi_dev_num_t* dev_num, const char* path ))
{
    dmnetif_register(context->config.if_name, path);
}
```

Everything else is consumer-facing - looking an interface up by name,
bringing it up, and assigning its network-layer configuration:

```c
#include "dmnetif.h"
#include "dmroute.h"

static dmroute_addr_t make_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmroute_addr_t addr = { 0 };
    addr.family = dmroute_family_v4;
    addr.addr.v4[0] = a;
    addr.addr.v4[1] = b;
    addr.addr.v4[2] = c;
    addr.addr.v4[3] = d;
    return addr;
}

void configure_interface(void)
{
    dmnetif_iface_t iface = dmnetif_find_by_name("eth0");
    if (iface == NULL)
    {
        return; /* no driver has registered this name (yet) */
    }

    dmnetif_up(iface);

    /* Set the netmask before the address - dmnetif_set_ip_address() reads
     * whatever netmask is on record when it updates the connected route */
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);
    dmnetif_set_netmask(iface, &netmask);

    dmroute_addr_t ip = make_v4(192, 168, 1, 42);
    dmnetif_set_ip_address(iface, &ip);

    /* dmroute now has a connected route for 192.168.1.0/24 via "eth0" */
    dmroute_addr_t neighbor = make_v4(192, 168, 1, 200);
    dmroute_route_t route = dmroute_lookup(&neighbor);
    if (route != NULL)
    {
        const char* via = dmroute_get_iface_name(route); /* "eth0" */
        (void)via;
    }
}
```

See [docs/examples.md](docs/examples.md) for more worked examples (listing
every interface, frame I/O with statistics, the raw ioctl escape hatch,
clean teardown).

## API Overview

| Category                   | Functions                                                              |
|-----------------------------|---------------------------------------------------------------------|
| Registration (driver-facing)| `dmnetif_register()`, `dmnetif_unregister()`                        |
| Lookup / enumeration        | `dmnetif_find_by_name()`, `dmnetif_count()`, `dmnetif_for_each()`, `dmnetif_get_name()` |
| State control               | `dmnetif_up()`, `dmnetif_down()`, `dmnetif_is_up()`, `dmnetif_get_link_status()`, `dmnetif_is_present()` |
| MTU                         | `dmnetif_get_mtu()`, `dmnetif_set_mtu()`                             |
| MAC address                 | `dmnetif_get_mac_address()`, `dmnetif_set_mac_address()`             |
| IP address                  | `dmnetif_get_ip_address()`, `dmnetif_set_ip_address()`, `dmnetif_get_netmask()`, `dmnetif_set_netmask()`, `dmnetif_get_broadcast()`, `dmnetif_set_broadcast()` |
| Frame I/O                   | `dmnetif_send()`, `dmnetif_receive()`                                |
| Statistics                  | `dmnetif_get_stats()`                                                |
| Escape hatch                | `dmnetif_ioctl()`                                                    |
| Types                       | `dmnetif_iface_t`, `dmnetif_mac_addr_t`, `dmnetif_link_status_t`, `dmnetif_stats_t`, `dmnetif_iterator_func_t` |

IP address/netmask/broadcast all use `dmroute_addr_t`/`dmroute_family_t`,
defined by [dmroute](https://github.com/choco-technologies/dmroute), not a
dmnetif-specific type. Full parameter/return documentation lives in
[docs/api-reference.md](docs/api-reference.md).

## Documentation

See the `docs/` directory:

- **[dmnetif.md](docs/dmnetif.md)** - Overview and architecture
- **[api-reference.md](docs/api-reference.md)** - Complete API documentation
- **[examples.md](docs/examples.md)** - Worked usage examples

View documentation using `dmf-man dmnetif`.

## Dependencies

- [`dmroute`](https://github.com/choco-technologies/dmroute) - `dmroute_addr_t`
  (the type used for every IP address/netmask/broadcast field), plus
  `dmnetif_set_ip_address()` calling `dmroute_add()`/`_remove()` directly to
  keep an interface's connected route in sync
- `dmdrvi` - generic driver interface (open/close/read/write/ioctl,
  `DMDRVI_IOCTL_NET_*` commands) used internally to talk to the device a
  driver registered
- `dmini` - `dmdrvi.h` includes `dmini.h` (`dmini_context_t` appears in
  `dmdrvi_create()`'s signature), needed transitively even though dmnetif
  never calls it directly
- `dmlist` - backs the interface registry
- `dmosi` - mutex guarding the registry

## Project Structure

```
dmnetif/
├── docs/              # Documentation (markdown format)
│   ├── README.md
│   ├── dmnetif.md
│   ├── api-reference.md
│   └── examples.md
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

MIT License (see [LICENSE](LICENSE) file for details)
