#ifndef DMNETIF_H
#define DMNETIF_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmroute.h"
#include "dmnetif_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file dmnetif.h
 * @brief DMOD Network Interface Manager - Public API
 *
 * dmnetif is the boundary between devfs (dmdevfs/dmdrvi device files, e.g.
 * "/dev/dmeth0") and the network stack. A driver (dmeth, and later others -
 * loopback, PPP, Wi-Fi, ...) registers the devfs path it was assigned as a
 * named network interface ("eth0"); everything above that line (a TCP/IP
 * stack, `netctl`/`ifconfig`) only ever talks to dmnetif by interface name
 * and never has to open a device file, know a dmdrvi ioctl command, or
 * depend on dmdrvi/dmdevfs headers at all.
 *
 * dmnetif itself opens the devfs path once at dmnetif_register() time
 * (through the same Dmod_FileOpen/_Ioctl SAL any application would use) and
 * keeps that file handle for the interface's lifetime - callers never see
 * it.
 *
 * There is exactly one dmnetif registry per system (like dmdevfs's own
 * mount registry) - functions here are plain Built-in API
 * (dmod_dmnetif_api), not a DIF/MAL, since there is only ever one manager
 * to call into.
 *
 * dmnetif_set_ip_address() also keeps an interface's directly-connected
 * route in sync by calling dmroute_add()/_remove() directly (see
 * dmnetif.c) - dmnetif depends on dmroute both for this and for the
 * address type itself: IP addresses use dmroute_addr_t (see dmroute.h),
 * dmroute's own type, rather than one of dmnetif's own - see dmroute's own
 * docs for the full rationale.
 */

/**
 * @brief Opaque handle to one registered network interface
 */
typedef struct dmnetif_iface* dmnetif_iface_t;

/**
 * @brief Length in bytes of a MAC address (mirrors DMDRVI_NET_MAC_ADDR_LEN -
 *        kept as its own type so callers never need dmdrvi_ioctl.h)
 */
#define DMNETIF_MAC_ADDR_LEN 6

/**
 * @brief MAC address type
 */
typedef struct
{
    uint8_t addr[DMNETIF_MAC_ADDR_LEN];
} dmnetif_mac_addr_t;

/**
 * @brief Maximum length of an interface name (e.g. "eth0"), excluding the
 *        null terminator
 */
#define DMNETIF_NAME_MAX_LEN 15

/**
 * @brief Link status of a network interface
 */
typedef enum
{
    dmnetif_link_down = 0,    /**< Link is down (cable/association lost, or interface not started) */
    dmnetif_link_up   = 1,    /**< Link is up */
} dmnetif_link_status_t;

/* ============================================================================
 *                      Registration (driver-facing)
 * ========================================================================== */

/**
 * @brief Register a devfs-backed device as a named network interface
 *
 * Called by a network driver once its device file's absolute path is known
 * (e.g. from dmdrvi's dmdrvi_path_ready() callback) - not from the driver's
 * dmod_init(), since dmdevfs has not necessarily created the device node
 * yet at that point.
 *
 * dmnetif opens device_path itself and keeps it open until
 * dmnetif_unregister() is called; the driver does not need to (and should
 * not) open it independently.
 *
 * @param name          Interface name (e.g. "eth0"), max DMNETIF_NAME_MAX_LEN
 *                      bytes. Copied internally. Must be unique among
 *                      currently registered interfaces.
 * @param device_path   Absolute devfs path backing this interface (e.g.
 *                      "/dev/dmeth0"). Copied internally.
 *
 * @return Handle to the registered interface, or NULL on failure (name
 *         already in use, name too long, device_path could not be opened,
 *         or out of memory).
 */
dmod_dmnetif_api(1.0, dmnetif_iface_t, _register, ( const char* name, const char* device_path ));

/**
 * @brief Unregister a network interface
 *
 * Brings the interface down first if it is still up, then closes its
 * underlying device file. Safe to call with NULL.
 *
 * @param iface Interface to unregister
 */
dmod_dmnetif_api(1.0, void, _unregister, ( dmnetif_iface_t iface ));

/* ============================================================================
 *                      Lookup / enumeration (consumer-facing)
 * ========================================================================== */

/**
 * @brief Find a registered interface by name
 *
 * @param name Interface name to look up
 *
 * @return Handle to the interface, or NULL if no interface with that name
 *         is currently registered
 */
dmod_dmnetif_api(1.0, dmnetif_iface_t, _find_by_name, ( const char* name ));

/**
 * @brief Number of currently registered interfaces
 */
dmod_dmnetif_api(1.0, size_t, _count, ( void ));

