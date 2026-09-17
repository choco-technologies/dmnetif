# DMNETIF - DMOD Network Interface Manager

## Overview

DMNETIF is the boundary between devfs and the network. A network driver
(`dmeth`, and later others - loopback, PPP, Wi-Fi, ...) shows up in devfs as
an ordinary device file (`/dev/dmeth0`) managed through `dmdrvi`, exactly
like any other driver. DMNETIF is what turns that device file into a named
network interface (`"eth0"`) that a TCP/IP stack or CLI tool can address
without ever knowing devfs paths or dmdrvi ioctl commands exist.

```
┌──────────────────────────────────────────────┐
│  TCP/IP stack (dmnetwork) / netctl / ifconfig │
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

The driver registers itself directly, and a few management-facing modules
(`dmdhcp` applying a lease, `ifconfig`/`dmnetwork` bringing an interface up)
call dmnetif directly too - but the path an actual frame takes when
something is sent or received always runs through **`dmnetbridge`**, not
straight from a protocol module to dmnetif:

- **Send:** a protocol module (`dmip`, `dmarp`, ...) calls
  `dmnetbridge_send()`/`_send_on_iface()`, which resolves the route/next-hop
  MAC and is the one that actually calls `dmnetif_send()`.
- **Receive:** `dmnetwork` runs one thread per interface calling
  `dmnetbridge_handle_netif_rx()`, the only code path allowed to call
  `dmnetif_receive()` on that interface - it then hands the frame to every
  protocol module that registered for it via a DIF, so they never call
  `dmnetif_receive()` themselves.

`dmarp` is the one exception that talks to dmnetif directly for frame I/O
(`dmnetif_send()`) rather than going through dmnetbridge, since ARP works
below IP addressing and has no route to resolve.

## Registration flow

A driver cannot call `dmnetif_register()` from its own `dmod_init()` -
`dmdevfs` has not necessarily created the driver's device node yet at that
point (device creation happens when `dmdevfs` later drives the driver's
`dmdrvi_create()`/`dmdrvi_path_ready()` DIFs). The natural place to register
is the driver's `dmdrvi_path_ready()` implementation, which fires once the
device's absolute path is actually known:

```c
dmod_dmdrvi_dif_api_declaration(1.0, dmeth, void, _path_ready,
    ( dmdrvi_context_t context, const dmdrvi_dev_num_t* dev_num, const char* path ))
{
    dmnetif_register(context->config.if_name, path);
}
```

`dmnetif_register()` opens `path` itself (the same `Dmod_FileOpen`/`Ioctl`
SAL any application would use) and keeps it open for the interface's
lifetime - the driver never opens its own device file.

Nothing about `dmnetif_register()` requires the caller to be a driver,
though - `ifconfig <name> create <device_path>` calls it directly too, for
registering an interface manually (e.g. against a devfs node a driver
already exposed under a different name, or for testing without a real
driver at all).

## IP address

Unlike the MAC address, an interface's IP address is never pushed down to
the driver via a `dmdrvi` ioctl - it's a network-layer concept the
driver/hardware has no notion of at all. `dmnetif_get_ip_address()`/
`_set_ip_address()` are purely local bookkeeping: whatever assigns
addresses above DMNETIF (a DHCP client or static config in `networkd`)
calls `dmnetif_set_ip_address()`, and anything else that needs to know an
interface's address (e.g. `ifconfig`) reads it back with
`dmnetif_get_ip_address()`.

`dmroute_addr_t` (a `family` tag plus a `v4[4]`/`v6[16]` union, defined by
`dmroute`, not by dmnetif) is the type for the IP address, netmask *and*
broadcast fields - all three are shaped exactly like an address (4 or 16
raw bytes), which is exactly why an IPv4 netmask/broadcast is conventionally
written as a dotted-quad value ("255.255.255.0") in the first place. dmroute
owns this type outright (it has no dependencies of its own to keep that
way), so dmnetif borrowing it rather than defining a parallel copy costs
nothing and keeps the two representations from ever drifting apart. This is
a plain compile-time header dependency (`#include "dmroute.h"`), the same as
depending on `dmlist` or `dmosi` - not a DIF/MAL, since a routing table is
always wanted alongside an interface registry, not an optional/pluggable
extra.

### Keeping a connected route in dmroute in sync

`dmnetif_set_ip_address()` is the *only* one of the three setters that talks
to dmroute - `dmnetif_set_netmask()`/`_set_broadcast()` are pure local
bookkeeping. It calls `update_connected_route()` (`src/dmnetif.c`) after
recording the new address, which:

1. Removes whatever route it previously added for this interface
   (`dmroute_remove(iface->connected_route)` - a no-op the first time, since
   that starts out `NULL`).
