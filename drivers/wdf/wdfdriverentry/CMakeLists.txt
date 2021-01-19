
add_library(wdfdriverentry STATIC wdfdriverentry.c)
add_dependencies(wdfdriverentry xdk)

target_compile_definitions(wdfdriverentry
    PUBLIC WDF_DEVICE_NO_WDMSEC_H _WIN32_WINNT=0x603 NTDDI_VERSION=0x06030000
)

target_include_directories(wdfdriverentry
    PUBLIC ${REACTOS_SOURCE_DIR}/sdk/include/wdf/kmdf/1.17
)
