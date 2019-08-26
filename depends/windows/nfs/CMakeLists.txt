cmake_minimum_required(VERSION 3.5)

project(libnfs LANGUAGES C VERSION 2.0.0)

add_library(libnfs
  lib/init.c
  lib/libnfs.c
  lib/libnfs-sync.c
  lib/libnfs-zdr.c
  lib/pdu.c
  lib/socket.c
  include/nfsc/libnfs.h
  include/nfsc/libnfs-raw.h
  include/nfsc/libnfs-zdr.h
  include/libnfs-private.h
  mount/libnfs-raw-mount.h
  mount/libnfs-raw-mount.c
  mount/mount.c
  nfs/libnfs-raw-nfs.h
  nfs/libnfs-raw-nfs.c
  nfs/nfs.c
  nlm/libnfs-raw-nlm.c
  nlm/libnfs-raw-nlm.h
  nlm/nlm.c
  nsm/libnfs-raw-nsm.c
  nsm/libnfs-raw-nsm.h
  nsm/nsm.c
  portmap/libnfs-raw-portmap.h
  portmap/libnfs-raw-portmap.c
  portmap/portmap.c
  rquota/libnfs-raw-rquota.h
  rquota/libnfs-raw-rquota.c
  rquota/rquota.c
  win32/win32_compat.h
  win32/win32_compat.c
  win32/win32_errnowrapper.h
)
target_include_directories(
  libnfs PRIVATE
  $<BUILD_INTERFACE:include/;include/nfsc/;win32/;rquota/;nsm/;nlm/;nfs/;mount/;lib/;portmap/>
  INTERFACE
  $<INSTALL_INTERFACE:include/nfsc>
)

target_compile_definitions(libnfs PRIVATE _U_= _CRT_NONSTDC_NO_DEPRECATE)
target_link_libraries(libnfs PRIVATE ws2_32.lib)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/libnfs-config-version.cmake
  VERSION ${libnfs_VERSION}
  COMPATIBILITY AnyNewerVersion
)

install(TARGETS libnfs EXPORT libnfs
  RUNTIME DESTINATION bin
  ARCHIVE DESTINATION lib
  LIBRARY DESTINATION lib)


install(DIRECTORY
  include/nfsc
  DESTINATION include
)

install(FILES
  mount/libnfs-raw-mount.h
  nfs/libnfs-raw-nfs.h
  nlm/libnfs-raw-nlm.h
  nsm/libnfs-raw-nsm.h
  portmap/libnfs-raw-portmap.h
  rquota/libnfs-raw-rquota.h
  DESTINATION include/nfsc
)


install(EXPORT libnfs
  FILE
    libnfs.cmake
  NAMESPACE
    libnfs::
  DESTINATION
    lib/cmake/libnfs
)
install(
  FILES
    cmake/libnfs-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/libnfs-config-version.cmake
  DESTINATION
    lib/cmake/libnfs
)

if(MSVC AND BUILD_SHARED_LIBS)
  install(FILES
    $<TARGET_PDB_FILE:libnfs>
    DESTINATION lib
  )
endif()
