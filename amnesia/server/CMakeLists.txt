cmake_minimum_required(VERSION 3.16)
project(AmnesiaDedicatedConfiguration LANGUAGES CXX)

add_library(DedicatedConfig STATIC src/DedicatedConfig.cpp)
target_include_directories(DedicatedConfig PUBLIC src)
target_compile_features(DedicatedConfig PUBLIC cxx_std_14)
set_target_properties(DedicatedConfig PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(DedicatedConfig PRIVATE /W4 /WX)
else()
    target_compile_options(DedicatedConfig PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# No HPL2/ENet/SDL/OpenGL/OpenAL dependency: these are configuration tools only.
add_executable(AmnesiaDedicatedConfig src/ConfigTool.cpp)
target_link_libraries(AmnesiaDedicatedConfig PRIVATE DedicatedConfig)

include(CTest)
if(BUILD_TESTING)
    add_executable(DedicatedConfigTests tests/ConfigTests.cpp)
    target_link_libraries(DedicatedConfigTests PRIVATE DedicatedConfig)
    add_test(NAME dedicated-config-validation COMMAND DedicatedConfigTests
        ${CMAKE_CURRENT_SOURCE_DIR}/config/linux-vps.ini)
    add_test(NAME dedicated-config-cli COMMAND AmnesiaDedicatedConfig --validate
        ${CMAKE_CURRENT_SOURCE_DIR}/config/linux-vps.ini)
endif()
