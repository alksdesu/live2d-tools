#include "mainwindow.h"
#include "cdeformerrenamedialog.h"
#include "cdeformergriddialog.h"
#include "cobjectnamesimportdialog.h"
#include "cmodeltreepanel.h"
#include "cinspectorpanel.h"
#include "cblendshapepanel.h"
#include "cexporttaskrunner.h"
#include "psdbatchjob.h"
#include "para/ccidjson.h"
#include "cmo3/ccmo3namereader.h"
#include "std/cexportcontrol.h"
#include <QFileDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QScopeGuard>
#include <QStatusBar>
#include <QTimer>

namespace {
using Names = live2dstruct::CModelDisplayNames;
using Reader = cmo3::CCmo3NameReader;
QString owned(const QString& value) { return QString(value.constData(), value.size()); }
QString matchStatus(Reader::MatchStatus status)
{
    switch (status) {
    case Reader::MatchStatus::Compatible: return QStringLiteral("结构匹配");
    case Reader::MatchStatus::MissingTarget: return QStringLiteral("模型中没有对应对象");
    case Reader::MatchStatus::MissingSource: return QStringLiteral("工程中没有对应对象");
    case Reader::MatchStatus::Conflict: return QStringLiteral("结构或身份冲突");
    case Reader::MatchStatus::InvalidName: return QStringLiteral("工程名称无效");
    }
    return {};
}
}

bool MainWindow::InitializeDisplayNames(live2dstruct::CModelDocument& document, QString* error) const
{
    if (error) error->clear();
    auto imported = m_cidJson ? m_cidJson->DisplayNames() : Names::State();
    live2dstruct::CModelDocument source;
    if (!source.LoadBytes(document.SourceBytes(), document.SourcePath(), error)) return false;
    const auto retain = [](const auto& objects, auto& entries) {
        QSet<QString> ids;
        for (const auto& object : objects) ids.insert(object.id);
        for (auto entry = entries.begin(); entry != entries.end();)
            if (!ids.contains(entry.key()) || entry.value().isEmpty()) entry = entries.erase(entry); else ++entry;
    };
    if (!m_cidJson || !m_cidJson->HasDisplayNamesExtension()) {
        retain(source.Parts(), imported.parts);
        retain(source.Drawables(), imported.artMeshes);
        if (!m_cidJson || !m_cidJson->HasDeformerDisplayNamesExtension()) retain(source.Deformers(), imported.deformers);
    }
    if (!source.SetDisplayNames(imported, error)) return false;
    auto names = source.DisplayNames();
    retain(document.Parts(), names.parts);
    retain(document.Drawables(), names.artMeshes);
    retain(document.Deformers(), names.deformers);
    if (document.HasDeformerStructure())
        for (auto entry = names.deformers.begin(); entry != names.deformers.end();)
            if (!document.SourceDeformerIds().contains(entry.key())) entry = names.deformers.erase(entry); else ++entry;
    return document.SetDisplayNames(names, error);
}

void MainWindow::RefreshDisplayNames()
{
    const QPointer<MainWindow> alive(this);
    if (m_treePanel) m_treePanel->ApplyDisplayNames(m_document.DisplayNames());
    if (!alive) return;
    if (m_inspectorPanel) m_inspectorPanel->RefreshSelection();
    if (!alive) return;
    if (m_blendShapePanel) m_blendShapePanel->RefreshDisplayNames();
    if (!alive) return;
    RefreshWindowTitle();
    if (alive) RefreshUndoActions();
    if (alive) RefreshObjectNameActions();
}

