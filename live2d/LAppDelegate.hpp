
#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include "LAppAllocator.hpp"
#include <QOpenGLWidget>
#include "QOpenGLContext"
#include "QEvent"
#include <QMap>
#include <vector>
#include "opengl/coffscreenmap.h"
#include "live2d/LAppModel.hpp"
#include "opengl/cselectstruct.h"
#include "opengl/creadopengl.h"
#include "opengl/coffscreenlive.h"
#include <QCoreApplication>
#include "opengl/cpsdwritelist.h"
#include "live2dstruct/cdraworder.h"
#include "live2dstruct/cmodelexportsnapshot.h"
#include <QSet>
#include <QHash>
#include <QVector>
#include <memory>

class LAppView;
class LAppTextureManager;
class CExportControl;
class CPsdRenderSession;
using Live2D::Cubism::Framework::CubismModel;

//这个渲染对象
class LAppDelegate : public  QOpenGLFunctions_3_3_Core
{
public:

    static LAppDelegate* GetInstance();
    static void ReleaseInstance();
    void Release();
    LAppView* GetView() { return _view; }
    LAppTextureManager* GetTextureManager();
    const QWidget* GetWindow();
    void setWindow(QWidget* windows);
    QOpenGLContext * getGlOwningContext();

    int width();
    int height();
    void paint();
    void resize(int width, int height);

    void SetOffsetWidthANDHeight(int width, int height);
    bool IsOffScreen();
    void SetIsOffset(bool f);

    void InitOffset(int w,int h);
    GLuint getTexFrame();



public:
    CPsdWriteList* GetCPsdWriteList();
    bool m_IsPrintALl;
    bool m_IsPrintFenzu;
    CReadOpengl read;

    // 可见性控制：隐藏的 drawable 名称集合
    QSet<QString> m_hiddenDrawables;
    QSet<QString> m_hiddenDeformerDrawables;
    QHash<QString, float> m_drawableOpacityOverrides;
    QHash<QString, int> m_drawableDrawOrderOverrides;
    QHash<QString, int> m_drawableOrderGroupIndices;
    QVector<live2dstruct::CDrawableOrderGroup> m_drawOrderGroups;
    QHash<QString, QHash<int, QVector<float>>> m_meshOverlayPositions;
    QHash<QString, int> m_meshOverlayKeyformIndices;
    QHash<QString, QVector<quint16>> m_meshIndexOverrides;

    // 背景色
    float m_clearColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    void SetClearColor(float r, float g, float b, float a = 1.0f){
        m_clearColor[0] = r; m_clearColor[1] = g;
        m_clearColor[2] = b; m_clearColor[3] = a;
    }

    // 变形器拖拽：冻结非目标 drawable 的顶点
    QSet<QString> m_dragTargetDrawables;       // 目标 drawable（跟着参数动）
    bool m_isDraggingDeformer = false;
    QMap<int, std::vector<float>> m_savedVertices; // 非目标 drawable 的顶点快照
    bool StartDeformerDrag(QString* error = nullptr);
    void StopDeformerDrag();    // 松手时调用
    bool StartOffset_FunpsdList(const QString& name,bool IsAll,bool IsFenzu,bool cubismCompat = false,
                               QString* error = nullptr, CExportControl* control = nullptr,
                               const live2dstruct::CModelExportSnapshot& snapshot = {});
    bool StartTextureExtraction(const QString& psdPath, bool includeHidden, bool groupByParts, bool cubismCompat = false,
                                QString* error = nullptr, CExportControl* control = nullptr,
                                const live2dstruct::CModelExportSnapshot& snapshot = {});
    bool RecoverPsdExport(QString* error = nullptr);



    bool ChangeScene(const QString& strMoldeName);//切换已经打开的场景
    bool BeginTemporaryScene(const QString& modelJsonName);
    bool EndTemporaryScene();
    bool IsRuntimeReady() const;
    bool Initialize(const QWidget* windows,const QString& strMoldeName="");//打开默认场景

    const QString& getResourcesPath(){
        return ResourcesPath;
    }
private:

    LAppDelegate();
    ~LAppDelegate();
    bool InitializeCubism(const QString& strMoldeName="");
    void ClearModelRuntimeState(bool clearOrderGroups);

    LAppAllocator _cubismAllocator;              ///< Cubism SDK Allocator
    Csm::CubismFramework::Option _cubismOption;  ///< Cubism SDK Option
    LAppView* _view;                             ///< View情報

    const QWidget* _windows;        ///< APP終了しているか
    LAppTextureManager* _textureManager;


    int _windowWidth;                            ///< Initialize関数で設定したウィンドウ幅
    int _windowHeight;

    //离屏渲染参数
    bool _IsOffScreen;//离屏渲染？
    int _OffWidth;//离屏幕渲染宽
    int _OffHeight;//离屏幕渲染高
    COffScreenLive coffscreen;
    CPsdWriteList* cpsdwriteList;
    bool m_exportInProgress = false;
    std::unique_ptr<CPsdRenderSession> m_psdExportSession;
    QString ResourcesPath;

};