/**
 * @brief Visitor callback for dmnetif_for_each()
 *
 * @param iface     One registered interface
 * @param user_data Passed through from dmnetif_for_each() unchanged
 *
 * @return true to continue iterating, false to stop early
 */
typedef bool (*dmnetif_iterator_func_t)( dmnetif_iface_t iface, void* user_data );

/**
 * @brief Visit every currently registered interface
 *
 * For listing every interface (e.g. "ifconfig -a") without the instability
 * of an index-based lookup across intervening register/unregister calls -
 * mirrors dmlist_foreach(), which dmnetif uses internally to hold its
 * registry. Visits interfaces in registration order.
 *
 * The registry is locked for the duration of the traversal: do not call
 * dmnetif_register()/dmnetif_unregister() (on any interface, including the
 * one currently being visited) from within callback - the underlying
 * dmlist_foreach() advances to the next node after the callback returns,
 * so removing the node currently being visited would leave it dangling,
 * and re-locking the registry's mutex from the same thread would deadlock.
 *
 * @param callback  Called once per interface until it returns false or
 *                  every interface has been visited
 * @param user_data Passed through to callback unchanged
 */
dmod_dmnetif_api(1.0, void, _for_each, ( dmnetif_iterator_func_t callback, void* user_data ));

/**
 * @brief Get an interface's name
 *
 * @param iface Interface handle
 *
 * @return The name passed to dmnetif_register(), or NULL if iface is invalid
 */
dmod_dmnetif_api(1.0, const char*, _get_name, ( dmnetif_iface_t iface ));

/* ============================================================================
 *                      State control
 * ========================================================================== */

/**
 * @brief Bring an interface up (starts packet reception/transmission)
 *
 * @param iface Interface handle
 *
 * @return 0 on success, negative errno on failure
 */
dmod_dmnetif_api(1.0, int, _up, ( dmnetif_iface_t iface ));

/**
 * @brief Bring an interface down
 *
 * @param iface Interface handle
 *
 * @return 0 on success, negative errno on failure
 */
dmod_dmnetif_api(1.0, int, _down, ( dmnetif_iface_t iface ));

/**
 * @brief Check whether dmnetif_up() has been called without a matching
 *        dmnetif_down() since
 *
 * @param iface Interface handle
 *
 * @return true if the interface is up
 */
dmod_dmnetif_api(1.0, bool, _is_up, ( dmnetif_iface_t iface ));

/**
 * @brief Query the interface's current link status from its driver
 *
 * @param iface Interface handle
 *
 * @return Current link status (dmnetif_link_down if iface is invalid)
 */
dmod_dmnetif_api(1.0, dmnetif_link_status_t, _get_link_status, ( dmnetif_iface_t iface ));

/**
 * @brief Check whether the interface's backing device file still exists
 *
 * Distinct from dmnetif_is_up(): that tracks administrative state (has
 * dmnetif_up() been called), this checks whether the devfs path opened in
 * dmnetif_register() is still present at all - e.g. a hot-unplugged driver
 * that removed its device node out from under an already-open interface.
 * Intended for a long-running reader (e.g. networkd's per-interface pump
 * loop) to know when to stop calling dmnetif_receive() on this interface
 * and let it go.
 *
 * @param iface Interface handle
 *
 * @return true if iface is valid and its backing device file still exists
 */
dmod_dmnetif_api(1.0, bool, _is_present, ( dmnetif_iface_t iface ));

/* ============================================================================
 *                      MTU
 * ========================================================================== */

/**
 * @brief Default MTU (bytes) a newly registered interface starts with -
 *        the standard Ethernet payload size
 */
#define DMNETIF_DEFAULT_MTU 1500

/**
 * @brief Get an interface's MTU
 *
 * Purely local bookkeeping, like the IP address - there is no dmdrvi ioctl
 * for this, the driver has no notion of it.
 *
 * @param iface Interface handle
 * @param mtu   Output buffer for the MTU
 *
 * @return 0 on success, negative errno on failure (invalid iface or NULL mtu)
 */
dmod_dmnetif_api(1.0, int, _get_mtu, ( dmnetif_iface_t iface, uint16_t* mtu ));

/**
 * @brief Set an interface's MTU
 *
 * @param iface Interface handle
 * @param mtu   MTU in bytes, must be non-zero
 *
 * @return 0 on success, negative errno on failure (invalid iface or mtu == 0)
 */
dmod_dmnetif_api(1.0, int, _set_mtu, ( dmnetif_iface_t iface, uint16_t mtu ));

/* ============================================================================
 *                      MAC address
 * ========================================================================== */

