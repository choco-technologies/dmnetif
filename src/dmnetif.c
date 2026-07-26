/**
 * @file dmnetif.c
 * @brief DMOD Network Interface Manager - Implementation
 *
 * Registry of registered interfaces (name -> device file) backed by dmlist,
 * guarded by a single mutex. Every function that talks to a specific
 * interface's device forwards to the generic file SAL (Dmod_FileRead/
 * _FileWrite/_Ioctl) opened once in dmnetif_register() - see dmnetif.h and
 * docs/dmnetif.md for the full rationale.
 *
 * Also keeps each interface's directly-connected route in dmroute up to
 * date (see update_connected_route()) - dmnetif depends on dmroute both
 * for this and for the shared dmroute_addr_t type its own IP/netmask/
 * broadcast fields use.
 *
 * No real network driver exercises this yet - dmeth doesn't call
 * dmnetif_register() from its dmdrvi_path_ready() implementation yet (see
 * docs/dmnetif.md's "Registration flow" section). tests/dmnetif_test.c
 * covers the registry against "/dev/null" as a stand-in device file.
 */
#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "dmnetif.h"
#include "dmroute.h"
#include "dmlist.h"
#include "dmdrvi.h"
#include "dmdrvi_ioctl.h"
#include "dmosi.h"
#include <string.h>
#include <errno.h>

/**
 * @brief Magic value guarding struct dmnetif_iface against stale/invalid
 *        pointers - set to "NTIF" packed into a uint32_t
 */
#define DMNETIF_CONTEXT_MAGIC    0x4E544946u

/**
 * @brief Private definition of the opaque dmnetif_iface_t handle
 *
 * One instance per interface registered via dmnetif_register(), held in
 * g_ifaces for the interface's lifetime.
 */
struct dmnetif_iface
{
    uint32_t           magic;                          /**< Must equal DMNETIF_CONTEXT_MAGIC for this struct to be considered valid - see is_valid_iface() */
    char               name[DMNETIF_NAME_MAX_LEN + 1];  /**< Interface name, as passed to dmnetif_register() (null-terminated) */
    char*              device_path;                    /**< Dmod_StrDup()'d copy of the devfs path passed to dmnetif_register() */
    void*              file;                            /**< Handle returned by Dmod_FileOpen(device_path, "r+"), kept open for the interface's lifetime */
    bool               up;                               /**< Whether dmnetif_up() has been applied without a matching dmnetif_down() since */
    dmroute_addr_t        ip;                                /**< Currently assigned IP address; family is dmroute_family_none until dmnetif_set_ip_address() is called */
    dmroute_addr_t        netmask;                            /**< Currently assigned netmask; family is dmroute_family_none until dmnetif_set_netmask() is called */
    dmroute_addr_t        broadcast;                           /**< Currently assigned broadcast address; family is dmroute_family_none until dmnetif_set_broadcast() is called */
    uint16_t           mtu;                                  /**< MTU in bytes; DMNETIF_DEFAULT_MTU until dmnetif_set_mtu() is called */
    dmnetif_stats_t    stats;                                 /**< Packet counters, updated by dmnetif_send()/_receive() */
    dmroute_route_t    connected_route;                        /**< This interface's directly-connected route in dmroute, or NULL if it currently has none - see update_connected_route() */
};

/**
 * @brief Registry of every currently registered interface (struct dmnetif_iface*)
 *
 * Created in dmod_init(), destroyed in dmod_deinit(). Guarded by g_mutex.
 */
static dmlist_context_t* g_ifaces = NULL;

/**
 * @brief Mutex guarding g_ifaces against concurrent register/unregister/
 *        lookup calls
 *
 * Created in dmod_init(), destroyed in dmod_deinit().
 */
static dmosi_mutex_t g_mutex = NULL;

/**
 * @brief Check whether a handle is a live, correctly-typed dmnetif_iface_t
 *
 * Called at the top of every function that dereferences an interface
 * handle, before it is used, to catch a stale/freed or wrong-type pointer
 * early instead of leading to undefined behavior further down.
 *
 * @param iface Handle to validate (NULL is a valid input - returns false)
 *
 * @return true if iface is non-NULL and its magic field is intact
 */
static bool is_valid_iface(dmnetif_iface_t iface)
{
    return iface != NULL && iface->magic == DMNETIF_CONTEXT_MAGIC;
}

