#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmnetif.h"

static dmnetif_t g_handle = NULL;

void dmod_test_setup(void)
{
    g_handle = dmnetif_create();
}

void dmod_test_teardown(void)
{
    dmnetif_destroy(g_handle);
    g_handle = NULL;
}

DMOD_TEST_STEP(dmnetif_create)
{
    DMOD_TEST_EXPECT_NOT_NULL(g_handle);
}

DMOD_TEST_STEP(dmnetif_is_valid)
{
    DMOD_TEST_EXPECT_TRUE(dmnetif_is_valid(g_handle));
}

DMOD_TEST_STEP(dmnetif_destroy_null)
{
    /* Destroying NULL must not crash. */
    dmnetif_destroy(NULL);
}
