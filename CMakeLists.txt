project(vfs.nfs)

cmake_minimum_required(VERSION 2.6)

list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR})

enable_language(CXX)

find_package(NFS REQUIRED)
find_package(kodi REQUIRED)
find_package(kodiplatform REQUIRED)

include_directories(${KODI_INCLUDE_DIR}
                    ${KODIPLATFORM_INCLUDE_DIRS}
                    ${NFS_INCLUDE_DIRS})

set(NFS_SOURCES src/NFSConnection.cpp
                src/NFSFile.cpp)

set(DEPLIBS ${NFS_LIBRARIES})

add_definitions(-D_LARGEFILE64_SOURCE
                -D_FILE_OFFSET_BITS=64
                -D__STDC_FORMAT_MACROS)

build_addon(vfs.nfs NFS DEPLIBS)
