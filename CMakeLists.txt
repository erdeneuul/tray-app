cmake_minimum_required(VERSION 3.15)
project(TrayApp VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static runtime: /MT in Release, /MTd in Debug.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(TrayApp WIN32 main.cpp)

target_link_libraries(TrayApp PRIVATE
    shell32
    user32
    gdi32
)
