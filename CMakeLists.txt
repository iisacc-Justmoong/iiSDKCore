cmake_minimum_required(VERSION 3.31)
project(iiSDKCore VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

configure_file(src/library.h "${CMAKE_CURRENT_BINARY_DIR}/include/iiSDKCore/library.h" COPYONLY)
add_library(iiSDKCore SHARED src/library.cpp)
add_library(iiSDKCore::iiSDKCore ALIAS iiSDKCore)
target_compile_features(iiSDKCore PUBLIC cxx_std_20)
target_include_directories(iiSDKCore PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/iiSDKCore>)
set_target_properties(iiSDKCore PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    WINDOWS_EXPORT_ALL_SYMBOLS ON)

set(_package_dir "${CMAKE_INSTALL_LIBDIR}/cmake/iiSDKCore")
install(TARGETS iiSDKCore EXPORT iiSDKCoreTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES src/library.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSDKCore)
install(EXPORT iiSDKCoreTargets NAMESPACE iiSDKCore:: DESTINATION ${_package_dir})
configure_package_config_file(cmake/iiSDKCoreConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/iiSDKCoreConfig.cmake"
    INSTALL_DESTINATION ${_package_dir})
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiSDKCoreConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/iiSDKCoreConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/iiSDKCoreConfigVersion.cmake"
    DESTINATION ${_package_dir})

if(BUILD_TESTING)
    add_executable(iiSDKCoreConsumer tests/consumer/main.cpp)
    target_link_libraries(iiSDKCoreConsumer PRIVATE iiSDKCore::iiSDKCore)
    add_test(NAME iiSDKCore.Consumer COMMAND iiSDKCoreConsumer)
    set_tests_properties(iiSDKCore.Consumer PROPERTIES PASS_REGULAR_EXPRESSION "Hello, World!")
endif()
