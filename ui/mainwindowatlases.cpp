#include "mainwindow.h"
#include "catlaseditdialog.h"
#include "cpsdimportdialog.h"
#include "cdraftdialoglifecycle.h"
#include "psdbatchjob.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include <QCryptographicHash>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QScopeGuard>
#include <QStatusBar>

namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
}

bool MainWindow::PublishAtlasPreview(const live2dstruct::CModelDocument& before,
                                     const live2dstruct::CModelDocument& after,
                                     const QSet<QString>& ids, QString* error)
{
    if (m_keyformEditor.IsPublishing()) return fail(error, QStringLiteral("模型编辑正在提交，请稍后修改图集"));
    if (m_materialEditor.IsActive() || m_closingModel || m_applyingSession || m_batchExporting || m_psdBatch ||
        m_previewExport.IsBusy() || !m_runtimePanelsInitialized || !m_openglwindows || !m_openglwindows->IsModelReady())
        return fail(error, QStringLiteral("请先结束当前 UV 编辑、导出或会话恢复"));
    if (!RuntimeMatchesDocument(error) || !before.HasTextureWorkspace() || !after.HasTextureWorkspace()) return false;
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    if (!model || before.TextureSources() != model->GetTextureSources() || before.TextureSources() != after.TextureSources())
        return fail(error, QStringLiteral("图集来源已改变，请重新打开图集编辑"));
    QByteArray previous, candidate, workspace;
    if (!before.BuildRuntimeMoc(previous, error) || !after.BuildRuntimeMoc(candidate, error) ||
        !live2dstruct::CTextureWorkspace::Fingerprint(before.TextureSources(), before.TextureWorkspace(), workspace, error)) return false;
    return m_openglwindows->ApplyAtlasPreview(candidate, after.SourceDigest(),
        QCryptographicHash::hash(previous, QCryptographicHash::Sha256), workspace, after.TextureWorkspace(), ids, error);
}

void MainWindow::EditAtlas(const QString& selectedMesh)
{
    if (m_artworkDialog) { const auto dialog = m_artworkDialog; dialog->raise(); if (dialog) dialog->activateWindow(); return; }
    if (m_artworkLoading) return;
    if (m_atlasDialog) { m_atlasDialog->raise(); m_atlasDialog->activateWindow(); return; }
    if (m_materialEditor.IsActive() || !m_workspaceReady || m_closingModel || m_batchExporting || m_psdBatch ||
        m_previewExport.IsBusy() || !m_openglwindows || !m_openglwindows->IsModelReady()) return;
    QString error;
    if (m_atlasEditor.IsActive() && !m_atlasEditor.Cancel(&error)) { statusBar()->showMessage(error, 15000); return; }
    if (!FinishParameterValueEditing(&error) || !m_openglwindows->FinishMeshDrag(false, &error) || !RuntimeMatchesDocument(&error)) {
        statusBar()->showMessage(error, 10000); return;
    }
    live2dstruct::CAtlasRepackPlanner::Snapshot snapshot;
    snapshot.sourceDigest = m_document.SourceDigest(); snapshot.documentRevision = m_document.Revision();
    snapshot.sources = m_document.TextureSources(); snapshot.workspace = m_document.TextureWorkspace();
    snapshot.artwork = m_document.Artwork();
    QByteArray bytes; live2dstruct::CModelAtlasState state;
    if (!m_document.BuildRuntimeMoc(bytes, &error) || !m_document.CaptureAtlasState(state, &error)) {
        statusBar()->showMessage(error, 10000); return;
    }
    snapshot.topologyDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
    for (const auto& value : state.materials) {
        const auto found = std::find_if(m_document.Drawables().cbegin(), m_document.Drawables().cend(),
                                       [&](const auto& item) { return item.id == value.id; });
        if (found == m_document.Drawables().cend()) { statusBar()->showMessage(QStringLiteral("图集素材目录与模型不一致"), 10000); return; }
        const auto reason = DrawableMaterialLockReason(*found);
        snapshot.meshes.append({value, m_document.DrawableIndices(value.id), reason.isEmpty(), reason});
    }
    {
        const QPointer<QOpenGLContext> previous(QOpenGLContext::currentContext());
        auto* surface = previous ? previous->surface() : nullptr;
        m_openglwindows->makeCurrent();
        const auto restore = qScopeGuard([previous, surface, view = QPointer<OpengGlForm>(m_openglwindows)] {
            if (previous && surface) previous->makeCurrent(surface);
            else if (view) view->doneCurrent();
        });
        auto* context = QOpenGLContext::currentContext();
        int limit = 0;
        if (context) context->functions()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
        if (limit <= 0) { statusBar()->showMessage(QStringLiteral("无法读取图集设备尺寸限制"), 10000); return; }
        snapshot.limits.maximumDimension = limit;
    }
    QStringList selection;
    for (int index = 0; index < m_selectedObjectIds.size() && index < m_selectedObjectTypes.size(); ++index)
        if (m_selectedObjectTypes[index] == QLatin1String("artmesh")) selection.append(m_selectedObjectIds[index]);
    if (selection.isEmpty() && !selectedMesh.isEmpty()) selection.append(selectedMesh);
    const QPointer<MainWindow> window(this);
    m_atlasDialog = new CAtlasEditDialog(snapshot, selection, this);
    const QPointer<CAtlasEditDialog> dialog = m_atlasDialog;
    const auto settle = CDraftDialogLifecycle::Attach(dialog, this,
        [window, dialog](QString* detail) {
            if (!window || window->m_closingModel || window->m_artworkDialog || (window->m_atlasDialog && window->m_atlasDialog != dialog)) return true;
            return !window->m_atlasEditor.IsActive() || window->m_atlasEditor.Cancel(detail);
        }, [window, dialog] {
            if (!window || window->m_closingModel || window->m_artworkDialog || (window->m_atlasDialog && window->m_atlasDialog != dialog)) return;
            window->m_atlasDialog.clear(); window->RefreshTextureEditing(false);
            if (window) window->RefreshMaterialEditing(false);
        }, [window](const QString& detail) { if (window) window->statusBar()->showMessage(detail, 15000); });
    const auto cleanup = qScopeGuard(settle);
    if (!m_atlasDialog->IsValid() || !m_atlasEditor.Begin(&error)) {
        statusBar()->showMessage(m_atlasDialog->IsValid() ? error : m_atlasDialog->Error(), 10000); return;
    }
    m_atlasDialog->SetPreviewHandler([window](const auto& plan, QString* detail) {
        if (!window || !window->m_atlasEditor.Preview(plan.state, detail) || !window) return false;
        window->RefreshTextureEditing(false);
        if (window) window->RefreshMaterialEditing(false);
        return !window.isNull();
    });
    m_atlasDialog->SetSubmissionHandler([window](const auto& plan, QString* detail) {
        if (!window) return fail(detail, QStringLiteral("模型窗口已关闭"));
        const auto revision = window->m_document.Revision();
        if (!window->m_atlasEditor.Commit(plan.state, detail) || !window) return false;
        const bool changed = revision != window->m_document.Revision();
        window->RefreshTextureEditing(changed);
        if (window) window->RefreshMaterialEditing(changed);
        return !window.isNull();
    });
    m_atlasDialog->SetCancellationHandler([window](QString* detail) {
        if (!window || !window->m_atlasEditor.Cancel(detail) || !window) return false;
        window->RefreshTextureEditing(false);
        if (window) window->RefreshMaterialEditing(false);
        return !window.isNull();
    });
    RefreshTextureEditing(false);
    if (window && dialog) dialog->exec();
}
