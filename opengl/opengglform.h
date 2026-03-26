#ifndef OPENGGLFORM_H
#define OPENGGLFORM_H


#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLWidget>
#include "QOpenGLContext"
#include "live2d/LAppDelegate.hpp"
#include <QTimer>
#include <QEvent>
#include <QMouseEvent>
#include <ui/para/ccidjson.h>
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "QFileDialog"
#include "QMessageBox"
#include "cselectstruct.h"
#include <QDesktopServices>
#include <QKeyEvent>
#include <QWheelEvent>
#include "edit/ceditstate.h"
#include "edit/csettingconfig.h"
#include "edit/cdrawrect.h"
#include "live2d/LAppView.hpp"

using Live2D::Cubism::Framework::CubismModel;


class OpengGlForm : public QOpenGLWidget
{
    //支持信号和槽
    Q_OBJECT
public:
    explicit OpengGlForm(QWidget *par = nullptr);
    ~OpengGlForm();

    //初始化第一次打开模型文件
    void SetInitOpenName(const QString& Name);
    //改变模型
    void do_ModelChange(const QString& name);
    //进行离开屏幕渲染打印出psd文件
    void do_OffSeceen(const QString& name,bool IsALl,bool IsFenzu);
    // 批量导出静默版本（不弹窗、不自动打开文件夹）
    void do_OffSeceenSilent(const QString& name,bool IsALl,bool IsFenzu);
    // 原始纹理提取导出（不需要OpenGL渲染）
    void do_TextureExtract(const QString& name, bool isAll, bool isFenzu);
    void do_TextureExtractSilent(const QString& name, bool isAll, bool isFenzu);
    const QString& GetCurrentModelJson() const { return m_currentModelJson; }

    //点击对应的art 显示对应的mesh网格
    //点击对应网格  显示对应item
    void do_ShowMesh(const QString& artName);
    void AddWrapKey(std::vector<C2DTreeNode*>& node );
    void do_ShowWarp(const QString& artName);
public slots:
    //刷新每一帧    //跳转模型的位置
    void on_timeout();
    void do_setPoint(float x,float y);
    void do_change_para_value(const QString& name,float value,bool IsInit);
    void do_SetVisibility(const QString& name, const QString& type, bool visible);
    void do_ShowDeformers(const QStringList& names);
    void do_ShowDeformerMeshes(const QStringList& artMeshNames);
    void do_SetDeformerDragParams(const QStringList& paramIds, const QList<float>& minVals,
                                  const QList<float>& maxVals, const QList<float>& defaultVals);
    void do_ResetView();


signals:
    void SendSelectMesh(const QString& artName);

protected:
    virtual void initializeGL();
    virtual void resizeGL(int w, int h);
    virtual void paintGL();


public:
    QTimer m_timer;//用来刷新帧

protected:
    virtual void mousePressEvent(QMouseEvent *event) override;
    virtual void mouseReleaseEvent(QMouseEvent *event) override;
    virtual void mouseMoveEvent(QMouseEvent *event) override;
    virtual void keyPressEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
    virtual void keyReleaseEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
    virtual void wheelEvent(QWheelEvent *event) Q_DECL_OVERRIDE;


    CEditState m_ceditstate;
    CSettingConfig m_config;
    std::vector<C2DTreeNode*> m_mapWarpNode;
    QString m_currentModelJson;



};

#endif // OPENGGLFORM_H
