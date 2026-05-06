
cmake_minimum_required(VERSION 3.16)
project(test_myrouter_ReRoute LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(test_myrouter_ReRoute
  main.cpp
  Parser.cpp
  RouterDB.cpp
  Grid.cpp
  PDTreeRouter.cpp
  EDCompute.cpp
  HBTResourceManager.cpp
  NetReRoute.cpp
  NetReRoute2D.cpp
  NetReRoute3D.cpp
  RouteTopologyValidator.cpp
  ExperimentConfig.cpp
  ExperimentSummaryWriter.cpp
  NetInfoWriter.cpp
  PlotDataWriter.cpp
  TimingAnnotator.cpp
  VerilogNetlist.cpp
  LibertyData.cpp
  RCDefaults.cpp
)

target_include_directories(test_myrouter_ReRoute PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/src/odb/include
  ${CMAKE_SOURCE_DIR}/src/utl/include
)

target_link_libraries(test_myrouter_ReRoute PRIVATE
  db
  lefin
  defin
  utl_lib
)

target_compile_options(test_myrouter_ReRoute PRIVATE -g -O0)
