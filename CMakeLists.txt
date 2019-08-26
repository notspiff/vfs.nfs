cmake_minimum_required(VERSION 3.5)
project(vfs.nfs)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR})

find_package(NFS REQUIRED)
find_package(Kodi REQUIRED)
find_package(p8-platform REQUIRED)
find_package(kodiplatform REQUIRED)

include_directories(${KODI_INCLUDE_DIR}/.. # Hack way with "/..", need bigger Kodi cmake rework to match right include ways
                    ${kodiplatform_INCLUDE_DIRS}
                    ${p8-platform_INCLUDE_DIRS}
                    ${NFS_INCLUDE_DIRS})

set(NFS_HEADERS src/NFSConnection.h
                src/NFSFile.h)

set(NFS_SOURCES src/NFSConnection.cpp
                src/NFSFile.cpp)

set(DEPLIBS ${NFS_LIBRARIES} ${p8-platform_LIBRARIES} ${kodiplatform_LIBRARIES})

add_definitions(-D_LARGEFILE64_SOURCE
                -D_FILE_OFFSET_BITS=64
                -D__STDC_FORMAT_MACROS)

if(CORE_SYSTEM_NAME MATCHES "windows")
  add_definitions(-D_CRT_NONSTDC_NO_DEPRECATE)
endif()

build_addon(vfs.nfs NFS DEPLIBS)

include(CPack)
