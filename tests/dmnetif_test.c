/**
 * @file dmnetif_test.c
 * @brief Test steps for dmnetif
 *
 * Exercises the registry (register/unregister/find/count/for_each) against
 * "/dev/null" as a stand-in device file - real enough for Dmod_FileOpen()
 * to succeed without needing an actual dmdrvi-backed network driver.
 * DMDRVI_IOCTL_NET_* driven behavior (up/down/MAC/link status/send/receive)
 * only gets exercised here for the NULL-handle error paths; real coverage
 * of that needs a real network driver behind dmnetif_register().
 *
 * Also covers dmnetif_set_ip_address()'s side effect on dmroute (see the
 * "Connected route" section near the end) - dmnetif calls dmroute_add()/
 * _remove() directly (see update_connected_route() in src/dmnetif.c), so
 * this is checked against dmroute's own API rather than a mock.
 */
#include "dmod_test.h"
#include "dmnetif.h"
#include "dmroute.h"
#include <string.h>

#define TEST_DEVICE_PATH "/null"

/* dmod modules have no libc memcmp() (see dmod/src/module/string.c's
 * minimal replacement set) - a small manual comparison stands in for it. */
static bool bytes_equal(const uint8_t* a, const uint8_t* b, size_t length)
{
    for (size_t i = 0; i < length; i++)
    {
        if (a[i] != b[i])
            return false;
    }
    return true;
}

static dmnetif_iface_t g_iface = NULL;

void dmod_test_setup(void)
{
    g_iface = dmnetif_register("test0", TEST_DEVICE_PATH);
}

void dmod_test_teardown(void)
{
    dmnetif_unregister(g_iface);
    g_iface = NULL;
}

/* ---- Registration ---- */

DMOD_TEST_STEP(register_returns_valid_handle)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_iface);
}

DMOD_TEST_STEP(register_null_name_fails)
{
    DMOD_TEST_EXPECT_NULL(dmnetif_register(NULL, TEST_DEVICE_PATH));
}

DMOD_TEST_STEP(register_null_device_path_fails)
{
    DMOD_TEST_EXPECT_NULL(dmnetif_register("bad0", NULL));
}

DMOD_TEST_STEP(register_nonexistent_device_fails)
{
    /* fopen(..., "r+") fails for a path that doesn't already exist */
    DMOD_TEST_EXPECT_NULL(dmnetif_register("bad1", "/nonexistent/dmnetif-test-path"));
}

DMOD_TEST_STEP(register_duplicate_name_fails)
{
    DMOD_TEST_EXPECT_NULL(dmnetif_register("test0", TEST_DEVICE_PATH));
    /* the earlier registration must still be the one on record */
    DMOD_TEST_EXPECT_EQ(dmnetif_find_by_name("test0"), g_iface);
}

DMOD_TEST_STEP(unregister_removes_interface)
{
    dmnetif_unregister(g_iface);
    DMOD_TEST_EXPECT_NULL(dmnetif_find_by_name("test0"));
    DMOD_TEST_EXPECT_EQ(dmnetif_count(), (size_t)0);
    g_iface = NULL; /* already unregistered - teardown's unregister(NULL) is a no-op */
}

DMOD_TEST_STEP(unregister_null_is_safe)
{
    dmnetif_unregister(NULL);
}

/* ---- Lookup / enumeration ---- */

DMOD_TEST_STEP(find_by_name_finds_registered_interface)
{
    DMOD_TEST_EXPECT_EQ(dmnetif_find_by_name("test0"), g_iface);
}

DMOD_TEST_STEP(find_by_name_unknown_returns_null)
{
    DMOD_TEST_EXPECT_NULL(dmnetif_find_by_name("does-not-exist"));
}

DMOD_TEST_STEP(count_reflects_registered_interface)
{
    DMOD_TEST_EXPECT_EQ(dmnetif_count(), (size_t)1);
}

DMOD_TEST_STEP(get_name_returns_registered_name)
{
    const char* name = dmnetif_get_name(g_iface);
    DMOD_TEST_EXPECT_NOT_NULL(name);
    if (name != NULL)
    {
        DMOD_TEST_EXPECT_EQ(strcmp(name, "test0"), 0);
    }
}

