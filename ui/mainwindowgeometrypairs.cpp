#include "mainwindow.h"
#include "cgeometrypairsdialog.h"
#include "cmodeltreepanel.h"
#include "cparampanel.h"
#include "cinspectorpanel.h"
#include "psdbatchjob.h"
#include "live2d/LAppLive2DManager.hpp"
#include "live2d/LAppModel.hpp"
#include "live2dstruct/cgeometrypairplanner.h"
#include "live2dstruct/ccanvasgeometry.h"
#include "live2dstruct/csectiontable.h"
#include <QPointer>
#include <QScopeGuard>
#include <QStatusBar>

namespace {
using Pairs = live2dstruct::CGeometryObjectPairs;
using Planner = live2dstruct::CGeometryPairPlanner;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool object(const QString& id, const QString& type, Pairs::Object& result)
{
    if (type == QLatin1String("artmesh")) result = {Pairs::Type::ArtMesh, id};
    else if (type == QLatin1String("warp")) result = {Pairs::Type::Warp, id};
    else if (type == QLatin1String("rotation")) result = {Pairs::Type::Rotation, id};
    else return false;
    return !id.isEmpty();
}
const Pairs::Pair* selectedPair(const Pairs::State& state, const Pairs::Object& source, bool& forward)
{
    for (const auto& pair : state.pairs) {
        if (pair.first.object == source) { forward = true; return &pair; }
        if (pair.second.object == source) { forward = false; return &pair; }
    }
    return nullptr;
}
QMap<QString, float> currentValues(CParamPanel* panel)
{
    auto* manager = LAppLive2DManager::GetInstance();
    auto* model = manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
    return model ? model->GetParameterValues() : panel ? panel->CurrentValues() : QMap<QString, float>{};
}
bool pixels(const live2dstruct::CCanvas_info* canvas, const QVector<float>& model, QVector<QPointF>& result, QString* error)
{
    if (model.size() % 2 != 0) return fail(error, QStringLiteral("配对预览坐标不完整"));
    QVector<QPointF> candidate; candidate.reserve(model.size() / 2);
    for (int index = 0; index < model.size(); index += 2) {
        QPointF value;
        if (!live2dstruct::CCanvasGeometry::ModelToPixels(canvas, {model[index], model[index + 1]}, value))
            return fail(error, QStringLiteral("配对预览超出画布坐标范围"));
        candidate.append(value);
    }
    result.swap(candidate); return true;
}
QVector<QPair<int, int>> gridEdges(const Pairs::Endpoint& endpoint)
{
    QVector<QPair<int, int>> result;
    if (endpoint.object.type != Pairs::Type::Warp) return result;
    const int stride = endpoint.columns + 1;
    for (int row = 0; row <= endpoint.rows; ++row) for (int column = 0; column <= endpoint.columns; ++column) {
        const int vertex = row * stride + column;
        if (column < endpoint.columns) result.append({vertex, vertex + 1});
        if (row < endpoint.rows) result.append({vertex, vertex + stride});
    }
    return result;
}
}

void MainWindow::RefreshGeometryPairActions()
{
    if (!m_treePanel) return;
    const bool ready = m_workspaceReady && m_document.IsLoaded() && !m_closingModel && !m_batchExporting &&
        !m_psdBatch && !m_previewExport.IsBusy() && !m_geometryPairsOpen && m_openglwindows && m_openglwindows->IsModelReady();
    bool mirror = false, forward = true;
    QString detail;
    Pairs::Object source;
    if (ready && m_selectedObjectIds.size() == 1 && m_selectedObjectTypes.size() == 1 &&
        object(m_selectedObjectIds.first(), m_selectedObjectTypes.first(), source)) {
        const auto* pair = selectedPair(m_document.GeometryObjectPairs(), source, forward);
        mirror = pair && !pair->requiresReview;
        if (pair) {
            if (pair->requiresReview) detail = QStringLiteral("配对结构已改变，请重新确认对应关系。");
            else if (!pair->enabled) detail = QStringLiteral("自动同步已关闭；仍可手动镜像或从右键菜单查看对侧。");
            else {
                int keyform = m_inspectorPanel ? m_inspectorPanel->GetActiveGeometryKeyformIndex() : -1;
                if (m_keyformEditor.IsActive() && m_keyformEditor.Target() == source.id && m_keyformEditor.Type() == Pairs::TypeName(source.type))
                    keyform = m_keyformEditor.Keyform();
                int targetKey = -1;
                if (Pairs::ResolveKeyform(*pair, keyform, forward, targetKey))
                    detail = QStringLiteral("同步到对侧第 %1 个关键形；预览保持当前姿态。右键可查看对侧。").arg(targetKey + 1);
            }
        }
    }
    m_treePanel->SetGeometryPairActionsEnabled(ready, mirror, detail);
}

