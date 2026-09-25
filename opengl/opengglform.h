#ifndef OPENGGLFORM_H
#define OPENGGLFORM_H
#include <optional>
#include <functional>


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
    bool do_ModelChange(const QString& name);
    bool LoadExportModel(const QString& name, QString* error = nullptr);
    bool BeginTemporaryModel(const QString& name, QString* error = nullptr);
    bool EndTemporaryModel(QString* error = nullptr);
    bool SetExportSuspended(bool suspended, QString* error = nullptr);
    bool IsExportSuspended() const { return m_exportSuspended; }
    bool RecoverExportResources(QString* error = nullptr);
    bool IsModelReady() const { return m_ceditstate.IsContins(CEditState::ModelOpen); }
    bool HasInitializationResult() const { return m_initializationCompleted; }
    //进行离开屏幕渲染打印出psd文件
    bool do_OffSeceen(const QString& name,bool IsALl,bool IsFenzu,bool cubismCompat = false);
    // 批量导出静默版本（不弹窗、不自动打开文件夹）
    bool do_OffSeceenSilent(const QString& name,bool IsALl,bool IsFenzu,bool cubismCompat = false, QString* error = nullptr,
                            CExportControl* control = nullptr);
    // 原始纹理提取导出（不需要OpenGL渲染）
    bool do_TextureExtract(const QString& name, bool isAll, bool isFenzu, bool cubismCompat = false);
    bool do_TextureExtractSilent(const QString& name, bool isAll, bool isFenzu, bool cubismCompat = false, QString* error = nullptr,
                                 CExportControl* control = nullptr);
    const QString& GetCurrentModelJson() const { return m_currentModelJson; }
    float ViewScale() const { return m_config.GeteScalefactor(); }
    QVector2D ViewPosition() const;
    using MeshActivate = std::function<bool(const QString&, int, QString*)>;
    using MeshBegin = std::function<bool(const QVector<int>&, const QVector<float>&, QString*)>;
    using MeshMove = std::function<bool(const QPointF&, QString*)>;
    using MeshFinish = std::function<bool(bool, QString*)>;
    void SetMeshEditHandlers(MeshActivate activate, MeshBegin begin, MeshMove move, MeshFinish finish);
    bool ApplyMocPreview(const QByteArray& bytes, const QByteArray& sourceDigest,
                         const QMap<QString, float>* pose, QString* error = nullptr);
    bool EndEditorPreview(QString* error = nullptr);
    bool FinishMeshDrag(bool cancel, QString* error = nullptr);
    bool CancelMeshDrag(QString* error = nullptr);
    bool IsMeshDragging() const { return m_meshDrag.has_value(); }

    //点击对应的art 显示对应的mesh网格
    //点击对应网格  显示对应item
    void do_ShowMesh(const QString& artName);
    void do_ShowWarp(const QString& artName);
public slots:
    //刷新每一帧    //跳转模型的位置
    void on_timeout();
    void do_setPoint(float x,float y);
    void do_change_para_value(const QString& name,float value,bool IsInit);
    void do_SetVisibility(const QString& name, const QString& type, bool visible);
    void SetHiddenDeformerDrawables(const QSet<QString>& names);
    void ClearEditorOverrides();
    void do_SetDrawableOpacity(const QString& name, float opacity);
    void do_SetDrawableDrawOrder(const QString& name, int drawOrder);
    void SetMeshEditEnabled(bool enabled);
    void SetActiveKeyform(const QString& name, int keyformIndex);
    void do_SetDrawableIndices(const QString& name, const QVector<quint16>& indices);
    void do_ShowDeformers(const QStringList& names);
    void do_ShowDeformerMeshes(const QStringList& artMeshNames);
    void do_SetDeformerDragParams(const QStringList& paramIds, const QList<float>& minVals,
                                  const QList<float>& maxVals, const QList<float>& defaultVals);
    void do_ResetView();
    void SetViewState(float scale, const QVector2D& position);
    void PublishRenderOrder(const QStringList& names);
    void SetDrawOrderGroups(const QHash<QString, int>& groupIndices,
                            const QVector<live2dstruct::CDrawableOrderGroup>& groups);
    void SyncPreviewView();
    bool SetHighPrecisionMasks(bool enabled, QString* error = nullptr);
    bool HighPrecisionMasks() const;


signals:
    void RuntimeInitialized(bool modelLoaded);
    void SendSelectMesh(const QString& artName);
    void RenderOrderChanged(const QStringList& names);
    void MeshEditRejected(const QString& error);
    void PreviewFailed(const QString& error);
    void KeyformEditingFinishedRequested();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;


public:
    QTimer m_timer;//用来刷新帧

protected:
    virtual void mousePressEvent(QMouseEvent *event) override;
    virtual void mouseReleaseEvent(QMouseEvent *event) override;
    virtual void mouseMoveEvent(QMouseEvent *event) override;
    virtual void keyPressEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
    virtual void keyReleaseEvent(QKeyEvent *event) Q_DECL_OVERRIDE;
    virtual void wheelEvent(QWheelEvent *event) Q_DECL_OVERRIDE;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;


    CEditState m_ceditstate;
    CSettingConfig m_config;
    QStringList m_lastRenderOrder;
    QString m_lastPreviewError;
    QString m_currentModelJson;
    struct SuspendedModel { QString path; bool ready; LAppModel* identity; };
    std::optional<SuspendedModel> m_suspendedModel;
    bool m_exportSuspended = false;
    std::optional<QSize> m_deferredResize;
    bool m_runtimeReady = false;
    bool m_initializationCompleted = false;
    bool MakeRuntimeCurrent();
    bool ChangeModel(const QString& name, QString* error);
    bool m_meshEditEnabled = false;
    QString m_meshEditName;
    int m_activeKeyformIndex = 0;
    struct MeshDrag
    {
        QString name;
        int keyform;
        QVector<int> selected;
        QPointF start;
        QMatrix4x4 mvp;
        QSize size;
    };
    std::optional<MeshDrag> m_meshDrag;
    MeshActivate m_meshActivate;
    MeshBegin m_meshBegin;
    MeshMove m_meshMove;
    MeshFinish m_meshFinish;
    bool m_meshEditCallbackActive = false;
    bool m_controlDragActive = false;
    QPointF m_pointerAnchor;
    bool MeshDragFrameMatches() const;
    bool BeginMeshDrag(const QString& name, const QPointF& pixel, QString* error);
    void FinishDeformerDrag();
    QString SelectCoreMesh(const QPointF& pixel, bool hover);



};

#endif // OPENGGLFORM_H