2. If the address was just cleared (`family == dmroute_family_none`), stops
   there - clearing the address alone is enough to drop the route, nothing
   else needs to call `dmroute_remove()` for that case.
3. Otherwise adds a fresh one: `dmroute_add(&iface->ip, &netmask, NULL,
   iface->name, DMROUTE_DEFAULT_METRIC, dmroute_origin_connected)` - no
   gateway (`NULL`, it's directly connected), keyed by the interface's name
   rather than its handle (dmroute has no notion of a dmnetif handle), and
   tagged `dmroute_origin_connected` so it's identifiable as an
   automatically-managed route rather than one a user added by hand.

The netmask used in step 3 is whatever `dmnetif_set_netmask()` last recorded
- **but only if its family matches the address's family**. If no netmask is
on record yet, or it was set for the other IP family, `update_connected_route()`
falls back to an all-ones host mask instead, so the interface is at least
reachable by its own address in the meantime (e.g. a DHCP client that sets
the address before the netmask). This is why the netmask has to be set
*before* the address for it to be picked up - a netmask set afterward never
retroactively refreshes an already-added route.

The connected route is also dropped on interface teardown
(`close_iface()`, called from `dmnetif_unregister()` and `dmod_deinit()`),
so an unregistered/unplugged interface never leaves a stale route behind.

This dependency is one-directional: dmnetif calls into dmroute, but dmroute
has no dependency on dmnetif at all and knows nothing about interface
handles - see dmroute's own docs for how it stores the interface name it
gets handed here.

## MTU

Same story as the IP address: an MTU is a concept dmnetif tracks for
consumers to read (e.g. `ifconfig`'s `mtu` line), not something pushed down
to the driver - there is no `DMDRVI_IOCTL_NET_*` command for it. A newly
registered interface starts at `DMNETIF_DEFAULT_MTU` (1500, the standard
Ethernet payload size) until `dmnetif_set_mtu()` changes it.

## Frame I/O

`dmnetif_send()`/`dmnetif_receive()` move one whole frame per call,
mirroring the underlying `dmdrvi` driver's `write()`/`read()` contract.
`dmnetif_receive()` is documented as non-blocking, returning 0 immediately
if nothing is pending, but this is only as good as the driver behind it:
`dmeth`'s own `_receive_frame()` actually blocks indefinitely waiting on
an RX-ISR-posted semaphore (dmdrvi has no `O_NONBLOCK`/timeout concept to
plumb through - see `dmeth_port.h`). In practice this is fine because
exactly one thread is meant to ever call `dmnetif_receive()` on a given
interface: `dmnetbridge_handle_netif_rx()` (run by the `networkd`
service, in the `dmnet` repository), in its own dedicated thread per
interface, is happy to block there - see dmnetbridge's own docs. A second
caller polling the same interface concurrently would race it.

## Interface presence

`dmnetif_is_present(iface)` checks whether the interface's backing devfs
file still exists (e.g. a hot-unplugged driver that removed its device
node), distinct from `dmnetif_is_up()`'s administrative up/down state.
`dmnetbridge_handle_netif_rx()`'s pump loop uses this to know when to stop
reading from an interface and let the thread pumping it end.

## Statistics

Every `dmnetif_send()`/`_receive()` call updates `dmnetif_stats_t` counters
on the interface (`rx_packets`/`rx_bytes`/`tx_packets`/`tx_bytes`/
`tx_errors`), readable via `dmnetif_get_stats()`. There is deliberately no
`rx_errors` counter: `dmnetif_receive()` returning 0 means either "no frame
currently pending" or a genuine read failure, and the underlying `dmdrvi`
`read()` contract does not distinguish the two - counting one as an error
would be misleading. `tx_errors` has no such ambiguity: it only increments
when the interface is up (so a frame was genuinely attempted) and the
driver's `write()` still returned 0, which unambiguously means the driver
rejected the frame.

## Escape hatch

`dmnetif_ioctl()` forwards a raw ioctl straight to the interface's
underlying device file, for driver-specific commands beyond the generic
control surface (start/stop, link status, MAC address) - e.g. `dmeth`'s
`DMETH_IOCTL_SET_PROMISCUOUS_MODE`. The caller must already know which
driver backs the interface to interpret `command`/`arg`; every other
DMNETIF function keeps that knowledge out of its callers by design, this
one intentionally does not.

## Dependencies

- `dmroute` - `dmroute_addr_t`, the type used for every IP address/
  netmask/broadcast field, plus `dmnetif_set_ip_address()` calling
  `dmroute_add()`/`_remove()` directly to keep an interface's connected
  route in sync (see the "IP address" section above)
- `dmdrvi` - generic driver interface (open/close/read/write/ioctl,
  `DMDRVI_IOCTL_NET_*` commands) used internally to talk to the device
  a driver registered
