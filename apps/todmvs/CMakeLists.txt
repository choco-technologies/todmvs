# ======================================================================
#               todmvs - page -> .dmvs command-line tool
# ======================================================================
set(DMOD_MODULE_NAME        todmvs)
set(DMOD_AUTHOR_NAME        "Patryk Kubiak")
set(DMOD_STACK_SIZE         8192)
set(DMOD_PRIORITY           0)
set(DMOD_DMR_PATH           ${CMAKE_CURRENT_SOURCE_DIR}/todmvs.dmr)
set(DMOD_MAL_IMPLS          )

dmod_add_executable(${DMOD_MODULE_NAME} ${DMOD_MODULE_VERSION}
    src/todmvs.c
)

target_link_libraries(${DMOD_MODULE_NAME}
    libtodmvs_if
)

# dmvsi.h, which libtodmvs.h includes
dmod_link_modules(${DMOD_MODULE_NAME}
    dmvsi@>=0.5
)