void MainWindow::ConfigureObjectNameActions()
{
    m_renameObjectAction = new QAction(QStringLiteral("重命名对象…"), this);
    m_renameObjectAction->setObjectName(QStringLiteral("renameObject"));
    m_renameObjectAction->setStatusTip(QStringLiteral("修改显示名称，保留 ID、层级和绑定"));
    m_renameObjectAction->setEnabled(false);
    connect(m_renameObjectAction, &QAction::triggered, this, [this] {
        if (m_selectedObjectIds.size() == 1 && m_selectedObjectTypes.size() == 1)
            RenameObject(m_selectedObjectIds.first(), m_selectedObjectTypes.first());
    });
    m_importObjectNamesAction = new QAction(QStringLiteral("从 CMO3 导入名称…"), this);
    m_importObjectNamesAction->setObjectName(QStringLiteral("importObjectNames"));
    m_importObjectNamesAction->setStatusTip(QStringLiteral("预览结构匹配的对象名称，默认保留已有名称"));
    m_importObjectNamesAction->setEnabled(false);
    connect(m_importObjectNamesAction, &QAction::triggered, this, [this] { ImportObjectNames(); });
}

void MainWindow::RefreshObjectNameActions()
{
    const QPointer<MainWindow> alive(this);
    const bool ready = AuthoringReady() && !m_applyingSession && !m_batchExporting && !m_psdBatch && !m_previewExport.IsBusy();
    const bool single = m_selectedObjectIds.size() == 1 && m_selectedObjectTypes.size() == 1;
    const bool renameReady = ready && single && m_document.CanEditDisplayName(m_selectedObjectTypes.first(), m_selectedObjectIds.first());
    if (m_renameObjectAction) m_renameObjectAction->setEnabled(renameReady);
    if (!alive) return;
    if (m_importObjectNamesAction) m_importObjectNamesAction->setEnabled(ready);
    if (!alive) return;
    if (m_treePanel) m_treePanel->SetRenameObjectEnabled(renameReady);
    if (!alive) return;
    if (m_inspectorPanel) m_inspectorPanel->SetRenameObjectEnabled(ready);
}

void MainWindow::RenameObject(const QString& requestedId, const QString& requestedType)
{
    const QString id = owned(requestedId), type = owned(requestedType);
    if (m_objectNamesDialog) { m_objectNamesDialog->raise(); return; }
    if (m_deformerGridDialog) { m_deformerGridDialog->raise(); return; }
    if (m_deformerRenameDialog) { m_deformerRenameDialog->raise(); return; }
    const QPointer<MainWindow> alive(this);
    QString error;
    if (!AuthoringReady(&error) || !FinishParameterValueEditing(&error) || !alive ||
        m_batchExporting || m_psdBatch || m_previewExport.IsBusy()) return;
    if (!m_openglwindows || !m_openglwindows->FinishMeshDrag(false, &error) || !alive) return;
    if (m_selectedObjectIds != QStringList{id} || m_selectedObjectTypes != QStringList{type} ||
        !m_document.CanEditDisplayName(type, id, &error)) return;
    const auto source = m_document.SourceDigest(); const auto path = m_document.SourcePath();
    const auto revision = m_document.Revision();
    auto* dialog = new CDeformerRenameDialog(id, type, m_document.DisplayName(type, id), this);
    m_deformerRenameDialog = dialog;
    const QPointer<CDeformerRenameDialog> child(dialog);
    const auto cleanup = qScopeGuard([child] { if (child) delete child; });
    dialog->SetSubmissionHandler([alive, id, type, source, path, revision](const QString& name, QString* detail) {
        if (!alive || !alive->AuthoringReady(detail) || alive->m_document.Revision() != revision ||
            alive->m_document.SourceDigest() != source || alive->m_document.SourcePath() != path ||
            alive->m_selectedObjectIds != QStringList{id} || alive->m_selectedObjectTypes != QStringList{type}) {
            if (detail) *detail = QStringLiteral("模型或选区已改变，请重新打开重命名窗口");
            return false;
        }
        live2dstruct::CModelEdit command;
        if (!alive->m_document.MakeDisplayNameEdit(type, id, name, command, detail)) return false;
        if (command.before.value<Names::State>() == command.after.value<Names::State>()) return true;
        return alive->SubmitInspectorEdit(command, detail);
    });
    dialog->exec();
}