DMOD_TEST_STEP(get_name_invalid_handle_returns_null)
{
    DMOD_TEST_EXPECT_NULL(dmnetif_get_name(NULL));
}

typedef struct
{
    int             visit_count;
    dmnetif_iface_t last_seen;
} for_each_capture_t;

static bool capture_iface(dmnetif_iface_t iface, void* user_data)
{
    for_each_capture_t* capture = (for_each_capture_t*)user_data;
    capture->visit_count++;
    capture->last_seen = iface;
    return true;
}

DMOD_TEST_STEP(for_each_visits_registered_interface_exactly_once)
{
    for_each_capture_t capture = { 0 };
    dmnetif_for_each(capture_iface, &capture);
    DMOD_TEST_EXPECT_EQ(capture.visit_count, 1);
    DMOD_TEST_EXPECT_EQ(capture.last_seen, g_iface);
}

static bool stop_after_first(dmnetif_iface_t iface, void* user_data)
{
    (void)iface;
    int* visit_count = (int*)user_data;
    (*visit_count)++;
    return false;
}

DMOD_TEST_STEP(for_each_stops_when_callback_returns_false)
{
    int visit_count = 0;
    dmnetif_for_each(stop_after_first, &visit_count);
    DMOD_TEST_EXPECT_EQ(visit_count, 1);
}

/* ---- State control (fresh, never-started interface) ---- */

DMOD_TEST_STEP(is_up_starts_false)
{
    DMOD_TEST_EXPECT_FALSE(dmnetif_is_up(g_iface));
}

DMOD_TEST_STEP(is_present_true_for_registered_interface)
{
    DMOD_TEST_EXPECT_TRUE(dmnetif_is_present(g_iface));
}

DMOD_TEST_STEP(is_present_invalid_handle_returns_false)
{
    DMOD_TEST_EXPECT_FALSE(dmnetif_is_present(NULL));
}

/* ---- IP address ---- */

DMOD_TEST_STEP(ip_address_starts_unassigned)
{
    dmroute_addr_t ip = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_ip_address(g_iface, &ip), 0);
    DMOD_TEST_EXPECT_EQ(ip.family, dmroute_family_none);
}

DMOD_TEST_STEP(set_and_get_ipv4_address_roundtrips)
{
    dmroute_addr_t set_ip = { 0 };
    set_ip.family = dmroute_family_v4;
    set_ip.addr.v4[0] = 192;
    set_ip.addr.v4[1] = 168;
    set_ip.addr.v4[2] = 1;
    set_ip.addr.v4[3] = 42;
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &set_ip), 0);

    dmroute_addr_t got_ip = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_ip_address(g_iface, &got_ip), 0);
    DMOD_TEST_EXPECT_EQ(got_ip.family, dmroute_family_v4);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(got_ip.addr.v4, set_ip.addr.v4, DMROUTE_IPV4_ADDR_LEN));
}

DMOD_TEST_STEP(set_and_get_ipv6_address_roundtrips)
{
    dmroute_addr_t set_ip = { 0 };
    set_ip.family = dmroute_family_v6;
    for (int i = 0; i < DMROUTE_IPV6_ADDR_LEN; i++)
    {
        set_ip.addr.v6[i] = (uint8_t)(i + 1);
    }
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &set_ip), 0);

    dmroute_addr_t got_ip = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_ip_address(g_iface, &got_ip), 0);
    DMOD_TEST_EXPECT_EQ(got_ip.family, dmroute_family_v6);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(got_ip.addr.v6, set_ip.addr.v6, DMROUTE_IPV6_ADDR_LEN));
}

DMOD_TEST_STEP(set_ip_address_none_clears_it)
{
    dmroute_addr_t v4_ip = { 0 };
    v4_ip.family = dmroute_family_v4;
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &v4_ip), 0);

    dmroute_addr_t clear_ip = { 0 };
    clear_ip.family = dmroute_family_none;
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &clear_ip), 0);

    dmroute_addr_t got_ip = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_ip_address(g_iface, &got_ip), 0);
    DMOD_TEST_EXPECT_EQ(got_ip.family, dmroute_family_none);
}

