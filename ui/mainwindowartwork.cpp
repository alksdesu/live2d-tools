#include "mainwindow.h"
#include "cpsdimportdialog.h"
#include "ccmosourceimportdialog.h"
#include "live2dstruct/cartworksourcelibraryplanner.h"
#include "catlaseditdialog.h"
#include "cdraftdialoglifecycle.h"
#include "cexporttaskrunner.h"
#include "psdbatchjob.h"
#include "psd/cpsdreader.h"
#include "std/cexportcontrol.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QVBoxLayout>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QProgressDialog>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>
#include <algorithm>

namespace {
using Artwork = live2dstruct::CModelArtwork;
using Planner = live2dstruct::CArtworkUpdatePlanner;
using LibraryPlanner = live2dstruct::CArtworkSourceLibraryPlanner;
bool fail(QString* error, const QString& text) { if (error) *error = text; return false; }
bool captureSnapshot(const live2dstruct::CModelDocument& document, const QPointer<OpengGlForm>& surface, Planner::Snapshot& snapshot,
                     const std::function<QString(const live2dstruct::CModelDrawable&)>& lockReason, QString* error)
{
    snapshot.sourceDigest = document.SourceDigest(); snapshot.documentRevision = document.Revision(); snapshot.sources = document.TextureSources();
    if (!document.CaptureAtlasState(snapshot.baseline, error)) return false;
    QHash<QString, const live2dstruct::CModelDrawable*> drawables;
    for (const auto& drawable : document.Drawables()) drawables.insert(drawable.id, &drawable);
    for (const auto& material : snapshot.baseline.materials) {
        const auto found = drawables.constFind(material.id);
        if (found == drawables.cend()) return fail(error, QStringLiteral("素材目录与模型对象不一致"));
        const auto reason = lockReason(**found); snapshot.meshes.append({material, document.DrawableIndices(material.id), reason.isEmpty(), reason});
    }
    {
        const QPointer<QOpenGLContext> previous(QOpenGLContext::currentContext()); auto* previousSurface = previous ? previous->surface() : nullptr;
        surface->makeCurrent();
        const auto restore = qScopeGuard([previous, previousSurface, surface] { if (previous && previousSurface) previous->makeCurrent(previousSurface); else if (surface) surface->doneCurrent(); });
        auto* context = QOpenGLContext::currentContext(); int limit = 0; if (context) context->functions()->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
        if (limit <= 0) return fail(error, QStringLiteral("无法读取图集设备尺寸限制"));
        snapshot.limits.maximumDimension = limit;
    }
    return true;
}
bool freezeSnapshot(Planner::Snapshot& s, QString* detail, CExportControl* control)
{
    if (!control->Report(CExportControl::Phase::Preparing, 0, 0, QStringLiteral("正在冻结模型与图集来源"))) return false;
    for (auto it = s.sources.begin(); it != s.sources.end(); ++it) {
        live2dstruct::CTextureAssets::Source frozen;
        if (!live2dstruct::CTextureAssets::MakeSource(it->path, it->png, frozen, detail) || !control->Checkpoint()) return false;
        *it = std::move(frozen);
    }
    if (!live2dstruct::CTextureWorkspace::Freeze(s.sources, s.baseline.workspace, s.baseline.workspace, detail) ||
        !Artwork::Freeze(s.baseline.artwork, s.baseline.artwork, detail) || !control->Checkpoint()) return false;
    return true;
}
QString selectArtworkItem(QWidget* parent, const QString& objectName, const QString& title, const QString& prompt,
                          const QVector<QPair<QString, QString>>& items, const QString& current = {})
{
    if (items.isEmpty()) return {};
    QPointer<QDialog> dialog = new QDialog(parent); dialog->setObjectName(objectName); dialog->setWindowTitle(title);
    const auto cleanup = qScopeGuard([dialog] { if (dialog) dialog->deleteLater(); });
    auto* layout = new QVBoxLayout(dialog); auto* label = new QLabel(prompt, dialog); label->setTextFormat(Qt::PlainText); label->setWordWrap(true); layout->addWidget(label);
    QPointer<QComboBox> choices = new QComboBox(dialog); choices->setObjectName(QStringLiteral("artworkChoice")); choices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    for (const auto& item : items) choices->addItem(item.second, item.first);
    const int selected = choices->findData(current); choices->setCurrentIndex(selected < 0 ? 0 : selected); layout->addWidget(choices);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog); layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept); QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    dialog->resize(dialog->fontMetrics().averageCharWidth() * 68, dialog->sizeHint().height());
    const int result = dialog->exec(); return result == QDialog::Accepted && dialog && choices ? choices->currentData().toString() : QString();
}
QString selectFamily(QWidget* parent, const Artwork::State& artwork, bool reimportableOnly)
{
    QVector<QPair<QString, QString>> items;
    for (const auto& family : artwork.families) if (!reimportableOnly || family.kind == Artwork::SourceKind::Psd || family.kind == Artwork::SourceKind::Cmo3) {
        const auto revision = artwork.revisions.value(family.currentRevision);
        const auto name = family.name.isEmpty() ? QStringLiteral("未命名来源") : family.name;
        const auto document = revision.sourceDocumentId.isEmpty() ? QString() : QStringLiteral(" · 文档 %1").arg(revision.sourceDocumentId);
        items.append({family.id, QStringLiteral("%1. %2 · %3%4").arg(items.size() + 1).arg(name, revision.fileName, document)});
    }
    return selectArtworkItem(parent, QStringLiteral("artworkSourceChoice"), reimportableOnly ? QStringLiteral("重新导入来源") : QStringLiteral("管理素材关联"),
        QStringLiteral("选择来源素材；图层同名不会自动合并来源"), items);
}
}

