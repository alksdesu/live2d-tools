QT       += core gui opengl  gui-private network
greaterThan(QT_MAJOR_VERSION, 5): QT += openglwidgets
greaterThan(QT_MAJOR_VERSION, 5): QT += core5compat
greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

TARGET = live2d2psd

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0
RC_ICONS = assets\images\logo.ico


# Windows大对象支持

msvc {
    QMAKE_CXXFLAGS += /utf-8
}

unix {
    LIBS += -ldl
}

CONFIG(debug, debug|release): OUTPUTDIR = $${PWD}/build/DEBUG
else: CONFIG(release, debug|release): OUTPUTDIR = $${PWD}/build/RELEASE

MOC_DIR = $${OUTPUTDIR}/moc
RCC_DIR = $${OUTPUTDIR}/rcc
UI_DIR = $${OUTPUTDIR}/ui
OBJECTS_DIR = $${OUTPUTDIR}/objs

# output dir
DESTDIR = $${PWD}/softwareEnv/physicsV1

SOURCES += \
    edit/cdrawobject.cpp \
    edit/cdrawrotation.cpp \
    edit/cdrawwarp.cpp \
    edit/ceditstate.cpp \
    edit/cmeshmap.cpp \
    edit/csettingconfig.cpp \
    live2dstruct/coor/ccoortypeobject.cpp \
    live2dstruct/coor/cgridtransform.cpp \
    live2dstruct/coor/crotationcoortype.cpp \
    live2dstruct/coor/cwarpcoortype.cpp \
    live2dstruct/csectionconfig.cpp \
    live2dstruct/ctreemanage.cpp \
    live2dstruct/keyform/cartmeshkeyform.cpp \
    live2dstruct/keyform/ckeyformobject.cpp \
    live2dstruct/keyform/clinearinterpolation.cpp \
    live2dstruct/keyform/crotationnodekeyform.cpp \
    live2dstruct/keyform/cwarpnodekeyform.cpp \
    live2dstruct/struct/cblend_shape_constraint_indices.cpp \
    live2dstruct/struct/cblend_shape_constraint_values.cpp \
    live2dstruct/struct/cblend_shape_constraints.cpp \
    live2dstruct/struct/cblend_shape_keyform_bindings.cpp \
    live2dstruct/struct/cblend_shape_parameter_bindings.cpp \
    live2dstruct/struct/cblend_shapes_art_meshes.cpp \
    live2dstruct/struct/cblend_shapes_warp_deformers.cpp \
    live2dstruct/struct/ckeyform_multiply_colors.cpp \
    live2dstruct/struct/ckeyform_screen_colors.cpp \
    ui/cpsdprintdialog.cpp \
    ui/cpsdbatchdialog.cpp \
    ui/psdbatchworker.cpp \
    ui/cmodeltreepanel.cpp \
    ui/cparampanel.cpp \
    ui/cmotionpanel.cpp \
    ui/cphysicspanel.cpp \
    ui/cblendshapepanel.cpp \
    ui/ctexturepanel.cpp \
    live2dstruct/tree/c2dtree.cpp \
    live2dstruct/tree/c2dtreenode.cpp \
    live2dstruct/csectiontable.cpp \
    live2dstruct/struct/cart_mesh_keyforms.cpp \
    live2dstruct/struct/cart_meshes.cpp \
    live2dstruct/struct/ccanvas_info.cpp \
    live2dstruct/struct/ccount_info.cpp \
    live2dstruct/struct/cdeformers.cpp \
    live2dstruct/struct/cdraw_order_group_objects.cpp \
    live2dstruct/struct/cdraw_order_groups.cpp \
    live2dstruct/struct/cdrawable_masks.cpp \
    live2dstruct/struct/cglue.cpp \
    live2dstruct/struct/cglue_info.cpp \
    live2dstruct/struct/cglue_keyforms.cpp \
    live2dstruct/struct/ckeyform_bindings.cpp \
    live2dstruct/struct/ckeyform_positions.cpp \
    live2dstruct/struct/ckeys.cpp \
    live2dstruct/struct/cparameter_binding_indices.cpp \
    live2dstruct/struct/cparameter_bindings.cpp \
    live2dstruct/struct/cparameters.cpp \
    live2dstruct/struct/cpart_keyforms.cpp \
    live2dstruct/struct/cparts.cpp \
    live2dstruct/struct/cposition_indices.cpp \
    live2dstruct/struct/crotation_deformer_keyforms.cpp \
    live2dstruct/struct/crotation_deformers.cpp \
    live2dstruct/csectionoffsettable.cpp \
    live2dstruct/struct/cuvs.cpp \
    live2dstruct/struct/cwarp_deformer_keyforms.cpp \
    live2dstruct/struct/cwarp_deformers.cpp \
    live2dstruct/tree/cartmeshnode.cpp \
    live2dstruct/tree/cdeformernode.cpp \
    live2dstruct/tree/cparameternode.cpp \
    live2dstruct/tree/cpartsnode.cpp \
    live2dstruct/tree/crotationnode.cpp \
    live2dstruct/tree/cwarpnode.cpp \
    live2dstruct/util/cfilereader.cpp \
    opengl/cmesh.cpp \
    opengl/coffscreenlive.cpp \
    opengl/coffscreenmap.cpp \
    opengl/cpsdwritelist.cpp \
    opengl/ctextureextractor.cpp \
    opengl/cselectstruct.cpp \
    opengl/cshader.cpp \
    opengl/opengglform.cpp \
    psd/cpsdlist.cpp \
    opengl/creadopengl.cpp \
    ui/para/ccidjson.cpp \
    main.cpp \
    ui\mainwindow.cpp \
    framework\CubismCdiJson.cpp\
    framework\CubismDefaultParameterId.cpp\
    framework\CubismFramework.cpp\
    framework\CubismModelSettingJson.cpp\
    framework\Effect\CubismBreath.cpp\
    framework\Effect\CubismEyeBlink.cpp\
    framework\Effect\CubismPose.cpp\
    framework\Id\CubismId.cpp\
    framework\Id\CubismIdManager.cpp\
    framework\Math\CubismMath.cpp\
    framework\Math\CubismMatrix44.cpp\
    framework\Math\CubismModelMatrix.cpp\
    framework\Math\CubismTargetPoint.cpp\
    framework\Math\CubismVector2.cpp\
    framework\Math\CubismViewMatrix.cpp\
    framework\Model\CubismMoc.cpp\
    framework\Model\CubismModel.cpp\
    framework\Model\CubismModelUserData.cpp\
    framework\Model\CubismModelUserDataJson.cpp\
    framework\Model\CubismUserModel.cpp\
    framework\Motion\ACubismMotion.cpp\
    framework\Motion\CubismExpressionMotion.cpp\
    framework\Motion\CubismExpressionMotionManager.cpp\
    framework\Motion\CubismMotion.cpp\
    framework\Motion\CubismMotionJson.cpp\
    framework\Motion\CubismMotionManager.cpp\
    framework\Motion\CubismMotionQueueEntry.cpp\
    framework\Motion\CubismMotionQueueManager.cpp\
    framework\Physics\CubismPhysics.cpp\
    framework\Physics\CubismPhysicsJson.cpp\
    framework\Rendering\CubismClippingManager.tpp\
    framework\Rendering\CubismRenderer.cpp\
    framework\Rendering\OpenGL\CubismOffscreenSurface_OpenGLES2.cpp\
    framework\Rendering\OpenGL\CubismRenderer_OpenGLES2.cpp\
    framework\Rendering\OpenGL\CubismShader_OpenGLES2.cpp\
    framework\Type\csmRectF.cpp\
    framework\Type\csmString.cpp\
    framework\Utils\CubismDebug.cpp\
    framework\Utils\CubismJson.cpp\
    framework\Utils\CubismString.cpp\
    live2d\LAppAllocator.cpp\
    live2d\LAppDefine.cpp\
    live2d\LAppDelegate.cpp\
    live2d\LAppLive2DManager.cpp\
    live2d\LAppModel.cpp\
    live2d\LAppPal.cpp\
    live2d\LAppSprite.cpp\
    live2d\LAppTextureManager.cpp\
    live2d\LAppView.cpp\
    live2d\LAppWavFileHandler.cpp\
    live2d\TouchManager.cpp\
    psd\psdSamples\Exporter\PsdExport.cpp\
    psd\psdSamples\ImageUtil\PsdDecompressRle.cpp\
    psd\psdSamples\ImageUtil\PsdInterleave.cpp\
    psd\psdSamples\ImageUtil\PsdLayerCanvasCopy.cpp\
    psd\psdSamples\Interfaces\PsdAllocator.cpp\
    psd\psdSamples\Interfaces\PsdFile.cpp\
    psd\psdSamples\Interfaces\PsdMallocAllocator.cpp\
    psd\psdSamples\Interfaces\PsdNativeFile.cpp\
    psd\psdSamples\Parser\PsdParseColorModeDataSection.cpp\
    psd\psdSamples\Parser\PsdParseDocument.cpp\
    psd\psdSamples\Parser\PsdParseImageDataSection.cpp\
    psd\psdSamples\Parser\PsdParseImageResourcesSection.cpp\
    psd\psdSamples\Parser\PsdParseLayerMaskSection.cpp\
    psd\psdSamples\PsdPch.cpp\
    psd\psdSamples\Types\PsdBlendMode.cpp\
    psd\psdSamples\Types\PsdColorMode.cpp\
    psd\psdSamples\Util\PsdBitUtil.inl\
    psd\psdSamples\Util\PsdEndianConversion.inl\
    psd\psdSamples\Util\PsdFixedSizeString.cpp\
    psd\psdSamples\Util\PsdMemoryUtil.inl\
    psd\psdSamples\Util\PsdSyncFileReader.cpp\
    psd\psdSamples\Util\PsdSyncFileUtil.inl\
    psd\psdSamples\Util\PsdSyncFileWriter.cpp\
    psd\psdSamples\Util\PsdUnionCast.inl\
    psd\psdSamples\Zip\Psdminiz.c\
    psd\psdSrc\PsdSamples.cpp\
    psd\psdSrc\PsdTgaExporter.cpp\
    edit\cdrawmesh.cpp \
    edit\cdrawrect.cpp \
    edit\ctriangl.cpp \
    std\cfileoption.cpp \
    std\cmodeljson.cpp \
    std\cmodeljsonwriter.cpp \
    cmo3\ccaffarchive.cpp \
    cmo3\cxsxmlwriter.cpp \
    cmo3\ccmo3entities.cpp \
    cmo3\ccmo3builder.cpp \