DMOD_TEST_STEP(set_ip_address_invalid_family_fails)
{
    dmroute_addr_t bad_ip = { 0 };
    bad_ip.family = (dmroute_family_t)7;
    DMOD_TEST_EXPECT_TRUE(dmnetif_set_ip_address(g_iface, &bad_ip) < 0);
}

/* ---- Netmask ---- */

DMOD_TEST_STEP(netmask_starts_unassigned)
{
    dmroute_addr_t netmask = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_netmask(g_iface, &netmask), 0);
    DMOD_TEST_EXPECT_EQ(netmask.family, dmroute_family_none);
}

DMOD_TEST_STEP(set_and_get_ipv4_netmask_roundtrips)
{
    dmroute_addr_t set_netmask = { 0 };
    set_netmask.family = dmroute_family_v4;
    set_netmask.addr.v4[0] = 255;
    set_netmask.addr.v4[1] = 255;
    set_netmask.addr.v4[2] = 255;
    set_netmask.addr.v4[3] = 0;
    DMOD_TEST_EXPECT_EQ(dmnetif_set_netmask(g_iface, &set_netmask), 0);

    dmroute_addr_t got_netmask = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_netmask(g_iface, &got_netmask), 0);
    DMOD_TEST_EXPECT_EQ(got_netmask.family, dmroute_family_v4);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(got_netmask.addr.v4, set_netmask.addr.v4, DMROUTE_IPV4_ADDR_LEN));
}

DMOD_TEST_STEP(set_and_get_ipv6_netmask_roundtrips)
{
    dmroute_addr_t set_netmask = { 0 };
    set_netmask.family = dmroute_family_v6;
    for (int i = 0; i < DMROUTE_IPV6_ADDR_LEN; i++)
    {
        set_netmask.addr.v6[i] = 0xFF;
    }
    DMOD_TEST_EXPECT_EQ(dmnetif_set_netmask(g_iface, &set_netmask), 0);

    dmroute_addr_t got_netmask = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_netmask(g_iface, &got_netmask), 0);
    DMOD_TEST_EXPECT_EQ(got_netmask.family, dmroute_family_v6);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(got_netmask.addr.v6, set_netmask.addr.v6, DMROUTE_IPV6_ADDR_LEN));
}

DMOD_TEST_STEP(set_netmask_none_clears_it)
{
    dmroute_addr_t v4_netmask = { 0 };
    v4_netmask.family = dmroute_family_v4;
    DMOD_TEST_EXPECT_EQ(dmnetif_set_netmask(g_iface, &v4_netmask), 0);

    dmroute_addr_t clear_netmask = { 0 };
    clear_netmask.family = dmroute_family_none;
    DMOD_TEST_EXPECT_EQ(dmnetif_set_netmask(g_iface, &clear_netmask), 0);

    dmroute_addr_t got_netmask = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_netmask(g_iface, &got_netmask), 0);
    DMOD_TEST_EXPECT_EQ(got_netmask.family, dmroute_family_none);
}

DMOD_TEST_STEP(set_netmask_invalid_family_fails)
{
    dmroute_addr_t bad_netmask = { 0 };
    bad_netmask.family = (dmroute_family_t)7;
    DMOD_TEST_EXPECT_TRUE(dmnetif_set_netmask(g_iface, &bad_netmask) < 0);
}

DMOD_TEST_STEP(invalid_handle_netmask_returns_error)
{
    dmroute_addr_t netmask = { 0 };
    DMOD_TEST_EXPECT_TRUE(dmnetif_get_netmask(NULL, &netmask) < 0);
    DMOD_TEST_EXPECT_TRUE(dmnetif_set_netmask(NULL, &netmask) < 0);
}

/* ---- Broadcast address ---- */

DMOD_TEST_STEP(broadcast_starts_unassigned)
{
    dmroute_addr_t broadcast = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_broadcast(g_iface, &broadcast), 0);
    DMOD_TEST_EXPECT_EQ(broadcast.family, dmroute_family_none);
}