void MainWindow::ConfigureArtworkActions()
{
    m_importPsdAction = new QAction(QStringLiteral("导入 PSD 素材…"), this); m_importPsdAction->setObjectName(QStringLiteral("importPsdArtwork"));
    m_importCmoAction = new QAction(QStringLiteral("导入 CMO3 素材…"), this); m_importCmoAction->setObjectName(QStringLiteral("importCmoArtwork"));
    m_reimportPsdAction = new QAction(QStringLiteral("重新导入来源…"), this); m_reimportPsdAction->setObjectName(QStringLiteral("reimportPsdArtwork"));
    m_manageArtworkAction = new QAction(QStringLiteral("管理素材关联…"), this); m_manageArtworkAction->setObjectName(QStringLiteral("manageArtworkAssociations"));
    connect(m_importPsdAction, &QAction::triggered, this, [this] { ImportPsd(false); });
    connect(m_importCmoAction, &QAction::triggered, this, &MainWindow::ImportCmoSources);
    connect(m_reimportPsdAction, &QAction::triggered, this, [this] { ImportPsd(true); });
    connect(m_manageArtworkAction, &QAction::triggered, this, &MainWindow::ManageArtwork);
    RefreshArtworkActions();
}

void MainWindow::RefreshArtworkActions()
{
    const QPointer<MainWindow> alive(this);
    const bool enabled = m_workspaceReady && m_runtimePanelsInitialized && !m_closingModel && !m_artworkLoading && !m_nameImportControl && !m_applyingSession &&
        !m_batchExporting && !m_psdBatch && !m_previewExport.IsBusy() && !m_materialEditor.IsActive() && !m_atlasEditor.IsActive() &&
        m_openglwindows && m_openglwindows->IsModelReady() && m_document.HasTextureWorkspace();
    bool reimportable = false; for (const auto& family : m_document.Artwork().families) if (family.kind == Artwork::SourceKind::Psd || family.kind == Artwork::SourceKind::Cmo3) { reimportable = true; break; }
    const bool sources = !m_document.Artwork().families.isEmpty();
    if (m_importPsdAction) m_importPsdAction->setEnabled(enabled);
    if (!alive) return;
    if (m_importCmoAction) m_importCmoAction->setEnabled(enabled);
    if (!alive) return;
    if (m_reimportPsdAction) m_reimportPsdAction->setEnabled(enabled && reimportable);
    if (!alive) return;
    if (m_manageArtworkAction) m_manageArtworkAction->setEnabled(enabled && sources);
}