/**
 * @brief dmlist_compare_func_t comparing a struct dmnetif_iface's name
 *        against a plain "const char*" needle
 *
 * dmlist_find()/_remove() call compare_func(list_element, data) - see
 * dmlist.c - so `data` here is the list element (a struct dmnetif_iface*)
 * and `user_data` is the name string passed to dmnetif_find_by_name()/
 * the duplicate-name check in dmnetif_register().
 *
 * @param data      One interface currently in the registry (struct dmnetif_iface*)
 * @param user_data Name to compare against (const char*)
 *
 * @return 0 if the interface's name matches user_data, matching strcmp() otherwise
 */
static int compare_name(const void* data, const void* user_data)
{
    const struct dmnetif_iface* iface = (const struct dmnetif_iface*)data;
    return strcmp(iface->name, (const char*)user_data);
}

/**
 * @brief dmlist_compare_func_t comparing two elements by pointer identity
 *
 * Used by dmnetif_unregister() to remove a specific interface from the
 * registry by its own address, regardless of its contents.
 *
 * @param data      One interface currently in the registry
 * @param user_data The interface pointer being searched for
 *
 * @return 0 if data and user_data are the same pointer, -1 otherwise
 */
static int compare_pointer(const void* data, const void* user_data)
{
    return (data == user_data) ? 0 : -1;
}

/**
 * @brief Check whether an IP family value is one dmnetif recognizes
 *
 * Shared by dmnetif_set_ip_address() and dmnetif_set_netmask(), which both
 * accept the same three values (none/v4/v6).
 *
 * @param family Value to check
 *
 * @return true if family is dmroute_family_none/_v4/_v6
 */
static bool is_valid_ip_family(dmroute_family_t family)
{
    return family == dmroute_family_none ||
           family == dmroute_family_v4 ||
           family == dmroute_family_v6;
}

/**
 * @brief Keep an interface's directly-connected route in dmroute in sync
 *        with its current IP address/netmask
 *
 * Called by dmnetif_set_ip_address() after it records the new address.
 * Always drops whatever route this function previously added for `iface`
 * (if any) first, then - unless the address was just cleared - adds a
 * fresh one: dmnetif_set_ip_address(iface, family_none) alone is enough to
 * remove the connected route, nothing else needs to call dmroute_remove()
 * for that case.
 *
 * If the interface doesn't have a usable netmask on record yet
 * (dmnetif_set_netmask() hasn't been called, or was called with a
 * different family than the address), falls back to an all-ones host mask
 * so the interface is at least reachable by its own address in the
 * meantime - e.g. a DHCP client that sets the address before the netmask.
 *
 * @param iface Interface whose connected route should be refreshed (must
 *              already be a valid handle)
 */
static void update_connected_route(struct dmnetif_iface* iface)
{
    dmroute_remove(iface->connected_route);
    iface->connected_route = NULL;

    if (iface->ip.family == dmroute_family_none)
        return;

    dmroute_addr_t netmask = iface->netmask;
    if (netmask.family != iface->ip.family)
    {
        netmask.family = iface->ip.family;
        memset(netmask.addr.v6, 0xFF, sizeof(netmask.addr.v6));
    }

    iface->connected_route = dmroute_add(&iface->ip, &netmask, NULL, iface->name, DMROUTE_DEFAULT_METRIC, dmroute_origin_connected);
    if (iface->connected_route == NULL)
    {
        DMOD_LOG_ERROR("dmnetif: failed to add connected route for '%s'\n", iface->name);
    }
}

/**
 * @brief Tear down one interface: drop its connected route, stop it if
 *        running, close its device file, and free it
 *
 * Does not touch the registry - callers are responsible for having already
 * removed iface from g_ifaces before calling this (see dmnetif_unregister()
 * and dmod_deinit()).
 *
 * @param iface Interface to tear down (must be a valid, non-NULL handle)
 */
static void close_iface(struct dmnetif_iface* iface)
{
    dmroute_remove(iface->connected_route);
    if (iface->up)
    {
        Dmod_Ioctl(iface->file, DMDRVI_IOCTL_NET_STOP, NULL);
    }
    Dmod_FileClose(iface->file);
    Dmod_Free(iface->device_path);
    iface->magic = 0;
    Dmod_Free(iface);
}

/* ---- DMOD lifecycle ---- */

/**
 * @brief Module initialization - allocates the interface registry and its
 *        guarding mutex
 *
 * @param Config Module configuration (unused)
 *
 * @return 0 on success, -1 if the registry or mutex could not be allocated
 */
int dmod_init(const Dmod_Config_t *Config)
{
    g_ifaces = dmlist_create(Dmod_GetCurrentAllocatorName());
    g_mutex  = dmosi_mutex_create(false);
    if (g_ifaces == NULL || g_mutex == NULL)
    {
        DMOD_LOG_ERROR("Failed to allocate dmnetif registry\n");
        return -1;
    }

    DMOD_LOG_INFO("DMNETIF interface manager initialized\n");
    return 0;
}