void MainWindow::EditGeometryPairs()
{
    if (m_geometryPairsOpen || !m_workspaceReady || m_closingModel || m_batchExporting || m_psdBatch ||
        m_previewExport.IsBusy() || !m_openglwindows || !m_openglwindows->IsModelReady()) return;
    QString error;
    if (!FinishParameterValueEditing(&error) || !m_openglwindows->FinishMeshDrag(false, &error) || !RuntimeMatchesDocument(&error)) {
        statusBar()->showMessage(error, 8000); return;
    }
    const QPointer<MainWindow> window(this);
    const QPointer<OpengGlForm> surface(m_openglwindows);
    const auto snapshot = std::make_shared<const live2dstruct::CModelDocument>(m_document);
    const auto base = currentValues(m_paramPanel);
    const auto valid = [window, surface, snapshot] {
        return window && surface && window->m_openglwindows == surface && window->m_document.SourceDigest() == snapshot->SourceDigest() &&
            window->m_document.SourcePath() == snapshot->SourcePath() && window->m_document.Revision() == snapshot->Revision();
    };
    m_geometryPairsOpen = true;
    RefreshGeometryPairActions();
    const auto guard = qScopeGuard([window] { if (window) { window->m_geometryPairsOpen = false; window->RefreshGeometryPairActions(); } });
    QVector<Pairs::Object> selection;
    for (int index = 0; index < m_selectedObjectIds.size() && index < m_selectedObjectTypes.size(); ++index) {
        Pairs::Object value;
        if (object(m_selectedObjectIds[index], m_selectedObjectTypes[index], value)) selection.append(value);
    }
    QPointer<CGeometryPairsDialog> dialog = new CGeometryPairsDialog(*snapshot, m_cidJson, selection, this);
    dialog->SetPairInitializer([snapshot, base, valid](Pairs::Pair& input, int firstKey, QString* detail) {
        if (!valid()) return fail(detail, QStringLiteral("模型已改变，请重新打开配对设置"));
        if (input.first.object.type != Pairs::Type::Rotation) return true;
        auto candidate = input; candidate.enabled = true; candidate.requiresReview = false;
        int secondKey = -1;
        if (!Pairs::ResolveKeyform(candidate, firstKey, true, secondKey, detail)) return false;
        Pairs::Pair prepared;
        if (!Planner::PrepareRotationCorrespondence(*snapshot, candidate, firstKey, secondKey, base, prepared, detail)) return false;
        input.rotationLocalX = prepared.rotationLocalX;
        input.rotationLocalY = prepared.rotationLocalY;
        return true;
    });
    dialog->SetPreviewProvider([snapshot, base, valid](const Pairs::Pair& input, int firstKey, CGeometryPairsDialog::Preview& output, QString* detail) {
        if (!valid()) return fail(detail, QStringLiteral("模型已改变，请重新打开配对设置"));
        auto pair = input; pair.enabled = true;
        Planner::Result prepared;
        if (!Planner::PrepareMirror(*snapshot, pair, true, firstKey, base, prepared, detail)) return false;
        auto reader = live2dstruct::CReader::FromBytes(snapshot->WorkingBaseBytes());
        live2dstruct::CSectionTable table(reader);
        if (!table.IsValid()) return fail(detail, reader.Error());
        CGeometryPairsDialog::Preview preview;
        if (!pixels(table.canvas_info, prepared.sourceModel, preview.firstPoints, detail) ||
            !pixels(table.canvas_info, prepared.targetBeforeModel, preview.secondPoints, detail) ||
            !pixels(table.canvas_info, prepared.targetModel, preview.proposedSecondPoints, detail)) return false;
        int targetKey = -1;
        if (!Pairs::ResolveKeyform(pair, firstKey, true, targetKey, detail)) return false;
        preview.firstTriangles = pair.first.triangles; preview.secondTriangles = pair.second.triangles;
        preview.firstEdges = gridEdges(pair.first); preview.secondEdges = gridEdges(pair.second);
        if (pair.first.object.type == Pairs::Type::Rotation) {
            if (preview.firstPoints.size() == 3) preview.firstPoints.insert(2, preview.firstPoints.first());
            if (preview.secondPoints.size() == 3) preview.secondPoints.insert(2, preview.secondPoints.first());
            if (preview.proposedSecondPoints.size() == 3) preview.proposedSecondPoints.insert(2, preview.proposedSecondPoints.first());
        }
        preview.firstCorner = QStringLiteral("%1 · 第 %2 个关键形").arg(pair.first.object.id).arg(firstKey + 1);
        preview.secondCorner = QStringLiteral("%1 · 第 %2 个关键形").arg(pair.second.object.id).arg(targetKey + 1);
        if (!prepared.diagnostics.isEmpty()) preview.secondCorner += QStringLiteral("\n") + prepared.diagnostics.join(QStringLiteral("；"));
        output = std::move(preview); return true;
    });
    dialog->SetSubmissionHandler([window, valid](const Pairs::State& state, QString* detail) {
        if (!valid()) return fail(detail, QStringLiteral("模型已改变，请重新打开配对设置，当前草稿尚未写入"));
        live2dstruct::CModelEdit edit;
        if (!window->m_document.MakeGeometryObjectPairsEdit(state, edit, detail)) return false;
        if (edit.before == edit.after) return true;
        if (!window->SubmitInspectorEdit(edit, detail) || !window) return false;
        window->m_historySource = HistorySource::Inspector;
        window->RefreshUndoActions(); window->RefreshWindowTitle();
        return true;
    });
    dialog->exec();
    if (dialog) delete dialog;
}