DMOD_TEST_STEP(set_and_get_ipv4_broadcast_roundtrips)
{
    dmroute_addr_t set_broadcast = { 0 };
    set_broadcast.family = dmroute_family_v4;
    set_broadcast.addr.v4[0] = 192;
    set_broadcast.addr.v4[1] = 168;
    set_broadcast.addr.v4[2] = 1;
    set_broadcast.addr.v4[3] = 255;
    DMOD_TEST_EXPECT_EQ(dmnetif_set_broadcast(g_iface, &set_broadcast), 0);

    dmroute_addr_t got_broadcast = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_broadcast(g_iface, &got_broadcast), 0);
    DMOD_TEST_EXPECT_EQ(got_broadcast.family, dmroute_family_v4);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(got_broadcast.addr.v4, set_broadcast.addr.v4, DMROUTE_IPV4_ADDR_LEN));
}

DMOD_TEST_STEP(set_and_get_ipv6_broadcast_roundtrips)
{
    dmroute_addr_t set_broadcast = { 0 };
    set_broadcast.family = dmroute_family_v6;
    for (int i = 0; i < DMROUTE_IPV6_ADDR_LEN; i++)
    {
        set_broadcast.addr.v6[i] = 0xAA;
    }
    DMOD_TEST_EXPECT_EQ(dmnetif_set_broadcast(g_iface, &set_broadcast), 0);

    dmroute_addr_t got_broadcast = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_broadcast(g_iface, &got_broadcast), 0);
    DMOD_TEST_EXPECT_EQ(got_broadcast.family, dmroute_family_v6);
    DMOD_TEST_EXPECT_TRUE(bytes_equal(got_broadcast.addr.v6, set_broadcast.addr.v6, DMROUTE_IPV6_ADDR_LEN));
}

DMOD_TEST_STEP(set_broadcast_none_clears_it)
{
    dmroute_addr_t v4_broadcast = { 0 };
    v4_broadcast.family = dmroute_family_v4;
    DMOD_TEST_EXPECT_EQ(dmnetif_set_broadcast(g_iface, &v4_broadcast), 0);

    dmroute_addr_t clear_broadcast = { 0 };
    clear_broadcast.family = dmroute_family_none;
    DMOD_TEST_EXPECT_EQ(dmnetif_set_broadcast(g_iface, &clear_broadcast), 0);

    dmroute_addr_t got_broadcast = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_get_broadcast(g_iface, &got_broadcast), 0);
    DMOD_TEST_EXPECT_EQ(got_broadcast.family, dmroute_family_none);
}

DMOD_TEST_STEP(set_broadcast_invalid_family_fails)
{
    dmroute_addr_t bad_broadcast = { 0 };
    bad_broadcast.family = (dmroute_family_t)7;
    DMOD_TEST_EXPECT_TRUE(dmnetif_set_broadcast(g_iface, &bad_broadcast) < 0);
}

DMOD_TEST_STEP(invalid_handle_broadcast_returns_error)
{
    dmroute_addr_t broadcast = { 0 };
    DMOD_TEST_EXPECT_TRUE(dmnetif_get_broadcast(NULL, &broadcast) < 0);
    DMOD_TEST_EXPECT_TRUE(dmnetif_set_broadcast(NULL, &broadcast) < 0);
}

/* ---- MTU ---- */

DMOD_TEST_STEP(mtu_starts_at_default)
{
    uint16_t mtu = 0;
    DMOD_TEST_EXPECT_EQ(dmnetif_get_mtu(g_iface, &mtu), 0);
    DMOD_TEST_EXPECT_EQ(mtu, (uint16_t)DMNETIF_DEFAULT_MTU);
}

DMOD_TEST_STEP(set_and_get_mtu_roundtrips)
{
    DMOD_TEST_EXPECT_EQ(dmnetif_set_mtu(g_iface, 1400), 0);

    uint16_t mtu = 0;
    DMOD_TEST_EXPECT_EQ(dmnetif_get_mtu(g_iface, &mtu), 0);
    DMOD_TEST_EXPECT_EQ(mtu, (uint16_t)1400);
}

DMOD_TEST_STEP(set_mtu_zero_fails)
{
    DMOD_TEST_EXPECT_TRUE(dmnetif_set_mtu(g_iface, 0) < 0);
}

/* ---- Statistics ---- */

