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
    live2dstruct/cmeshtopology.cpp
    live2dstruct/cmotiontimeline.cpp
    live2dstruct/cmotionblendpreview.cpp
    live2dstruct/cmodeldocument.cpp
    live2dstruct/cmodeluserdata.cpp
    live2dstruct/cmodelsession.cpp
    std/cmodeljson.cpp
    std/cmodeljsonwriter.cpp
    live2dstruct/util/cfilereader.cpp
    live2dstruct/csectionoffsettable.cpp
    live2dstruct/csectiontable.cpp
)
target_include_directories(model_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(model_data PUBLIC Qt6::Core)
if(MSVC)
    target_compile_options(model_data PUBLIC /utf-8)
endif()

add_library(psd_zip STATIC psd/psdSamples/Zip/PsdMiniz.cpp)
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
    ui/cuserdatapanel.cpp
    ui/cparampanel.cpp
    ui/cxyparameterpad.cpp
)
target_include_directories(ui_inspector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ui_inspector PUBLIC model_data Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL)

if(BUILD_TESTING)
    add_executable(model_data_tests tests/model_data_tests.cpp)
    target_link_libraries(model_data_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(model_data_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME model_data COMMAND model_data_tests)

    add_executable(model_userdata_tests tests/model_userdata_tests.cpp)
    target_link_libraries(model_userdata_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(model_userdata_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME model_userdata COMMAND model_userdata_tests)
    add_executable(userdata_panel_tests tests/userdata_panel_tests.cpp ui/cuserdatapanel.cpp)
    target_link_libraries(userdata_panel_tests PRIVATE cmo3_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(userdata_panel_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME userdata_panel COMMAND userdata_panel_tests)
    set_tests_properties(userdata_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(draw_order_tests tests/draw_order_tests.cpp)
    target_link_libraries(draw_order_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(draw_order_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME draw_order COMMAND draw_order_tests)
    add_executable(mesh_topology_tests tests/mesh_topology_tests.cpp)
    target_link_libraries(mesh_topology_tests PRIVATE model_data Qt6::Test)
    add_test(NAME mesh_topology COMMAND mesh_topology_tests)
    add_executable(mesh_index_overrides_tests tests/mesh_index_overrides_tests.cpp)
    target_link_libraries(mesh_index_overrides_tests PRIVATE model_data Qt6::Test)
    add_test(NAME mesh_index_overrides COMMAND mesh_index_overrides_tests)
    add_executable(triangl_mesh_tests tests/triangl_mesh_tests.cpp edit/ctriangl.cpp)
    target_link_libraries(triangl_mesh_tests PRIVATE model_data Qt6::Gui Qt6::Test)
    add_test(NAME triangl_mesh COMMAND triangl_mesh_tests)
    add_executable(grid_transform_tests tests/grid_transform_tests.cpp
        live2dstruct/coor/cgridtransform.cpp live2dstruct/coor/cwarpcoortype.cpp
        live2dstruct/coor/ccoortypeobject.cpp)
    target_include_directories(grid_transform_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(grid_transform_tests PRIVATE Qt6::Gui Qt6::Test)
    add_test(NAME grid_transform COMMAND grid_transform_tests)
    add_executable(rotation_transform_tests tests/rotation_transform_tests.cpp
        live2dstruct/coor/crotationcoortype.cpp live2dstruct/coor/ccoortypeobject.cpp
        live2dstruct/keyform/crotationnodekeyform.cpp live2dstruct/keyform/ckeyformobject.cpp)
    target_include_directories(rotation_transform_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(rotation_transform_tests PRIVATE Qt6::Gui Qt6::Test)
    add_test(NAME rotation_transform COMMAND rotation_transform_tests)
    add_executable(inspector_history_tests tests/inspector_history_tests.cpp)
    target_sources(inspector_history_tests PRIVATE ui/cinspectorpanel.cpp)
    target_link_libraries(inspector_history_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(inspector_history_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME inspector_history COMMAND inspector_history_tests)
    set_tests_properties(inspector_history PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(model_tree_tests tests/model_tree_tests.cpp ui/cmodeltreepanel.cpp ui/para/ccidjson.cpp)
    target_link_libraries(model_tree_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(model_tree_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME model_tree COMMAND model_tree_tests)
    set_tests_properties(model_tree PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(physics_panel_tests tests/physics_panel_tests.cpp ui/cphysicspanel.cpp ui/cphysicsgraph.cpp)
    target_include_directories(physics_panel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(physics_panel_tests PRIVATE cmo3_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(physics_panel_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME physics_panel COMMAND physics_panel_tests)
    set_tests_properties(physics_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(xy_parameter_pad_tests tests/xy_parameter_pad_tests.cpp ui/cxyparameterpad.cpp)
    target_include_directories(xy_parameter_pad_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(xy_parameter_pad_tests PRIVATE Qt6::Test Qt6::Widgets)
    add_test(NAME xy_parameter_pad COMMAND xy_parameter_pad_tests)
    set_tests_properties(xy_parameter_pad PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(psd_batch_worker_tests tests/psd_batch_worker_tests.cpp ui/psdbatchworker.cpp)
    target_include_directories(psd_batch_worker_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(psd_batch_worker_tests PRIVATE Qt6::Test Qt6::Core)
    add_test(NAME psd_batch_worker COMMAND psd_batch_worker_tests)
    add_executable(psd_print_dialog_tests tests/psd_print_dialog_tests.cpp ui/cpsdprintdialog.cpp)
    target_include_directories(psd_print_dialog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(psd_print_dialog_tests PRIVATE Qt6::Test Qt6::Widgets)
    add_test(NAME psd_print_dialog COMMAND psd_print_dialog_tests)
    set_tests_properties(psd_print_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(param_panel_tests tests/param_panel_tests.cpp tests/param_panel_runtime_stubs.cpp
        ui/cparampanel.cpp ui/cxyparameterpad.cpp)
    target_link_libraries(param_panel_tests PRIVATE model_data Qt6::Test Qt6::Widgets Qt6::OpenGL)
    target_compile_definitions(param_panel_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME param_panel COMMAND param_panel_tests)
    set_tests_properties(param_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(cmotiontimeline_tests tests/cmotiontimeline_tests.cpp)
    target_include_directories(cmotiontimeline_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cmotiontimeline_tests PRIVATE model_data Qt6::Test Qt6::Core)
    target_compile_definitions(cmotiontimeline_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME cmotiontimeline COMMAND cmotiontimeline_tests)
    add_executable(cmotionblendpreview_tests tests/cmotionblendpreview_tests.cpp)
    target_link_libraries(cmotionblendpreview_tests PRIVATE model_data Qt6::Test)
    add_test(NAME cmotionblendpreview COMMAND cmotionblendpreview_tests)
    add_executable(cmotioncurveview_tests tests/cmotioncurveview_tests.cpp ui/cmotioncurveview.cpp)
    target_include_directories(cmotioncurveview_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cmotioncurveview_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    add_test(NAME cmotioncurveview COMMAND cmotioncurveview_tests)
    set_tests_properties(cmotioncurveview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(motion_panel_tests tests/motion_panel_tests.cpp ui/cmotionpanel.cpp ui/cmotioncurveview.cpp ui/cmotionsettingsdialog.cpp)
    target_link_libraries(motion_panel_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(motion_panel_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME motion_panel COMMAND motion_panel_tests)
    set_tests_properties(motion_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(wav_handler_tests tests/wav_handler_tests.cpp live2d/LAppWavFileHandler.cpp)
    target_include_directories(wav_handler_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(wav_handler_tests PRIVATE Qt6::Test Qt6::Core Qt6::Gui)
    if(APPLE)
        target_compile_options(wav_handler_tests PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(wav_handler_tests PRIVATE -Wl,-dead_strip)
    elseif(UNIX)
        target_compile_options(wav_handler_tests PRIVATE -ffunction-sections -fdata-sections)
        target_link_options(wav_handler_tests PRIVATE -Wl,--gc-sections)
    endif()
    add_test(NAME wav_handler COMMAND wav_handler_tests)
    add_executable(cmo3_tests tests/cmo3_tests.cpp)
    target_link_libraries(cmo3_tests PRIVATE cmo3_data Qt6::Test)
    target_compile_definitions(cmo3_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME cmo3 COMMAND cmo3_tests)
    add_executable(cmo3_deformer_tests tests/cmo3_deformer_tests.cpp)
    target_link_libraries(cmo3_deformer_tests PRIVATE cmo3_data Qt6::Test)
    target_compile_definitions(cmo3_deformer_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME cmo3_deformers COMMAND cmo3_deformer_tests)
    add_library(psd_data STATIC
        psd/cpsdlist.cpp
        psd/psdSamples/Exporter/PsdExport.cpp
        psd/psdSamples/ImageUtil/PsdDecompressRle.cpp
        psd/psdSamples/Interfaces/PsdAllocator.cpp
        psd/psdSamples/Interfaces/PsdFile.cpp
        psd/psdSamples/Interfaces/PsdMallocAllocator.cpp
        psd/psdSamples/Interfaces/PsdNativeFile.cpp
        psd/psdSamples/Util/PsdSyncFileWriter.cpp
        psd/psdSamples/Util/PsdFixedSizeString.cpp
    )
    target_include_directories(psd_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(psd_data PUBLIC Qt6::Core Qt6::Gui PRIVATE psd_zip)
    if(MSVC)
        target_compile_options(psd_data PUBLIC /utf-8)
    endif()
    add_executable(psd_tests tests/psd_tests.cpp assets/icc.qrc)
    set_target_properties(psd_tests PROPERTIES AUTORCC ON)
    target_link_libraries(psd_tests PRIVATE psd_data Qt6::Test)
    add_test(NAME psd COMMAND psd_tests)
    add_executable(psd_native_file_tests tests/psd_native_file_tests.cpp)
    target_link_libraries(psd_native_file_tests PRIVATE psd_data Qt6::Test)
    add_test(NAME psd_native_file COMMAND psd_native_file_tests)

endif()
