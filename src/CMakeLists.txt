cmake_minimum_required(VERSION 3.20)
project(ethercat_mqtt_gateway CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(EC_BACKEND "SOEM" CACHE STRING "EtherCAT backend: SOEM or IGH")
set_property(CACHE EC_BACKEND PROPERTY STRINGS SOEM IGH)

find_package(PkgConfig REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(cxxopts CONFIG REQUIRED)
find_package(tinyxml2 CONFIG REQUIRED)
pkg_check_modules(MOSQUITTO REQUIRED IMPORTED_TARGET libmosquitto)

add_executable(ethercat-mqtt-gateway
    src/main.cpp
    src/config.cpp
    src/logging.cpp
    src/ethercat_types.cpp
    src/esi_repository.cpp
    src/slave_device.cpp
    src/mqtt_client.cpp
)

target_include_directories(ethercat-mqtt-gateway PRIVATE include)

target_link_libraries(ethercat-mqtt-gateway PRIVATE
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    cxxopts::cxxopts
    tinyxml2::tinyxml2
    PkgConfig::MOSQUITTO
)

if(EC_BACKEND STREQUAL "SOEM")
    include(FetchContent)
    FetchContent_Declare(
        soem
        GIT_REPOSITORY https://github.com/OpenEtherCATsociety/SOEM.git
        # v2.0.0 rewrote SOEM's API entirely (soem/ec_*.h, no more ec_slave[]/
        # ec_SDOread/ethercat.h umbrella header). Pin to the last classic-API
        # release, which is what soem_backend.cpp is written against.
        GIT_TAG v1.4.0
    )
    FetchContent_MakeAvailable(soem)

    # SOEM 1.4.0 builds with -Werror under its own CMakeLists.txt; newer GCC's
    # stricter -Wstringop-truncation turns a benign strncpy pattern in
    # oshw/linux/oshw.c into a hard build failure. Not our code to fix upstream.
    target_compile_options(soem PRIVATE -Wno-error=stringop-truncation)

    target_sources(ethercat-mqtt-gateway PRIVATE src/backends/soem_backend.cpp)
    target_link_libraries(ethercat-mqtt-gateway PRIVATE soem)
    target_compile_definitions(ethercat-mqtt-gateway PRIVATE EC_BACKEND_SOEM=1)

    # SOEM 1.4.0's CMakeLists.txt uses directory-scoped include_directories()
    # rather than target_include_directories(... PUBLIC ...), so linking
    # against the `soem` target alone doesn't propagate its headers here.
    target_include_directories(ethercat-mqtt-gateway PRIVATE
        ${soem_SOURCE_DIR}/soem
        ${soem_SOURCE_DIR}/osal
        ${soem_SOURCE_DIR}/osal/linux
        ${soem_SOURCE_DIR}/oshw/linux
    )

elseif(EC_BACKEND STREQUAL "IGH")
    find_path(IGH_INCLUDE_DIR ecrt.h
        HINTS /usr/local/include /opt/etherlab/include)
    find_library(IGH_LIBRARY NAMES ethercat
        HINTS /usr/local/lib /opt/etherlab/lib)

    if(NOT IGH_INCLUDE_DIR OR NOT IGH_LIBRARY)
        message(FATAL_ERROR
            "EC_BACKEND=IGH requires a preinstalled IGH (Etherlab) EtherCAT master "
            "(ecrt.h + libethercat) -- it cannot be fetched automatically since it "
            "needs a matching kernel module built for your NIC. Build/install it "
            "from https://gitlab.com/etherlab.org/ethercat first, or pass "
            "-DIGH_INCLUDE_DIR=... -DIGH_LIBRARY=... explicitly.")
    endif()

    add_library(igh_ethercat INTERFACE)
    target_include_directories(igh_ethercat INTERFACE ${IGH_INCLUDE_DIR})
    target_link_libraries(igh_ethercat INTERFACE ${IGH_LIBRARY})

    target_sources(ethercat-mqtt-gateway PRIVATE src/backends/igh_backend.cpp)
    target_link_libraries(ethercat-mqtt-gateway PRIVATE igh_ethercat)
    target_compile_definitions(ethercat-mqtt-gateway PRIVATE EC_BACKEND_IGH=1)

else()
    message(FATAL_ERROR "Unknown EC_BACKEND '${EC_BACKEND}': expected SOEM or IGH")
endif()

message(STATUS "ethercat-mqtt-gateway: EC_BACKEND=${EC_BACKEND}")
