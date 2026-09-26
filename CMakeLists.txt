cmake_minimum_required(VERSION 3.21)
project(live2d_tools VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(APPLE)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-include>"
                        "$<$<COMPILE_LANGUAGE:CXX>:${CMAKE_CURRENT_SOURCE_DIR}/std/cmacosqtcompat.h>")
endif()
include(CTest)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Core5Compat Test)
set(LIVE2D_CORE_LIBRARY "$ENV{LIVE2D_CORE_LIBRARY}" CACHE FILEPATH "Cubism Core library compatible with core/Live2DCubismCore.h")
if(LIVE2D_CORE_LIBRARY)
    if(NOT EXISTS "${LIVE2D_CORE_LIBRARY}")
        message(FATAL_ERROR "LIVE2D_CORE_LIBRARY does not exist: ${LIVE2D_CORE_LIBRARY}")
    endif()
    add_library(live2d_core UNKNOWN IMPORTED)
    set_target_properties(live2d_core PROPERTIES IMPORTED_LOCATION "${LIVE2D_CORE_LIBRARY}")
endif()

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

add_library(export_control STATIC std/cexportcontrol.cpp)
target_include_directories(export_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(export_control PUBLIC Qt6::Core)
add_library(export_task_runner STATIC ui/cexporttaskrunner.cpp)
target_link_libraries(export_task_runner PUBLIC export_control)
if(MSVC)
    target_compile_options(export_control PUBLIC /utf-8)
endif()

add_library(model_data STATIC
    live2dstruct/cparentgraph.cpp
    live2dstruct/cblendshaperelations.cpp
    live2d/cdeformerdrag.cpp
    opengl/cmeshbounds.cpp
    opengl/cuvgeometry.cpp
    live2dstruct/ccanvasgeometry.cpp
    live2dstruct/cdraworder.cpp
    live2dstruct/cmeshtopology.cpp
    live2dstruct/cmotiontimeline.cpp
    live2dstruct/cmotionblendpreview.cpp
    live2dstruct/cmodeldocument.cpp
    live2dstruct/cmodeldocumentstructure.cpp
    live2dstruct/cparameterreferences.cpp
    live2dstruct/cparameterreferenceeditor.cpp
    live2dstruct/cparameterresourceedits.cpp
    live2d/cparameterreferenceplan.cpp
    live2dstruct/ckeyformbindinggrid.cpp
    live2dstruct/cmocpreviewbuilder.cpp
    live2dstruct/cmocruntimebuilder.cpp
    live2dstruct/cruntimepackage.cpp
    live2dstruct/cphysicsdocument.cpp
    live2dstruct/cmocparameterstructurebuilder.cpp
    live2dstruct/cmoclayoutrewriter.cpp
    live2dstruct/cmodeluserdata.cpp
    live2dstruct/cmodelsession.cpp
    std/cmodeljson.cpp
    std/cmodelresourcepath.cpp
    std/cdirectoryexport.cpp
    std/cmodeljsonwriter.cpp
    live2dstruct/util/cfilereader.cpp
    live2dstruct/csectionoffsettable.cpp
    live2dstruct/csectiontable.cpp
    live2dstruct/csectionvalidation.cpp
)
target_include_directories(model_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(model_data PUBLIC Qt6::Core export_control)
if(MSVC)
    target_compile_options(model_data PUBLIC /utf-8)
endif()

add_library(model_hierarchy_ui STATIC ui/cmodelhierarchymodel.cpp ui/cmodelhierarchyview.cpp)
target_include_directories(model_hierarchy_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(model_hierarchy_ui PUBLIC model_data Qt6::Widgets)

add_library(model_coordinates STATIC
    live2dstruct/coor/ccoordinateframe.cpp
    live2dstruct/coor/ccoortypeobject.cpp
    live2dstruct/coor/cgridtransform.cpp
    live2dstruct/coor/crotationcoortype.cpp
    live2dstruct/coor/cwarpcoortype.cpp
    live2dstruct/keyform/ckeyformobject.cpp
    live2dstruct/keyform/crotationnodekeyform.cpp
)
target_include_directories(model_coordinates PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(model_coordinates PUBLIC Qt6::Gui)
if(MSVC)
    target_compile_options(model_coordinates PUBLIC /utf-8)
endif()

add_library(deformer_tree_data STATIC
    live2dstruct/ctreemanage.cpp
    live2dstruct/ctreemanage.h
    live2dstruct/ckeyformmappingcontext.cpp
    live2dstruct/ckeyformmappingcontext.h
    live2dstruct/ckeyformeditcontroller.cpp
    live2dstruct/ckeyformeditcontroller.h
    live2dstruct/tree/c2dtree.cpp
    live2dstruct/tree/c2dtreenode.cpp
    live2dstruct/tree/cpartsnode.cpp
    live2dstruct/tree/cparameternode.cpp
    live2dstruct/tree/cdeformernode.cpp
    live2dstruct/tree/crotationnode.cpp
    live2dstruct/tree/cwarpnode.cpp
    live2dstruct/tree/cartmeshnode.cpp
    live2dstruct/keyform/cartmeshkeyform.cpp
    live2dstruct/keyform/cwarpnodekeyform.cpp
    live2dstruct/keyform/clinearinterpolation.cpp
)
target_link_libraries(deformer_tree_data PUBLIC model_coordinates model_data)

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
    if(TARGET live2d_core)
        add_executable(physics_bake_tests tests/physics_bake_tests.cpp live2d/cphysicsbake.cpp
            framework/CubismFramework.cpp framework/Model/CubismMoc.cpp framework/Model/CubismModel.cpp
            framework/Physics/CubismPhysics.cpp framework/Physics/CubismPhysicsJson.cpp
            framework/Id/CubismId.cpp framework/Id/CubismIdManager.cpp framework/Type/csmString.cpp
            framework/Utils/CubismJson.cpp framework/Utils/CubismString.cpp framework/Utils/CubismDebug.cpp
            framework/Math/CubismMath.cpp framework/Math/CubismVector2.cpp
            edit/cmeshmap.cpp edit/cdrawmesh.cpp edit/ctriangl.cpp opengl/cglmeshbuffer.cpp)
        target_link_libraries(physics_bake_tests PRIVATE model_data live2d_core Qt6::Gui Qt6::OpenGL Qt6::Test)
        target_compile_definitions(physics_bake_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME physics_bake COMMAND physics_bake_tests)
        add_executable(runtime_moc_core_tests tests/runtime_moc_core_tests.cpp)
        target_link_libraries(runtime_moc_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(runtime_moc_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME runtime_moc_core COMMAND runtime_moc_core_tests)
        add_executable(real_core_preview_tests tests/real_core_preview_tests.cpp)
        target_link_libraries(real_core_preview_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(real_core_preview_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME real_core_preview COMMAND real_core_preview_tests)
        add_executable(parameter_structure_core_tests tests/parameter_structure_core_tests.cpp)
        target_link_libraries(parameter_structure_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(parameter_structure_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME parameter_structure_core COMMAND parameter_structure_core_tests)
        add_executable(parameter_delete_core_tests tests/parameter_delete_core_tests.cpp)
        target_link_libraries(parameter_delete_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(parameter_delete_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME parameter_delete_core COMMAND parameter_delete_core_tests)
    endif()
    add_executable(physics_bake_dialog_tests tests/physics_bake_dialog_tests.cpp ui/cphysicsbakedialog.cpp)
    target_link_libraries(physics_bake_dialog_tests PRIVATE model_data Qt6::Widgets Qt6::Test)
    add_test(NAME physics_bake_dialog COMMAND physics_bake_dialog_tests)
    set_tests_properties(physics_bake_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(runtime_export_dialog_tests tests/runtime_export_dialog_tests.cpp ui/cruntimeexportdialog.cpp)
    target_include_directories(runtime_export_dialog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(runtime_export_dialog_tests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME runtime_export_dialog COMMAND runtime_export_dialog_tests)
    set_tests_properties(runtime_export_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(export_control_tests tests/export_control_tests.cpp)
    target_link_libraries(export_control_tests PRIVATE export_control Qt6::Test)
    add_test(NAME export_control COMMAND export_control_tests)
    add_executable(export_task_runner_tests tests/export_task_runner_tests.cpp)
    target_link_libraries(export_task_runner_tests PRIVATE export_task_runner Qt6::Test Qt6::Widgets)
    add_test(NAME export_task_runner COMMAND export_task_runner_tests)
    set_tests_properties(export_task_runner PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(preview_export_transaction_tests tests/preview_export_transaction_tests.cpp
        ui/cpreviewexporttransaction.cpp)
    target_include_directories(preview_export_transaction_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(preview_export_transaction_tests PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME preview_export_transaction COMMAND preview_export_transaction_tests)

    add_executable(theme_tests tests/theme_tests.cpp
        ui/cxyparameterpad.cpp ui/cphysicsgraph.cpp ui/cmotioncurveview.cpp)
    target_link_libraries(theme_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_include_directories(theme_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME theme COMMAND theme_tests)
    set_tests_properties(theme PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(APPLE)
        add_test(NAME theme_native COMMAND theme_tests)
        set_tests_properties(theme_native PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=cocoa" RUN_SERIAL TRUE)
    endif()

    add_executable(caff_archive_tests tests/caff_archive_tests.cpp)
    target_link_libraries(caff_archive_tests PRIVATE cmo3_data Qt6::Test)
    add_test(NAME caff_archive COMMAND caff_archive_tests)

    foreach(data_test IN ITEMS parent_graph blend_shape_relations visibility_edit model_resource_path directory_export runtime_moc runtime_package physics_document parameter_references parameter_reference_editor parameter_resource_edits parameter_reference_plan parameter_structure parameter_structure_document parameter_delete)
        add_executable(${data_test}_tests tests/${data_test}_tests.cpp)
        target_link_libraries(${data_test}_tests PRIVATE model_data Qt6::Test)
        target_compile_definitions(${data_test}_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME ${data_test} COMMAND ${data_test}_tests)
    endforeach()
    add_executable(hierarchy_view_tests tests/hierarchy_view_tests.cpp)
    target_link_libraries(hierarchy_view_tests PRIVATE model_hierarchy_ui Qt6::Test)
    add_test(NAME hierarchy_view COMMAND hierarchy_view_tests)
    set_tests_properties(hierarchy_view PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(model_data_tests tests/model_data_tests.cpp)
    target_link_libraries(model_data_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(model_data_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME model_data COMMAND model_data_tests)
    foreach(preview_test IN ITEMS keyform_binding_grid moc_preview_builder model_snapshot session_restore deformer_visibility)
        set(preview_test_source ${preview_test})
        if(preview_test STREQUAL "moc_preview_builder")
            set(preview_test_source preview_moc_builder)
        endif()
        add_executable(${preview_test}_tests tests/${preview_test_source}_tests.cpp)
        target_link_libraries(${preview_test}_tests PRIVATE model_data Qt6::Test)
        target_compile_definitions(${preview_test}_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME ${preview_test} COMMAND ${preview_test}_tests)
    endforeach()
    add_executable(model_settings_tests tests/model_settings_tests.cpp)
    target_link_libraries(model_settings_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(model_settings_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME model_settings COMMAND model_settings_tests)
    add_executable(section_preflight_tests tests/section_preflight_tests.cpp)
    target_link_libraries(section_preflight_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(section_preflight_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME section_preflight COMMAND section_preflight_tests)
    add_executable(canvas_geometry_tests tests/canvas_geometry_tests.cpp)
    target_link_libraries(canvas_geometry_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(canvas_geometry_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME canvas_geometry COMMAND canvas_geometry_tests)
    add_executable(canvas_coordinates_tests tests/canvas_coordinates_tests.cpp opengl/ccanvascoordinates.cpp)
    target_include_directories(canvas_coordinates_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(canvas_coordinates_tests PRIVATE Qt6::Gui Qt6::Test)
    add_test(NAME canvas_coordinates COMMAND canvas_coordinates_tests)
    foreach(keyform_test IN ITEMS keyform_mapping keyform_edit_controller parameter_definition_edit parameter_structure_controller tree_snapshot)
        add_executable(${keyform_test}_tests tests/${keyform_test}_tests.cpp)
        target_link_libraries(${keyform_test}_tests PRIVATE deformer_tree_data Qt6::Test)
        target_compile_definitions(${keyform_test}_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME ${keyform_test} COMMAND ${keyform_test}_tests)
    endforeach()
    add_executable(moc_deformer_preview_tests tests/deformer_moc_preview_tests.cpp)
    add_executable(moc_parameter_preview_tests tests/parameter_moc_preview_tests.cpp)
    target_link_libraries(moc_parameter_preview_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(moc_parameter_preview_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME moc_parameter_preview COMMAND moc_parameter_preview_tests)
    add_executable(moc_layout_rewriter_tests tests/layout_moc_rewriter_tests.cpp)
    target_link_libraries(moc_layout_rewriter_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(moc_layout_rewriter_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME moc_layout_rewriter COMMAND moc_layout_rewriter_tests)
    target_link_libraries(moc_deformer_preview_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(moc_deformer_preview_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME moc_deformer_preview COMMAND moc_deformer_preview_tests)
    add_executable(shared_keyform_preview_tests tests/shared_keyform_preview_tests.cpp)
    target_link_libraries(shared_keyform_preview_tests PRIVATE deformer_tree_data Qt6::Test)
    target_compile_definitions(shared_keyform_preview_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME shared_keyform_preview COMMAND shared_keyform_preview_tests)
    add_executable(deformer_drag_tests tests/deformer_drag_tests.cpp)
    target_link_libraries(deformer_drag_tests PRIVATE model_data Qt6::Test)
    add_test(NAME deformer_drag COMMAND deformer_drag_tests)
    add_executable(mask_moc_preview_tests tests/mask_moc_preview_tests.cpp)
    target_link_libraries(mask_moc_preview_tests PRIVATE cmo3_data Qt6::Test)
    target_compile_definitions(mask_moc_preview_tests PRIVATE TEST_RESOURCES=\"${CMAKE_CURRENT_SOURCE_DIR}/Resources\")
    add_test(NAME mask_moc_preview COMMAND mask_moc_preview_tests)
    add_executable(clipping_editor_tests tests/clipping_editor_tests.cpp ui/cinspectorpanel.cpp)
    target_link_libraries(clipping_editor_tests PRIVATE deformer_tree_data cmo3_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(clipping_editor_tests PRIVATE TEST_RESOURCES=\"${CMAKE_CURRENT_SOURCE_DIR}/Resources\")
    add_test(NAME clipping_editor COMMAND clipping_editor_tests)
    set_tests_properties(clipping_editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(deformer_editor_tests tests/deformer_editor_tests.cpp ui/cinspectorpanel.cpp
        ui/cmodeltreepanel.cpp ui/para/ccidjson.cpp)
    target_link_libraries(deformer_editor_tests PRIVATE deformer_tree_data model_hierarchy_ui Qt6::Test Qt6::Widgets)
    target_compile_definitions(deformer_editor_tests PRIVATE TEST_RESOURCES=\"${CMAKE_CURRENT_SOURCE_DIR}/Resources\")
    add_test(NAME deformer_editor COMMAND deformer_editor_tests)
    set_tests_properties(deformer_editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(deformer_overlay_snapshot_tests tests/deformer_overlay_snapshot_tests.cpp
        live2d/LAppViewDeformerOverlays.cpp edit/cdrawobject.cpp edit/cdrawwarp.cpp
        edit/cdrawrotation.cpp edit/cgridlines.cpp opengl/cglmeshbuffer.cpp assets/shaders/shaders.qrc assets/images/images.qrc)
    set_target_properties(deformer_overlay_snapshot_tests PROPERTIES AUTORCC ON)
    target_link_libraries(deformer_overlay_snapshot_tests PRIVATE deformer_tree_data Qt6::OpenGL Qt6::Test)
    target_compile_definitions(deformer_overlay_snapshot_tests PRIVATE TEST_RESOURCES=\"${CMAKE_CURRENT_SOURCE_DIR}/Resources\")
    add_test(NAME deformer_overlay_snapshot COMMAND deformer_overlay_snapshot_tests)
    add_executable(model_matrix_tests tests/model_matrix_tests.cpp
        framework/Math/CubismModelMatrix.cpp framework/Math/CubismMatrix44.cpp framework/Type/csmString.cpp)
    target_include_directories(model_matrix_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(model_matrix_tests PRIVATE Qt6::Gui Qt6::Test)
    add_test(NAME model_matrix COMMAND model_matrix_tests)
    add_executable(cubism_moc_lifecycle_tests tests/cubism_moc_lifecycle_tests.cpp framework/Model/CubismMoc.cpp)
    target_include_directories(cubism_moc_lifecycle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cubism_moc_lifecycle_tests PRIVATE Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Test)
    target_compile_definitions(cubism_moc_lifecycle_tests PRIVATE CSM_DEBUG)
    if(MSVC)
        target_compile_options(cubism_moc_lifecycle_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(cubism_moc_lifecycle_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME cubism_moc_lifecycle COMMAND cubism_moc_lifecycle_tests)
    foreach(texture_variant IN ITEMS texture_loader texture_loader_premultiplied)
        add_executable(${texture_variant}_tests tests/texture_loader_tests.cpp
            live2d/LAppTextureManager.cpp live2d/LAppPal.cpp)
        target_include_directories(${texture_variant}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(${texture_variant}_tests PRIVATE Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Test)
        add_test(NAME ${texture_variant} COMMAND ${texture_variant}_tests)
    endforeach()
    target_compile_definitions(texture_loader_premultiplied_tests PRIVATE PREMULTIPLIED_ALPHA_ENABLE)
    add_executable(offscreen_buffer_tests tests/offscreen_buffer_tests.cpp opengl/coffscreenbuffer.cpp)
    target_include_directories(offscreen_buffer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(offscreen_buffer_tests PRIVATE Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME offscreen_buffer COMMAND offscreen_buffer_tests)
    add_executable(mesh_context_tests tests/mesh_context_tests.cpp opengl/cmesh.cpp
        live2d/LAppSprite.cpp live2d/LAppAllocator.cpp opengl/coffscreenbuffer.cpp)
    target_include_directories(mesh_context_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mesh_context_tests PRIVATE Qt6::Gui Qt6::OpenGL Qt6::Widgets Qt6::OpenGLWidgets Qt6::Core5Compat Qt6::Test)
    add_test(NAME mesh_context COMMAND mesh_context_tests)
    add_executable(gl_mesh_buffer_tests tests/gl_mesh_buffer_tests.cpp opengl/cglmeshbuffer.cpp)
    target_include_directories(gl_mesh_buffer_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(gl_mesh_buffer_tests PRIVATE Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME gl_mesh_buffer COMMAND gl_mesh_buffer_tests)
    add_executable(gl_render_state_tests tests/gl_render_state_tests.cpp opengl/cglrenderstate.cpp)
    target_include_directories(gl_render_state_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(gl_render_state_tests PRIVATE Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME gl_render_state COMMAND gl_render_state_tests)
    add_executable(cubism_shader_tests tests/cubism_shader_tests.cpp opengl/ccubismshaderprograms.cpp)
    target_include_directories(cubism_shader_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cubism_shader_tests PRIVATE Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME cubism_shader COMMAND cubism_shader_tests)
    set(cubism_model_test_sources
        tests/cubism_model_fixture.cpp framework/Model/CubismModel.cpp
        framework/Id/CubismId.cpp framework/Id/CubismIdManager.cpp framework/Type/csmString.cpp
        edit/cmeshmap.cpp edit/cdrawmesh.cpp edit/ctriangl.cpp opengl/cglmeshbuffer.cpp)
    add_executable(lapp_export_preview_tests tests/lapp_export_preview_tests.cpp
        live2d/LAppModelUpdate.cpp ${cubism_model_test_sources} opengl/coffscreenbuffer.cpp)
    target_link_libraries(lapp_export_preview_tests PRIVATE model_data Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME lapp_export_preview COMMAND lapp_export_preview_tests)
    set_tests_properties(lapp_export_preview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(model_runtime_state_tests tests/model_runtime_state_tests.cpp ${cubism_model_test_sources})
    target_link_libraries(model_runtime_state_tests PRIVATE model_data Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME model_runtime_state COMMAND model_runtime_state_tests)
    add_executable(model_replacement_tests tests/model_replacement_tests.cpp ${cubism_model_test_sources}
        live2d/LAppModelParameterDefinitions.cpp opengl/coffscreenbuffer.cpp
        live2d/LAppModelParameterStructure.cpp framework/Model/CubismUserModelReplacement.cpp
        live2d/LAppModelParameterReferences.cpp
        framework/Motion/ACubismMotion.cpp framework/Motion/CubismMotion.cpp framework/Motion/CubismMotionJson.cpp
        framework/Motion/CubismMotionManager.cpp framework/Motion/CubismMotionQueueManager.cpp framework/Motion/CubismMotionQueueEntry.cpp
        framework/Motion/CubismExpressionMotion.cpp framework/Motion/CubismExpressionMotionManager.cpp
        framework/Effect/CubismBreath.cpp framework/Effect/CubismEyeBlink.cpp
        framework/Physics/CubismPhysics.cpp framework/Physics/CubismPhysicsJson.cpp
        framework/Effect/CubismPose.cpp
        framework/Utils/CubismJson.cpp framework/Utils/CubismString.cpp
        framework/Math/CubismMath.cpp framework/Math/CubismVector2.cpp framework/Math/CubismModelMatrix.cpp
        framework/Model/CubismModelReplacement.cpp framework/Model/CubismMoc.cpp
        framework/Rendering/CubismRenderer.cpp framework/Math/CubismMatrix44.cpp framework/Type/csmRectF.cpp)
    target_link_libraries(model_replacement_tests PRIVATE model_data Qt6::Gui Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Test)
    target_compile_definitions(model_replacement_tests PRIVATE CSM_DEBUG)
    if(MSVC)
        target_compile_options(model_replacement_tests PRIVATE /UNDEBUG)
    else()
        target_compile_options(model_replacement_tests PRIVATE -UNDEBUG)
    endif()
    add_test(NAME model_replacement COMMAND model_replacement_tests)
    add_executable(physics_parameter_definition_tests tests/physics_parameter_definition_tests.cpp
        ${cubism_model_test_sources}
        framework/Physics/CubismPhysics.cpp framework/Physics/CubismPhysicsJson.cpp
        framework/Utils/CubismJson.cpp framework/Utils/CubismString.cpp
        framework/Math/CubismMath.cpp framework/Math/CubismVector2.cpp)
    target_link_libraries(physics_parameter_definition_tests PRIVATE model_data Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME physics_parameter_definition COMMAND physics_parameter_definition_tests)
    add_executable(cubism_renderer_preparation_tests tests/cubism_renderer_preparation_tests.cpp
        ${cubism_model_test_sources} framework/Rendering/CubismRenderer.cpp
        framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.cpp
        framework/Rendering/OpenGL/CubismShader_OpenGLES2.cpp
        framework/Math/CubismMatrix44.cpp framework/Type/csmRectF.cpp
        opengl/ccubismshaderprograms.cpp opengl/cglrenderstate.cpp opengl/coffscreenbuffer.cpp)
    target_link_libraries(cubism_renderer_preparation_tests PRIVATE model_data
        Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Core5Compat Qt6::Test)
    add_test(NAME cubism_renderer_preparation COMMAND cubism_renderer_preparation_tests)
    add_executable(pose_rebind_tests tests/pose_rebind_tests.cpp framework/Effect/CubismPose.cpp
        framework/Utils/CubismJson.cpp framework/Id/CubismId.cpp framework/Id/CubismIdManager.cpp
        framework/Type/csmString.cpp)
    target_include_directories(pose_rebind_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(pose_rebind_tests PRIVATE Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME pose_rebind COMMAND pose_rebind_tests)
    add_executable(mesh_editor_state_tests tests/mesh_editor_state_tests.cpp
        edit/cmeshmap.cpp edit/cdrawmesh.cpp edit/ctriangl.cpp opengl/cglmeshbuffer.cpp)
    target_link_libraries(mesh_editor_state_tests PRIVATE model_data Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME mesh_editor_state COMMAND mesh_editor_state_tests)
    add_executable(editor_overlay_render_tests tests/editor_overlay_render_tests.cpp
        edit/cmeshmap.cpp edit/cdrawmesh.cpp edit/cdrawrect.cpp edit/cdrawrotation.cpp edit/cdrawobject.cpp edit/ctriangl.cpp
        opengl/cglmeshbuffer.cpp assets/shaders/shaders.qrc assets/images/images.qrc)
    set_target_properties(editor_overlay_render_tests PROPERTIES AUTORCC ON)
    target_link_libraries(editor_overlay_render_tests PRIVATE deformer_tree_data Qt6::OpenGL Qt6::Test)
    add_test(NAME editor_overlay_render COMMAND editor_overlay_render_tests)
    add_executable(moc_optional_fields_tests tests/optional_moc_fields_tests.cpp)
    target_link_libraries(moc_optional_fields_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(moc_optional_fields_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME moc_optional_fields COMMAND moc_optional_fields_tests)

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
    add_executable(drawable_flags_tests tests/drawable_flags_tests.cpp)
    target_link_libraries(drawable_flags_tests PRIVATE cmo3_data Qt6::Test)
    target_compile_definitions(drawable_flags_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME drawable_flags COMMAND drawable_flags_tests)
    add_executable(texture_extraction_tests tests/texture_extraction_tests.cpp)
    target_link_libraries(texture_extraction_tests PRIVATE cmo3_data Qt6::Test)
    target_compile_definitions(texture_extraction_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME texture_extraction COMMAND texture_extraction_tests)
    add_executable(uv_geometry_tests tests/uv_geometry_tests.cpp)
    target_link_libraries(uv_geometry_tests PRIVATE model_data Qt6::Test)
    add_test(NAME uv_geometry COMMAND uv_geometry_tests)
    add_executable(texture_panel_tests tests/texture_panel_tests.cpp ui/ctexturepanel.cpp)
    target_link_libraries(texture_panel_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(texture_panel_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME texture_panel COMMAND texture_panel_tests)
    set_tests_properties(texture_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(mesh_topology_tests tests/mesh_topology_tests.cpp)
    target_link_libraries(mesh_topology_tests PRIVATE model_data Qt6::Test)
    add_test(NAME mesh_topology COMMAND mesh_topology_tests)
    add_executable(mesh_index_overrides_tests tests/mesh_index_overrides_tests.cpp)
    target_link_libraries(mesh_index_overrides_tests PRIVATE model_data Qt6::Test)
    add_test(NAME mesh_index_overrides COMMAND mesh_index_overrides_tests)
    add_executable(triangl_mesh_tests tests/triangl_mesh_tests.cpp edit/ctriangl.cpp)
    target_link_libraries(triangl_mesh_tests PRIVATE model_data Qt6::Gui Qt6::Test)
    add_test(NAME triangl_mesh COMMAND triangl_mesh_tests)
    add_executable(grid_transform_tests tests/grid_transform_tests.cpp)
    target_link_libraries(grid_transform_tests PRIVATE model_coordinates Qt6::Test)
    add_test(NAME grid_transform COMMAND grid_transform_tests)
    add_executable(rotation_transform_tests tests/rotation_transform_tests.cpp)
    target_link_libraries(rotation_transform_tests PRIVATE model_coordinates Qt6::Test)
    add_test(NAME rotation_transform COMMAND rotation_transform_tests)
    add_executable(coordinate_frame_tests tests/coordinate_frame_tests.cpp)
    target_link_libraries(coordinate_frame_tests PRIVATE model_coordinates Qt6::Test)
    add_test(NAME coordinate_frame COMMAND coordinate_frame_tests)
    add_executable(grid_lines_tests tests/grid_lines_tests.cpp edit/cgridlines.cpp)
    target_include_directories(grid_lines_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(grid_lines_tests PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME grid_lines COMMAND grid_lines_tests)
    add_executable(deformer_tree_tests tests/deformer_tree_tests.cpp)
    target_link_libraries(deformer_tree_tests PRIVATE deformer_tree_data Qt6::Test)
    target_compile_definitions(deformer_tree_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME deformer_tree COMMAND deformer_tree_tests)
    add_executable(keyform_interpolation_tests tests/keyform_interpolation_tests.cpp)
    target_link_libraries(keyform_interpolation_tests PRIVATE deformer_tree_data Qt6::Test)
    target_compile_definitions(keyform_interpolation_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME keyform_interpolation COMMAND keyform_interpolation_tests)
    add_executable(warp_render_tests tests/warp_render_tests.cpp edit/cdrawobject.cpp
        edit/cdrawwarp.cpp edit/cgridlines.cpp assets/shaders/shaders.qrc)
    set_target_properties(warp_render_tests PROPERTIES AUTORCC ON)
    target_link_libraries(warp_render_tests PRIVATE deformer_tree_data Qt6::OpenGL Qt6::Test)
    target_compile_definitions(warp_render_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME warp_render COMMAND warp_render_tests)
    add_executable(inspector_history_tests tests/inspector_history_tests.cpp)
    target_sources(inspector_history_tests PRIVATE ui/cinspectorpanel.cpp)
    target_link_libraries(inspector_history_tests PRIVATE deformer_tree_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(inspector_history_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME inspector_history COMMAND inspector_history_tests)
    set_tests_properties(inspector_history PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(model_tree_tests tests/model_tree_tests.cpp ui/cmodeltreepanel.cpp ui/cinspectorpanel.cpp ui/para/ccidjson.cpp)
    target_link_libraries(model_tree_tests PRIVATE model_data model_hierarchy_ui Qt6::Test Qt6::Widgets)
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
    add_executable(psd_batch_worker_tests tests/psd_batch_worker_tests.cpp
        ui/psdbatchworker.cpp ui/psdbatchjob.cpp ui/psdbatchresult.cpp)
    target_include_directories(psd_batch_worker_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(psd_batch_worker_tests PRIVATE Qt6::Test Qt6::Core)
    add_test(NAME psd_batch_worker COMMAND psd_batch_worker_tests)
    add_executable(export_results_panel_tests tests/export_results_panel_tests.cpp
        ui/cexportresultspanel.cpp ui/psdbatchresult.cpp)
    target_include_directories(export_results_panel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(export_results_panel_tests PRIVATE Qt6::Test Qt6::Widgets)
    add_test(NAME export_results_panel COMMAND export_results_panel_tests)
    set_tests_properties(export_results_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(psd_print_dialog_tests tests/psd_print_dialog_tests.cpp ui/cpsdprintdialog.cpp)
    target_include_directories(psd_print_dialog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(psd_print_dialog_tests PRIVATE Qt6::Test Qt6::Widgets)
    add_test(NAME psd_print_dialog COMMAND psd_print_dialog_tests)
    set_tests_properties(psd_print_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(param_panel_tests tests/param_panel_tests.cpp tests/param_panel_runtime_stubs.cpp
        ui/cparampanel.cpp ui/cxyparameterpad.cpp ui/para/ccidjson.cpp)
    target_link_libraries(param_panel_tests PRIVATE model_data Qt6::Test Qt6::Widgets Qt6::OpenGL)
    target_compile_definitions(param_panel_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME param_panel COMMAND param_panel_tests)
    set_tests_properties(param_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(blend_shape_panel_tests tests/blend_shape_panel_tests.cpp
        ui/cblendshapepanel.cpp ui/para/ccidjson.cpp)
    target_link_libraries(blend_shape_panel_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(blend_shape_panel_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME blend_shape_panel COMMAND blend_shape_panel_tests)
    set_tests_properties(blend_shape_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
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
    add_executable(cmo3_physics_tests tests/cmo3_physics_tests.cpp)
    target_link_libraries(cmo3_physics_tests PRIVATE cmo3_data Qt6::Test)
    add_test(NAME cmo3_physics COMMAND cmo3_physics_tests)
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
    target_link_libraries(psd_data PUBLIC Qt6::Core Qt6::Gui export_control PRIVATE psd_zip)
    if(MSVC)
        target_compile_options(psd_data PUBLIC /utf-8)
    endif()
    add_executable(psd_tests tests/psd_tests.cpp assets/icc.qrc)
    set_target_properties(psd_tests PROPERTIES AUTORCC ON)
    target_link_libraries(psd_tests PRIVATE psd_data Qt6::Test)
    add_test(NAME psd COMMAND psd_tests)
    add_executable(psd_cooperative_gl_tests tests/psd_cooperative_gl_tests.cpp
        ${cubism_model_test_sources}
        framework/Rendering/CubismRenderer.cpp
        framework/Rendering/OpenGL/CubismRenderer_OpenGLES2.cpp
        framework/Rendering/OpenGL/CubismShader_OpenGLES2.cpp
        framework/Math/CubismMatrix44.cpp framework/Type/csmRectF.cpp
        opengl/ccubismshaderprograms.cpp opengl/cglrenderstate.cpp
        opengl/coffscreenbuffer.cpp opengl/creadopengl.cpp
        opengl/cpsdfiletask.cpp opengl/cpsdrendersession.cpp opengl/cpsdwritelist.cpp
        ui/para/ccidjson.cpp assets/icc.qrc)
    set_target_properties(psd_cooperative_gl_tests PROPERTIES AUTORCC ON)
    target_link_libraries(psd_cooperative_gl_tests PRIVATE psd_data model_data export_task_runner
        Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Test Qt6::Core5Compat)
    add_test(NAME psd_cooperative_gl COMMAND psd_cooperative_gl_tests)
    add_executable(rendered_psd_tests tests/rendered_psd_tests.cpp
        opengl/creadopengl.cpp opengl/coffscreenbuffer.cpp opengl/ccubismshaderprograms.cpp assets/icc.qrc)
    set_target_properties(rendered_psd_tests PROPERTIES AUTORCC ON)
    target_link_libraries(rendered_psd_tests PRIVATE model_data psd_data Qt6::OpenGL Qt6::Test)
    add_test(NAME rendered_psd COMMAND rendered_psd_tests)
    add_executable(psd_native_file_tests tests/psd_native_file_tests.cpp)
    target_link_libraries(psd_native_file_tests PRIVATE psd_data Qt6::Test)
    add_test(NAME psd_native_file COMMAND psd_native_file_tests)

endif()