void MainWindow::ImportPsd(bool reimport)
{
    if (m_artworkDialog) { const auto dialog = m_artworkDialog; dialog->raise(); if (dialog) dialog->activateWindow(); return; }
    QString error; if (!AuthoringReady(&error) || m_batchExporting || m_psdBatch || m_previewExport.IsBusy()) { statusBar()->showMessage(error, 8000); return; }
    const QPointer<MainWindow> alive(this); const auto source = m_document.SourceDigest(); const auto revision = m_document.Revision();
    const auto surface = QPointer<OpengGlForm>(m_openglwindows); const auto artwork = m_document.Artwork();
    QString family;
    if (reimport) { family = selectFamily(this, artwork, true); if (!alive || family.isEmpty()) return; }
    const auto previous = artwork.revisions.value(artwork.families.value(family).currentRevision);
    const bool cmo = artwork.families.value(family).kind == Artwork::SourceKind::Cmo3;
    const auto path = QFileDialog::getOpenFileName(this, reimport ? QStringLiteral("选择重新导入的来源") : QStringLiteral("导入 PSD 素材"), previous.path,
        cmo ? QStringLiteral("Cubism 工程 (*.cmo3)") : QStringLiteral("Photoshop PSD (*.psd)"));
    if (!alive || path.isEmpty()) return;
    if (m_document.SourceDigest() != source || m_document.Revision() != revision || m_openglwindows != surface) { statusBar()->showMessage(QStringLiteral("模型已改变，请重新选择来源素材"), 10000); return; }
    EditArtwork(family, path);
}

