set(RPC_CLIENT "${CMAKE_BINARY_DIR}/rpc_generated/rpc_interface_c.c")

set_source_files_properties("${RPC_CLIENT}" PROPERTIES GENERATED TRUE LANGUAGE C)

add_executable(TrayApp WIN32 main.cpp "${RPC_CLIENT}")
add_dependencies(TrayApp RpcStubs)
target_include_directories(TrayApp PRIVATE "${CMAKE_BINARY_DIR}/rpc_generated")
target_link_libraries(TrayApp PRIVATE
    rpcrt4
    shell32
    user32
    gdi32
    advapi32
)
