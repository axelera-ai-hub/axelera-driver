# Build dkms version of pcie-driver

cmake_minimum_required(VERSION 3.10)

project(metis-dkms LANGUAGES C)

set(DRIVER_NAME metis)
set(DRIVER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/..)

message(
  STATUS
    "Building DKMS package for driver: ${DRIVER_NAME} directory: ${DRIVER_SRC_DIR}"
)

# Set DEFAULT_KERNEL if not already set, for local builds
if(NOT DEFAULT_KERNEL)
  execute_process(
    COMMAND uname -r
    OUTPUT_STRIP_TRAILING_WHITESPACE
    OUTPUT_VARIABLE DEFAULT_KERNEL
  )
endif()

# Extract driver version from source file
file(READ ${DRIVER_SRC_DIR}/axl-aipu-version.h AXL_AIPU_VERSION_H)
string(REGEX MATCH "#define DRIVER_VERSION[ \t\r\n]+\"([^\"]*)\"" _
             ${AXL_AIPU_VERSION_H}
)
set(DRIVER_VERSION ${CMAKE_MATCH_1})
set_property(GLOBAL PROPERTY axelera-metis-dkms-version ${DRIVER_VERSION})

# Copy driver source and config to driver directory
set(DRIVER_BASE_DIR
    ${CMAKE_CURRENT_BINARY_DIR}/${DRIVER_NAME}-${DRIVER_VERSION}
)

file(MAKE_DIRECTORY ${DRIVER_BASE_DIR}/driver)
# `conf` directory does not need to go into the final output. Files from this
# directory are configured below Copy driver source files (excluding dkms, conf,
# .git directories)
file(GLOB DRIVER_SOURCE_FILES "${DRIVER_SRC_DIR}/*.c" "${DRIVER_SRC_DIR}/*.h"
     "${DRIVER_SRC_DIR}/Makefile" "${DRIVER_SRC_DIR}/LICENSE"
)
file(COPY ${DRIVER_SOURCE_FILES} DESTINATION ${DRIVER_BASE_DIR}/driver)

# Create extra content for driver
set(DRIVER_PKG_EXTRA_DIR ${DRIVER_BASE_DIR}/extra)

set(DRIVER_RULES_DIR ${DRIVER_PKG_EXTRA_DIR}/etc/udev/rules.d)
file(MAKE_DIRECTORY ${DRIVER_RULES_DIR})
file(COPY ${DRIVER_SRC_DIR}/udev/72-axelera.rules
     DESTINATION ${DRIVER_RULES_DIR}/
)

# Copy common scripts (shared between deb and rpm)
set(DRIVER_SCRIPTS_DIR ${DRIVER_BASE_DIR}/scripts)
file(MAKE_DIRECTORY ${DRIVER_SCRIPTS_DIR})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/common/scripts/
     DESTINATION ${DRIVER_SCRIPTS_DIR}
)

# Configure dkms.conf from common directory
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/common/dkms.in ${DRIVER_BASE_DIR}/dkms.conf @ONLY
)

set(PKG_NAME ${DRIVER_NAME}-dkms)

set(DKMS_TREE_DIR ${CMAKE_CURRENT_BINARY_DIR}/dkms_tree)
# despite being passed as --sourcetree, DRIVER_SRC_TREE_DIR has to be the parent
# of the driver source, because dkms will look for the driver source in
# ${DRIVER_SRC_TREE_DIR}/${DRIVER_NAME}-${DRIVER_VERSION}
get_filename_component(DRIVER_SRC_TREE_DIR ${DRIVER_BASE_DIR} DIRECTORY)

set(DKMS_ARGS
    --verbose
    --no-prepare-kernel
    --no-clean-kernel
    --sourcetree
    ${DRIVER_SRC_TREE_DIR}
    --dkmstree
    ${DKMS_TREE_DIR}
    --kernelsourcedir
    /lib/modules/${DEFAULT_KERNEL}/build
    -m
    ${DRIVER_NAME}
    -v
    ${DRIVER_VERSION}
)

add_custom_target(
  ${PKG_NAME}-build
  COMMAND rm -rf ${DKMS_TREE_DIR}
  COMMAND mkdir -p ${DKMS_TREE_DIR}
  COMMAND fakeroot dkms add ${DKMS_ARGS}
  COMMAND fakeroot dkms build ${DKMS_ARGS}
)

