
cmake_minimum_required(VERSION 3.20)

project(heterocompute_backend VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Boost 1.74 REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(Threads REQUIRED)

add_compile_definitions(
        BOOST_ERROR_CODE_HEADER_ONLY
        BOOST_SYSTEM_NO_DEPRECATED
)

add_executable(hetero_backend
        src/main.cpp
        src/http_server.cpp
        src/simulation_backend.cpp
        src/controller.cpp
)

target_include_directories(hetero_backend PRIVATE include)

target_link_libraries(hetero_backend PRIVATE
        Boost::headers
        nlohmann_json::nlohmann_json
        Threads::Threads
)

target_compile_options(hetero_backend PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
)

include(CTest)

if(BUILD_TESTING)
    find_package(GTest REQUIRED)

    add_executable(test_scheduler
            tests/test_scheduler.cpp
            src/simulation_backend.cpp
    )

    target_include_directories(test_scheduler PRIVATE include)

    target_link_libraries(test_scheduler PRIVATE
            nlohmann_json::nlohmann_json
            Threads::Threads
            GTest::gtest_main
    )

    target_compile_options(test_scheduler PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
    )

    include(GoogleTest)
    gtest_discover_tests(test_scheduler)
endif()