/**
 * @brief Module deinitialization - tears down every still-registered
 *        interface, then frees the registry and its mutex
 *
 * @return Always 0
 */
int dmod_deinit(void)
{
    size_t count = dmlist_size(g_ifaces);
    for (size_t i = 0; i < count; i++)
    {
        close_iface((struct dmnetif_iface*)dmlist_pop_front(g_ifaces));
    }
    dmlist_destroy(g_ifaces);
    g_ifaces = NULL;

    dmosi_mutex_destroy(g_mutex);
    g_mutex = NULL;

    DMOD_LOG_INFO("DMNETIF interface manager deinitialized\n");
    return 0;
}

/* ---- Registration (driver-facing) ---- */

/**
 * @brief Implementation of dmnetif_register() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, dmnetif_iface_t, _register, ( const char* name, const char* device_path ))
{
    if (name == NULL || device_path == NULL)
    {
        DMOD_LOG_ERROR("dmnetif_register: NULL name or device_path\n");
        return NULL;
    }

    if (strlen(name) > DMNETIF_NAME_MAX_LEN)
    {
        DMOD_LOG_ERROR("dmnetif_register: name '%s' longer than %d bytes\n", name, DMNETIF_NAME_MAX_LEN);
        return NULL;
    }

    struct dmnetif_iface* iface = Dmod_Malloc(sizeof(*iface));
    if (iface == NULL)
        return NULL;

    memset(iface, 0, sizeof(*iface));
    iface->magic = DMNETIF_CONTEXT_MAGIC;
    iface->mtu = DMNETIF_DEFAULT_MTU;
    Dmod_SnPrintf(iface->name, sizeof(iface->name), "%s", name);
    iface->device_path = Dmod_StrDup(device_path);
    iface->file = (iface->device_path != NULL) ? Dmod_FileOpen(device_path, "r+") : NULL;
    if (iface->file == NULL)
    {
        DMOD_LOG_ERROR("dmnetif_register: failed to open '%s'\n", device_path);
        Dmod_Free(iface->device_path);
        Dmod_Free(iface);
        return NULL;
    }

    /* Duplicate-name check and insertion happen under the same lock, so a
     * concurrent dmnetif_register() with the same name cannot slip in
     * between the check and the insert. */
    dmosi_mutex_lock(g_mutex);
    bool duplicate = dmlist_find(g_ifaces, name, compare_name) != NULL;
    bool added = !duplicate && dmlist_push_back(g_ifaces, iface);
    dmosi_mutex_unlock(g_mutex);

    if (!added)
    {
        if (duplicate)
        {
            DMOD_LOG_ERROR("dmnetif_register: interface '%s' already registered\n", name);
        }
        Dmod_FileClose(iface->file);
        Dmod_Free(iface->device_path);
        Dmod_Free(iface);
        return NULL;
    }

    DMOD_LOG_INFO("Registered network interface '%s' -> %s\n", name, device_path);
    return iface;
}

/**
 * @brief Implementation of dmnetif_unregister() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, void, _unregister, ( dmnetif_iface_t iface ))
{
    if (!is_valid_iface(iface))
        return;

    dmosi_mutex_lock(g_mutex);
    dmlist_remove(g_ifaces, iface, compare_pointer);
    dmosi_mutex_unlock(g_mutex);

    close_iface(iface);
}

/* ---- Lookup / enumeration ---- */

/**
 * @brief Implementation of dmnetif_find_by_name() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, dmnetif_iface_t, _find_by_name, ( const char* name ))
{
    if (name == NULL)
        return NULL;

    dmosi_mutex_lock(g_mutex);
    void* found = dmlist_find(g_ifaces, name, compare_name);
    dmosi_mutex_unlock(g_mutex);
    return (dmnetif_iface_t)found;
}

/**
 * @brief Implementation of dmnetif_count() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, size_t, _count, ( void ))
{
    dmosi_mutex_lock(g_mutex);
    size_t count = dmlist_size(g_ifaces);
    dmosi_mutex_unlock(g_mutex);
    return count;
}

/**
 * @brief Closure passed as dmlist_foreach()'s user_data by dmnetif_for_each(),
 *        letting for_each_trampoline() recover the caller's own callback/
 *        user_data pair
 */
typedef struct
{
    dmnetif_iterator_func_t callback;    /**< Caller-supplied callback to invoke per interface */
    void*                   user_data;   /**< Caller-supplied opaque data, passed through to callback unchanged */
} for_each_ctx_t;

