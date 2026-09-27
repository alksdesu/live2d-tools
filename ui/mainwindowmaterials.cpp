#include "mainwindow.h"
#include "cuveditdialog.h"
#include "cdraftdialoglifecycle.h"
#include "cinspectorpanel.h"
#include "ctexturepanel.h"
#include "psdbatchjob.h"
#include "std/cpngimage.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include <QCryptographicHash>
#include <QScopeGuard>
#include <QStatusBar>

namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
QString lockedReason(const live2dstruct::CModelDocument& document, const live2dstruct::CModelDrawable& mesh)
{
    const auto parts = [&](int index) -> QString {
        QSet<int> visited;
        while (index != -1) {
            if (index < 0 || index >= document.Parts().size() || visited.contains(index))
                return QStringLiteral("部件父链无效");
            visited.insert(index);
            const auto& part = document.Parts()[index];
            if (document.IsLocked(QStringLiteral("part"), part.id)) return QStringLiteral("所属部件已锁定：%1").arg(part.id);
            index = part.parentIndex;
        }
        return {};
    };
    if (document.IsLocked(QStringLiteral("artmesh"), mesh.id)) return QStringLiteral("网格已锁定");
    auto reason = parts(mesh.parentPartIndex);
    if (!reason.isEmpty()) return reason;
    QSet<int> visited;
    for (int index = mesh.parentDeformerIndex; index != -1;) {
        if (index < 0 || index >= document.Deformers().size() || visited.contains(index)) return QStringLiteral("变形器父链无效");
        visited.insert(index);
        const auto& deformer = document.Deformers()[index];
        const auto type = deformer.type == live2dstruct::CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation");
        if (document.IsLocked(type, deformer.id)) return QStringLiteral("父变形器已锁定：%1").arg(deformer.id);
        reason = parts(deformer.parentPartIndex);
        if (!reason.isEmpty()) return reason;
        index = document.DeformerParentIndex(deformer.id);
    }
    return {};
}
}

QString MainWindow::DrawableMaterialLockReason(const live2dstruct::CModelDrawable& mesh) const
{
    return lockedReason(m_document, mesh);
}

bool MainWindow::PublishDrawableMaterials(const live2dstruct::CModelDocument& before,
                                          const live2dstruct::CModelDocument& after,
                                          const QSet<QString>& ids, QString* error)
{
    if (m_keyformEditor.IsPublishing()) return fail(error, QStringLiteral("模型编辑正在提交，请稍后修改素材"));
    if (m_closingModel || m_applyingSession || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() ||
        !m_runtimePanelsInitialized || !m_openglwindows || !m_openglwindows->IsModelReady())
        return fail(error, QStringLiteral("请等待模型就绪，并先结束当前导出或会话恢复"));
    if (!RuntimeMatchesDocument(error)) return false;
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    if (!model || m_atlasEditor.IsActive() || model->GetTextureSources() != before.TextureSources() ||
        model->GetTextureWorkspace() != before.TextureWorkspace() || before.TextureSources() != after.TextureSources() ||
        before.TextureWorkspace() != after.TextureWorkspace())
        return fail(error, QStringLiteral("纹理来源或预览已改变，请重新打开 UV 编辑"));
    QByteArray previous, candidate;
    if (!before.BuildRuntimeMoc(previous, error) || !after.BuildRuntimeMoc(candidate, error)) return false;
    return m_openglwindows->ApplyDrawableMaterialPreview(candidate, after.SourceDigest(),
        QCryptographicHash::hash(previous, QCryptographicHash::Sha256), ids, error);
}

void MainWindow::RefreshMaterialEditing(bool changed)
{
    const QPointer<MainWindow> alive(this);
    if (m_texturePanel) m_texturePanel->SetPreviewActive(m_atlasEditor.IsActive() || m_materialEditor.IsActive());
    if (!alive) return;
    RefreshKeyformUi(false, false, false);
    if (!alive) return;
    if (m_inspectorPanel) m_inspectorPanel->RefreshSelection();
    if (!alive) return;
    if (changed) {
        m_sessionDirty = true;
        m_historySource = HistorySource::Inspector;
    }
    RefreshWindowTitle();
    if (!alive) return;
    RefreshUndoActions();
}

