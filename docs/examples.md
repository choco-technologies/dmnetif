# dmnetif Usage Examples

All examples assume this helper for building an IPv4 `dmroute_addr_t` (the
address type dmnetif borrows from [dmroute](https://github.com/choco-technologies/dmroute)):

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
```

---

## Example 1: Registering a driver-backed interface

A driver never calls `dmnetif_register()` from its own `dmod_init()` -
`dmdevfs` has not necessarily created its device node yet at that point. The
natural place is the driver's `dmdrvi_path_ready()` implementation, which
fires once the device's absolute path is actually known:

```c
dmod_dmdrvi_dif_api_declaration(1.0, dmeth, void, _path_ready,
    ( dmdrvi_context_t context, const dmdrvi_dev_num_t* dev_num, const char* path ))
{
    dmnetif_register(context->config.if_name, path);
}
```

`dmnetif_register()` opens `path` itself and keeps it open for the
interface's lifetime - the driver never opens its own device file.
Registration isn't limited to drivers, though: `ifconfig <name> create
<device_path>` calls it directly too (e.g. for testing without a real driver
at all, or to register a devfs node a driver exposed under a different
name).

---

## Example 2: Basic consumer usage - find, bring up, configure

```c
void configure_interface(void)
{
    dmnetif_iface_t iface = dmnetif_find_by_name("eth0");
    if (iface == NULL)
    {
        return; /* no driver has registered this name (yet) */
    }

    if (dmnetif_up(iface) != 0)
    {
        return; /* driver rejected the start ioctl */
    }

    dmnetif_mac_addr_t mac = { 0 };
    dmnetif_get_mac_address(iface, &mac);

    uint16_t mtu = 0;
    dmnetif_get_mtu(iface, &mtu); /* DMNETIF_DEFAULT_MTU (1500) unless changed */
}
```

---

## Example 3: Assigning an IP address keeps dmroute in sync

Set the netmask *before* the address - `dmnetif_set_ip_address()` reads
whatever netmask is currently on record when it updates the interface's
connected route in dmroute. Getting the order backwards still works, just
with a host-only route until the address is set again.

```c
void assign_address_example(void)
{
    dmnetif_iface_t iface = dmnetif_find_by_name("eth0");
    if (iface == NULL)
    {
        return;
    }

    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);
    dmnetif_set_netmask(iface, &netmask);

    dmroute_addr_t ip = make_v4(192, 168, 1, 42);
    dmnetif_set_ip_address(iface, &ip);
    /* dmroute now has a connected route for 192.168.1.0/24 via "eth0",
     * with dmroute_get_origin() == dmroute_origin_connected */

    dmroute_addr_t neighbor = make_v4(192, 168, 1, 200);
    dmroute_route_t route = dmroute_lookup(&neighbor);
    if (route != NULL)
    {
        const char* via = dmroute_get_iface_name(route); /* "eth0" */
        (void)via;
    }

    /* Clearing the address removes the connected route again - nothing
     * else needs to call dmroute_remove() for that */
    dmroute_addr_t none = { 0 };
    none.family = dmroute_family_none;
    dmnetif_set_ip_address(iface, &none);
}
```

Reassigning the address (without clearing first) replaces the previous
connected route rather than leaving a stale one behind - the route count in
dmroute does not grow across repeated `dmnetif_set_ip_address()` calls on
the same interface.

---

## Example 4: Listing every interface (`ifconfig -a`-style)

`dmnetif_for_each()` visits every registered interface in registration
order, without the instability of an index-based loop across intervening
register/unregister calls.

```c
#include <stdio.h>

static bool print_iface(dmnetif_iface_t iface, void* user_data)
{
    (void)user_data;

    dmroute_addr_t ip = { 0 };
    dmnetif_get_ip_address(iface, &ip);

    printf("%s: link %s, ip family=%d\n",
           dmnetif_get_name(iface),
           dmnetif_get_link_status(iface) == dmnetif_link_up ? "up" : "down",
           (int)ip.family);

    return true; /* keep going */
}

void list_interfaces_example(void)
{
    dmnetif_for_each(print_iface, NULL);
    printf("total interfaces: %zu\n", dmnetif_count());
}
```

Do not call `dmnetif_register()`/`dmnetif_unregister()` from within the
callback - the registry is locked for the whole traversal, so unregistering
the interface currently being visited would leave the traversal dangling,
and registering a new one would deadlock re-locking the same mutex.

---

## Example 5: Frame I/O and statistics

`dmnetif_send()`/`dmnetif_receive()` move exactly one frame per call,
mirroring the underlying driver's `write()`/`read()` contract. Both return 0
if the interface isn't up.

```c
void frame_io_example(void)
{
    dmnetif_iface_t iface = dmnetif_find_by_name("eth0");
    if (iface == NULL || dmnetif_up(iface) != 0)
    {
        return;
    }

    uint8_t frame[64] = { 0 }; /* dest MAC, src MAC, ethertype, payload */
    size_t sent = dmnetif_send(iface, frame, sizeof(frame));

    uint8_t buffer[1500];
    size_t received = dmnetif_receive(iface, buffer, sizeof(buffer));

    dmnetif_stats_t stats;
    dmnetif_get_stats(iface, &stats);
    printf("sent %zu, received %zu, tx_errors=%u\n",
           sent, received, stats.tx_errors);
}
```

Exactly one thread is meant to ever call `dmnetif_receive()` on a given
interface - a second caller polling the same interface concurrently would
race it (see [docs/dmnetif.md](dmnetif.md#frame-io)).

---

## Example 6: The raw ioctl escape hatch

`dmnetif_ioctl()` forwards a raw ioctl straight to the interface's
underlying device file, for driver-specific commands beyond the generic
control surface above - e.g. `dmeth`'s `DMETH_IOCTL_SET_PROMISCUOUS_MODE`.
Unlike every other dmnetif function, the caller must already know which
driver backs the interface to interpret `command`/`arg` correctly.

```c
void enable_promiscuous_mode(dmnetif_iface_t iface)
{
    int enable = 1;
    dmnetif_ioctl(iface, DMETH_IOCTL_SET_PROMISCUOUS_MODE, &enable);
}
```

---

## Example 7: Clean teardown

`dmnetif_unregister()` brings the interface down first if it is still up,
closes its device file, and removes its connected route from dmroute - safe
to call with `NULL`.

```c
void teardown_example(void)
{
    dmnetif_iface_t iface = dmnetif_find_by_name("eth0");
    dmnetif_unregister(iface); /* down + close + connected route removed */
}
```