/**
 * @brief dmlist_iterator_func_t adapting dmlist's "void* data" element type
 *        to dmnetif_for_each()'s "dmnetif_iface_t iface" callback signature
 *
 * @param data      One interface currently in the registry (struct dmnetif_iface*)
 * @param user_data A for_each_ctx_t* (see dmnetif_for_each())
 *
 * @return Whatever the caller's own callback returns (true to continue, false to stop)
 */
static bool for_each_trampoline(void* data, void* user_data)
{
    for_each_ctx_t* ctx = (for_each_ctx_t*)user_data;
    return ctx->callback((dmnetif_iface_t)data, ctx->user_data);
}

/**
 * @brief Implementation of dmnetif_for_each() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, void, _for_each, ( dmnetif_iterator_func_t callback, void* user_data ))
{
    if (callback == NULL)
        return;

    for_each_ctx_t ctx = { .callback = callback, .user_data = user_data };
    dmosi_mutex_lock(g_mutex);
    dmlist_foreach(g_ifaces, for_each_trampoline, &ctx);
    dmosi_mutex_unlock(g_mutex);
}

/**
 * @brief Implementation of dmnetif_get_name() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, const char*, _get_name, ( dmnetif_iface_t iface ))
{
    return is_valid_iface(iface) ? iface->name : NULL;
}

/* ---- State control ---- */

/**
 * @brief Implementation of dmnetif_up() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _up, ( dmnetif_iface_t iface ))
{
    if (!is_valid_iface(iface))
        return -EINVAL;

    int ret = Dmod_Ioctl(iface->file, DMDRVI_IOCTL_NET_START, NULL);
    if (ret == 0)
        iface->up = true;
    return ret;
}

/**
 * @brief Implementation of dmnetif_down() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _down, ( dmnetif_iface_t iface ))
{
    if (!is_valid_iface(iface))
        return -EINVAL;

    int ret = Dmod_Ioctl(iface->file, DMDRVI_IOCTL_NET_STOP, NULL);
    iface->up = false;
    return ret;
}

/**
 * @brief Implementation of dmnetif_is_up() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, bool, _is_up, ( dmnetif_iface_t iface ))
{
    return is_valid_iface(iface) && iface->up;
}

/**
 * @brief Implementation of dmnetif_get_link_status() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, dmnetif_link_status_t, _get_link_status, ( dmnetif_iface_t iface ))
{
    if (!is_valid_iface(iface))
        return dmnetif_link_down;

    dmdrvi_net_link_status_t status = DMDRVI_NET_LINK_DOWN;
    if (Dmod_Ioctl(iface->file, DMDRVI_IOCTL_NET_GET_LINK_STATUS, &status) != 0)
        return dmnetif_link_down;

    return (status == DMDRVI_NET_LINK_UP) ? dmnetif_link_up : dmnetif_link_down;
}

/**
 * @brief Implementation of dmnetif_is_present() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, bool, _is_present, ( dmnetif_iface_t iface ))
{
    return is_valid_iface(iface) && Dmod_FileAvailable(iface->device_path);
}

/* ---- MTU ---- */

/**
 * @brief Implementation of dmnetif_get_mtu() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _get_mtu, ( dmnetif_iface_t iface, uint16_t* mtu ))
{
    if (!is_valid_iface(iface) || mtu == NULL)
        return -EINVAL;

    *mtu = iface->mtu;
    return 0;
}

/**
 * @brief Implementation of dmnetif_set_mtu() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _set_mtu, ( dmnetif_iface_t iface, uint16_t mtu ))
{
    if (!is_valid_iface(iface) || mtu == 0)
        return -EINVAL;

    iface->mtu = mtu;
    return 0;
}

/* ---- MAC address ---- */

/**
 * @brief Implementation of dmnetif_get_mac_address() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _get_mac_address, ( dmnetif_iface_t iface, dmnetif_mac_addr_t* mac ))
{
    if (!is_valid_iface(iface) || mac == NULL)
        return -EINVAL;

    dmdrvi_net_mac_addr_t addr;
    int ret = Dmod_Ioctl(iface->file, DMDRVI_IOCTL_NET_GET_MAC_ADDR, &addr);
    if (ret == 0)
    {
        memcpy(mac->addr, addr.addr, DMNETIF_MAC_ADDR_LEN);
    }
    return ret;
}

/**
 * @brief Implementation of dmnetif_set_mac_address() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _set_mac_address, ( dmnetif_iface_t iface, const dmnetif_mac_addr_t* mac ))
{
    if (!is_valid_iface(iface) || mac == NULL)
        return -EINVAL;

    dmdrvi_net_mac_addr_t addr;
    memcpy(addr.addr, mac->addr, DMNETIF_MAC_ADDR_LEN);
    return Dmod_Ioctl(iface->file, DMDRVI_IOCTL_NET_SET_MAC_ADDR, &addr);
}

/* ---- IP address ---- */