void MainWindow::MirrorGeometryPair()
{
    if (!m_workspaceReady || m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() ||
        m_geometryPairsOpen || !m_openglwindows || !m_openglwindows->IsModelReady() || m_selectedObjectIds.size() != 1 ||
        m_selectedObjectTypes.size() != 1) return;
    QString error;
    if (!FinishParameterValueEditing(&error) || !m_openglwindows->FinishMeshDrag(false, &error) || !RuntimeMatchesDocument(&error)) {
        statusBar()->showMessage(error, 8000); return;
    }
    Pairs::Object source;
    if (!object(m_selectedObjectIds.first(), m_selectedObjectTypes.first(), source)) return;
    bool forward = true;
    const auto* stored = selectedPair(m_document.GeometryObjectPairs(), source, forward);
    if (!stored || stored->requiresReview) { statusBar()->showMessage(QStringLiteral("请先确认当前对象的配对关系"), 8000); return; }
    auto pair = *stored; pair.enabled = true;
    int keyform = m_inspectorPanel ? m_inspectorPanel->GetActiveGeometryKeyformIndex() : -1;
    if (m_keyformEditor.IsActive() && m_keyformEditor.Target() == source.id && m_keyformEditor.Type() == Pairs::TypeName(source.type))
        keyform = m_keyformEditor.Keyform();
    if (keyform < 0) { statusBar()->showMessage(QStringLiteral("请先选择要镜像的关键形"), 8000); return; }
    Planner::Result prepared;
    if (!Planner::PrepareMirror(m_document, pair, forward, keyform, currentValues(m_paramPanel), prepared, &error)) {
        statusBar()->showMessage(error, 10000); return;
    }
    if (!prepared.changed) { statusBar()->showMessage(QStringLiteral("配对对象的当前关键形已与镜像结果一致"), 6000); return; }
    const QPointer<MainWindow> alive(this);
    if (!SubmitInspectorEdit(prepared.edit, &error)) { if (alive) statusBar()->showMessage(error, 10000); return; }
    if (!alive) return;
    m_historySource = HistorySource::Inspector;
    RefreshUndoActions(); RefreshGeometryPairActions();
    const auto& target = forward ? pair.second : pair.first;
    int targetKey = -1; Pairs::ResolveKeyform(pair, keyform, forward, targetKey);
    QString message = QStringLiteral("已镜像到 %1 的第 %2 个关键形").arg(target.object.id).arg(targetKey + 1);
    if (!prepared.diagnostics.isEmpty()) message += QStringLiteral("；") + prepared.diagnostics.join(QStringLiteral("；"));
    statusBar()->showMessage(message, 12000);
}

void MainWindow::ViewGeometryPair()
{
    if (!m_workspaceReady || m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() ||
        m_geometryPairsOpen || !m_openglwindows || !m_openglwindows->IsModelReady() || m_selectedObjectIds.size() != 1 ||
        m_selectedObjectTypes.size() != 1) return;
    QString error;
    if (!FinishParameterValueEditing(&error) || !m_openglwindows->FinishMeshDrag(false, &error)) {
        statusBar()->showMessage(error, 8000); return;
    }
    Pairs::Object source;
    if (!object(m_selectedObjectIds.first(), m_selectedObjectTypes.first(), source)) return;
    bool forward = true;
    const auto* stored = selectedPair(m_document.GeometryObjectPairs(), source, forward);
    if (!stored || stored->requiresReview) return;
    auto pair = *stored; pair.enabled = true;
    int keyform = m_inspectorPanel ? m_inspectorPanel->GetActiveGeometryKeyformIndex() : -1;
    if (m_keyformEditor.IsActive() && m_keyformEditor.Target() == source.id && m_keyformEditor.Type() == Pairs::TypeName(source.type))
        keyform = m_keyformEditor.Keyform();
    int targetKey = -1;
    if (!Pairs::ResolveKeyform(pair, keyform, forward, targetKey, &error)) { statusBar()->showMessage(error, 8000); return; }
    const auto target = (forward ? pair.second : pair.first).object;
    const auto type = Pairs::TypeName(target.type);
    const QPointer<MainWindow> alive(this);
    SelectModelObjects({target.id}, {type});
    if (!alive || m_selectedObjectIds != QStringList{target.id} || m_selectedObjectTypes != QStringList{type}) return;
    if (!ActivateKeyform(target.id, type, targetKey, &error, true)) { if (alive) statusBar()->showMessage(error, 8000); return; }
    if (alive && m_treePanel) m_treePanel->HighlightObject(target.id, type);
}
