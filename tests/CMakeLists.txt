set(DMOD_MODULE_NAME        test_dmnetif)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         1024)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    dmnetif_test.c
)

target_include_directories(${DMOD_MODULE_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

target_link_libraries(${DMOD_MODULE_NAME} dmnetif_if)

# dmroute: dmroute_addr_t (used directly to build test addresses), and
#          verifies the connected-route side effect of
#          dmnetif_set_ip_address() directly against dmroute's own API
#          (dmroute_lookup() etc.) - see the "connected route" test steps
#          in dmnetif_test.c.
dmod_link_modules(${DMOD_MODULE_NAME}
    dmroute
)
