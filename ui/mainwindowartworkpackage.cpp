#include "mainwindow.h"
#include "cexporttaskrunner.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "live2dstruct/cartworkpackage.h"
#include "std/cexportcontrol.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QProgressDialog>
#include <QScopeGuard>

bool MainWindow::RestoreArtworkPackage(OpengGlForm* expectedWindow, QString* warning)
{
    if (warning) warning->clear();
    if (m_applyingSession || m_restoringSession || !m_document.Artwork().IsEmpty()) return true;
    const QPointer<MainWindow> window(this);
    const QPointer<OpengGlForm> surface(expectedWindow);
    if (!surface || m_openglwindows != surface) return false;
    const auto modelPath = surface->GetCurrentModelJson();
    const QFileInfo modelFile(modelPath);
    const auto manifest = live2dstruct::CArtworkPackage::ManifestFile(modelFile.fileName());
    const QFileInfo attachment(modelFile.absoluteDir().filePath(manifest));
    if (manifest.isEmpty() || (!attachment.exists() && !attachment.isSymLink())) return true;
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    if (!model) return false;
    const auto document = m_document;
    const auto& sourceSettings = model->GetModelSettingsBytes();
    const QByteArray settings(sourceSettings.constData(), sourceSettings.size());
    const auto sourcePath = document.SourcePath();
    const auto sourceDigest = document.SourceDigest();
    const auto revision = document.Revision();
    const auto unchanged = [&] {
        return window && surface && window->m_openglwindows == surface &&
            window->m_document.SourcePath() == sourcePath && window->m_document.SourceDigest() == sourceDigest &&
            window->m_document.Revision() == revision && surface->GetCurrentModelJson() == modelPath;
    };
    auto control = std::make_shared<CExportControl>();
    auto result = std::make_shared<live2dstruct::CArtworkPackage::Loaded>();
    m_artworkLoadControl = control;
    m_artworkLoading = true;
    const auto destroyed = connect(this, &QObject::destroyed, [control] { control->Cancel(); });
    const auto finishLoading = qScopeGuard([window, control, destroyed] {
        QObject::disconnect(destroyed);
        if (window && window->m_artworkLoadControl == control) {
            window->m_artworkLoadControl.reset(); window->m_artworkLoading = false;
        }
    });
    QPointer<QProgressDialog> progress = new QProgressDialog(QStringLiteral("正在恢复来源素材…"), QStringLiteral("取消"), 0, 0, this);
    progress->setObjectName(QStringLiteral("artworkPackageProgress"));
    progress->setWindowTitle(QStringLiteral("载入来源素材"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    connect(progress, &QProgressDialog::canceled, this, [control] { control->Cancel(); });
    const auto closeProgress = qScopeGuard([progress] {
        if (progress) { progress->close(); if (progress) progress->deleteLater(); }
    });
    RefreshExportActions(); if (!window) return false;
    RefreshUndoActions(); if (!window) return false;
    progress->show(); if (!window) return false;
    QString error;
    const bool loaded = CExportTaskRunner::Run([modelPath, settings, document, result, control](const auto&, QString* detail) {
        return live2dstruct::CArtworkPackage::Load(modelPath, settings, document, *result, control.get(), detail);
    }, control.get(), &error);
    if (progress) progress->hide();
    if (!unchanged()) return false;
    if (!loaded) {
        if (warning) *warning = control->IsCancellationRequested() ? QStringLiteral("已取消来源读取；当前运行模型可以继续使用") : error;
        return true;
    }
    if (!result->present) return true;
    auto candidate = m_document;
    if (!candidate.SetTextureWorkspace(result->state.workspace, &error) || !candidate.SetArtwork(result->state.artwork, &error)) {
        if (warning) *warning = error;
        return true;
    }
    live2dstruct::CModelMaterialState materials;
    QByteArray before, after;
    if (!candidate.CaptureDrawableMaterials(materials, &error) || materials != result->state.materials ||
        !m_document.BuildRuntimeMoc(before, &error) || !candidate.BuildRuntimeMoc(after, &error)) {
        if (warning) *warning = error.isEmpty() ? QStringLiteral("来源附件与当前模型材质不一致") : error;
        return true;
    }
    if (!surface->ApplyAtlasPreview(after, sourceDigest, QCryptographicHash::hash(before, QCryptographicHash::Sha256),
                                   m_document.TextureWorkspaceFingerprint(), candidate.TextureWorkspace(), {}, &error)) {
        if (!unchanged()) return false;
        if (warning) *warning = error;
        return true;
    }
    if (!unchanged()) return false;
    m_document.CommitCandidate(std::move(candidate));
    m_document.ClearEditHistory();
    return true;
}