# ============================================================================
# Debian package
# ============================================================================
set(DRIVER_PKG_DEB_DIR ${DRIVER_BASE_DIR}/debian)
file(MAKE_DIRECTORY ${DRIVER_PKG_DEB_DIR})
file(GLOB debian_files ${CMAKE_CURRENT_SOURCE_DIR}/debian/*)
foreach(deb_file IN LISTS debian_files)
  get_filename_component(deb_file_ext ${deb_file} EXT)
  get_filename_component(deb_file_name ${deb_file} NAME_WE)
  if("${deb_file_ext}" STREQUAL ".in")
    configure_file(${deb_file} ${DRIVER_PKG_DEB_DIR}/${deb_file_name} @ONLY)
  else()
    # Nothing to configure. Just copy
    file(COPY ${deb_file} DESTINATION ${DRIVER_PKG_DEB_DIR})
  endif()
endforeach()

set(DEB_PKG ${DRIVER_NAME}-dkms_${DRIVER_VERSION}_all.deb)
set(DEB_PKG_PATH ${CMAKE_CURRENT_BINARY_DIR}/${DEB_PKG})

add_custom_command(
  OUTPUT ${DEB_PKG_PATH}
  COMMAND dpkg-buildpackage -us -uc
  COMMENT "Package ${DRIVER_NAME}-dkms debian package"
  WORKING_DIRECTORY ${DRIVER_BASE_DIR}
)

add_custom_target(
  ${PKG_NAME}-deb-pkg
  DEPENDS ${DEB_PKG_PATH}
          ${PKG_NAME}-build # Theoretically this is not a necessary dependency,
                            # but is considered a `dry-run` to ensure that the
                            # driver can be registered by dkms
)
set_target_properties(
  ${PKG_NAME}-deb-pkg
  PROPERTIES PACKAGE_NAME ${PKG_NAME} #
             PACKAGE_BUILD_PATH ${DEB_PKG_PATH} #
             PACKAGE_DEB ${DEB_PKG} #
             PACKAGE_DIR ${DRIVER_BASE_DIR}
)

# ============================================================================
# RPM package
# ============================================================================
set(RPM_BUILD_DIR ${CMAKE_CURRENT_BINARY_DIR}/rpmbuild)
set(RPM_SPEC_FILE ${RPM_BUILD_DIR}/SPECS/${PKG_NAME}.spec)
set(RPM_PKG ${DRIVER_NAME}-dkms-${DRIVER_VERSION}-1.noarch.rpm)
set(RPM_PKG_PATH ${RPM_BUILD_DIR}/RPMS/noarch/${RPM_PKG})

# Create RPM build directory structure
file(MAKE_DIRECTORY ${RPM_BUILD_DIR}/BUILD)
file(MAKE_DIRECTORY ${RPM_BUILD_DIR}/RPMS)
file(MAKE_DIRECTORY ${RPM_BUILD_DIR}/SOURCES)
file(MAKE_DIRECTORY ${RPM_BUILD_DIR}/SPECS)
file(MAKE_DIRECTORY ${RPM_BUILD_DIR}/SRPMS)

# Configure the spec file
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/rpm/metis-dkms.spec.in ${RPM_SPEC_FILE} @ONLY
)

# Copy sources to RPM SOURCES directory
file(COPY ${DRIVER_BASE_DIR}/driver DESTINATION ${RPM_BUILD_DIR}/SOURCES)
file(COPY ${DRIVER_BASE_DIR}/dkms.conf DESTINATION ${RPM_BUILD_DIR}/SOURCES)
file(COPY ${DRIVER_BASE_DIR}/extra DESTINATION ${RPM_BUILD_DIR}/SOURCES)
file(COPY ${DRIVER_BASE_DIR}/scripts DESTINATION ${RPM_BUILD_DIR}/SOURCES)

add_custom_command(
  OUTPUT ${RPM_PKG_PATH}
  COMMAND rpmbuild -bb --define "_topdir ${RPM_BUILD_DIR}" --define
          "_sourcedir ${RPM_BUILD_DIR}/SOURCES" ${RPM_SPEC_FILE}
  COMMENT "Package ${DRIVER_NAME}-dkms RPM package"
  WORKING_DIRECTORY ${RPM_BUILD_DIR}
)

add_custom_target(
  ${PKG_NAME}-rpm-pkg
  DEPENDS ${RPM_PKG_PATH} ${PKG_NAME}-build # Dry-run to ensure the driver can
                                            # be registered by dkms
)
set_target_properties(
  ${PKG_NAME}-rpm-pkg
  PROPERTIES PACKAGE_NAME ${PKG_NAME} #
             PACKAGE_BUILD_PATH ${RPM_PKG_PATH} #
             PACKAGE_RPM ${RPM_PKG} #
             PACKAGE_DIR ${DRIVER_BASE_DIR}
)
