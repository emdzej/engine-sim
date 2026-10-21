# Engine Sim as a gasm guest (engine-sim.wasm). Built by tools/gasm/build.sh:
#   cmake -S tools/gasm -B build-gasm -DCMAKE_TOOLCHAIN_FILE=<gasm>/sdk/c/cmake/gasm-toolchain.cmake
cmake_minimum_required(VERSION 3.20)
project(engine_sim_gasm C CXX)

include(Gasm)

set(CMAKE_CXX_STANDARD 17)
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(SUBMODULES "${ROOT}/dependencies/submodules")
set(PIRANHA "${SUBMODULES}/piranha")

# Host tools: flex and bison generate the script parser
find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)
flex_target(lexer "${PIRANHA}/flex-bison/scanner.l" "${CMAKE_CURRENT_BINARY_DIR}/scanner.auto.cpp"
    DEFINES_FILE "${CMAKE_CURRENT_BINARY_DIR}/scanner.auto.h")
bison_target(parser "${PIRANHA}/flex-bison/specification.y" "${CMAKE_CURRENT_BINARY_DIR}/parser.auto.cpp"
    DEFINES_FILE "${CMAKE_CURRENT_BINARY_DIR}/parser.auto.h")
add_flex_bison_dependency(lexer parser)

file(GLOB PIRANHA_SOURCES "${PIRANHA}/src/*.cpp")
file(GLOB SCS_SOURCES "${SUBMODULES}/simple-2d-constraint-solver/src/*.cpp")
file(GLOB SCRIPTING_SOURCES "${ROOT}/scripting/src/*.cpp")

# The simulation core: the engine-sim library's sources in the top-level CMakeLists.txt
set(CORE_SOURCES
    audio_buffer camshaft crankshaft combustion_chamber connecting_rod convolution_filter
    cylinder_bank cylinder_head delay_filter derivative_filter direct_throttle_linkage
    dynamometer engine exhaust_system feedback_comb_filter filter fuel function gas_system
    gaussian_filter governor ignition_module impulse_response intake jitter_filter
    leveling_filter low_pass_filter part piston piston_engine_simulator simulator
    standard_valvetrain starter_motor synthesizer throttle transmission utilities valvetrain
    vehicle vehicle_drag_constraint vtec_valvetrain)
list(TRANSFORM CORE_SOURCES PREPEND "${ROOT}/src/")
list(TRANSFORM CORE_SOURCES APPEND ".cpp")

gasm_add_game(engine-sim
    engine_sim_gasm.cpp
    ${CORE_SOURCES}
    ${SCRIPTING_SOURCES}
    ${PIRANHA_SOURCES}
    ${FLEX_lexer_OUTPUTS}
    ${BISON_parser_OUTPUTS}
    ${SCS_SOURCES})

target_include_directories(engine-sim PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}"
    "${FLEX_INCLUDE_DIRS}"
    "${PIRANHA}/include"
    "${SUBMODULES}")

target_compile_definitions(engine-sim PRIVATE
    PIRANHA_NO_BOOST
    ATG_ENGINE_SIM_FAST_CONVOLUTION
    ATG_ENGINE_SIM_PIRANHA_ENABLED
    "__forceinline=inline __attribute__((always_inline))")

target_compile_options(engine-sim PRIVATE -Wno-deprecated-declarations)

option(ENGINE_SIM_GASM_SIMD "Build with wasm SIMD (simd128)" OFF)
if (ENGINE_SIM_GASM_SIMD)
    target_compile_options(engine-sim PRIVATE -msimd128)
    target_link_options(engine-sim PRIVATE -msimd128)
endif ()
