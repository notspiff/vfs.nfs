cmake_minimum_required(VERSION 3.5)
project(nfs)

include(ExternalProject)
externalproject_add(nfs
                    SOURCE_DIR ${CMAKE_SOURCE_DIR}
                    UPDATE_COMMAND ./bootstrap WORKING_DIRECTORY <SOURCE_DIR>
                    CONFIGURE_COMMAND <SOURCE_DIR>/configure
                      --prefix=${OUTPUT_DIR}
                      --enable-static
                      --disable-shared
                      --disable-werror
                      CFLAGS=-fPIC
                    INSTALL_COMMAND ""
                    BUILD_IN_SOURCE 1)

install(CODE "execute_process(COMMAND make install WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})")
