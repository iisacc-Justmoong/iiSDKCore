cmake_minimum_required(VERSION 3.31)
project(iiSDKCoreInstalledConsumer LANGUAGES CXX)

find_package(iiSDKCore 0.1 CONFIG REQUIRED)
add_executable(iiSDKCoreInstalledConsumer main.cpp)
target_link_libraries(iiSDKCoreInstalledConsumer PRIVATE iiSDKCore::iiSDKCore)
enable_testing()
add_test(NAME iiSDKCore.InstalledConsumer COMMAND iiSDKCoreInstalledConsumer)
set_tests_properties(iiSDKCore.InstalledConsumer PROPERTIES PASS_REGULAR_EXPRESSION "Hello, World!")
