
cmake_minimum_required(VERSION 3.16)
project(test_myrouter_PDtree LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(test_myrouter_PDtree
  main.cpp
  Parser.cpp
  RouterDB.cpp
  Grid.cpp
  PDTreeRouter.cpp
  EDCompute.cpp
  ExperimentConfig.cpp
  ExperimentSummaryWriter.cpp
  NetInfoWriter.cpp
  PlotDataWriter.cpp
  TimingAnnotator.cpp
  VerilogNetlist.cpp
  LibertyData.cpp
  RCDefaults.cpp
)

target_include_directories(test_myrouter_PDtree PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/odb/include
  ${CMAKE_SOURCE_DIR}/src/utl/include
)

target_link_libraries(test_myrouter_PDtree PRIVATE
  db
  lefin
  defin
  utl_lib
)

target_compile_options(test_myrouter_PDtree PRIVATE -g -O0)
