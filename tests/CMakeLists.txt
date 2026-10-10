set(DMOD_MODULE_NAME        test_libtodmvs)
set(DMOD_MODULE_VERSION     ${PROJECT_VERSION})
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         16384)
set(DMOD_PRIORITY           0)

dmod_add_test(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    libtodmvs_test.c
)

target_link_libraries(${DMOD_MODULE_NAME} libtodmvs_if)

# dmvsi: the documents written; libtodmv: the views assembled, to check them
dmod_link_modules(${DMOD_MODULE_NAME}
    dmvsi@>=0.5
    libtodmv@>=0.4
)

target_compile_definitions(${DMOD_MODULE_NAME} PRIVATE
    LIBTODMVS_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}"
    LIBTODMVS_TEST_FONTS="${CMAKE_CURRENT_SOURCE_DIR}/fonts"
)

# Register the test module with ctest - this replays the same two steps CI
# runs: install the test module's dependencies with dmf-get, then execute it
# through dmod_loader. The test's exit code equals its number of failed steps.
find_program(DMF_GET_EXECUTABLE dmf-get HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)
find_program(DMOD_LOADER_EXECUTABLE dmod_loader HINTS ${DMOD_TOOLS_BIN_DIR} PATHS ENV PATH)

if(DMF_GET_EXECUTABLE AND DMOD_LOADER_EXECUTABLE)
    add_test(
        NAME ${DMOD_MODULE_NAME}
        COMMAND ${CMAKE_COMMAND} -E env DMOD_DMF_DIR=${DMOD_DMF_DIR}
            bash -c "${DMF_GET_EXECUTABLE} install -d ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}-local.dmd -y && ${DMOD_LOADER_EXECUTABLE} ${DMOD_DMF_DIR}/${DMOD_MODULE_NAME}.dmf"
    )
else()
    message(WARNING "dmf-get/dmod_loader not found - 'ctest' will not run ${DMOD_MODULE_NAME}. Build the tools first with -DDMOD_BUILD_TOOLS=ON")
endif()
