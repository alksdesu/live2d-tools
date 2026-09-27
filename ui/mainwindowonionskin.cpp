#include "mainwindow.h"
#include "ui/cmotionpanel.h"
#include "ui/cparampanel.h"
#include "ui/psdbatchjob.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"

using live2dstruct::COnionSkin;

void MainWindow::ConfigureOnionSkin()
{
    if (!m_openglwindows || !m_motionPanel) return;
    const QPointer<MainWindow> window(this);
    const QPointer<OpengGlForm> surface(m_openglwindows);
    m_openglwindows->SetOnionSkinRequestProvider([window, surface](COnionSkin::Request& request, QString* error) {
        if (!window || !surface || window->m_openglwindows != surface) { request = {}; return true; }
        return window->CaptureOnionSkinRequest(request, error);
    });
    connect(m_motionPanel, &CMotionPanel::OnionSkinRequestChanged, this, &MainWindow::RefreshOnionSkin);
    connect(m_openglwindows, &OpengGlForm::OnionSkinStatusChanged, m_motionPanel, &CMotionPanel::SetOnionSkinStatus);
    RefreshOnionSkin();
}

bool MainWindow::CaptureOnionSkinRequest(COnionSkin::Request& request, QString* error) const
{
    if (!m_openglwindows || !m_motionPanel || m_closingModel) { request = {}; return true; }
    COnionSkin::Request candidate;
    if (!m_motionPanel->CaptureOnionSkinRequest(candidate, error)) return false;
    candidate.sourceRevision = m_document.Revision();
    QString reason;
    if (m_workspaceMode != 1) reason = QStringLiteral("切换到动画工作区以显示洋葱皮");
    else if (!m_runtimePanelsInitialized || m_initializingRuntimePanels || !m_workspaceReady
             || !m_openglwindows->IsModelReady()) reason = QStringLiteral("模型准备完成后恢复洋葱皮");
    else if (m_applyingSession || m_applyingParameterReferences || m_previewExport.IsBusy()
             || m_activeExportControl || m_batchExporting || m_psdBatch || m_openglwindows->IsExportSuspended())
        reason = QStringLiteral("当前任务完成后恢复洋葱皮");
    else if (m_materialEditor.IsActive() || m_atlasEditor.IsActive() || m_artworkLoading
             || m_parameterValueEdit || m_publishingParameterValues || m_keyformEditor.IsActive()
             || m_openglwindows->IsKeyformDragging() || (m_paramPanel && m_paramPanel->HasUserValueGesture()))
        reason = QStringLiteral("完成当前模型编辑后恢复洋葱皮");
    else {
        auto* manager = LAppLive2DManager::GetInstance();
        auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
        if (!model || !model->HasTimelinePreview()) reason = QStringLiteral("先选择时间轴帧以显示洋葱皮");
    }
    if (!reason.isEmpty()) {
        candidate.suspended = true;
        candidate.suspensionReason = reason;
    }
    request = std::move(candidate);
    return true;
}

void MainWindow::RefreshOnionSkin()
{
    if (!m_openglwindows || !m_motionPanel) return;
    COnionSkin::Request request;
    QString error;
    if (!CaptureOnionSkinRequest(request, &error) || !m_openglwindows->SetOnionSkinRequest(request, &error)) {
        m_openglwindows->ClearOnionSkin(error.isEmpty() ? QStringLiteral("无法读取洋葱皮预览状态") : error);
    }
    m_openglwindows->update();
}
