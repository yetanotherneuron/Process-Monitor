cmake_minimum_required(VERSION 3.20)

project(ProcessMonitor VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /EHsc)
endif()

add_executable(process_monitor
    source/main.cpp
    source/process.cpp
    source/utilities.cpp
    resources/app.rc
)
set_source_files_properties(resources/app.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/resources/app.ico"
)

target_include_directories(process_monitor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/resources
)
target_link_libraries(process_monitor PRIVATE psapi)
target_compile_definitions(process_monitor PRIVATE UNICODE _UNICODE NOMINMAX _WIN32_WINNT=0x0A00)
set_target_properties(process_monitor PROPERTIES OUTPUT_NAME ProcessMonitor)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/process_monitor_tests.cpp")
    add_executable(process_monitor_tests
        tests/process_monitor_tests.cpp
        source/process.cpp
        source/utilities.cpp
    )

    target_include_directories(process_monitor_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)
    target_link_libraries(process_monitor_tests PRIVATE psapi)

    enable_testing()
    add_test(NAME process_monitor_tests COMMAND process_monitor_tests)
endif()