void MainWindow::ImportObjectNames(const QString& requestedPath)
{
    const QPointer<MainWindow> alive(this);
    if (m_objectNamesDialog) { m_objectNamesDialog->raise(); return; }
    if (m_deformerRenameDialog) { m_deformerRenameDialog->raise(); return; }
    if (m_deformerGridDialog) { m_deformerGridDialog->raise(); return; }
    QString error;
    const auto ready = [&] { return AuthoringReady(&error) && !m_applyingSession && !m_batchExporting && !m_psdBatch && !m_previewExport.IsBusy(); };
    if (!ready() || !FinishParameterValueEditing(&error) || !alive) return;
    if (!m_openglwindows || !m_openglwindows->FinishMeshDrag(false, &error) || !alive) return;
    if (requestedPath.size() > 32768 || requestedPath.contains(QChar::Null)) return;
    QString path = owned(requestedPath);
    if (path.isEmpty()) path = QFileDialog::getOpenFileName(this, QStringLiteral("选择名称来源工程"), {}, QStringLiteral("Cubism 工程 (*.cmo3)"));
    if (!alive || path.isEmpty() || !ready()) return;
    if (path.size() > 32768 || path.contains(QChar::Null)) return;
    path = owned(path);
    const auto source = m_document.SourceDigest(); const auto sourcePath = m_document.SourcePath();
    const auto revision = m_document.Revision();
    const QByteArray bytes(m_document.SourceBytes().constData(), m_document.SourceBytes().size());
    auto catalog = std::make_shared<Reader::Catalog>(); auto proposal = std::make_shared<Reader::Proposal>();
    auto control = std::make_shared<CExportControl>(); m_nameImportControl = control;
    const auto finish = qScopeGuard([alive, control] {
        if (!alive || alive->m_nameImportControl != control) return;
        alive->m_nameImportControl.reset();
        alive->RefreshExportActions();
        if (alive) alive->RefreshMeshTools();
        if (alive && alive->m_closeAfterNameImport) QTimer::singleShot(0, alive, [alive] {
            if (alive) { alive->m_closeAfterNameImport = false; alive->close(); }
        });
    });
    RefreshExportActions(); if (!alive) return;
    RefreshMeshTools(); if (!alive) return;
    QPointer<QProgressDialog> progress = new QProgressDialog(QStringLiteral("正在读取并匹配工程名称"), QStringLiteral("取消"), 0, 0, this);
    progress->setObjectName(QStringLiteral("objectNamesProgress")); progress->setWindowTitle(QStringLiteral("导入对象名称"));
    progress->setWindowModality(Qt::ApplicationModal); progress->setMinimumDuration(0);
    for (auto* label : progress->findChildren<QLabel*>()) label->setTextFormat(Qt::PlainText);
    connect(progress, &QProgressDialog::canceled, progress, [control] { control->Cancel(); });
    connect(this, &QObject::destroyed, progress, [control] { control->Cancel(); });
    const auto closeProgress = qScopeGuard([progress] { if (progress) { progress->hide(); progress->deleteLater(); } });
    QTimer timer; timer.setInterval(50);
    connect(&timer, &QTimer::timeout, progress, [progress, control] {
        if (!progress) return;
        const auto state = control->Snapshot();
        progress->setLabelText(control->IsCancellationRequested() ? QStringLiteral("正在取消，请稍候…") :
            QStringLiteral("正在读取并匹配工程名称%1").arg(state.detail.isEmpty() ? QString() : QLatin1Char('\n') + state.detail));
    });
    progress->show(); if (!alive) return; timer.start();
    const bool success = CExportTaskRunner::Run([path, bytes, catalog, proposal, control](const CExportTaskRunner::GuiCall&, QString* detail) {
        return Reader::ReadFile(path, *catalog, detail, control.get()) && Reader::BuildProposal(*catalog, bytes, *proposal, detail, control.get());
    }, control.get(), &error);
    timer.stop(); if (!alive) return;
    if (progress) progress->hide();
    m_nameImportControl.reset();
    RefreshExportActions(); if (!alive) return;
    RefreshMeshTools(); if (!alive) return;
    if (m_closeAfterNameImport) { QTimer::singleShot(0, this, [this] { m_closeAfterNameImport = false; close(); }); return; }
    if (!success || control->WasCanceled()) {
        if (!control->WasCanceled()) {
            auto* message = new QMessageBox(QMessageBox::Warning, QStringLiteral("无法导入名称"), error, QMessageBox::Ok, this);
            message->setTextFormat(Qt::PlainText); message->setAttribute(Qt::WA_DeleteOnClose); message->open();
        }
        return;
    }
    if (!ready() || m_document.SourceDigest() != source || m_document.SourcePath() != sourcePath || m_document.Revision() != revision) {
        statusBar()->showMessage(QStringLiteral("模型已改变，请重新读取名称来源工程"), 8000); return;
    }
    CObjectNamesImportDialog::Context context;
    context.sourcePath = catalog->SourcePath(); context.sourceDigest = catalog->SourceDigest();
    context.diagnostics = catalog->Diagnostics() + proposal->diagnostics;
    context.diagnostics.append(QStringLiteral("结构匹配不代表同一工程来源。请核对名称；只有选中的显示名称会改变，ID、层级和绑定保持不变。"));
    QMap<QPair<QString, QString>, QString> eligible;
    for (const auto& match : proposal->matches) {
        CObjectNamesImportDialog::Row row;
        row.type = Reader::TypeName(match.type); row.id = match.id; row.sourceName = match.name;
        row.currentName = m_document.DisplayName(row.type, row.id); row.status = matchStatus(match.status);
        row.reason = match.diagnostics.join(QLatin1Char('\n')); row.evidence = match.evidence;
        QString editError;
        row.eligible = match.status == Reader::MatchStatus::Compatible && m_document.CanEditDisplayName(row.type, row.id, &editError);
        if (row.eligible && (row.type == QLatin1String("warp") || row.type == QLatin1String("rotation")) &&
            m_document.HasDeformerStructure() && !m_document.SourceDeformerIds().contains(row.id)) {
            row.eligible = false; editError = QStringLiteral("当前变形器是新建对象，不能沿用来源中同 ID 对象的名称");
        }
        if (!editError.isEmpty()) row.reason += (row.reason.isEmpty() ? QString() : QStringLiteral("\n")) + editError;
        if (row.eligible) eligible.insert({row.type, row.id}, row.sourceName);
        context.rows.append(std::move(row));
    }
    auto* dialog = new CObjectNamesImportDialog(context, this); m_objectNamesDialog = dialog;
    const QPointer<CObjectNamesImportDialog> child(dialog);
    const auto cleanup = qScopeGuard([child] { if (child) delete child; });
    dialog->SetSubmissionHandler([alive, source, sourcePath, revision, eligible](const QVector<CObjectNamesImportDialog::Selection>& selection, QString* detail) {
        if (!alive || !alive->AuthoringReady(detail) || alive->m_document.SourceDigest() != source ||
            alive->m_document.SourcePath() != sourcePath || alive->m_document.Revision() != revision) {
            if (detail) *detail = QStringLiteral("模型已改变，请重新读取并确认工程名称"); return false;
        }
        auto names = alive->m_document.DisplayNames(); QSet<QPair<QString, QString>> seen;
        for (const auto& selected : selection) {
            const auto key = qMakePair(selected.type, selected.id);
            if (seen.contains(key) || !eligible.contains(key) || eligible.value(key) != selected.name) {
                if (detail) *detail = QStringLiteral("名称候选已失效，请重新读取来源工程"); return false;
            }
            seen.insert(key);
            auto& values = selected.type == QLatin1String("part") ? names.parts : selected.type == QLatin1String("artmesh") ? names.artMeshes : names.deformers;
            values.insert(selected.id, selected.name);
        }
        live2dstruct::CModelEdit edit;
        if (!alive->m_document.MakeDisplayNamesEdit(names, edit, detail)) return false;
        if (edit.before.value<Names::State>() == edit.after.value<Names::State>()) return true;
        return alive->SubmitInspectorEdit(edit, detail);
    });
    dialog->exec();
}
