cmake_minimum_required(VERSION 3.20)
project(oreslang_llvm VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(LLVM 14 CONFIG REQUIRED)
message(STATUS "Building Oreslang LLVM against LLVM ${LLVM_PACKAGE_VERSION}")

add_executable(oreslang-llvmc
    src/main/cpp/oreslang/main.cpp
    src/main/cpp/oreslang/frontend/Parser.cpp
    src/main/cpp/oreslang/backend/LlvmCodegen.cpp
)

target_include_directories(oreslang-llvmc PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp"
    "${LLVM_INCLUDE_DIRS}"
)
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(ORE_LLVM_LIBS core support)
target_link_libraries(oreslang-llvmc PRIVATE ${ORE_LLVM_LIBS})

include(CTest)
if(BUILD_TESTING)
    add_test(NAME emits_constant_main
        COMMAND oreslang-llvmc
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/constant-main.ores")
    set_tests_properties(emits_constant_main PROPERTIES
        PASS_REGULAR_EXPRESSION "ret i64 42")

    add_test(NAME emits_module_symbol
        COMMAND oreslang-llvmc
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/module-main.ores")
    set_tests_properties(emits_module_symbol PROPERTIES
        PASS_REGULAR_EXPRESSION "demo.main")

    add_test(NAME rejects_unsupported_actor_syntax
        COMMAND oreslang-llvmc
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/unsupported-actor.ores")
    set_tests_properties(rejects_unsupported_actor_syntax PROPERTIES
        WILL_FAIL TRUE)
endif()