void MainWindow::ImportCmoSources()
{
    if (m_artworkDialog) { const auto dialog = m_artworkDialog; dialog->raise(); if (dialog) dialog->activateWindow(); return; }
    QString error;
    if (!AuthoringReady(&error) || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() || m_applyingSession) {
        statusBar()->showMessage(error.isEmpty() ? QStringLiteral("请先结束当前编辑、导出或会话恢复") : error, 10000); return;
    }
    const QPointer<MainWindow> window(this); const QPointer<OpengGlForm> surface(m_openglwindows);
    if (!FinishParameterValueEditing(&error) || !window || !surface || !surface->FinishMeshDrag(false, &error) || !window || !RuntimeMatchesDocument(&error)) {
        if (window) statusBar()->showMessage(error, 10000); return;
    }
    const auto sourcePath = m_document.SourcePath(), modelPath = surface->GetCurrentModelJson();
    auto snapshot = std::make_shared<Planner::Snapshot>();
    if (!captureSnapshot(m_document, surface, *snapshot, [this](const auto& drawable) { return DrawableMaterialLockReason(drawable); }, &error)) {
        statusBar()->showMessage(error, 10000); return;
    }
    const auto unchanged = [window, surface, sourcePath, modelPath, snapshot] {
        return window && surface && window->m_openglwindows == surface && window->m_document.SourcePath() == sourcePath &&
            window->m_document.SourceDigest() == snapshot->sourceDigest && window->m_document.Revision() == snapshot->documentRevision && surface->GetCurrentModelJson() == modelPath;
    };
    const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("导入 CMO3 素材"), {}, QStringLiteral("Cubism 工程 (*.cmo3)"));
    if (!window || path.isEmpty()) return;
    if (!unchanged() || !AuthoringReady(&error)) { statusBar()->showMessage(QStringLiteral("模型已改变，请重新选择来源素材"), 10000); return; }
    auto project = std::make_shared<cmo3::CCmo3Project>(); auto control = std::make_shared<CExportControl>();
    m_artworkLoadControl = control; m_artworkLoading = true;
    const auto destroyed = connect(this, &QObject::destroyed, [control] { control->Cancel(); });
    const auto releaseLoading = [window, control] {
        if (!window || window->m_artworkLoadControl != control) return;
        window->m_artworkLoadControl.reset(); window->m_artworkLoading = false; window->RefreshExportActions();
        if (window) window->RefreshUndoActions();
    };
    const auto finishLoading = qScopeGuard([releaseLoading, destroyed] { QObject::disconnect(destroyed); releaseLoading(); });
    QPointer<QProgressDialog> progress = new QProgressDialog(QStringLiteral("正在读取 CMO3 原始图层…"), QStringLiteral("取消"), 0, 0, this);
    progress->setObjectName(QStringLiteral("cmoArtworkReadProgress")); progress->setWindowTitle(QStringLiteral("导入 CMO3 素材"));
    progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0);
    connect(progress, &QProgressDialog::canceled, this, [control] { control->Cancel(); });
    const auto closeProgress = qScopeGuard([progress] { if (progress) { progress->close(); if (progress) progress->deleteLater(); } });
    QTimer progressTimer; progressTimer.setInterval(50);
    connect(&progressTimer, &QTimer::timeout, &progressTimer, [progress, control] {
        if (!progress) return; const auto state = control->Snapshot();
        progress->setLabelText(control->IsCancellationRequested() ? QStringLiteral("正在取消来源导入…") : state.detail.isEmpty() ? QStringLiteral("正在准备来源素材…") : state.detail);
    });
    RefreshExportActions(); if (!window) return; RefreshUndoActions(); if (!window) return;
    progress->show(); if (!window) return; progressTimer.start();
    const bool loaded = CExportTaskRunner::Run([snapshot, project, path, control](const auto&, QString* detail) {
        if (!freezeSnapshot(*snapshot, detail, control.get()) || !cmo3::CCmo3Project::OpenFile(path, *project, detail, control.get())) return false;
        if (!project->Artwork()) return fail(detail, project->ArtworkError());
        return control->Checkpoint();
    }, control.get(), &error);
    progressTimer.stop(); if (progress) progress->hide(); if (!window) return;
    if (!loaded) { statusBar()->showMessage(control->IsCancellationRequested() ? QStringLiteral("来源导入已取消，模型保持不变") : QStringLiteral("CMO3 来源读取失败：%1").arg(error), 15000); return; }
    if (!unchanged()) { statusBar()->showMessage(QStringLiteral("读取期间模型已改变，来源结果已丢弃"), 15000); return; }
    QPointer<CCmoSourceImportDialog> selection = new CCmoSourceImportDialog(*project, this);
    const auto closeSelection = qScopeGuard([selection] { if (selection) selection->deleteLater(); });
    const int selected = selection->exec(); if (!window || !selection || selected != QDialog::Accepted) return;
    const auto documentIds = selection->SelectedDocumentIds(); const auto policy = selection->ColorPolicy();
    if (!unchanged() || documentIds.isEmpty()) { statusBar()->showMessage(QStringLiteral("模型或选中文档已改变，未导入任何来源"), 15000); return; }
    auto plan = std::make_shared<LibraryPlanner::Plan>();
    if (progress) progress->show(); if (!window) return; progressTimer.start();
    const bool prepared = CExportTaskRunner::Run([snapshot, project, documentIds, policy, plan, control](const auto&, QString* detail) {
        QVector<LibraryPlanner::Source> sources;
        for (const auto& documentId : documentIds) {
            if (!control->Checkpoint()) return false;
            const CPsdSourceDocument* document = nullptr;
            for (const auto& entry : project->Artwork()->documents) if (const auto* candidate = entry.Reconstructed(); candidate && candidate->id == documentId) {
                if (document) return fail(detail, QStringLiteral("CMO3 包含重复文档标识，未导入任何来源"));
                document = candidate;
            }
            if (!document) return fail(detail, QStringLiteral("CMO3 中找不到所选文档，未导入任何来源"));
            LibraryPlanner::Source source; source.familyName = document->name;
            const auto family = QStringLiteral("source:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
            const auto revision = live2dstruct::CArtworkSourceImport::RevisionIdentity(*project, documentId, family, policy, {});
            if (!live2dstruct::CArtworkSourceImport::Prepare(*project, documentId, family, revision, policy, {}, source.incoming, detail, control.get())) return false;
            sources.append(std::move(source));
        }
        return LibraryPlanner::Build(*snapshot, sources, *plan, detail, control.get()) && control->Checkpoint();
    }, control.get(), &error);
    progressTimer.stop(); if (progress) progress->hide(); if (!window) return;
    releaseLoading(); if (!window) return;
    if (!prepared) { statusBar()->showMessage(control->IsCancellationRequested() ? QStringLiteral("来源导入已取消，模型保持不变") : QStringLiteral("来源导入失败：%1").arg(error), 15000); return; }
    if (!unchanged() || !AuthoringReady(&error) || !RuntimeMatchesDocument(&error) || !FinishParameterValueEditing(&error) || !window || !surface ||
        !surface->FinishMeshDrag(false, &error) || !window || !unchanged()) {
        if (window) statusBar()->showMessage(error.isEmpty() ? QStringLiteral("文档已改变，未导入任何来源") : error, 15000); return;
    }
    if (!LibraryPlanner::Validate(*snapshot, *plan, &error) || !m_atlasEditor.Submit(plan->state, &error) || !window) {
        if (window) statusBar()->showMessage(QStringLiteral("来源导入失败：%1").arg(error), 15000); return;
    }
    RefreshTextureEditing(true); if (!window) return; RefreshMaterialEditing(true); if (!window) return;
    statusBar()->showMessage(QStringLiteral("已保存 %1 个来源文档、%2 个图层与文件夹；模型外观保持不变，可一次撤销。可从已保存来源恢复 PSD。")
        .arg(plan->documentCount).arg(plan->layerCount), 15000);
}