DMOD_TEST_STEP(stats_start_at_zero)
{
    dmnetif_stats_t stats;
    memset(&stats, 0xFF, sizeof(stats)); /* poison, so a no-op get would be caught */
    DMOD_TEST_EXPECT_EQ(dmnetif_get_stats(g_iface, &stats), 0);
    DMOD_TEST_EXPECT_EQ(stats.rx_packets, (uint32_t)0);
    DMOD_TEST_EXPECT_EQ(stats.rx_bytes, (uint32_t)0);
    DMOD_TEST_EXPECT_EQ(stats.tx_packets, (uint32_t)0);
    DMOD_TEST_EXPECT_EQ(stats.tx_bytes, (uint32_t)0);
    DMOD_TEST_EXPECT_EQ(stats.tx_errors, (uint32_t)0);
}

DMOD_TEST_STEP(stats_unaffected_by_send_receive_while_down)
{
    /* Real counter increments need a real driver behind an "up" interface
     * (dmnetif_up() fails with -ENOSYS against this stand-in device, per
     * not_started_interface_send_receive_return_zero above) - what's
     * testable here is that a no-op send/receive on a down interface
     * doesn't touch the counters either. */
    uint8_t buffer[8] = { 0 };
    dmnetif_send(g_iface, buffer, sizeof(buffer));
    dmnetif_receive(g_iface, buffer, sizeof(buffer));

    dmnetif_stats_t stats;
    DMOD_TEST_EXPECT_EQ(dmnetif_get_stats(g_iface, &stats), 0);
    DMOD_TEST_EXPECT_EQ(stats.rx_packets, (uint32_t)0);
    DMOD_TEST_EXPECT_EQ(stats.tx_packets, (uint32_t)0);
}

DMOD_TEST_STEP(invalid_handle_mtu_returns_error)
{
    uint16_t mtu = 0;
    DMOD_TEST_EXPECT_TRUE(dmnetif_get_mtu(NULL, &mtu) < 0);
    DMOD_TEST_EXPECT_TRUE(dmnetif_set_mtu(NULL, 1500) < 0);
}

DMOD_TEST_STEP(invalid_handle_stats_returns_error)
{
    dmnetif_stats_t stats;
    DMOD_TEST_EXPECT_TRUE(dmnetif_get_stats(NULL, &stats) < 0);
}

/* ---- Invalid-handle error paths ---- */

DMOD_TEST_STEP(invalid_handle_state_control_returns_errors)
{
    DMOD_TEST_EXPECT_TRUE(dmnetif_up(NULL) < 0);
    DMOD_TEST_EXPECT_TRUE(dmnetif_down(NULL) < 0);
    DMOD_TEST_EXPECT_FALSE(dmnetif_is_up(NULL));
    DMOD_TEST_EXPECT_EQ(dmnetif_get_link_status(NULL), dmnetif_link_down);
}

DMOD_TEST_STEP(invalid_handle_mac_address_returns_error)
{
    dmnetif_mac_addr_t mac = { 0 };
    DMOD_TEST_EXPECT_TRUE(dmnetif_get_mac_address(NULL, &mac) < 0);
    DMOD_TEST_EXPECT_TRUE(dmnetif_set_mac_address(NULL, &mac) < 0);
}

DMOD_TEST_STEP(invalid_handle_ip_address_returns_error)
{
    dmroute_addr_t ip = { 0 };
    DMOD_TEST_EXPECT_TRUE(dmnetif_get_ip_address(NULL, &ip) < 0);
    DMOD_TEST_EXPECT_TRUE(dmnetif_set_ip_address(NULL, &ip) < 0);
}

DMOD_TEST_STEP(invalid_handle_send_receive_return_zero)
{
    uint8_t buffer[8];
    DMOD_TEST_EXPECT_EQ(dmnetif_send(NULL, buffer, sizeof(buffer)), (size_t)0);
    DMOD_TEST_EXPECT_EQ(dmnetif_receive(NULL, buffer, sizeof(buffer)), (size_t)0);
}

DMOD_TEST_STEP(not_started_interface_send_receive_return_zero)
{
    uint8_t buffer[8] = { 0 };
    DMOD_TEST_EXPECT_EQ(dmnetif_send(g_iface, buffer, sizeof(buffer)), (size_t)0);
    DMOD_TEST_EXPECT_EQ(dmnetif_receive(g_iface, buffer, sizeof(buffer)), (size_t)0);
}