/**
 * @brief Get an interface's MAC address
 *
 * @param iface Interface handle
 * @param mac   Output buffer for the MAC address
 *
 * @return 0 on success, negative errno on failure
 */
dmod_dmnetif_api(1.0, int, _get_mac_address, ( dmnetif_iface_t iface, dmnetif_mac_addr_t* mac ));

/**
 * @brief Set an interface's MAC address
 *
 * @param iface Interface handle
 * @param mac   MAC address to set
 *
 * @return 0 on success, negative errno on failure
 */
dmod_dmnetif_api(1.0, int, _set_mac_address, ( dmnetif_iface_t iface, const dmnetif_mac_addr_t* mac ));

/* ============================================================================
 *                      IP address
 * ========================================================================== */

/**
 * @brief Get an interface's currently assigned IP address
 *
 * @param iface Interface handle
 * @param ip    Output buffer. On success, ip->family is dmroute_family_none
 *              if no IP address has been assigned yet
 *
 * @return 0 on success, negative errno on failure (invalid iface or NULL ip)
 */
dmod_dmnetif_api(1.0, int, _get_ip_address, ( dmnetif_iface_t iface, dmroute_addr_t* ip ));

/**
 * @brief Set an interface's IP address
 *
 * Purely local bookkeeping, unlike dmnetif_set_mac_address() - there is no
 * dmdrvi ioctl for this. An IP address is a network-layer concept the
 * driver/hardware below dmnetif has no notion of; it's assigned by whatever
 * runs above dmnetif (e.g. a DHCP client or static config in networkd) and
 * simply stored here for lookup by name, same as everything else dmnetif
 * tracks per interface.
 *
 * Also keeps the interface's directly-connected route in dmroute in sync:
 * removes whichever route this call previously added for the interface (if
 * any), then - unless `ip` clears the address - adds a fresh one from the
 * new address and whatever netmask is currently on record (falling back to
 * an all-ones host mask if none is set yet). See dmnetif.c's
 * update_connected_route() for the details.
 *
 * @param iface Interface handle
 * @param ip    IP address to assign (family must be dmroute_family_v4 or
 *              _v6; pass dmroute_family_none to clear the assigned address)
 *
 * @return 0 on success, negative errno on failure (invalid iface, NULL ip,
 *         or an unrecognized family)
 */
dmod_dmnetif_api(1.0, int, _set_ip_address, ( dmnetif_iface_t iface, const dmroute_addr_t* ip ));

/**
 * @brief Get an interface's currently assigned netmask
 *
 * Reuses dmroute_addr_t rather than a dedicated type: a netmask is shaped
 * exactly like an address (4 or 16 raw bytes, discriminated by family) -
 * an IPv4 netmask is conventionally written as a dotted-quad value (e.g.
 * "255.255.255.0") for exactly this reason. Feeds directly into the
 * directly-connected route dmnetif_set_ip_address() keeps in dmroute (see
 * its own doc comment) - set the netmask before the address for it to be
 * picked up by that route, since dmnetif_set_netmask() itself does not
 * trigger a route update.
 *
 * @param iface   Interface handle
 * @param netmask Output buffer. On success, netmask->family is
 *                dmroute_family_none if none has been assigned yet
 *
 * @return 0 on success, negative errno on failure (invalid iface or NULL netmask)
 */
dmod_dmnetif_api(1.0, int, _get_netmask, ( dmnetif_iface_t iface, dmroute_addr_t* netmask ));

/**
 * @brief Set an interface's netmask
 *
 * Purely local bookkeeping, same as dmnetif_set_ip_address() - no dmdrvi
 * ioctl, the driver has no notion of it.
 *
 * @param iface   Interface handle
 * @param netmask Netmask to assign (family must be dmroute_family_v4 or _v6;
 *                pass dmroute_family_none to clear it)
 *
 * @return 0 on success, negative errno on failure (invalid iface, NULL
 *         netmask, or an unrecognized family)
 */
dmod_dmnetif_api(1.0, int, _set_netmask, ( dmnetif_iface_t iface, const dmroute_addr_t* netmask ));

/**
 * @brief Get an interface's currently assigned broadcast address
 *
 * Reuses dmroute_addr_t, same as the netmask - a broadcast address is shaped
 * exactly like a regular address.
 *
 * @param iface     Interface handle
 * @param broadcast Output buffer. On success, broadcast->family is
 *                  dmroute_family_none if none has been assigned yet
 *
 * @return 0 on success, negative errno on failure (invalid iface or NULL broadcast)
 */
dmod_dmnetif_api(1.0, int, _get_broadcast, ( dmnetif_iface_t iface, dmroute_addr_t* broadcast ));