void MainWindow::ManageArtwork()
{
    if (m_artworkDialog) { const auto dialog = m_artworkDialog; dialog->raise(); if (dialog) dialog->activateWindow(); return; }
    QString error; if (!AuthoringReady(&error)) { statusBar()->showMessage(error, 8000); return; }
    const QPointer<MainWindow> alive(this); const auto source = m_document.SourceDigest(); const auto revision = m_document.Revision();
    const auto surface = QPointer<OpengGlForm>(m_openglwindows); const auto artwork = m_document.Artwork();
    const auto family = selectFamily(this, artwork, false); if (!alive || family.isEmpty()) return;
    QVector<QPair<QString, QString>> revisions;
    for (const auto& item : artwork.revisions) if (item.familyId == family) {
        const auto current = item.id == artwork.families[family].currentRevision ? QStringLiteral(" · 目录当前版本") : QString();
        revisions.append({item.id, QStringLiteral("版本 %1 · %2 · %3 × %4 px%5").arg(revisions.size() + 1).arg(item.fileName).arg(item.canvasSize.width()).arg(item.canvasSize.height()).arg(current)});
    }
    const auto selected = selectArtworkItem(this, QStringLiteral("artworkRevisionChoice"), QStringLiteral("选择已保存来源版本"),
        QStringLiteral("保留原样的对象继续使用其旧版素材；选择版本后可逐对象更新"), revisions, artwork.families[family].currentRevision);
    if (!alive || selected.isEmpty()) return;
    if (m_document.SourceDigest() != source || m_document.Revision() != revision || m_openglwindows != surface) { statusBar()->showMessage(QStringLiteral("模型已改变，请重新选择来源素材"), 10000); return; }
    EditArtwork(family, {}, selected);
}