void MainWindow::EditUv(const QString& selectedMesh)
{
    if (m_uvDialog) { m_uvDialog->raise(); m_uvDialog->activateWindow(); return; }
    if (m_atlasEditor.IsActive() || !m_workspaceReady || m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() ||
        !m_openglwindows || !m_openglwindows->IsModelReady()) return;
    QString error;
    if (m_materialEditor.IsActive() && !m_materialEditor.Cancel(&error)) { statusBar()->showMessage(error, 10000); return; }
    if (!FinishParameterValueEditing(&error) || !m_openglwindows->FinishMeshDrag(false, &error) || !RuntimeMatchesDocument(&error)) {
        statusBar()->showMessage(error, 10000); return;
    }
    live2dstruct::CUvEditDraft::Snapshot snapshot;
    snapshot.sourceDigest = m_document.SourceDigest();
    snapshot.documentRevision = m_document.Revision();
    QByteArray moc;
    live2dstruct::CModelMaterialState materials;
    if (!m_document.BuildRuntimeMoc(moc, &error) || !m_document.CaptureDrawableMaterials(materials, &error)) {
        statusBar()->showMessage(error, 10000); return;
    }
    snapshot.topologyDigest = QCryptographicHash::hash(moc, QCryptographicHash::Sha256);
    QMap<int, QImage> images;
    live2dstruct::CTextureAssets::State pngs;
    if (!live2dstruct::CTextureWorkspace::Effective(m_document.TextureSources(), m_document.TextureWorkspace(), pngs, &error)) {
        statusBar()->showMessage(error, 10000); return;
    }
    for (auto slot = pngs.cbegin(); slot != pngs.cend(); ++slot) {
        QImage image;
        const auto size = m_document.TextureWorkspace().atlases.value(slot.key()).size;
        if (!CPngImage::Decode(*slot, image, &error) || image.size() != size) {
            statusBar()->showMessage(QStringLiteral("纹理 %1 无法读取，UV 编辑未打开").arg(slot.key()), 10000); return;
        }
        images.insert(slot.key(), image);
        snapshot.atlasSizes.insert(slot.key(), image.size());
    }
    QHash<QString, live2dstruct::CModelDrawableMaterial> byId;
    for (const auto& material : materials) byId.insert(material.id, material);
    for (const auto& drawable : m_document.Drawables()) {
        const auto material = byId.constFind(drawable.id);
        if (material == byId.cend() || material->uvs.isEmpty()) continue;
        const auto reason = DrawableMaterialLockReason(drawable);
        snapshot.meshes.append({*material, m_document.DrawableIndices(drawable.id), reason.isEmpty(), reason});
    }
    const QPointer<MainWindow> window(this);
    auto* dialog = new CUvEditDialog(snapshot, images, selectedMesh, this);
    m_uvDialog = dialog;
    const QPointer<CUvEditDialog> retained = dialog;
    const auto settle = CDraftDialogLifecycle::Attach(dialog, this,
        [window, retained](QString* detail) {
            if (!window || window->m_closingModel || (window->m_uvDialog && window->m_uvDialog != retained)) return true;
            return !window->m_materialEditor.IsActive() || window->m_materialEditor.Cancel(detail);
        }, [window, retained] {
            if (!window || window->m_closingModel || (window->m_uvDialog && window->m_uvDialog != retained)) return;
            window->m_uvDialog.clear(); window->RefreshMaterialEditing(false);
        }, [window](const QString& detail) { if (window) window->statusBar()->showMessage(detail, 15000); });
    const auto cleanup = qScopeGuard(settle);
    if (!dialog->IsValid() || !m_materialEditor.Begin(&error)) {
        statusBar()->showMessage(dialog->IsValid() ? error : dialog->Error(), 10000); return;
    }
    const auto sources = m_document.TextureSources();
    const auto textures = m_document.TextureWorkspace();
    const auto valid = [window, sources, textures](QString* detail) {
        return window && window->m_document.TextureSources() == sources && window->m_document.TextureWorkspace() == textures
            ? true : fail(detail, QStringLiteral("纹理来源已改变，请取消当前 UV 编辑"));
    };
    dialog->SetPreviewHandler([window, valid](const auto& state, QString* detail) {
        if (!valid(detail) || !window->m_materialEditor.Preview(state, detail) || !window) return false;
        window->RefreshMaterialEditing(false);
        return !window.isNull();
    });
    dialog->SetSubmissionHandler([window, valid](const auto& state, QString* detail) {
        if (!valid(detail)) return false;
        const auto revision = window->m_document.Revision();
        if (!window->m_materialEditor.Commit(state, detail) || !window) return false;
        window->RefreshMaterialEditing(revision != window->m_document.Revision());
        return !window.isNull();
    });
    dialog->SetCancellationHandler([window](QString* detail) {
        if (!window || !window->m_materialEditor.Cancel(detail) || !window) return false;
        window->RefreshMaterialEditing(false);
        return !window.isNull();
    });
    RefreshMaterialEditing(false);
    if (window && retained) retained->exec();
}
