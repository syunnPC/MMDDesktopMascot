cmake_minimum_required(VERSION 3.27)

project(MMDDesktopMascot LANGUAGES CXX RC)

if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0")
    # bullet3 v3.25 still declares very old policy compatibility.
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MMDM_FETCH_BULLET "Fetch Bullet Physics from the official repository during configure" OFF)
set(MMDM_BULLET_TAG "3.25" CACHE STRING "Bullet git tag to use when fetching from official repository")
set(MMDM_BULLET_SOURCE_DIR "${CMAKE_SOURCE_DIR}/external/bullet3" CACHE PATH "Path to an official bullet3 source tree")

set(BUILD_BULLET2_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_CPU_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_OPENGL3_DEMOS OFF CACHE BOOL "" FORCE)
set(BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
set(INSTALL_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_BULLET3 OFF CACHE BOOL "" FORCE)
set(BUILD_PYBULLET OFF CACHE BOOL "" FORCE)
set(BUILD_ENET OFF CACHE BOOL "" FORCE)
set(BUILD_CLSOCKET OFF CACHE BOOL "" FORCE)
set(USE_GRAPHICAL_BENCHMARK OFF CACHE BOOL "" FORCE)

if(MMDM_FETCH_BULLET)
    include(FetchContent)
    FetchContent_Declare(
        bullet3
        GIT_REPOSITORY https://github.com/bulletphysics/bullet3.git
        GIT_TAG ${MMDM_BULLET_TAG}
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(bullet3)
    set(MMDM_BULLET_INCLUDE_DIR "${bullet3_SOURCE_DIR}/src")
else()
    if(NOT EXISTS "${MMDM_BULLET_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "Bullet source not found at ${MMDM_BULLET_SOURCE_DIR}. Run fetch_bullet.ps1 or set MMDM_FETCH_BULLET=ON.")
    endif()
    add_subdirectory("${MMDM_BULLET_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/bullet3")
    set(MMDM_BULLET_INCLUDE_DIR "${MMDM_BULLET_SOURCE_DIR}/src")
endif()

file(GLOB MMDM_CPP_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/MMDDesktopMascot/*.cpp"
)
file(GLOB MMDM_HEADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/MMDDesktopMascot/*.hpp"
)
file(GLOB MMDM_SHADER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/MMDDesktopMascot/*.hlsl"
)
set_source_files_properties(${MMDM_SHADER_SOURCES} PROPERTIES HEADER_FILE_ONLY TRUE)

add_executable(MMDDesktopMascot WIN32
    ${MMDM_CPP_SOURCES}
    ${MMDM_HEADER_SOURCES}
    ${MMDM_SHADER_SOURCES}
    "${CMAKE_CURRENT_SOURCE_DIR}/MMDDesktopMascot/app.manifest"
    "${CMAKE_CURRENT_SOURCE_DIR}/MMDDesktopMascot/MMDDesktopMascot.rc"
)

set_target_properties(MMDDesktopMascot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
    PDB_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/x64/$<CONFIG>"
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/MMDDesktopMascot" FILES
    ${MMDM_CPP_SOURCES}
    ${MMDM_HEADER_SOURCES}
    ${MMDM_SHADER_SOURCES}
)

target_include_directories(MMDDesktopMascot PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/MMDDesktopMascot"
)
target_include_directories(MMDDesktopMascot SYSTEM PRIVATE
    "${MMDM_BULLET_INCLUDE_DIR}"
)

target_link_libraries(MMDDesktopMascot PRIVATE
    BulletSoftBody
    BulletDynamics
    BulletCollision
    LinearMath
)

target_compile_definitions(MMDDesktopMascot PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    _WIN32_WINNT=0x0A00
)

if(MSVC)
    target_compile_options(MMDDesktopMascot PRIVATE
        /W4
        /permissive-
        /EHsc
        /utf-8
        /MP
        /external:W0
        /external:anglebrackets
    )

    target_link_options(MMDDesktopMascot PRIVATE
        /MANIFEST:EMBED
        "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/MMDDesktopMascot/app.manifest"
    )
endif()

set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT MMDDesktopMascot)
