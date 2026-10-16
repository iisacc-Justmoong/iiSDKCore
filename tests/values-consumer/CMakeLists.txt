cmake_minimum_required(VERSION 3.31)
project(iiSDKCoreIndependentValues LANGUAGES CXX)
set(CMAKE_DISABLE_FIND_PACKAGE_iiFileProviderNative TRUE)
find_package(iiSDKCore 0.12 CONFIG REQUIRED COMPONENTS Values)
if(TARGET iiSDKCore::iiSDKCore OR TARGET Threads::Threads
    OR TARGET PkgConfig::IISDKCORE_LINUX_IO
    OR TARGET iiFileProvider::Native
    OR TARGET iiDeviceMultimedia::iiDeviceMultimedia
    OR TARGET PkgConfig::IIDEVICEMULTIMEDIA_LINUX_IO)
    message(FATAL_ERROR "Value-only import pulled runtime dependencies")
endif()
add_executable(iiSDKCoreIndependentValues ../value_modules.cpp)
target_link_libraries(iiSDKCoreIndependentValues PRIVATE iiSDKCore::Values)
set_target_properties(iiSDKCoreIndependentValues PROPERTIES CXX_EXTENSIONS OFF)
enable_testing()
add_test(NAME iiSDKCore.IndependentValues COMMAND iiSDKCoreIndependentValues)

add_executable(iiSDKCoreIndependentSerialization ../serialization.cpp)
target_link_libraries(iiSDKCoreIndependentSerialization PRIVATE iiSDKCore::Values)
add_test(NAME iiSDKCore.IndependentSerialization COMMAND iiSDKCoreIndependentSerialization)

add_executable(iiSDKCoreIndependentSharing ../sharing.cpp)
target_link_libraries(iiSDKCoreIndependentSharing PRIVATE iiSDKCore::Values)
add_test(NAME iiSDKCore.IndependentSharing COMMAND iiSDKCoreIndependentSharing)