/**
 * @brief Implementation of dmnetif_get_ip_address() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _get_ip_address, ( dmnetif_iface_t iface, dmroute_addr_t* ip ))
{
    if (!is_valid_iface(iface) || ip == NULL)
        return -EINVAL;

    *ip = iface->ip;
    return 0;
}

/**
 * @brief Implementation of dmnetif_set_ip_address() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _set_ip_address, ( dmnetif_iface_t iface, const dmroute_addr_t* ip ))
{
    if (!is_valid_iface(iface) || ip == NULL)
        return -EINVAL;

    if (!is_valid_ip_family(ip->family))
    {
        DMOD_LOG_ERROR("dmnetif_set_ip_address: unrecognized IP family %d\n", (int)ip->family);
        return -EINVAL;
    }

    iface->ip = *ip;

    update_connected_route(iface);
    return 0;
}

/**
 * @brief Implementation of dmnetif_get_netmask() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _get_netmask, ( dmnetif_iface_t iface, dmroute_addr_t* netmask ))
{
    if (!is_valid_iface(iface) || netmask == NULL)
        return -EINVAL;

    *netmask = iface->netmask;
    return 0;
}

/**
 * @brief Implementation of dmnetif_set_netmask() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _set_netmask, ( dmnetif_iface_t iface, const dmroute_addr_t* netmask ))
{
    if (!is_valid_iface(iface) || netmask == NULL)
        return -EINVAL;

    if (!is_valid_ip_family(netmask->family))
    {
        DMOD_LOG_ERROR("dmnetif_set_netmask: unrecognized IP family %d\n", (int)netmask->family);
        return -EINVAL;
    }

    iface->netmask = *netmask;
    return 0;
}

/**
 * @brief Implementation of dmnetif_get_broadcast() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _get_broadcast, ( dmnetif_iface_t iface, dmroute_addr_t* broadcast ))
{
    if (!is_valid_iface(iface) || broadcast == NULL)
        return -EINVAL;

    *broadcast = iface->broadcast;
    return 0;
}

/**
 * @brief Implementation of dmnetif_set_broadcast() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _set_broadcast, ( dmnetif_iface_t iface, const dmroute_addr_t* broadcast ))
{
    if (!is_valid_iface(iface) || broadcast == NULL)
        return -EINVAL;

    if (!is_valid_ip_family(broadcast->family))
    {
        DMOD_LOG_ERROR("dmnetif_set_broadcast: unrecognized IP family %d\n", (int)broadcast->family);
        return -EINVAL;
    }

    iface->broadcast = *broadcast;
    return 0;
}

/* ---- Frame I/O ---- */

/**
 * @brief Implementation of dmnetif_send() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, size_t, _send, ( dmnetif_iface_t iface, const void* frame, size_t length ))
{
    if (!is_valid_iface(iface) || frame == NULL || length == 0 || !iface->up)
        return 0;

    size_t sent = Dmod_FileWrite(frame, 1, length, iface->file);
    if (sent > 0)
    {
        iface->stats.tx_packets++;
        iface->stats.tx_bytes += (uint32_t)sent;
    }
    else
    {
        iface->stats.tx_errors++;
    }
    return sent;
}

/**
 * @brief Implementation of dmnetif_receive() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, size_t, _receive, ( dmnetif_iface_t iface, void* buffer, size_t size ))
{
    if (!is_valid_iface(iface) || buffer == NULL || size == 0 || !iface->up)
        return 0;

    size_t received = Dmod_FileRead(buffer, 1, size, iface->file);
    if (received > 0)
    {
        iface->stats.rx_packets++;
        iface->stats.rx_bytes += (uint32_t)received;
    }
    return received;
}

/* ---- Statistics ---- */

/**
 * @brief Implementation of dmnetif_get_stats() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _get_stats, ( dmnetif_iface_t iface, dmnetif_stats_t* stats ))
{
    if (!is_valid_iface(iface) || stats == NULL)
        return -EINVAL;

    *stats = iface->stats;
    return 0;
}

/* ---- Escape hatch ---- */

/**
 * @brief Implementation of dmnetif_ioctl() - see dmnetif.h
 */
dmod_dmnetif_api_declaration(1.0, int, _ioctl, ( dmnetif_iface_t iface, int command, void* arg ))
{
    if (!is_valid_iface(iface))
        return -EINVAL;

    return Dmod_Ioctl(iface->file, command, arg);
}
