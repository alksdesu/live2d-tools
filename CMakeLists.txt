cmake_minimum_required(VERSION 3.21)
project(live2d_tools VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
include(CTest)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL Test)

add_library(model_data STATIC
    opengl/cmeshbounds.cpp
    live2dstruct/cmodeldocument.cpp
    live2dstruct/util/cfilereader.cpp
    live2dstruct/csectionoffsettable.cpp
    live2dstruct/csectiontable.cpp
)
target_include_directories(model_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(model_data PUBLIC Qt6::Core)
if(MSVC)
    target_compile_options(model_data PUBLIC /utf-8)
endif()

add_library(cmo3_data STATIC
    cmo3/ccaffarchive.cpp
    cmo3/ccmo3entities.cpp
    cmo3/ccmo3builder.cpp
    cmo3/cxsxmlwriter.cpp
    opengl/ctextureextractor.cpp
)
target_include_directories(cmo3_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cmo3_data PUBLIC Qt6::Core Qt6::Gui)

add_library(ui_inspector STATIC
    ui/cinspectorpanel.cpp
    ui/cmodeldiagnosticspanel.cpp
    ui/cparampanel.cpp
)
target_include_directories(ui_inspector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(ui_inspector PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/opencv/include)
target_link_libraries(ui_inspector PUBLIC model_data Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL)

if(BUILD_TESTING)
    add_executable(model_data_tests tests/model_data_tests.cpp)
    target_link_libraries(model_data_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(model_data_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME model_data COMMAND model_data_tests)
endif()
