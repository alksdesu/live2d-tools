cmake_minimum_required(VERSION 3.21)
project(live2d_tools VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
include(CTest)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL Test)

if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    if(CMAKE_OSX_SYSROOT)
        set(_live2d_macos_sdk "${CMAKE_OSX_SYSROOT}")
    else()
        execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
                        OUTPUT_VARIABLE _live2d_macos_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    if(NOT EXISTS "${_live2d_macos_sdk}/System/Library/Frameworks/AGL.framework")
        get_target_property(_wrap_opengl_links WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
        list(FILTER _wrap_opengl_links EXCLUDE REGEX "(^|[ /])AGL($|\\.framework$)")
        set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${_wrap_opengl_links}")
    endif()
endif()

add_library(model_data STATIC
    opengl/cmeshbounds.cpp
    live2dstruct/cdraworder.cpp
    live2dstruct/cmodeldocument.cpp
    live2dstruct/cmodelsession.cpp
    std/cmodeljson.cpp
    live2dstruct/util/cfilereader.cpp
    live2dstruct/csectionoffsettable.cpp
    live2dstruct/csectiontable.cpp
)
target_include_directories(model_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(model_data PUBLIC Qt6::Core)
if(MSVC)
    target_compile_options(model_data PUBLIC /utf-8)
endif()

add_library(psd_zip STATIC psd/psdSamples/Zip/Psdminiz.c)
set_source_files_properties(psd/psdSamples/Zip/Psdminiz.c PROPERTIES LANGUAGE CXX)
target_include_directories(psd_zip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(psd_zip PROPERTIES AUTOMOC OFF)

add_library(cmo3_data STATIC
    cmo3/ccaffarchive.cpp
    cmo3/ccmo3entities.cpp
    cmo3/ccmo3builder.cpp
    cmo3/cxsxmlwriter.cpp
    opengl/ctextureextractor.cpp
)
target_include_directories(cmo3_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cmo3_data PUBLIC model_data Qt6::Core Qt6::Gui)
target_link_libraries(cmo3_data PRIVATE psd_zip)

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
    add_executable(draw_order_tests tests/draw_order_tests.cpp)
    target_link_libraries(draw_order_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(draw_order_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME draw_order COMMAND draw_order_tests)
    add_executable(inspector_history_tests tests/inspector_history_tests.cpp)
    target_sources(inspector_history_tests PRIVATE ui/cinspectorpanel.cpp)
    target_link_libraries(inspector_history_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(inspector_history_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME inspector_history COMMAND inspector_history_tests)
    set_tests_properties(inspector_history PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(cmo3_tests tests/cmo3_tests.cpp)
    target_link_libraries(cmo3_tests PRIVATE cmo3_data Qt6::Test)
    target_compile_definitions(cmo3_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME cmo3 COMMAND cmo3_tests)
    if(WIN32)
        add_library(psd_data STATIC
            psd/cpsdlist.cpp
            psd/psdSamples/Exporter/PsdExport.cpp
            psd/psdSamples/ImageUtil/PsdDecompressRle.cpp
            psd/psdSamples/Interfaces/PsdAllocator.cpp
            psd/psdSamples/Interfaces/PsdFile.cpp
            psd/psdSamples/Interfaces/PsdMallocAllocator.cpp
            psd/psdSamples/Interfaces/PsdNativeFile.cpp
            psd/psdSamples/Util/PsdSyncFileWriter.cpp
        )
        target_include_directories(psd_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(psd_data PUBLIC Qt6::Core Qt6::Gui PRIVATE psd_zip)
        if(MSVC)
            target_compile_options(psd_data PUBLIC /utf-8)
        endif()
        add_executable(psd_tests tests/psd_tests.cpp)
        target_link_libraries(psd_tests PRIVATE psd_data Qt6::Test)
        add_test(NAME psd COMMAND psd_tests)

    endif()
endif()
