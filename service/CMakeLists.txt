set(IDL_FILE    "${CMAKE_CURRENT_SOURCE_DIR}/rpc_interface.idl")
set(MIDL_OUTDIR "${CMAKE_BINARY_DIR}/rpc_generated")
file(MAKE_DIRECTORY "${MIDL_OUTDIR}")

set(RPC_HEADER "${MIDL_OUTDIR}/rpc_interface_h.h")
set(RPC_CLIENT "${MIDL_OUTDIR}/rpc_interface_c.c")
set(RPC_SERVER "${MIDL_OUTDIR}/rpc_interface_s.c")

find_program(MIDL_EXECUTABLE midl HINTS
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
    "C:/Program Files (x86)/Windows Kits/10/bin/x64"
)
if(NOT MIDL_EXECUTABLE)
    set(MIDL_EXECUTABLE midl)
endif()

add_custom_command(
    OUTPUT  "${RPC_HEADER}" "${RPC_CLIENT}" "${RPC_SERVER}"
    COMMAND "${MIDL_EXECUTABLE}"
            /nologo /Oicf /W1 /char signed
            /env    x64
            /out    "${MIDL_OUTDIR}"
            /header rpc_interface_h.h
            /cstub  rpc_interface_c.c
            /sstub  rpc_interface_s.c
            /client stub
            /server stub
            "${IDL_FILE}"
    DEPENDS "${IDL_FILE}"
    COMMENT "MIDL: compiling rpc_interface.idl"
)

set_source_files_properties(
    "${RPC_CLIENT}" "${RPC_SERVER}"
    PROPERTIES GENERATED TRUE LANGUAGE C
)

add_custom_target(RpcStubs ALL
    DEPENDS "${RPC_HEADER}" "${RPC_CLIENT}" "${RPC_SERVER}"
)

add_executable(TrayService service.cpp "${RPC_SERVER}")
add_dependencies(TrayService RpcStubs)
target_include_directories(TrayService PRIVATE "${MIDL_OUTDIR}")
target_link_libraries(TrayService PRIVATE
    rpcrt4
    shell32
    user32
    wtsapi32
    userenv
    advapi32
    winhttp
)