HEADERS += \
    CPrecompileHead.h \
    edit/cdrawobject.h \
    edit/cdrawrotation.h \
    edit/cdrawwarp.h \
    edit/ceditstate.h \
    edit/cmeshmap.h \
    edit/csettingconfig.h \
    live2dstruct/CTreePrecompile.h \
    live2dstruct/coor/ccoortypeobject.h \
    live2dstruct/coor/cgridtransform.h \
    live2dstruct/coor/crotationcoortype.h \
    live2dstruct/coor/cwarpcoortype.h \
    live2dstruct/csectionconfig.h \
    live2dstruct/ctreemanage.h \
    live2dstruct/keyform/cartmeshkeyform.h \
    live2dstruct/keyform/ckeyformobject.h \
    live2dstruct/keyform/clinearinterpolation.h \
    live2dstruct/keyform/crotationnodekeyform.h \
    live2dstruct/keyform/cwarpnodekeyform.h \
    live2dstruct/struct/cblend_shape_constraint_indices.h \
    live2dstruct/struct/cblend_shape_constraint_values.h \
    live2dstruct/struct/cblend_shape_constraints.h \
    live2dstruct/struct/cblend_shape_keyform_bindings.h \
    live2dstruct/struct/cblend_shape_parameter_bindings.h \
    live2dstruct/struct/cblend_shapes_art_meshes.h \
    live2dstruct/struct/cblend_shapes_warp_deformers.h \
    live2dstruct/struct/ckeyform_multiply_colors.h \
    live2dstruct/struct/ckeyform_screen_colors.h \
    ui/cpsdprintdialog.h \
    ui/cmodeltreepanel.h \
    ui/ctreenodedelegate.h \
    ui/theme.h \
    ui/cparampanel.h \
    ui/cmotionpanel.h \
    ui/cphysicspanel.h \
    ui/cblendshapepanel.h \
    ui/ctexturepanel.h \
    live2dstruct/tree/c2dtree.h \
    live2dstruct/tree/c2dtreenode.h \
    live2dstruct/csectiontable.h \
    live2dstruct/struct/cart_mesh_keyforms.h \
    live2dstruct/struct/cart_meshes.h \
    live2dstruct/struct/ccanvas_info.h \
    live2dstruct/struct/ccount_info.h \
    live2dstruct/struct/cdeformers.h \
    live2dstruct/struct/cdraw_order_group_objects.h \
    live2dstruct/struct/cdraw_order_groups.h \
    live2dstruct/struct/cdrawable_masks.h \
    live2dstruct/struct/cglue.h \
    live2dstruct/struct/cglue_info.h \
    live2dstruct/struct/cglue_keyforms.h \
    live2dstruct/struct/ckeyform_bindings.h \
    live2dstruct/struct/ckeyform_positions.h \
    live2dstruct/struct/ckeys.h \
    live2dstruct/struct/cparameter_binding_indices.h \
    live2dstruct/struct/cparameter_bindings.h \
    live2dstruct/struct/cparameters.h \
    live2dstruct/struct/cpart_keyforms.h \
    live2dstruct/struct/cparts.h \
    live2dstruct/struct/cposition_indices.h \
    live2dstruct/struct/crotation_deformer_keyforms.h \
    live2dstruct/struct/crotation_deformers.h \
    live2dstruct/csectionoffsettable.h \
    live2dstruct/struct/cuvs.h \
    live2dstruct/struct/cwarp_deformer_keyforms.h \
    live2dstruct/struct/cwarp_deformers.h \
    live2dstruct/tree/cartmeshnode.h \
    live2dstruct/tree/cdeformernode.h \
    live2dstruct/tree/cparameternode.h \
    live2dstruct/tree/cpartsnode.h \
    live2dstruct/tree/crotationnode.h \
    live2dstruct/tree/cwarpnode.h \
    live2dstruct/util/cfilereader.h \
    opengl/cmesh.h \
    opengl/coffscreenlive.h \
    opengl/coffscreenmap.h \
    opengl/cpsdwritelist.h \
    opengl/ctextureextractor.h \
    opengl/cselectstruct.h \
    opengl/cshader.h \
    opengl\opengglform.h \
    ui/cpsdbatchdialog.h \
    ui/psdbatchworker.h \
    psd/cpsdlist.h \
    opengl/creadopengl.h \
    ui/para/ccidjson.h \
    ui\mainwindow.h \
    framework\CubismCdiJson.hpp\
    framework\CubismDefaultParameterId.hpp\
    framework\CubismFramework.hpp\
    framework\CubismFrameworkConfig.hpp\
    framework\CubismJsonHolder.hpp\
    framework\CubismModelSettingJson.hpp\
    framework\Effect\CubismBreath.hpp\
    framework\Effect\CubismEyeBlink.hpp\
    framework\Effect\CubismPose.hpp\
    framework\ICubismAllocator.hpp\
    framework\ICubismModelSetting.hpp\
    framework\Id\CubismId.hpp\
    framework\Id\CubismIdManager.hpp\
    framework\Live2DCubismCore.hpp\
    framework\Math\CubismMath.hpp\
    framework\Math\CubismMatrix44.hpp\
    framework\Math\CubismModelMatrix.hpp\
    framework\Math\CubismTargetPoint.hpp\
    framework\Math\CubismVector2.hpp\
    framework\Math\CubismViewMatrix.hpp\
    framework\Model\CubismMoc.hpp\
    framework\Model\CubismModel.hpp\
    framework\Model\CubismModelUserData.hpp\
    framework\Model\CubismModelUserDataJson.hpp\
    framework\Model\CubismUserModel.hpp\
    framework\Motion\ACubismMotion.hpp\
    framework\Motion\CubismExpressionMotion.hpp\
    framework\Motion\CubismExpressionMotionManager.hpp\
    framework\Motion\CubismMotion.hpp\
    framework\Motion\CubismMotionInternal.hpp\
    framework\Motion\CubismMotionJson.hpp\
    framework\Motion\CubismMotionManager.hpp\
    framework\Motion\CubismMotionQueueEntry.hpp\
    framework\Motion\CubismMotionQueueManager.hpp\
    framework\Physics\CubismPhysics.hpp\
    framework\Physics\CubismPhysicsInternal.hpp\
    framework\Physics\CubismPhysicsJson.hpp\
    framework\Rendering\CubismClippingManager.hpp\
    framework\Rendering\CubismRenderer.hpp\
    framework\Rendering\OpenGL\CubismOffscreenSurface_OpenGLES2.hpp\
    framework\Rendering\OpenGL\CubismRenderer_OpenGLES2.hpp\
    framework\Rendering\OpenGL\CubismShader_OpenGLES2.hpp\
    framework\Type\csmMap.hpp\
    framework\Type\csmRectF.hpp\
    framework\Type\csmString.hpp\
    framework\Type\csmVector.hpp\
    framework\Type\CubismBasicType.hpp\
    framework\Utils\CubismDebug.hpp\
    framework\Utils\CubismJson.hpp\
    framework\Utils\CubismString.hpp\
    core\Live2DCubismCore.h\
    framework\Rendering\CubismClippingManager.hpp\
    live2d\LAppAllocator.hpp\
    live2d\LAppDefine.hpp\
    live2d\LAppDelegate.hpp\
    live2d\LAppLive2DManager.hpp\
    live2d\LAppModel.hpp\
    live2d\LAppPal.hpp\
    live2d\LAppSprite.hpp\
    live2d\LAppTextureManager.hpp\
    live2d\LAppView.hpp\
    live2d\LAppWavFileHandler.hpp\
    live2d\TouchManager.hpp\
    std\stb_image.h\
    psd\psdSamples\Exporter\PsdExport.h\
    psd\psdSamples\Exporter\PsdExportChannel.h\
    psd\psdSamples\Exporter\PsdExportColorMode.h\
    psd\psdSamples\Exporter\PsdExportDocument.h\
    psd\psdSamples\Exporter\PsdExportLayer.h\
    psd\psdSamples\Exporter\PsdExportMetaDataAttribute.h\
    psd\psdSamples\ImageUtil\PsdDecompressRle.h\
    psd\psdSamples\ImageUtil\PsdInterleave.h\
    psd\psdSamples\ImageUtil\PsdLayerCanvasCopy.h\
    psd\psdSamples\Interfaces\PsdAllocator.h\
    psd\psdSamples\Interfaces\PsdFile.h\
    psd\psdSamples\Interfaces\PsdMallocAllocator.h\
    psd\psdSamples\Interfaces\PsdNativeFile.h\
    psd\psdSamples\Parser\PsdParseColorModeDataSection.h\
    psd\psdSamples\Parser\PsdParseDocument.h\
    psd\psdSamples\Parser\PsdParseImageDataSection.h\
    psd\psdSamples\Parser\PsdParseImageResourcesSection.h\
    psd\psdSamples\Parser\PsdParseLayerMaskSection.h\
    psd\psdSamples\Platform\PsdAssert.h\
    psd\psdSamples\Platform\PsdCompilerMacros.h\
    psd\psdSamples\Platform\PsdLog.h\
    psd\psdSamples\Platform\PsdNamespace.h\
    psd\psdSamples\Platform\PsdPlatform.h\
    psd\psdSamples\Platform\PsdTypes.h\
    psd\psdSamples\Psd.h\
    psd\psdSamples\PsdDocumentation.h\
    psd\psdSamples\Psdinttypes.h\
    psd\psdSamples\Psdispod.h\
    psd\psdSamples\Psdisunsigned.h\
    psd\psdSamples\PsdPch.h\
    psd\psdSamples\Psdstdint.h\
    psd\psdSamples\Sections\PsdColorModeDataSection.h\
    psd\psdSamples\Sections\PsdImageDataSection.h\
    psd\psdSamples\Sections\PsdImageResourcesSection.h\
    psd\psdSamples\Sections\PsdLayerMaskSection.h\
    psd\psdSamples\Types\PsdAlphaChannel.h\
    psd\psdSamples\Types\PsdBlendMode.h\
    psd\psdSamples\Types\PsdChannel.h\
    psd\psdSamples\Types\PsdColorMode.h\
    psd\psdSamples\Types\PsdCompressionType.h\
    psd\psdSamples\Types\PsdDocument.h\
    psd\psdSamples\Types\PsdImageResourceType.h\
    psd\psdSamples\Types\PsdLayer.h\
    psd\psdSamples\Types\PsdLayerMask.h\
    psd\psdSamples\Types\PsdLayerType.h\
    psd\psdSamples\Types\PsdPlanarImage.h\
    psd\psdSamples\Types\PsdSection.h\
    psd\psdSamples\Types\PsdVectorMask.h\
    psd\psdSamples\Util\PsdBitUtil.h\
    psd\psdSamples\Util\PsdEndianConversion.h\
    psd\psdSamples\Util\PsdFixedSizeString.h\
    psd\psdSamples\Util\PsdKey.h\
    psd\psdSamples\Util\PsdMemoryUtil.h\
    psd\psdSamples\Util\PsdSyncFileReader.h\
    psd\psdSamples\Util\PsdSyncFileUtil.h\
    psd\psdSamples\Util\PsdSyncFileWriter.h\
    psd\psdSamples\Util\PsdUnionCast.h\
    psd\psdSamples\Zip\Psdminiz.h\
    psd\psdSrc\PsdDebug.h\
    psd\psdSrc\PsdTgaExporter.h\
    psd\psdSamples\PsdChannelType.h\
    psd\psdSamples\PsdThumbnail.h\
    edit\cdrawmesh.h \
    edit\cdrawrect.h \
    edit\ctriangl.h \
    std\cfileoption.h \
    std\cmodeljson.h \
    std\cmodeljsonwriter.h \
    cmo3\ccaffarchive.h \
    cmo3\cxsxmlwriter.h \
    cmo3\ccmo3ids.h \
    cmo3\ccmo3entities.h \
    cmo3\ccmo3builder.h \





INCLUDEPATH += $$PWD/opencv/include \


LIBS += $$PWD/windows/x86_64/Live2DCubismCore.lib
win32: LIBS += -luser32



CONFIG(debug, debug|release) {
    LIBS += $$PWD/opencv/x64/vc16/lib/opencv_world4100d.lib
} else {
    LIBS += $$PWD/opencv/x64/vc16/lib/opencv_world4100.lib
}

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

RESOURCES += \
    assets/images/images.qrc\
    assets/shaders/shaders.qrc\
    assets/icc.qrc