DMOD_TEST_STEP(invalid_handle_ioctl_returns_error)
{
    DMOD_TEST_EXPECT_TRUE(dmnetif_ioctl(NULL, 0, NULL) < 0);
}

/* ---- Connected route (dmroute integration) ---- */

static dmroute_addr_t make_v4(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    dmroute_addr_t ip = { 0 };
    ip.family = dmroute_family_v4;
    ip.addr.v4[0] = a;
    ip.addr.v4[1] = b;
    ip.addr.v4[2] = c;
    ip.addr.v4[3] = d;
    return ip;
}

DMOD_TEST_STEP(assigning_ip_without_netmask_adds_host_route)
{
    dmroute_addr_t ip = make_v4(10, 5, 5, 5);
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &ip), 0);

    /* Exact address matches (host route) ... */
    DMOD_TEST_EXPECT_NOT_NULL(dmroute_lookup(&ip));

    /* ... but a different host in the same /24 does not, since no netmask
     * was ever assigned. */
    dmroute_addr_t neighbor = make_v4(10, 5, 5, 6);
    DMOD_TEST_EXPECT_NULL(dmroute_lookup(&neighbor));
}

DMOD_TEST_STEP(assigning_ip_with_netmask_adds_subnet_route)
{
    dmroute_addr_t netmask = make_v4(255, 255, 255, 0);
    DMOD_TEST_EXPECT_EQ(dmnetif_set_netmask(g_iface, &netmask), 0);

    dmroute_addr_t ip = make_v4(192, 168, 1, 42);
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &ip), 0);

    dmroute_addr_t other_host = make_v4(192, 168, 1, 200);
    dmroute_route_t route = dmroute_lookup(&other_host);
    DMOD_TEST_EXPECT_NOT_NULL(route);
    if (route != NULL)
    {
        DMOD_TEST_EXPECT_EQ(dmroute_get_origin(route), dmroute_origin_connected);

        const char* iface_name = dmroute_get_iface_name(route);
        DMOD_TEST_EXPECT_NOT_NULL(iface_name);
        if (iface_name != NULL)
        {
            DMOD_TEST_EXPECT_EQ(strcmp(iface_name, "test0"), 0);
        }
    }
}

DMOD_TEST_STEP(clearing_ip_removes_connected_route)
{
    dmroute_addr_t ip = make_v4(10, 8, 8, 8);
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &ip), 0);
    DMOD_TEST_EXPECT_NOT_NULL(dmroute_lookup(&ip));

    dmroute_addr_t none = { 0 };
    none.family = dmroute_family_none;
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &none), 0);

    DMOD_TEST_EXPECT_NULL(dmroute_lookup(&ip));
}

DMOD_TEST_STEP(reassigning_ip_replaces_previous_connected_route)
{
    dmroute_addr_t first_ip = make_v4(10, 1, 1, 1);
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &first_ip), 0);
    DMOD_TEST_EXPECT_NOT_NULL(dmroute_lookup(&first_ip));

    size_t count_after_first = dmroute_count();

    dmroute_addr_t second_ip = make_v4(10, 2, 2, 2);
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &second_ip), 0);

    /* the stale route to the old address is gone, the new one exists, and
     * the table didn't grow - the old connected route was replaced, not
     * left behind alongside the new one. */
    DMOD_TEST_EXPECT_NULL(dmroute_lookup(&first_ip));
    DMOD_TEST_EXPECT_NOT_NULL(dmroute_lookup(&second_ip));
    DMOD_TEST_EXPECT_EQ(dmroute_count(), count_after_first);
}

DMOD_TEST_STEP(unregister_removes_connected_route)
{
    dmroute_addr_t ip = make_v4(10, 9, 9, 9);
    DMOD_TEST_EXPECT_EQ(dmnetif_set_ip_address(g_iface, &ip), 0);
    DMOD_TEST_EXPECT_NOT_NULL(dmroute_lookup(&ip));

    dmnetif_unregister(g_iface);
    g_iface = NULL; /* already unregistered - teardown's unregister(NULL) is a no-op */

    DMOD_TEST_EXPECT_NULL(dmroute_lookup(&ip));
}
