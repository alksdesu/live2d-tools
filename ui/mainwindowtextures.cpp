#include "mainwindow.h"
#include "ctexturepanel.h"
#include "psdbatchjob.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "std/cpngimage.h"
#include <QStatusBar>

bool MainWindow::InitializeTextureEditing(QString* error)
{
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    if (!model) { if (error) *error = QStringLiteral("纹理预览尚未就绪"); return false; }
    const bool initializing = !m_document.HasTextureWorkspace();
    if (initializing && !m_document.InitializeTextureWorkspace(model->GetTextureSources(), model->GetTextureState(), error)) return false;
    if (m_document.TextureSources() != model->GetTextureSources() || m_document.TextureWorkspace() != model->GetTextureWorkspace()) {
        if (error) *error = QStringLiteral("图集文档与运行时不一致，初始化保持原状态");
        return false;
    }
    m_atlasEditor.SetDocument(&m_document);
    m_atlasEditor.SetPublisher([this](const auto& before, const auto& after, const auto& ids, QString* detail) {
        return PublishAtlasPreview(before, after, ids, detail);
    });
    if (initializing) MarkDocumentSaved();
    return true;
}

bool MainWindow::ReplaceTexture(int slot, const QByteArray& png, QString* error)
{
    if (m_artworkLoading || m_nameImportControl) { if (error) *error = QStringLiteral("来源素材正在读取，请先完成或取消"); return false; }
    if (!FinishParameterValueEditing(error) || !m_openglwindows || !m_openglwindows->FinishMeshDrag(false, error)) return false;
    live2dstruct::CModelAtlasState state;
    if (!m_document.CaptureAtlasState(state, error)) return false;
    const auto atlas = state.workspace.atlases.find(slot);
    if (atlas == state.workspace.atlases.end()) { if (error) *error = QStringLiteral("纹理槽不存在"); return false; }
    const auto source = m_document.TextureSources().constFind(atlas->sourceSlot);
    const auto requested = png.isEmpty() && source != m_document.TextureSources().cend() ? source->png : png;
    QImage decoded;
    if (!CPngImage::Decode(requested, decoded, error)) return false;
    if (decoded.size() != atlas->size) {
        if (error) *error = QStringLiteral("替换 PNG 必须与当前图集同尺寸；调整尺寸请使用图集编辑");
        return false;
    }
    atlas->png = source != m_document.TextureSources().cend() && source->size == atlas->size && source->png == requested
        ? QByteArray() : QByteArray(requested.constData(), requested.size());
    const auto revision = m_document.Revision();
    if (!m_atlasEditor.Submit(state, error)) return false;
    if (revision != m_document.Revision()) RefreshTextureEditing(true);
    return true;
}

void MainWindow::RefreshTextureEditing(bool changed)
{
    const QPointer<MainWindow> alive(this);
    if (changed) {
        m_historySource = HistorySource::Inspector;
        m_sessionDirty = true;
        RefreshWindowTitle();
        if (!alive) return;
    }
    if (m_texturePanel) {
        QString error;
        const bool available = m_runtimePanelsInitialized && !m_closingModel && !m_batchExporting && !m_psdBatch &&
            !m_previewExport.IsBusy() && !m_artworkLoading && !m_nameImportControl && m_openglwindows && m_openglwindows->IsModelReady();
        auto* manager = available ? LAppLive2DManager::GetInstance() : nullptr;
        auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
        const bool loaded = model && m_texturePanel->LoadWorkspaceSnapshot(model->GetTextureSources(), model->GetTextureWorkspace(),
                                                                           model->GetPreviewMocBytes(), &error);
        if (!alive) return;
        const QPointer<CTexturePanel> panel(m_texturePanel);
        panel->SetArtworkLoadWarning(m_artworkPackageWarning); if (!alive || !panel) return;
        panel->SetSnapshotAvailable(loaded, error); if (!alive || !panel) return;
        panel->SetPreviewActive(m_atlasEditor.IsActive() || m_materialEditor.IsActive()); if (!alive || !panel) return;
        panel->SetEditingEnabled(available && loaded, error); if (!alive || !panel) return;
        if (available && !loaded) statusBar()->showMessage(QStringLiteral("图集状态已保留，界面刷新失败：%1").arg(error), 10000);
    }
    if (!alive) return;
    RefreshUndoActions();
    if (alive) RefreshArtworkActions();
}
