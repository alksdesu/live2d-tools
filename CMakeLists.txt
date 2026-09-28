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

add_library(export_control STATIC std/cexportcontrol.cpp std/cexportdiagnostics.cpp std/cexportsourceinfo.cpp
    std/cpsdexportcoverage.cpp psd/cpsdpreview.h)
target_include_directories(export_control PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(export_control PUBLIC Qt6::Core)
add_library(export_task_runner STATIC ui/cexporttaskrunner.cpp)
target_link_libraries(export_task_runner PUBLIC export_control)
if(MSVC)
    target_compile_options(export_control PUBLIC /utf-8)
endif()

add_library(texture_data STATIC std/cpngimage.cpp live2dstruct/cmaterialvalidationcache.cpp live2dstruct/ctextureworkspace.cpp live2dstruct/ctextureassets.cpp live2dstruct/ctextureeditcontroller.cpp)
target_include_directories(texture_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(texture_data PUBLIC Qt6::Core Qt6::Gui)

add_library(model_data STATIC
    live2dstruct/cparentgraph.cpp
    live2dstruct/cblendshaperelations.cpp
    live2d/cdeformerdrag.cpp
    opengl/cmeshbounds.cpp
    opengl/cuvgeometry.cpp
    live2dstruct/ccanvasgeometry.cpp
    live2dstruct/cdraworder.cpp
    live2dstruct/cmeshtopology.cpp
    live2dstruct/cmesheditplanner.cpp
    live2dstruct/cvertexselection.cpp
    live2dstruct/calphameshplanner.cpp
    live2dstruct/calphameshsource.cpp
    live2dstruct/cmotiontimeline.cpp
    live2dstruct/cmotiontangent.cpp
    live2dstruct/conionskin.cpp
    live2dstruct/cmotionkeyclipboard.cpp
    live2dstruct/cmotionbezier.cpp
    live2dstruct/cmotionblendpreview.cpp
    live2dstruct/cmodeldocument.cpp
    live2dstruct/cdeformerdisplaynames.cpp live2dstruct/cmodeldisplaynames.cpp live2dstruct/cdisplaynamemetadata.cpp live2dstruct/cmodelexportsnapshot.cpp
    live2dstruct/cmodeldocumentnames.cpp
    live2dstruct/cmodeldocumentmasks.cpp
    live2dstruct/cwarpgridresampler.cpp
    live2dstruct/cmodelmaterialedit.cpp
    live2dstruct/cmodelartwork.cpp
    live2dstruct/cmodeldocumentartwork.cpp
    live2dstruct/cartworkreconciler.cpp
    live2dstruct/cartworkpackage.cpp
    live2dstruct/cmaterialresourcebudget.cpp
    live2dstruct/cmocmaterialbuilder.cpp
    live2dstruct/cmodeldocumentmaterial.cpp
    live2dstruct/cmodelatlasedit.cpp
    live2dstruct/cmodeldocumentatlas.cpp
    live2dstruct/catlaseditcontroller.cpp
    live2dstruct/catlasrepackplanner.cpp
    live2dstruct/cmaterialeditcontroller.cpp
    live2dstruct/cuveditdraft.cpp
    live2dstruct/cmodeldocumentstructure.cpp
    live2dstruct/cmodeldocumentmeshverification.cpp
    live2dstruct/cmodeldocumentorganization.cpp
    live2dstruct/cmodelgeometryedit.cpp
    live2dstruct/cmodeldocumentgeometry.cpp
    live2dstruct/cgeometryobjectpairs.cpp
    live2dstruct/cmodeldocumentpairs.cpp
    live2dstruct/cmodelsavefingerprint.cpp
    live2dstruct/cparameterorganization.cpp
    live2dstruct/cparametervalueeditcontroller.cpp
    live2dstruct/cparameterselectionquery.cpp
    live2dstruct/cparameterreferences.cpp
    live2dstruct/cparameterreferenceeditor.cpp
    live2dstruct/cparameterresourceedits.cpp
    live2d/cparameterreferenceplan.cpp
    live2dstruct/ckeyformbindinggrid.cpp
    live2dstruct/ckeyformgridsnapshot.cpp
    live2dstruct/cmocpreviewbuilder.cpp
    live2dstruct/cmocruntimebuilder.cpp
    live2dstruct/cmocruntimesnapshot.cpp
    live2dstruct/cmocdeformerorder.cpp
    live2dstruct/cruntimepackage.cpp
    live2dstruct/cphysicsdocument.cpp
    live2dstruct/cmocparameterstructurebuilder.cpp
    live2dstruct/cmocmeshstructurebuilder.cpp
    live2dstruct/cmocdeformerstructurebuilder.cpp
    live2dstruct/cdeformerstructurereplay.cpp
    live2dstruct/cartmeshgeometrylayout.cpp
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
target_link_libraries(model_data PUBLIC Qt6::Core export_control texture_data)
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
    live2dstruct/cdeformereditcontext.cpp
    live2dstruct/crotationframegeometry.cpp
    live2dstruct/cgeometrypairplanner.cpp
    live2dstruct/ckeyformpairing.cpp
    live2dstruct/cdeformereditcontext.h
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

add_library(psd_reader_data STATIC psd/cpsdreader.cpp psd/cpsdreader.h psd/cpsdreaddocument.h psd/cpsdsnapshot.h psd/cpsdoriginal.cpp)
target_include_directories(psd_reader_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(psd_reader_data PUBLIC Qt6::Core Qt6::Gui export_control PRIVATE psd_zip)

add_library(artwork_import_data STATIC live2dstruct/cartworksourceimport.cpp live2dstruct/cpsdlayermatcher.cpp live2dstruct/cartworksampler.cpp live2dstruct/cartworkupdateplanner.cpp)
target_include_directories(artwork_import_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(artwork_import_data PUBLIC model_data psd_reader_data cmo3_data)
add_library(artwork_recovery_data STATIC live2dstruct/cartworkpsdrecovery.cpp)
target_include_directories(artwork_recovery_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(artwork_recovery_data PUBLIC model_data psd_reader_data cmo3_data)

add_library(cmo3_data STATIC
    cmo3/ccaffarchive.cpp
    cmo3/ccmo3project.cpp
    cmo3/ccmo3artworkreader.cpp
    cmo3/ccmo3namereader.cpp
    cmo3/ccmo3entities.cpp
    cmo3/ccmo3builder.cpp
    cmo3/cxsxmlwriter.cpp
    opengl/ctextureextractor.cpp
)
target_include_directories(cmo3_data PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(cmo3_data PUBLIC model_data Qt6::Core Qt6::Gui)
target_link_libraries(cmo3_data PRIVATE psd_zip)

add_library(ui_inspector STATIC
    ui/cinspectorpanel.cpp ui/ckeyformmatrix.cpp ui/cbindingkeysdialog.cpp
    ui/cmodeldiagnosticspanel.cpp
    ui/cuserdatapanel.cpp
    ui/cparampanel.cpp
    ui/cxyparameterpad.cpp
)
target_include_directories(ui_inspector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ui_inspector PUBLIC model_data Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL)

if(BUILD_TESTING)
    add_executable(deformer_order_moc_tests tests/deformer_order_moc_tests.cpp)
    target_link_libraries(deformer_order_moc_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(deformer_order_moc_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME deformer_order_moc COMMAND deformer_order_moc_tests)
    add_executable(deformer_order_document_tests tests/deformer_order_document_tests.cpp)
    target_link_libraries(deformer_order_document_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(deformer_order_document_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME deformer_order_document COMMAND deformer_order_document_tests)
    if(TARGET live2d_core)
        add_library(warp_grid_preview STATIC live2d/cwarpgridpreview.cpp)
        target_include_directories(warp_grid_preview PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(warp_grid_preview PUBLIC Qt6::Core Qt6::Gui export_control live2d_core)
        add_executable(warp_grid_preview_core_tests tests/warp_grid_preview_core_tests.cpp)
        target_link_libraries(warp_grid_preview_core_tests PRIVATE warp_grid_preview model_data Qt6::Test)
        target_compile_definitions(warp_grid_preview_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME warp_grid_preview_core COMMAND warp_grid_preview_core_tests)
        add_executable(warp_grid_structure_core_tests tests/warp_grid_structure_core_tests.cpp)
        target_link_libraries(warp_grid_structure_core_tests PRIVATE model_data model_coordinates live2d_core Qt6::Test)
        target_compile_definitions(warp_grid_structure_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME warp_grid_structure_core COMMAND warp_grid_structure_core_tests)
        add_executable(deformer_order_moc_core_tests tests/deformer_order_moc_core_tests.cpp)
        target_link_libraries(deformer_order_moc_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(deformer_order_moc_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME deformer_order_moc_core COMMAND deformer_order_moc_core_tests)
    endif()
    add_executable(moc_mesh_structure_tests tests/mesh_structure_moc_tests.cpp)
    target_link_libraries(moc_mesh_structure_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(moc_mesh_structure_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME moc_mesh_structure COMMAND moc_mesh_structure_tests)
    foreach(deformer_test IN ITEMS deformer_names_document deformer_column_state deformer_structure_builder deformer_structure_replay deformer_structure_document)
        add_executable(${deformer_test}_tests tests/${deformer_test}_tests.cpp)
        target_link_libraries(${deformer_test}_tests PRIVATE model_data Qt6::Test)
        target_compile_definitions(${deformer_test}_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME ${deformer_test} COMMAND ${deformer_test}_tests)
    endforeach()
    add_executable(mesh_edit_planner_tests tests/mesh_edit_planner_tests.cpp)
    target_link_libraries(mesh_edit_planner_tests PRIVATE model_data Qt6::Test)
    add_test(NAME mesh_edit_planner COMMAND mesh_edit_planner_tests)
    add_executable(alpha_mesh_planner_tests tests/alpha_mesh_planner_tests.cpp)
    target_link_libraries(alpha_mesh_planner_tests PRIVATE model_data Qt6::Test)
    add_test(NAME alpha_mesh_planner COMMAND alpha_mesh_planner_tests)
    add_executable(alpha_mesh_source_tests tests/alpha_mesh_source_tests.cpp)
    target_link_libraries(alpha_mesh_source_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(alpha_mesh_source_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME alpha_mesh_source COMMAND alpha_mesh_source_tests)
    if(TARGET live2d_core)
        add_executable(deformer_column_state_core_tests tests/deformer_column_state_tests.cpp)
        target_link_libraries(deformer_column_state_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(deformer_column_state_core_tests PRIVATE LIVE2D_TEST_CORE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME deformer_column_state_core COMMAND deformer_column_state_core_tests)
        add_executable(deformer_structure_replay_core_tests tests/deformer_structure_replay_core_tests.cpp)
        target_link_libraries(deformer_structure_replay_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(deformer_structure_replay_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME deformer_structure_replay_core COMMAND deformer_structure_replay_core_tests)
        add_executable(deformer_structure_core_tests tests/deformer_structure_core_tests.cpp)
        target_link_libraries(deformer_structure_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(deformer_structure_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME deformer_structure_core COMMAND deformer_structure_core_tests)
        add_executable(keyform_mapping_core_tests tests/keyform_mapping_core_tests.cpp)
        target_link_libraries(keyform_mapping_core_tests PRIVATE deformer_tree_data live2d_core Qt6::Test)
        target_compile_definitions(keyform_mapping_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME keyform_mapping_core COMMAND keyform_mapping_core_tests)
        add_executable(rotation_frame_core_tests tests/rotation_frame_core_tests.cpp)
        target_link_libraries(rotation_frame_core_tests PRIVATE deformer_tree_data live2d_core Qt6::Test)
        target_compile_definitions(rotation_frame_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME rotation_frame_core COMMAND rotation_frame_core_tests)
        add_executable(geometry_pair_planner_core_tests tests/geometry_pair_planner_core_tests.cpp)
        target_link_libraries(geometry_pair_planner_core_tests PRIVATE deformer_tree_data live2d_core Qt6::Test)
        target_compile_definitions(geometry_pair_planner_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME geometry_pair_planner_core COMMAND geometry_pair_planner_core_tests)
        add_executable(wide_mesh_workflow_tests tests/wide_mesh_workflow_tests.cpp)
        target_link_libraries(wide_mesh_workflow_tests PRIVATE deformer_tree_data live2d_core Qt6::Test)
        target_compile_definitions(wide_mesh_workflow_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME wide_mesh_workflow COMMAND wide_mesh_workflow_tests)
        add_executable(deformer_edit_context_core_tests tests/deformer_edit_context_core_tests.cpp)
        target_link_libraries(deformer_edit_context_core_tests PRIVATE deformer_tree_data live2d_core Qt6::Test)
        target_compile_definitions(deformer_edit_context_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME deformer_edit_context_core COMMAND deformer_edit_context_core_tests)
        add_executable(cmo3_canvas_core_tests tests/cmo3_canvas_core_tests.cpp)
        target_link_libraries(cmo3_canvas_core_tests PRIVATE cmo3_data live2d_core Qt6::Test)
        target_compile_definitions(cmo3_canvas_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME cmo3_canvas_core COMMAND cmo3_canvas_core_tests)
        add_executable(mesh_edit_planner_core_tests tests/mesh_edit_planner_core_tests.cpp)
        target_link_libraries(mesh_edit_planner_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(mesh_edit_planner_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME mesh_edit_planner_core COMMAND mesh_edit_planner_core_tests)
        add_executable(alpha_mesh_core_tests tests/alpha_mesh_core_tests.cpp)
        target_link_libraries(alpha_mesh_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(alpha_mesh_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME alpha_mesh_core COMMAND alpha_mesh_core_tests)
        add_executable(moc_material_core_tests tests/material_moc_core_tests.cpp)
        target_link_libraries(moc_material_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(moc_material_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME moc_material_core COMMAND moc_material_core_tests)
        add_executable(moc_mesh_structure_core_tests tests/mesh_structure_moc_core_tests.cpp)
        target_link_libraries(moc_mesh_structure_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(moc_mesh_structure_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME moc_mesh_structure_core COMMAND moc_mesh_structure_core_tests)
        add_executable(mesh_geometry_replacement_core_tests tests/mesh_geometry_replacement_core_tests.cpp
            framework/CubismFramework.cpp framework/Model/CubismMoc.cpp framework/Model/CubismModel.cpp
            framework/Model/CubismModelReplacement.cpp framework/Rendering/CubismRenderer.cpp
            framework/Id/CubismId.cpp framework/Id/CubismIdManager.cpp framework/Type/csmString.cpp
            framework/Utils/CubismJson.cpp framework/Utils/CubismString.cpp framework/Utils/CubismDebug.cpp
            framework/Math/CubismMath.cpp framework/Math/CubismVector2.cpp framework/Math/CubismMatrix44.cpp
            framework/Type/csmRectF.cpp edit/cmeshmap.cpp edit/cdrawmesh.cpp edit/ctriangl.cpp opengl/cglmeshbuffer.cpp)
        target_link_libraries(mesh_geometry_replacement_core_tests PRIVATE model_data live2d_core Qt6::Gui Qt6::OpenGL Qt6::Test)
        target_compile_definitions(mesh_geometry_replacement_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME mesh_geometry_replacement_core COMMAND mesh_geometry_replacement_core_tests)
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
        add_executable(binding_keys_core_tests tests/binding_keys_core_tests.cpp)
        target_link_libraries(binding_keys_core_tests PRIVATE model_data live2d_core Qt6::Test)
        target_compile_definitions(binding_keys_core_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME binding_keys_core COMMAND binding_keys_core_tests)
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
    add_executable(cmo3_project_tests tests/cmo3_project_tests.cpp)
    target_link_libraries(cmo3_project_tests PRIVATE cmo3_data Qt6::Test)
    target_compile_definitions(cmo3_project_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME cmo3_project COMMAND cmo3_project_tests)
    add_executable(cmo_source_import_dialog_tests tests/cmo_source_import_dialog_tests.cpp ui/ccmosourceimportdialog.cpp)
    target_link_libraries(cmo_source_import_dialog_tests PRIVATE cmo3_data Qt6::Test Qt6::Widgets)
    add_test(NAME cmo_source_import_dialog COMMAND cmo_source_import_dialog_tests)
    set_tests_properties(cmo_source_import_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(cmo3_artwork_reader_tests tests/cmo3_artwork_reader_tests.cpp)
    target_link_libraries(cmo3_artwork_reader_tests PRIVATE cmo3_data Qt6::Test)
    add_test(NAME cmo3_artwork_reader COMMAND cmo3_artwork_reader_tests)

    add_executable(warp_grid_structure_tests tests/warp_grid_structure_tests.cpp)
    target_link_libraries(warp_grid_structure_tests PRIVATE model_data model_coordinates Qt6::Test)
    target_compile_definitions(warp_grid_structure_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME warp_grid_structure COMMAND warp_grid_structure_tests)
    add_executable(warp_grid_document_tests tests/warp_grid_document_tests.cpp)
    target_link_libraries(warp_grid_document_tests PRIVATE deformer_tree_data model_coordinates Qt6::Test)
    target_compile_definitions(warp_grid_document_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME warp_grid_document COMMAND warp_grid_document_tests)
    add_executable(mask_source_policy_tests tests/mask_source_policy_tests.cpp)
    target_link_libraries(mask_source_policy_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(mask_source_policy_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME mask_source_policy COMMAND mask_source_policy_tests)
    foreach(data_test IN ITEMS warp_grid_resampler parent_graph blend_shape_relations visibility_edit model_resource_path directory_export runtime_moc runtime_moc_snapshot runtime_package physics_document parameter_references parameter_reference_editor parameter_resource_edits parameter_reference_plan parameter_structure parameter_structure_document parameter_delete mesh_structure_session)
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
    add_executable(object_names_import_dialog_tests tests/object_names_import_dialog_tests.cpp ui/cobjectnamesimportdialog.cpp)
    target_link_libraries(object_names_import_dialog_tests PRIVATE model_data Qt6::Widgets Qt6::Test)
    add_test(NAME object_names_import_dialog COMMAND object_names_import_dialog_tests)
    set_tests_properties(object_names_import_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(model_display_names_tests tests/model_display_names_tests.cpp)
    target_link_libraries(model_display_names_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(model_display_names_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME model_display_names COMMAND model_display_names_tests)
    add_executable(model_export_snapshot_tests tests/model_export_snapshot_tests.cpp)
    target_link_libraries(model_export_snapshot_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(model_export_snapshot_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME model_export_snapshot COMMAND model_export_snapshot_tests)
    add_executable(cmo3_name_reader_tests tests/cmo3_name_reader_tests.cpp)
    target_link_libraries(cmo3_name_reader_tests PRIVATE cmo3_data Qt6::Test)
    target_compile_definitions(cmo3_name_reader_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME cmo3_name_reader COMMAND cmo3_name_reader_tests)
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
    add_executable(vertex_selection_tests tests/vertex_selection_tests.cpp)
    target_link_libraries(vertex_selection_tests PRIVATE model_data Qt6::Test)
    add_test(NAME vertex_selection COMMAND vertex_selection_tests)
    add_executable(vertex_selection_tools_tests tests/vertex_selection_tools_tests.cpp ui/cvertexselectiontools.cpp)
    target_link_libraries(vertex_selection_tools_tests PRIVATE model_data Qt6::Widgets Qt6::Test)
    add_test(NAME vertex_selection_tools COMMAND vertex_selection_tools_tests)
    set_tests_properties(vertex_selection_tools PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(vertex_selection_overlay_tests tests/vertex_selection_overlay_tests.cpp opengl/cvertexselectionoverlay.cpp)
    target_link_libraries(vertex_selection_overlay_tests PRIVATE model_data Qt6::Widgets Qt6::Test)
    add_test(NAME vertex_selection_overlay COMMAND vertex_selection_overlay_tests)
    set_tests_properties(vertex_selection_overlay PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(deformer_handle_overlay_tests tests/deformer_handle_overlay_tests.cpp
        opengl/cdeformerhandleoverlay.cpp opengl/ccanvascoordinates.cpp)
    target_include_directories(deformer_handle_overlay_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(deformer_handle_overlay_tests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME deformer_handle_overlay COMMAND deformer_handle_overlay_tests)
    set_tests_properties(deformer_handle_overlay PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(mesh_toolbar_tests tests/mesh_toolbar_tests.cpp ui/cmeshtoolbar.cpp)
    target_include_directories(mesh_toolbar_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(mesh_toolbar_tests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME mesh_toolbar COMMAND mesh_toolbar_tests)
    set_tests_properties(mesh_toolbar PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(alpha_mesh_dialog_tests tests/alpha_mesh_dialog_tests.cpp ui/calphameshdialog.cpp)
    target_link_libraries(alpha_mesh_dialog_tests PRIVATE model_data Qt6::Widgets Qt6::Test)
    add_test(NAME alpha_mesh_dialog COMMAND alpha_mesh_dialog_tests)
    set_tests_properties(alpha_mesh_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(dock_toggle_tests tests/dock_toggle_tests.cpp)
    target_include_directories(dock_toggle_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(dock_toggle_tests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME dock_toggle COMMAND dock_toggle_tests)
    set_tests_properties(dock_toggle PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(workspace_layout_tests tests/workspace_layout_tests.cpp ui/cworkspacelayouts.cpp)
    target_include_directories(workspace_layout_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(workspace_layout_tests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME workspace_layout COMMAND workspace_layout_tests)
    set_tests_properties(workspace_layout PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    foreach(keyform_test IN ITEMS keyform_mapping keyform_edit_controller keyform_logical_state geometry_batch_document geometry_batch_controller geometry_pair_controller rotation_frame geometry_pair_planner deformer_edit_context deformer_edit_controller parameter_definition_edit parameter_structure_controller mesh_structure_controller deformer_structure_controller tree_snapshot mesh_transaction)
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
    add_executable(clipping_editor_tests tests/clipping_editor_tests.cpp ui/cinspectorpanel.cpp ui/ckeyformmatrix.cpp ui/cbindingkeysdialog.cpp)
    target_link_libraries(clipping_editor_tests PRIVATE deformer_tree_data cmo3_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(clipping_editor_tests PRIVATE TEST_RESOURCES=\"${CMAKE_CURRENT_SOURCE_DIR}/Resources\")
    add_test(NAME clipping_editor COMMAND clipping_editor_tests)
    add_executable(model_diagnostics_panel_tests tests/model_diagnostics_panel_tests.cpp ui/cmodeldiagnosticspanel.cpp)
    target_link_libraries(model_diagnostics_panel_tests PRIVATE model_data Qt6::Widgets Qt6::Test)
    target_compile_definitions(model_diagnostics_panel_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME model_diagnostics_panel COMMAND model_diagnostics_panel_tests)
    set_tests_properties(model_diagnostics_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    set_tests_properties(clipping_editor PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(deformer_editor_tests tests/deformer_editor_tests.cpp ui/cinspectorpanel.cpp ui/ckeyformmatrix.cpp ui/cbindingkeysdialog.cpp
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
        target_link_libraries(${texture_variant}_tests PRIVATE texture_data Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Test)
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
        live2d/LAppModelUpdate.cpp live2d/LAppModelParameterValues.cpp ${cubism_model_test_sources} opengl/coffscreenbuffer.cpp)
    target_link_libraries(lapp_export_preview_tests PRIVATE model_data Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME lapp_export_preview COMMAND lapp_export_preview_tests)
    set_tests_properties(lapp_export_preview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(model_runtime_state_tests tests/model_runtime_state_tests.cpp ${cubism_model_test_sources})
    target_link_libraries(model_runtime_state_tests PRIVATE model_data Qt6::Gui Qt6::OpenGL Qt6::Test)
    add_test(NAME model_runtime_state COMMAND model_runtime_state_tests)
    add_executable(model_replacement_tests tests/model_replacement_tests.cpp ${cubism_model_test_sources}
        live2d/LAppModelParameterDefinitions.cpp opengl/coffscreenbuffer.cpp
        live2d/LAppModelParameterStructure.cpp live2d/LAppModelMeshGeometry.cpp framework/Model/CubismUserModelReplacement.cpp
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
    add_executable(psd_reader_tests tests/psd_reader_tests.cpp)
    target_link_libraries(psd_reader_tests PRIVATE psd_reader_data Qt6::Test)
    add_test(NAME psd_reader COMMAND psd_reader_tests)
    add_executable(psd_stream_reader_tests tests/psd_stream_reader_tests.cpp)
    target_link_libraries(psd_stream_reader_tests PRIVATE psd_reader_data Qt6::Test)
    add_test(NAME psd_stream_reader COMMAND psd_stream_reader_tests)
    add_executable(psd_original_tests tests/psd_original_tests.cpp)
    target_link_libraries(psd_original_tests PRIVATE psd_reader_data Qt6::Test)
    add_test(NAME psd_original COMMAND psd_original_tests)
    add_executable(artwork_psd_recovery_tests tests/artwork_psd_recovery_tests.cpp)
    target_link_libraries(artwork_psd_recovery_tests PRIVATE artwork_recovery_data artwork_import_data psd_data Qt6::Test)
    add_test(NAME artwork_psd_recovery COMMAND artwork_psd_recovery_tests)
    add_executable(model_artwork_tests tests/model_artwork_tests.cpp)
    target_link_libraries(model_artwork_tests PRIVATE model_data Qt6::Test)
    add_test(NAME model_artwork COMMAND model_artwork_tests)
    add_executable(artwork_document_tests tests/artwork_document_tests.cpp)
    target_link_libraries(artwork_document_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(artwork_document_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME artwork_document COMMAND artwork_document_tests)
    add_executable(artwork_package_tests tests/artwork_package_tests.cpp)
    target_link_libraries(artwork_package_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(artwork_package_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME artwork_package COMMAND artwork_package_tests)
    add_executable(material_resource_budget_tests tests/material_resource_budget_tests.cpp)
    target_link_libraries(material_resource_budget_tests PRIVATE model_data Qt6::Test)
    add_test(NAME material_resource_budget COMMAND material_resource_budget_tests)
    add_executable(artwork_reconciler_tests tests/artwork_reconciler_tests.cpp)
    target_link_libraries(artwork_reconciler_tests PRIVATE model_data Qt6::Test)
    add_test(NAME artwork_reconciler COMMAND artwork_reconciler_tests)
    add_executable(cmo_source_library_tests tests/cmo_source_library_tests.cpp)
    target_link_libraries(cmo_source_library_tests PRIVATE artwork_import_data artwork_recovery_data Qt6::Test)
    add_test(NAME cmo_source_library COMMAND cmo_source_library_tests)
    add_executable(artwork_source_import_tests tests/artwork_source_import_tests.cpp)
    target_link_libraries(artwork_source_import_tests PRIVATE artwork_import_data Qt6::Test)
    add_test(NAME artwork_source_import COMMAND artwork_source_import_tests)
    add_executable(psd_layer_matcher_tests tests/psd_layer_matcher_tests.cpp)
    target_link_libraries(psd_layer_matcher_tests PRIVATE artwork_import_data Qt6::Test)
    add_test(NAME psd_layer_matcher COMMAND psd_layer_matcher_tests)
    add_executable(artwork_sampler_tests tests/artwork_sampler_tests.cpp)
    target_link_libraries(artwork_sampler_tests PRIVATE artwork_import_data Qt6::Test)
    add_test(NAME artwork_sampler COMMAND artwork_sampler_tests)
    add_executable(artwork_update_planner_tests tests/artwork_update_planner_tests.cpp)
    target_link_libraries(artwork_update_planner_tests PRIVATE artwork_import_data Qt6::Test)
    add_test(NAME artwork_update_planner COMMAND artwork_update_planner_tests)
    add_executable(psd_import_dialog_tests tests/psd_import_dialog_tests.cpp ui/cpsdimportdialog.cpp ui/cartworksourceview.cpp)
    target_link_libraries(psd_import_dialog_tests PRIVATE artwork_import_data Qt6::Test Qt6::Widgets)
    add_test(NAME psd_import_dialog COMMAND psd_import_dialog_tests)
    set_tests_properties(psd_import_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(artwork_source_view_tests tests/artwork_source_view_tests.cpp ui/cartworksourceview.cpp opengl/cuvgeometry.cpp)
    target_include_directories(artwork_source_view_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(artwork_source_view_tests PRIVATE Qt6::Test Qt6::Widgets)
    if(MSVC)
        target_compile_options(artwork_source_view_tests PRIVATE /utf-8)
    endif()
    add_test(NAME artwork_source_view COMMAND artwork_source_view_tests)
    set_tests_properties(artwork_source_view PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(texture_material_tests tests/texture_material_tests.cpp)
    target_link_libraries(texture_material_tests PRIVATE cmo3_data Qt6::Test)
    target_compile_definitions(texture_material_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME texture_material COMMAND texture_material_tests)
    add_executable(uv_geometry_tests tests/uv_geometry_tests.cpp)
    target_link_libraries(uv_geometry_tests PRIVATE model_data Qt6::Test)
    add_test(NAME uv_geometry COMMAND uv_geometry_tests)
    add_executable(texture_session_tests tests/texture_session_tests.cpp)
    target_link_libraries(texture_session_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(texture_session_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME texture_session COMMAND texture_session_tests)
    add_executable(texture_assets_tests tests/texture_assets_tests.cpp)
    target_link_libraries(texture_assets_tests PRIVATE model_data Qt6::Test)
    add_test(NAME texture_assets COMMAND texture_assets_tests)
    add_executable(texture_replacement_panel_tests tests/texture_replacement_panel_tests.cpp ui/ctexturepanel.cpp)
    target_link_libraries(texture_replacement_panel_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    add_test(NAME texture_replacement_panel COMMAND texture_replacement_panel_tests)
    set_tests_properties(texture_replacement_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
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
    add_executable(keyform_grid_snapshot_tests tests/keyform_grid_snapshot_tests.cpp)
    target_link_libraries(keyform_grid_snapshot_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(keyform_grid_snapshot_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME keyform_grid_snapshot COMMAND keyform_grid_snapshot_tests)
    add_executable(binding_keys_moc_tests tests/binding_keys_moc_tests.cpp)
    target_link_libraries(binding_keys_moc_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(binding_keys_moc_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME binding_keys_moc COMMAND binding_keys_moc_tests)
    add_executable(keyform_matrix_tests tests/keyform_matrix_tests.cpp ui/ckeyformmatrix.cpp)
    target_link_libraries(keyform_matrix_tests PRIVATE model_data Qt6::Widgets Qt6::Test)
    target_compile_definitions(keyform_matrix_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME keyform_matrix COMMAND keyform_matrix_tests)
    set_tests_properties(keyform_matrix PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(deformer_grid_dialog_tests tests/deformer_grid_dialog_tests.cpp ui/cdeformergriddialog.cpp)
    target_include_directories(deformer_grid_dialog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(deformer_grid_dialog_tests PRIVATE Qt6::Widgets Qt6::Test)
    add_test(NAME deformer_grid_dialog COMMAND deformer_grid_dialog_tests)
    set_tests_properties(deformer_grid_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(deformer_rename_dialog_tests tests/deformer_rename_dialog_tests.cpp ui/cdeformerrenamedialog.cpp)
    target_link_libraries(deformer_rename_dialog_tests PRIVATE model_data Qt6::Widgets Qt6::Test)
    add_test(NAME deformer_rename_dialog COMMAND deformer_rename_dialog_tests)
    set_tests_properties(deformer_rename_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(deformer_create_dialog_tests tests/deformer_create_dialog_tests.cpp ui/cdeformercreatedialog.cpp)
    target_link_libraries(deformer_create_dialog_tests PRIVATE model_data Qt6::Widgets Qt6::Test)
    target_compile_definitions(deformer_create_dialog_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME deformer_create_dialog COMMAND deformer_create_dialog_tests)
    set_tests_properties(deformer_create_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(binding_keys_dialog_tests tests/binding_keys_dialog_tests.cpp ui/cbindingkeysdialog.cpp)
    target_link_libraries(binding_keys_dialog_tests PRIVATE model_data Qt6::Widgets Qt6::Test)
    target_compile_definitions(binding_keys_dialog_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME binding_keys_dialog COMMAND binding_keys_dialog_tests)
    set_tests_properties(binding_keys_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(inspector_history_tests tests/inspector_history_tests.cpp)
    target_sources(inspector_history_tests PRIVATE ui/cinspectorpanel.cpp ui/ckeyformmatrix.cpp ui/cbindingkeysdialog.cpp)
    target_link_libraries(inspector_history_tests PRIVATE deformer_tree_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(inspector_history_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME inspector_history COMMAND inspector_history_tests)
    set_tests_properties(inspector_history PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(deformer_tree_actions_tests tests/deformer_tree_actions_tests.cpp ui/cmodeltreepanel.cpp ui/para/ccidjson.cpp)
    target_link_libraries(deformer_tree_actions_tests PRIVATE model_data model_hierarchy_ui Qt6::Test Qt6::Widgets)
    target_compile_definitions(deformer_tree_actions_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME deformer_tree_actions COMMAND deformer_tree_actions_tests)
    set_tests_properties(deformer_tree_actions PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(model_tree_tests tests/model_tree_tests.cpp ui/cmodeltreepanel.cpp ui/cinspectorpanel.cpp ui/ckeyformmatrix.cpp ui/cbindingkeysdialog.cpp ui/para/ccidjson.cpp)
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
    target_link_libraries(psd_batch_worker_tests PRIVATE export_control Qt6::Test Qt6::Core)
    add_test(NAME psd_batch_worker COMMAND psd_batch_worker_tests)
    add_executable(psd_export_coverage_tests tests/psd_export_coverage_tests.cpp)
    target_link_libraries(psd_export_coverage_tests PRIVATE export_control Qt6::Test)
    add_test(NAME psd_export_coverage COMMAND psd_export_coverage_tests)
    add_executable(export_diagnostics_tests tests/export_diagnostics_tests.cpp)
    target_link_libraries(export_diagnostics_tests PRIVATE export_control Qt6::Test)
    add_test(NAME export_diagnostics COMMAND export_diagnostics_tests)
    add_executable(export_results_panel_tests tests/export_results_panel_tests.cpp
        ui/cexportresultspanel.cpp ui/psdbatchresult.cpp)
    target_include_directories(export_results_panel_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(export_results_panel_tests PRIVATE export_control Qt6::Test Qt6::Widgets)
    add_test(NAME export_results_panel COMMAND export_results_panel_tests)
    set_tests_properties(export_results_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(psd_print_dialog_tests tests/psd_print_dialog_tests.cpp ui/cpsdprintdialog.cpp)
    target_include_directories(psd_print_dialog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(psd_print_dialog_tests PRIVATE Qt6::Test Qt6::Widgets)
    add_test(NAME psd_print_dialog COMMAND psd_print_dialog_tests)
    set_tests_properties(psd_print_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(psd_recovery_dialog_tests tests/psd_recovery_dialog_tests.cpp ui/cpsdrecoverydialog.cpp)
    target_include_directories(psd_recovery_dialog_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(psd_recovery_dialog_tests PRIVATE psd_reader_data Qt6::Test Qt6::Widgets)
    add_test(NAME psd_recovery_dialog COMMAND psd_recovery_dialog_tests)
    set_tests_properties(psd_recovery_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    foreach(parameter_test IN ITEMS atlas_edit_controller atlas_repack_planner material_document material_edit_controller uv_edit_draft parameter_organization parameter_organization_document parameter_organization_export parameter_value_edit_controller parameter_selection_query model_save_fingerprint canvas_geometry_inverse geometry_object_pairs geometry_object_pairs_document geometry_object_pairs_session)
        add_executable(${parameter_test}_tests tests/${parameter_test}_tests.cpp)
        target_link_libraries(${parameter_test}_tests PRIVATE model_data Qt6::Test)
        target_compile_definitions(${parameter_test}_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
        add_test(NAME ${parameter_test} COMMAND ${parameter_test}_tests)
    endforeach()
    add_executable(parameter_organization_dialog_tests tests/parameter_organization_dialog_tests.cpp
        ui/cparameterorganizationdialog.cpp ui/para/ccidjson.cpp)
    target_link_libraries(parameter_organization_dialog_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(parameter_organization_dialog_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME parameter_organization_dialog COMMAND parameter_organization_dialog_tests)
    set_tests_properties(parameter_organization_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(geometry_pairs_export_tests tests/geometry_pairs_export_tests.cpp ui/para/ccidjson.cpp)
    target_link_libraries(geometry_pairs_export_tests PRIVATE model_data Qt6::Test)
    target_compile_definitions(geometry_pairs_export_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME geometry_pairs_export COMMAND geometry_pairs_export_tests)
    add_executable(atlas_edit_dialog_tests tests/atlas_edit_dialog_tests.cpp ui/catlascanvasview.cpp ui/catlaseditdialog.cpp ui/cdraftdialoglifecycle.h)
    target_link_libraries(atlas_edit_dialog_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(atlas_edit_dialog_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME atlas_edit_dialog COMMAND atlas_edit_dialog_tests)
    set_tests_properties(atlas_edit_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(uv_edit_dialog_tests tests/uv_edit_dialog_tests.cpp ui/cuvcanvasview.cpp ui/cuveditdialog.cpp)
    target_link_libraries(uv_edit_dialog_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(uv_edit_dialog_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME uv_edit_dialog COMMAND uv_edit_dialog_tests)
    set_tests_properties(uv_edit_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(geometry_pairs_dialog_tests tests/geometry_pairs_dialog_tests.cpp ui/cgeometrypairsdialog.cpp ui/para/ccidjson.cpp)
    target_link_libraries(geometry_pairs_dialog_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    target_compile_definitions(geometry_pairs_dialog_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME geometry_pairs_dialog COMMAND geometry_pairs_dialog_tests)
    set_tests_properties(geometry_pairs_dialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(parameter_organization_panel_tests tests/parameter_organization_panel_tests.cpp tests/param_panel_runtime_stubs.cpp
        ui/cparampanel.cpp ui/cparampanelorganization.cpp ui/cparameterorganizationdialog.cpp ui/cxyparameterpad.cpp ui/para/ccidjson.cpp)
    target_link_libraries(parameter_organization_panel_tests PRIVATE model_data Qt6::Test Qt6::Widgets Qt6::OpenGL)
    target_compile_definitions(parameter_organization_panel_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME parameter_organization_panel COMMAND parameter_organization_panel_tests)
    set_tests_properties(parameter_organization_panel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(param_panel_tests tests/param_panel_tests.cpp tests/param_panel_runtime_stubs.cpp
        ui/cparampanel.cpp ui/cparampanelorganization.cpp ui/cparameterorganizationdialog.cpp ui/cxyparameterpad.cpp ui/para/ccidjson.cpp)
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
    add_executable(motion_batch_edit_tests tests/motion_batch_edit_tests.cpp)
    target_link_libraries(motion_batch_edit_tests PRIVATE model_data Qt6::Test)
    add_test(NAME motion_batch_edit COMMAND motion_batch_edit_tests)
    add_executable(motion_tangent_tests tests/motion_tangent_tests.cpp)
    target_link_libraries(motion_tangent_tests PRIVATE model_data Qt6::Test)
    add_test(NAME motion_tangent COMMAND motion_tangent_tests)
    add_executable(onion_skin_tests tests/onion_skin_tests.cpp)
    target_link_libraries(onion_skin_tests PRIVATE model_data Qt6::Test)
    add_test(NAME onion_skin COMMAND onion_skin_tests)
    add_executable(motion_key_clipboard_tests tests/motion_key_clipboard_tests.cpp)
    target_link_libraries(motion_key_clipboard_tests PRIVATE model_data Qt6::Test)
    add_test(NAME motion_key_clipboard COMMAND motion_key_clipboard_tests)
    add_executable(cmotionblendpreview_tests tests/cmotionblendpreview_tests.cpp)
    target_link_libraries(cmotionblendpreview_tests PRIVATE model_data Qt6::Test)
    add_test(NAME cmotionblendpreview COMMAND cmotionblendpreview_tests)
    add_executable(cmotioncurveview_tests tests/cmotioncurveview_tests.cpp ui/cmotioncurveview.cpp)
    target_include_directories(cmotioncurveview_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(cmotioncurveview_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    add_test(NAME cmotioncurveview COMMAND cmotioncurveview_tests)
    set_tests_properties(cmotioncurveview PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(motion_record_values_tests tests/motion_record_values_tests.cpp)
    target_link_libraries(motion_record_values_tests PRIVATE model_data Qt6::Test)
    add_test(NAME motion_record_values COMMAND motion_record_values_tests)
    add_executable(parameter_edit_transport_tests tests/parameter_edit_transport_tests.cpp ui/cmotionpanel.cpp ui/cmotionpanelediting.cpp ui/cmotionkeymodel.cpp ui/cmotioncurveview.cpp ui/cmotionsettingsdialog.cpp)
    target_link_libraries(parameter_edit_transport_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    add_test(NAME parameter_edit_transport COMMAND parameter_edit_transport_tests)
    set_tests_properties(parameter_edit_transport PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(motion_lifetime_tests tests/motion_lifetime_tests.cpp ui/cmotionpanel.cpp ui/cmotionpanelediting.cpp ui/cmotionkeymodel.cpp ui/cmotioncurveview.cpp ui/cmotionsettingsdialog.cpp)
    target_link_libraries(motion_lifetime_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    add_test(NAME motion_lifetime COMMAND motion_lifetime_tests)
    set_tests_properties(motion_lifetime PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(motion_workspace_tests tests/motion_workspace_tests.cpp ui/cmotionpanel.cpp ui/cmotionpanelediting.cpp ui/cmotionkeymodel.cpp ui/cmotioncurveview.cpp ui/cmotionsettingsdialog.cpp)
    target_link_libraries(motion_workspace_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    add_test(NAME motion_workspace COMMAND motion_workspace_tests)
    set_tests_properties(motion_workspace PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(motion_panel_batch_tests tests/motion_panel_batch_tests.cpp ui/cmotionpanel.cpp ui/cmotionpanelediting.cpp ui/cmotionkeymodel.cpp ui/cmotioncurveview.cpp ui/cmotionsettingsdialog.cpp)
    target_link_libraries(motion_panel_batch_tests PRIVATE model_data Qt6::Test Qt6::Widgets)
    add_test(NAME motion_panel_batch COMMAND motion_panel_batch_tests)
    set_tests_properties(motion_panel_batch PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(motion_panel_tests tests/motion_panel_tests.cpp ui/cmotionpanel.cpp ui/cmotionpanelediting.cpp ui/cmotionkeymodel.cpp ui/cmotioncurveview.cpp ui/cmotionsettingsdialog.cpp)
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
    add_executable(cmo3_material_tests tests/cmo3_material_tests.cpp)
    target_link_libraries(cmo3_material_tests PRIVATE cmo3_data Qt6::Test)
    target_compile_definitions(cmo3_material_tests PRIVATE TEST_RESOURCES="${CMAKE_CURRENT_SOURCE_DIR}/Resources")
    add_test(NAME cmo3_material COMMAND cmo3_material_tests)
    add_library(psd_data STATIC
        psd/cpsdlist.cpp
        psd/cpsdcomposite.cpp psd/cpsdcomposite.h
        psd/cpsdsourcewriter.cpp
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
    target_link_libraries(psd_data PUBLIC Qt6::Core Qt6::Gui export_control PRIVATE psd_zip texture_data)
    if(MSVC)
        target_compile_options(psd_data PUBLIC /utf-8)
    endif()
    add_executable(psd_tests tests/psd_tests.cpp assets/icc.qrc)
    set_target_properties(psd_tests PROPERTIES AUTORCC ON)
    target_link_libraries(psd_tests PRIVATE psd_data Qt6::Test)
    add_test(NAME psd COMMAND psd_tests)
    add_executable(psd_composite_tests tests/psd_composite_tests.cpp)
    target_link_libraries(psd_composite_tests PRIVATE psd_data psd_reader_data Qt6::Test)
    add_test(NAME psd_composite COMMAND psd_composite_tests)
    add_executable(psd_source_writer_tests tests/psd_source_writer_tests.cpp)
    target_link_libraries(psd_source_writer_tests PRIVATE psd_data psd_reader_data Qt6::Test)
    add_test(NAME psd_source_writer COMMAND psd_source_writer_tests)
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