void MainWindow::EditArtwork(const QString& familyId, const QString& path, const QString& revisionId)
{
    if (m_artworkDialog) { const auto dialog = m_artworkDialog; dialog->raise(); if (dialog) dialog->activateWindow(); return; }
    QString error;
    if (!AuthoringReady(&error) || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() || m_artworkLoading || m_applyingSession) {
        statusBar()->showMessage(error.isEmpty() ? QStringLiteral("请先结束当前编辑、导出或会话恢复") : error, 10000); return;
    }
    const QPointer<MainWindow> window(this); const QPointer<OpengGlForm> surface(m_openglwindows);
    if (!FinishParameterValueEditing(&error) || !window || !surface || !surface->FinishMeshDrag(false, &error) || !window || !RuntimeMatchesDocument(&error)) {
        if (window) statusBar()->showMessage(error, 10000); return;
    }
    const auto sourcePath = m_document.SourcePath(), modelPath = surface->GetCurrentModelJson();
    CPsdImportDialog::Context initial;
    initial.meshDisplayNames = m_document.DisplayNames().artMeshes;
    auto& snapshot = initial.snapshot;
    if (!captureSnapshot(m_document, surface, snapshot, [this](const auto& drawable) { return DrawableMaterialLockReason(drawable); }, &error)) {
        statusBar()->showMessage(error, 10000); return;
    }
    const auto previousFamily = snapshot.baseline.artwork.families.value(familyId);
    if ((!familyId.isEmpty() && previousFamily.id.isEmpty()) || (path.isEmpty() && familyId.isEmpty()) ||
        (!path.isEmpty() && !familyId.isEmpty() && previousFamily.kind != Artwork::SourceKind::Psd && previousFamily.kind != Artwork::SourceKind::Cmo3)) { statusBar()->showMessage(QStringLiteral("所选来源不存在或不支持重新导入"), 10000); return; }
    const auto savedRevision = revisionId.isEmpty() ? previousFamily.currentRevision : revisionId;
    if (path.isEmpty() && (!snapshot.baseline.artwork.revisions.contains(savedRevision) || snapshot.baseline.artwork.revisions[savedRevision].familyId != familyId)) {
        statusBar()->showMessage(QStringLiteral("所选素材版本不属于当前来源"), 10000); return;
    }
    const QString family = familyId.isEmpty() ? QStringLiteral("source:") + QUuid::createUuid().toString(QUuid::WithoutBraces) : familyId;
    initial.familyName = previousFamily.id.isEmpty() ? QFileInfo(path).completeBaseName() : previousFamily.name;
    for (int index = 0; index < m_selectedObjectIds.size() && index < m_selectedObjectTypes.size(); ++index)
        if (m_selectedObjectTypes[index] == QLatin1String("artmesh")) initial.selectedMeshIds.append(m_selectedObjectIds[index]);
    auto prepared = std::make_shared<CPsdImportDialog::Context>(std::move(initial));
    auto control = std::make_shared<CExportControl>(); m_artworkLoadControl = control; m_artworkLoading = true;
    const auto destroyed = connect(this, &QObject::destroyed, [control] { control->Cancel(); });
    const auto releaseLoading = [window, control] {
        if (!window || window->m_artworkLoadControl != control) return;
        window->m_artworkLoadControl.reset(); window->m_artworkLoading = false; window->RefreshExportActions();
        if (window) window->RefreshUndoActions();
    };
    const auto finishLoading = qScopeGuard([releaseLoading, destroyed] { QObject::disconnect(destroyed); releaseLoading(); });
    QPointer<QProgressDialog> progress = new QProgressDialog(QStringLiteral("正在读取来源素材…"), QStringLiteral("取消"), 0, 0, this);
    progress->setObjectName(QStringLiteral("artworkReadProgress")); progress->setWindowTitle(QStringLiteral("准备来源素材")); progress->setWindowModality(Qt::WindowModal); progress->setMinimumDuration(0);
    connect(progress, &QProgressDialog::canceled, this, [control] { control->Cancel(); });
    const auto closeProgress = qScopeGuard([progress] { if (progress) { progress->close(); if (progress) progress->deleteLater(); } });
    QTimer progressTimer; progressTimer.setInterval(50);
    connect(&progressTimer, &QTimer::timeout, &progressTimer, [progress, control] {
        if (!progress) return; const auto state = control->Snapshot();
        progress->setLabelText(control->IsCancellationRequested() ? QStringLiteral("正在取消来源读取…") : state.detail.isEmpty() ? QStringLiteral("正在读取来源素材…") : state.detail);
    });
    RefreshExportActions(); if (!window) return; RefreshUndoActions(); if (!window) return;
    progress->show(); if (!window) return; progressTimer.start();
    const bool loaded = CExportTaskRunner::Run([prepared, control, family, savedRevision, path](const auto&, QString* detail) {
        auto& s = prepared->snapshot;
        if (!freezeSnapshot(s, detail, control.get())) return false;
        if (path.isEmpty()) {
            const auto revision = s.baseline.artwork.revisions[savedRevision]; prepared->incoming.revision = revision;
            prepared->incoming.kind = s.baseline.artwork.families[family].kind;
            for (const auto& layer : revision.layers) if (!layer.imageDigest.isEmpty()) prepared->incoming.images.insert(layer.imageDigest, s.baseline.artwork.images[layer.imageDigest]);
            prepared->incoming.diagnostics.append(QStringLiteral("使用已保存的来源像素；不需要磁盘上的原始文件。"));
        } else {
            const auto previous = s.baseline.artwork.revisions.value(s.baseline.artwork.families.value(family).currentRevision);
            const auto policy = previous.id.isEmpty() ? Artwork::ColorPolicy::ConvertToSRgb : previous.colorPolicy;
            const auto mapping = previous.id.isEmpty() ? QTransform() : previous.documentToFamily;
            if (s.baseline.artwork.families.value(family).kind == Artwork::SourceKind::Cmo3) {
                if (!control->Report(CExportControl::Phase::Preparing, 0, 0, QStringLiteral("正在读取 CMO3 文件"))) return false;
                cmo3::CCmo3Project project; if (!cmo3::CCmo3Project::OpenFile(path, project, detail, control.get())) return false;
                const auto identity = live2dstruct::CArtworkSourceImport::RevisionIdentity(project, previous.sourceDocumentId, family, policy, mapping);
                if (!live2dstruct::CArtworkSourceImport::Prepare(project, previous.sourceDocumentId, family, identity, policy, mapping, prepared->incoming, detail, control.get())) return false;
            } else {
                if (!control->Report(CExportControl::Phase::Preparing, 0, 0, QStringLiteral("正在读取 PSD 文件"))) return false;
                CPsdReadDocument source; if (!CPsdReader::OpenFile(path, source, detail, control.get())) return false;
                const auto identity = live2dstruct::CArtworkSourceImport::RevisionIdentity(*source.Directory(), family, policy, mapping);
                if (!live2dstruct::CArtworkSourceImport::Prepare(source, family, identity, policy, mapping, prepared->incoming, detail, control.get())) return false;
            }
        }
        return control->Checkpoint();
    }, control.get(), &error);
    progressTimer.stop(); if (progress) progress->hide(); if (!window) return;
    releaseLoading(); if (!window) return;
    if (!loaded) { statusBar()->showMessage(control->IsCancellationRequested() ? QStringLiteral("来源素材读取已取消，模型保持不变") : QStringLiteral("来源素材读取失败：%1").arg(error), 15000); return; }
    const auto unchanged = [&] {
        return window && surface && window->m_openglwindows == surface && window->m_document.SourcePath() == sourcePath &&
            window->m_document.SourceDigest() == prepared->snapshot.sourceDigest && window->m_document.Revision() == prepared->snapshot.documentRevision && surface->GetCurrentModelJson() == modelPath;
    };
    if (!unchanged()) { statusBar()->showMessage(QStringLiteral("读取期间模型或文档已改变，来源结果已丢弃，请重新打开"), 15000); return; }
    if (!AuthoringReady(&error) || !RuntimeMatchesDocument(&error) || !FinishParameterValueEditing(&error) || !window || !surface ||
        !surface->FinishMeshDrag(false, &error) || !window || !unchanged()) {
        if (window) statusBar()->showMessage(error.isEmpty() ? QStringLiteral("文档已改变，来源结果已丢弃") : error, 10000); return;
    }
    m_artworkDialog = new CPsdImportDialog(*prepared, this); const QPointer<CPsdImportDialog> dialog = m_artworkDialog;
    const auto settle = CDraftDialogLifecycle::Attach(dialog, this,
        [window, dialog](QString* detail) {
            if (!window || window->m_closingModel || window->m_atlasDialog || (window->m_artworkDialog && window->m_artworkDialog != dialog)) return true;
            return !window->m_atlasEditor.IsActive() || window->m_atlasEditor.Cancel(detail);
        }, [window, dialog] {
            if (!window || window->m_closingModel || window->m_atlasDialog || (window->m_artworkDialog && window->m_artworkDialog != dialog)) return;
            window->m_artworkDialog.clear(); window->RefreshTextureEditing(false); if (window) window->RefreshMaterialEditing(false);
        }, [window](const QString& detail) { if (window) window->statusBar()->showMessage(detail, 15000); });
    const auto cleanup = qScopeGuard(settle);
    if (!dialog->IsValid() || !m_atlasEditor.Begin(&error)) { statusBar()->showMessage(dialog->IsValid() ? error : dialog->Error(), 10000); return; }
    const auto frozen = prepared->snapshot;
    dialog->SetPreviewHandler([window, frozen](const auto& plan, QString* detail) {
        if (!window || !Planner::Validate(frozen, plan, detail) || !window->m_atlasEditor.Preview(plan.state, detail) || !window) return false;
        window->RefreshTextureEditing(false); if (window) window->RefreshMaterialEditing(false); return !window.isNull();
    });
    dialog->SetSubmissionHandler([window, frozen](const auto& plan, QString* detail) {
        if (!window) return fail(detail, QStringLiteral("模型窗口已关闭"));
        if (!Planner::Validate(frozen, plan, detail, true)) return false;
        const auto revision = window->m_document.Revision(); if (!window->m_atlasEditor.Commit(plan.state, detail) || !window) return false;
        const bool changed = revision != window->m_document.Revision(); window->RefreshTextureEditing(changed); if (window) window->RefreshMaterialEditing(changed); return !window.isNull();
    });
    dialog->SetCancellationHandler([window](QString* detail) {
        if (!window || !window->m_atlasEditor.Cancel(detail) || !window) return false;
        window->RefreshTextureEditing(false); if (window) window->RefreshMaterialEditing(false); return !window.isNull();
    });
    RefreshTextureEditing(false); if (window && dialog) dialog->exec();
}
