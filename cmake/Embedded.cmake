set(_iisdkcore_embedded_type STATIC)
if(NOT IISDKCORE_EMBEDDED_ONLY AND BUILD_SHARED_LIBS)
    set(_iisdkcore_embedded_type SHARED)
endif()
add_library(iiSDKCoreEmbedded ${_iisdkcore_embedded_type} src/Embedded/Embedded.cpp)
add_library(iiSDKCore::Embedded ALIAS iiSDKCoreEmbedded)
set_target_properties(iiSDKCoreEmbedded PROPERTIES EXPORT_NAME Embedded CXX_STANDARD 23 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS OFF)
if(_iisdkcore_embedded_type STREQUAL "STATIC")
    target_compile_definitions(iiSDKCoreEmbedded PUBLIC IISDKCORE_EMBEDDED_STATIC)
else()
    target_compile_definitions(iiSDKCoreEmbedded PRIVATE IISDKCORE_EMBEDDED_BUILD)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set_target_properties(iiSDKCoreEmbedded PROPERTIES FRAMEWORK TRUE)
    elseif(EMSCRIPTEN)
        target_compile_options(iiSDKCoreEmbedded PRIVATE -fPIC)
        target_link_options(iiSDKCoreEmbedded PRIVATE -sSIDE_MODULE=1)
    elseif(APPLE)
        set_target_properties(iiSDKCoreEmbedded PROPERTIES INSTALL_RPATH "@loader_path")
    elseif(UNIX)
        set_target_properties(iiSDKCoreEmbedded PROPERTIES INSTALL_RPATH "$ORIGIN")
    endif()
endif()
target_compile_features(iiSDKCoreEmbedded PUBLIC cxx_std_23)
configure_file(src/Embedded/Embedded.h "${CMAKE_CURRENT_BINARY_DIR}/include/iiSDKCore/Embedded/Embedded.h" COPYONLY)
target_include_directories(iiSDKCoreEmbedded PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
install(TARGETS iiSDKCoreEmbedded EXPORT iiSDKCoreEmbeddedTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} FRAMEWORK DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(EXPORT iiSDKCoreEmbeddedTargets NAMESPACE iiSDKCore:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSDKCore)
install(FILES src/Embedded/Embedded.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/iiSDKCore/Embedded)
if(BUILD_TESTING)
    add_executable(iiSDKCoreEmbeddedTests tests/embedded.cpp)
    target_link_libraries(iiSDKCoreEmbeddedTests PRIVATE iiSDKCore::Embedded)
    if(BUILD_SHARED_LIBS AND NOT IISDKCORE_EMBEDDED_ONLY AND NOT CMAKE_CROSSCOMPILING)
        add_test(NAME iiSDKCore.Embedded.SharedType COMMAND "${CMAKE_COMMAND}"
            "-DTARGET_TYPE=$<TARGET_PROPERTY:iiSDKCoreEmbedded,TYPE>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_target_type.cmake")
        add_test(NAME iiSDKCore.Embedded.SharedLink COMMAND "${CMAKE_COMMAND}"
            "-DIMAGE=$<TARGET_FILE:iiSDKCoreEmbeddedTests>" "-DEXPECTED=$<TARGET_FILE:iiSDKCoreEmbedded>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/shared_link.cmake")
    endif()
    set_target_properties(iiSDKCoreEmbeddedTests PROPERTIES CXX_STANDARD 23 CXX_EXTENSIONS OFF)
    add_test(NAME iiSDKCore.Embedded COMMAND iiSDKCoreEmbeddedTests)
    set_tests_properties(iiSDKCore.Embedded PROPERTIES TIMEOUT 20)
    add_executable(iiSDKCoreEmbeddedLanguageMacroTests tests/embedded_language_macro.cpp)
    target_link_libraries(iiSDKCoreEmbeddedLanguageMacroTests PRIVATE iiSDKCore::Embedded)
    add_test(NAME iiSDKCore.EmbeddedLanguageMacro COMMAND iiSDKCoreEmbeddedLanguageMacroTests)
    set_tests_properties(iiSDKCore.EmbeddedLanguageMacro PROPERTIES TIMEOUT 20)
endif()
if(IISDKCORE_EMBEDDED_ONLY)
    configure_package_config_file(cmake/EmbeddedConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/iiSDKCoreConfig.cmake"
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSDKCore)
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/iiSDKCoreConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/iiSDKCoreConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/iiSDKCoreConfigVersion.cmake"
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/iiSDKCore)
    install(FILES docs/embedded.md DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSDKCore/docs)
    install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/iiSDKCore)
    iisdkcore_enforce_architecture()
    if(BUILD_TESTING)
        add_test(NAME iiSDKCore.LicenseText COMMAND "${Python3_EXECUTABLE}" -B
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/license_text.py"
            "${CMAKE_CURRENT_SOURCE_DIR}"
            "${CMAKE_CURRENT_BINARY_DIR}/iiSDKCoreConfig.cmake")
        if(NOT CMAKE_CROSSCOMPILING)
            add_test(NAME iiSDKCore.LicenseInstalled COMMAND "${Python3_EXECUTABLE}" -B
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/license_text.py" --install
                "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_COMMAND}" "${CMAKE_INSTALL_DATADIR}")
            set_tests_properties(iiSDKCore.LicenseInstalled PROPERTIES TIMEOUT 120)
        endif()
    endif()
endif()
