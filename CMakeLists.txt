cmake_minimum_required(VERSION 3.18)
project(mod_audio_stream
        VERSION 1.0.0
        DESCRIPTION "Audio streaming module for FreeSWITCH."
        HOMEPAGE_URL "https://github.com/amigniter/mod_audio_stream")

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_SHARED_LIBRARY_PREFIX "")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g")

#set(ENV{PKG_CONFIG_PATH} "/usr/local/freeswitch/lib/pkgconfig:/usr/local/ssl/lib/pkgconfig/:$ENV{PKG_CONFIG_PATH}")
set(ENV{PKG_CONFIG_PATH} "/usr/local/freeswitch/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

find_package(PkgConfig REQUIRED)
find_package(SpeexDSP REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

pkg_check_modules(FreeSWITCH REQUIRED IMPORTED_TARGET freeswitch)
pkg_get_variable(FS_MOD_DIR freeswitch modulesdir)
message(STATUS "FreeSWITCH modules dir: ${FS_MOD_DIR}")

# Force use of libwsc WebSocket library
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/libs/libwsc/CMakeLists.txt")
    message(STATUS "Using libwsc WebSocket library (forced)")
    add_subdirectory(libs/libwsc)
    set(WEBSOCKET_LIB libwsc)
    set(USE_LIBWSC ON)
    add_definitions(-DUSE_LIBWSC)
else()
    message(FATAL_ERROR "libwsc library not found. Please initialize submodules.")
endif()

add_library(mod_audio_stream SHARED mod_audio_stream.c mod_audio_stream.h audio_streamer_glue.h audio_streamer_glue.cpp buffer/ringbuffer.c base64.cpp)

set_property(TARGET mod_audio_stream PROPERTY POSITION_INDEPENDENT_CODE ON)

# Include libwsc directory
target_include_directories(mod_audio_stream PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/libs/libwsc)

target_link_libraries(mod_audio_stream PRIVATE 
    PkgConfig::FreeSWITCH 
    pthread
    ${WEBSOCKET_LIB}
)

if(CMAKE_BUILD_TYPE MATCHES "Release")
    set_target_properties(${PROJECT_NAME} 
        PROPERTIES 
        LINK_FLAGS_RELEASE "-s")
endif()

set(CPACK_COMPONENTS_ALL ${PROJECT_NAME} changelog.gz copyright)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libc6, libspeexdsp1, openssl, zlib1g, libfreeswitch1")
set(CPACK_PACKAGE_NAME "mod-audio-stream")
set(CMAKE_INSTALL_DOCDIR "share/doc/${CPACK_PACKAGE_NAME}")

add_custom_command(
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog"
    COMMAND gzip -cn9 "${CMAKE_CURRENT_SOURCE_DIR}/debian/changelog" > "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz"
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz"
    COMMENT "Processing copyright file"
)
add_custom_target(changelog_target ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz")

add_custom_command(
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/debian/copyright"
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/debian/copyright" "${CMAKE_CURRENT_BINARY_DIR}/copyright"
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/copyright"
    COMMENT "Generating compressed changelog.gz"
)
add_custom_target(copyright_target ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/copyright")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/copyright"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}"
        COMPONENT copyright
)

install(TARGETS ${PROJECT_NAME}
        COMPONENT ${PROJECT_NAME}
        DESTINATION ${FS_MOD_DIR})

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/changelog.gz"
	DESTINATION "${CMAKE_INSTALL_DOCDIR}"
	COMPONENT changelog.gz
)

message(STATUS "Components to pack: ${CPACK_COMPONENTS_ALL}")

include(Packing)
