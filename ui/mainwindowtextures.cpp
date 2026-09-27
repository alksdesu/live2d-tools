#include "mainwindow.h"
#include "ctexturepanel.h"
#include "psdbatchjob.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include <QStatusBar>

bool MainWindow::InitializeTextureEditing(QString* error)
{
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    if (!model) { if (error) *error = QStringLiteral("纹理预览尚未就绪"); return false; }
    m_textureEditor.SetPublisher([this](const auto& state, QString* detail) { return PublishTextureState(state, detail); });
    return m_textureEditor.Initialize(model->GetTextureSources(), model->GetTextureState(), error);
}

bool MainWindow::PublishTextureState(const live2dstruct::CTextureAssets::State& state, QString* error)
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (m_closingModel || m_applyingSession || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() ||
        !m_runtimePanelsInitialized || !m_openglwindows || !m_openglwindows->IsModelReady())
        return fail(QStringLiteral("请等待模型就绪，并先结束当前导出或会话恢复"));
    if (!RuntimeMatchesDocument(error)) return false;
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    if (!model || model->GetTextureSources() != m_textureEditor.Sources() ||
        model->GetTextureState() != m_textureEditor.Current()) return fail(QStringLiteral("纹理来源或预览已改变，请重新打开模型"));
    return m_openglwindows->ApplyTextureState(state, error);
}

bool MainWindow::ReplaceTexture(int slot, const QByteArray& png, QString* error)
{
    const auto revision = m_textureEditor.Revision();
    if (!m_textureEditor.Replace(slot, png, error)) return false;
    if (revision != m_textureEditor.Revision()) RefreshTextureEditing(true);
    return true;
}

void MainWindow::RefreshTextureEditing(bool changed)
{
    if (changed) {
        m_historySource = HistorySource::Textures;
        RefreshWindowTitle();
    }
    if (m_texturePanel) {
        QString error;
        const bool available = m_runtimePanelsInitialized && !m_closingModel && !m_batchExporting && !m_psdBatch &&
            !m_previewExport.IsBusy() && m_openglwindows && m_openglwindows->IsModelReady();
        const bool loaded = m_texturePanel->LoadTextureSnapshot(m_textureEditor.Sources(), m_textureEditor.Current(), &error);
        m_texturePanel->SetEditingEnabled(available && loaded, error);
        if (!loaded) statusBar()->showMessage(QStringLiteral("纹理状态已保留，图集刷新失败：%1").arg(error), 10000);
    }
    RefreshTextureGeometry();
    RefreshUndoActions();
}

void MainWindow::ApplyTextureHistory(bool undo)
{
    QString error;
    if (!(undo ? m_textureEditor.Undo(&error) : m_textureEditor.Redo(&error))) {
        if (!error.isEmpty()) statusBar()->showMessage(error, 8000);
        return;
    }
    RefreshTextureEditing(true);
    statusBar()->showMessage(undo ? QStringLiteral("已撤销纹理修改") : QStringLiteral("已重做纹理修改"), 5000);
}