/**
 * @brief Set an interface's broadcast address
 *
 * Purely local bookkeeping, same as dmnetif_set_ip_address()/_set_netmask()
 * - no dmdrvi ioctl, the driver has no notion of it. Settable directly from
 * `ifconfig` (`ifconfig <iface> broadcast <addr>`), unlike the address and
 * netmask which are only ever assigned by whatever runs above dmnetif
 * (DHCP client or static config in networkd).
 *
 * @param iface     Interface handle
 * @param broadcast Broadcast address to assign (family must be
 *                  dmroute_family_v4 or _v6; pass dmroute_family_none to clear it)
 *
 * @return 0 on success, negative errno on failure (invalid iface, NULL
 *         broadcast, or an unrecognized family)
 */
dmod_dmnetif_api(1.0, int, _set_broadcast, ( dmnetif_iface_t iface, const dmroute_addr_t* broadcast ));

/* ============================================================================
 *                      Frame I/O (bridge to the network stack)
 * ========================================================================== */

/**
 * @brief Transmit one frame
 *
 * One call transmits exactly one Ethernet-style frame - same one-call/
 * one-frame contract as the underlying dmdrvi driver's write().
 *
 * @param iface  Interface handle
 * @param frame  Raw frame bytes (destination MAC, source MAC, ethertype,
 *               payload - dmnetif never touches the header)
 * @param length Frame length in bytes
 *
 * @return Bytes actually sent, or 0 if the interface is down or the driver
 *         could not accept the frame
 */
dmod_dmnetif_api(1.0, size_t, _send, ( dmnetif_iface_t iface, const void* frame, size_t length ));

/**
 * @brief Receive one frame, if any is available
 *
 * Non-blocking: returns 0 immediately if no frame is currently pending,
 * mirroring the underlying driver's read() semantics. Callers that need to
 * wait for data (e.g. a network stack's RX task) should poll on their own
 * schedule.
 *
 * @param iface  Interface handle
 * @param buffer Buffer to receive the frame into
 * @param size   Size of buffer in bytes
 *
 * @return Bytes actually received, or 0 if none were available
 */
dmod_dmnetif_api(1.0, size_t, _receive, ( dmnetif_iface_t iface, void* buffer, size_t size ));

/* ============================================================================
 *                      Statistics
 * ========================================================================== */

/**
 * @brief Per-interface packet counters, updated by dmnetif_send()/_receive()
 *
 * There is no rx_errors counter: dmnetif_receive() returning 0 means either
 * "no frame currently pending" or a genuine read failure - the underlying
 * dmdrvi read() contract does not distinguish the two, so counting one as
 * an error would be misleading. dmnetif_send() returning 0 while the
 * interface is up is unambiguously the driver rejecting the frame, which
 * tx_errors does track.
 *
 * Counters wrap on overflow like any other embedded packet counter (no
 * saturation) - callers that care about wraparound should sample often
 * enough relative to traffic volume.
 */
typedef struct
{
    uint32_t rx_packets;    /**< Frames received via dmnetif_receive() */
    uint32_t rx_bytes;      /**< Bytes received via dmnetif_receive() */
    uint32_t tx_packets;    /**< Frames sent via dmnetif_send() */
    uint32_t tx_bytes;      /**< Bytes sent via dmnetif_send() */
    uint32_t tx_errors;     /**< dmnetif_send() calls where the interface was up but the driver rejected the frame */
} dmnetif_stats_t;

/**
 * @brief Get an interface's packet statistics
 *
 * @param iface Interface handle
 * @param stats Output buffer for the statistics
 *
 * @return 0 on success, negative errno on failure (invalid iface or NULL stats)
 */
dmod_dmnetif_api(1.0, int, _get_stats, ( dmnetif_iface_t iface, dmnetif_stats_t* stats ));

/* ============================================================================
 *                      Escape hatch
 * ========================================================================== */

/**
 * @brief Forward a raw ioctl to the interface's underlying device file
 *
 * Covers driver-specific commands beyond the generic control surface above
 * (e.g. dmeth's DMETH_IOCTL_SET_PROMISCUOUS_MODE). The caller must know
 * which driver backs the interface to interpret command/arg correctly -
 * unlike every other dmnetif function, this one intentionally punches
 * through the devfs/network boundary, the same way a raw ioctl() punches
 * through a POSIX file descriptor.
 *
 * @param iface   Interface handle
 * @param command Driver-specific ioctl command
 * @param arg     Command-specific argument
 *
 * @return 0 on success, negative errno on failure
 */
dmod_dmnetif_api(1.0, int, _ioctl, ( dmnetif_iface_t iface, int command, void* arg ));

#ifdef __cplusplus
}
#endif

#endif // DMNETIF_H
