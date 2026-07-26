# dmnetif API Reference

See [dmnetif.md](dmnetif.md) for the architecture and rationale behind this
API's shape.

## Types

| Type                       | Description                                                       |
|----------------------------|---------------------------------------------------------------------|
| `dmnetif_iface_t`          | Opaque handle to one registered network interface                  |
| `dmnetif_mac_addr_t`       | `{ uint8_t addr[DMNETIF_MAC_ADDR_LEN] }` - MAC address              |
| `dmroute_family_t` (from dmroute) | `dmroute_family_none` \| `_v4` \| `_v6`         |
| `dmroute_addr_t` (from dmroute) | `{ family; union { v4[4]; v6[16]; } addr; }` - one type for both IPv4 and IPv6, discriminated by `family`; dmroute's own type, reused here rather than a dmnetif-specific one |
| `dmnetif_link_status_t`    | `dmnetif_link_down` \| `dmnetif_link_up`                            |
| `dmnetif_stats_t`          | `{ rx_packets; rx_bytes; tx_packets; tx_bytes; tx_errors; }` - packet counters, see `dmnetif_get_stats()` |
| `dmnetif_iterator_func_t`  | `bool (*)(dmnetif_iface_t iface, void* user_data)` - see `dmnetif_for_each()` |

## Registration (driver-facing)

| Function                                    | Description                                                        |
|-----------------------------------------------|-----------------------------------------------------------------------|
| `dmnetif_register(name, device_path)`        | Register a devfs-backed device as a named network interface. Opens `device_path` internally. |
| `dmnetif_unregister(iface)`                  | Bring the interface down, close its device file, remove it. Safe on `NULL`. |

## Lookup / enumeration (consumer-facing)

| Function                                    | Description                                                        |
|-----------------------------------------------|-----------------------------------------------------------------------|
| `dmnetif_find_by_name(name)`                 | Find a registered interface by name.                                |
| `dmnetif_count()`                            | Number of currently registered interfaces.                          |
| `dmnetif_for_each(callback, user_data)`      | Visit every registered interface (stops early if `callback` returns `false`). |
| `dmnetif_get_name(iface)`                    | Interface name, as passed to `dmnetif_register()`.                   |

## State control

| Function                        | Description                                             |
|-----------------------------------|-------------------------------------------------------------|
| `dmnetif_up(iface)`             | Bring the interface up.                                  |
| `dmnetif_down(iface)`           | Bring the interface down.                                |
| `dmnetif_is_up(iface)`          | Whether `dmnetif_up()` has been called without a matching `dmnetif_down()`. |
| `dmnetif_get_link_status(iface)`| Query the current link status from the driver.           |
| `dmnetif_is_present(iface)`     | Whether the interface's backing devfs file still exists - distinct from `dmnetif_is_up()`'s administrative state. |

## MTU

| Function                        | Description                                             |
|-----------------------------------|-------------------------------------------------------------|
| `dmnetif_get_mtu(iface, mtu)`    | Read the interface's MTU (`DMNETIF_DEFAULT_MTU` = 1500 until set). |
| `dmnetif_set_mtu(iface, mtu)`   | Set the interface's MTU. Fails on `mtu == 0`. Local bookkeeping only, same as the IP address - no dmdrvi ioctl. |

## MAC address

| Function                                          | Description               |
|------------------------------------------------------|-------------------------------|
| `dmnetif_get_mac_address(iface, mac)`                | Read the interface's MAC address. |
| `dmnetif_set_mac_address(iface, mac)`                | Set the interface's MAC address.  |

## IP address

| Function                                          | Description               |
|------------------------------------------------------|-------------------------------|
| `dmnetif_get_ip_address(iface, ip)`                  | Read the interface's currently assigned IP address (`family` is `dmroute_family_none` if none assigned). |
| `dmnetif_set_ip_address(iface, ip)`                  | Assign (or, with `dmroute_family_none`, clear) the interface's IP address. Local bookkeeping only - no dmdrvi ioctl, since IP addresses are a network-layer concept the driver has no notion of. Also calls `dmroute_add()`/`_remove()` directly to keep the interface's connected route in dmroute in sync. |
| `dmnetif_get_netmask(iface, netmask)`                | Read the interface's currently assigned netmask (`family` is `dmroute_family_none` if none assigned). Reuses `dmroute_addr_t` - a netmask is shaped exactly like an address. Set it before `dmnetif_set_ip_address()` for it to be picked up by the connected route - `dmnetif_set_netmask()` itself does not trigger a route update. |
| `dmnetif_set_netmask(iface, netmask)`                | Assign (or clear) the interface's netmask. Local bookkeeping only, same as the address. |
| `dmnetif_get_broadcast(iface, broadcast)`            | Read the interface's currently assigned broadcast address (`family` is `dmroute_family_none` if none assigned). |
| `dmnetif_set_broadcast(iface, broadcast)`            | Assign (or clear) the interface's broadcast address. Local bookkeeping only. Settable directly via `ifconfig <iface> broadcast <addr>`. |

## Frame I/O

| Function                                    | Description                                                        |
|-----------------------------------------------|-----------------------------------------------------------------------|
| `dmnetif_send(iface, frame, length)`         | Transmit one frame. Returns bytes actually sent.                     |
| `dmnetif_receive(iface, buffer, size)`       | Receive one frame if available (non-blocking). Returns bytes actually received. |

## Statistics

| Function                                    | Description                                                        |
|-----------------------------------------------|-----------------------------------------------------------------------|
| `dmnetif_get_stats(iface, stats)`            | Read the interface's packet counters, updated by `dmnetif_send()`/`_receive()`. No `rx_errors` field - see `dmnetif_stats_t`'s doc comment for why. |

## Escape hatch

| Function                                    | Description                                                        |
|-----------------------------------------------|-----------------------------------------------------------------------|
| `dmnetif_ioctl(iface, command, arg)`         | Forward a raw ioctl to the interface's underlying device file (driver-specific commands, e.g. `dmeth`'s `DMETH_IOCTL_SET_PROMISCUOUS_MODE`). |
