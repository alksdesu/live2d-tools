
#pragma once

#include <QOpenGLFunctions_3_3_Core>
#include "LAppAllocator.hpp"
#include <QOpenGLWidget>
#include "QOpenGLContext"
#include "QEvent"
#include "opengl/coffscreenmap.h"
#include "live2d/LAppModel.hpp"
#include "opengl/cselectstruct.h"
#include "opengl/creadopengl.h"
#include "opengl/coffscreenlive.h"
#include <QCoreApplication>
#include "opengl/cpsdwritelist.h"

class LAppView;
class LAppTextureManager;
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
    void StartOffset_FunpsdList(const QString& name,bool IsAll,bool IsFenzu);
    void StartTextureExtraction(const QString& psdPath, bool includeHidden, bool groupByParts);



    void ChangeScene(const QString& strMoldeName);//切换已经打开的场景
    bool Initialize(const QWidget* windows,const QString& strMoldeName="");//打开默认场景

    const QString& getResourcesPath(){
        return ResourcesPath;
    }
private:

    LAppDelegate();
    ~LAppDelegate();
    void InitializeCubism(const QString& strMoldeName="");

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
    QString ResourcesPath;

};


