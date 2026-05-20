cmake_minimum_required(VERSION 3.15)
project(TrayApp VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static runtime
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# ---------------------------------------------------------------------------
# MIDL — compile TrayService.idl into client and server stubs
# ---------------------------------------------------------------------------
set(IDL_FILE    "${CMAKE_SOURCE_DIR}/rpc/TrayService.idl")
set(MIDL_OUTDIR "${CMAKE_BINARY_DIR}/rpc_generated")
file(MAKE_DIRECTORY "${MIDL_OUTDIR}")

set(RPC_HEADER  "${MIDL_OUTDIR}/TrayService_h.h")
set(RPC_CLIENT  "${MIDL_OUTDIR}/TrayService_c.c")
set(RPC_SERVER  "${MIDL_OUTDIR}/TrayService_s.c")
set(RPC_IID     "${MIDL_OUTDIR}/TrayService_i.c")

find_program(MIDL_EXECUTABLE midl HINTS
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/x64"
)

if(NOT MIDL_EXECUTABLE)
    # Fallback: assume midl is on PATH
    set(MIDL_EXECUTABLE midl)
endif()

add_custom_command(
    OUTPUT  "${RPC_HEADER}" "${RPC_CLIENT}" "${RPC_SERVER}" "${RPC_IID}"
    COMMAND "${MIDL_EXECUTABLE}"
            /nologo /Oicf /W1 /char signed
            /env   win64
            /out   "${MIDL_OUTDIR}"
            /header "TrayService_h.h"
            /iid    "TrayService_i.c"
            /client stub
            /server stub
            "${IDL_FILE}"
    DEPENDS "${IDL_FILE}"
    COMMENT "MIDL: compiling TrayService.idl"
)

add_custom_target(RpcStubs DEPENDS "${RPC_HEADER}" "${RPC_CLIENT}" "${RPC_SERVER}" "${RPC_IID}")

# ---------------------------------------------------------------------------
# TrayApp.exe
# ---------------------------------------------------------------------------
add_executable(TrayApp WIN32
    app/main.cpp
    "${RPC_CLIENT}"
)
add_dependencies(TrayApp RpcStubs)
target_include_directories(TrayApp PRIVATE "${MIDL_OUTDIR}")
target_link_libraries(TrayApp PRIVATE
    shell32
    user32
    gdi32
    rpcrt4
    advapi32
)

# ---------------------------------------------------------------------------
# TrayService.exe
# ---------------------------------------------------------------------------
add_executable(TrayService
    service/service.cpp
    "${RPC_SERVER}"
    "${RPC_IID}"
)
add_dependencies(TrayService RpcStubs)
target_include_directories(TrayService PRIVATE "${MIDL_OUTDIR}")
target_link_libraries(TrayService PRIVATE
    advapi32
    rpcrt4
    wtsapi32
    userenv
)
