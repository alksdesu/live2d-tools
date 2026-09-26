#include "mainwindow.h"
#include "psdbatchjob.h"
#include "opengl/opengglform.h"
#include "edit/cmeshmap.h"
#include "cmeshtoolbar.h"
#include "cinspectorpanel.h"
#include "live2dstruct/cmesheditplanner.h"
#include "live2dstruct/ckeyformmappingcontext.h"
#include "opengl/ccanvascoordinates.h"
#include <QCryptographicHash>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

struct MeshToolCommitState
{
    std::shared_ptr<const live2dstruct::CMocMeshStructureBuilder::Request> request;
    OpengGlForm::MeshToolSnapshot snapshot;
    quint64 revision = 0;
    QVector<int> selected;
    std::optional<CMeshGeometryExpectation> expectation;
};

namespace {
OpengGlForm::MeshTool surfaceTool(CMeshToolBar::Tool tool)
{
    switch (tool) {
    case CMeshToolBar::Tool::Select: return OpengGlForm::MeshTool::Select;
    case CMeshToolBar::Tool::Insert: return OpengGlForm::MeshTool::Insert;
    case CMeshToolBar::Tool::Connect: return OpengGlForm::MeshTool::Connect;
    case CMeshToolBar::Tool::Delete: return OpengGlForm::MeshTool::Delete;
    }
    return OpengGlForm::MeshTool::Select;
}

bool unchangedMesh(const live2dstruct::CMeshEditPlanner::Input& input,
                   const live2dstruct::CMocMeshStructureBuilder::MeshEdit& edit)
{
    const int count = input.positions.size() / 2;
    if (edit.indices != input.indices || edit.vertices.size() != count || edit.oldToNew.size() != count) return false;
    for (int vertex = 0; vertex < count; ++vertex)
        if (edit.oldToNew[vertex] != vertex || edit.vertices[vertex].indices != QVector<int>{vertex} ||
            edit.vertices[vertex].weights != QVector<double>{1.0}) return false;
    return true;
}
}

void MainWindow::InitializeMeshTools(QVBoxLayout* layout)
{
    m_meshTools = new CMeshToolBar(m_canvasHost);
    layout->addWidget(m_meshTools);
    m_openglwindows->SetMeshToolHandler([this](auto tool, const auto& snapshot, QString* error) {
        return ApplyMeshTool(tool, snapshot, error);
    });
    connect(m_meshTools, &CMeshToolBar::EditingRequested, this, [this](bool enabled) {
        QString error;
        if (!SetMeshToolsEnabled(enabled, &error) && !error.isEmpty()) statusBar()->showMessage(error, 8000);
        RefreshMeshTools();
    });
    connect(m_meshTools, &CMeshToolBar::ToolRequested, this, [this](CMeshToolBar::Tool tool) {
        QString error;
        if (!m_openglwindows->SetMeshTool(surfaceTool(tool), m_keyformEditor.Target(), &error))
            statusBar()->showMessage(error, 8000);
        RefreshMeshTools();
    });
    connect(m_meshTools, &CMeshToolBar::RetriangulateRequested, this, &MainWindow::OptimizeSelectedMesh);
    connect(m_openglwindows, &OpengGlForm::MeshToolChanged, this, [this] { RefreshMeshTools(); });
    connect(m_openglwindows, &OpengGlForm::MeshSelectionChanged, this, [this] { RefreshMeshTools(); });
    RefreshMeshTools();
}

void MainWindow::RefreshMeshTools()
{
    RefreshDeformerActions();
    if (!m_meshTools || !m_openglwindows) return;
    const bool single = m_selectedObjectIds.size() == 1 && m_selectedObjectTypes.value(0) == QLatin1String("artmesh");
    const QString id = single ? m_selectedObjectIds.first() : QString();
    const auto found = std::find_if(m_document.Drawables().cbegin(), m_document.Drawables().cend(),
        [&](const auto& mesh) { return mesh.id == id; });
    const bool available = !m_closingModel && !m_previewExport.IsBusy() && m_openglwindows->IsModelReady() && single &&
        found != m_document.Drawables().cend() && !m_document.IsLocked(QStringLiteral("artmesh"), id);
    QString reason = QStringLiteral("选择一个网格以开始编辑");
    if (single && m_document.IsLocked(QStringLiteral("artmesh"), id)) reason = QStringLiteral("先解除网格锁定");
    else if (!m_openglwindows->IsModelReady()) reason = QStringLiteral("等待模型预览就绪");
    else if (m_previewExport.IsBusy()) reason = QStringLiteral("导出完成后可编辑网格");
    int selected = 0;
    CMeshEditorSnapshot snapshot;
    if (available && m_openglwindows->CaptureMeshEditorState(snapshot)) selected = snapshot.value(id).selectedVertices.size();
    m_meshTools->SetContext(id, found == m_document.Drawables().cend() ? 0 : found->vertexCount, selected, available, reason);
    const bool active = !m_openglwindows->MeshToolTarget().isEmpty();
    m_meshTools->SetEditing(active && available);
    if (active) {
        switch (m_openglwindows->CurrentMeshTool()) {
        case OpengGlForm::MeshTool::Select: m_meshTools->SetTool(CMeshToolBar::Tool::Select); break;
        case OpengGlForm::MeshTool::Insert: m_meshTools->SetTool(CMeshToolBar::Tool::Insert); break;
        case OpengGlForm::MeshTool::Connect: m_meshTools->SetTool(CMeshToolBar::Tool::Connect); break;
        case OpengGlForm::MeshTool::Delete: m_meshTools->SetTool(CMeshToolBar::Tool::Delete); break;
        }
    }
}

bool MainWindow::SetMeshToolsEnabled(bool enabled, QString* error)
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!m_openglwindows || m_previewExport.IsBusy()) return fail(QStringLiteral("当前不能切换网格工具"));
    if (!enabled) return m_openglwindows->ClearMeshTool(error);
    if (m_selectedObjectIds.size() != 1 || m_selectedObjectTypes.value(0) != QLatin1String("artmesh"))
        return fail(QStringLiteral("先选择一个 ArtMesh"));
    if (!RuntimeMatchesDocument(error) || !m_openglwindows->IsModelReady()) return false;
    const QString id = m_selectedObjectIds.first();
    int keyform = m_inspectorPanel ? m_inspectorPanel->GetActiveKeyformIndex() : 0;
    if (keyform < 0) keyform = 0;
    live2dstruct::CMocMeshStructureBuilder::Request request;
    live2dstruct::CMocMeshStructureBuilder::MeshEdit mesh;
    mesh.artMeshId = id;
    mesh.indices = m_document.DrawableIndices(id);
    const auto local = m_document.DrawableKeyformPositions(id, keyform);
    for (int vertex = 0; vertex < local.size() / 2; ++vertex) {
        mesh.vertices.append({{vertex}, {1.0}});
        mesh.oldToNew.append(vertex);
    }
    request.meshes.append(mesh);
    live2dstruct::CMocMeshStructureBuilder::Impact impact;
    if (!m_document.AnalyzeMeshStructureEdit(request, impact, error)) return false;
    if (!m_openglwindows->CanSetMeshTool(surfaceTool(m_meshTools->CurrentTool()), id, error)) return false;
    if (!ActivateKeyform(id, keyform, error)) return false;
    if (!m_openglwindows->SetMeshTool(surfaceTool(m_meshTools->CurrentTool()), id, error)) return false;
    statusBar()->showMessage(QStringLiteral("网格工具已启用；选择、插点或连接顶点，Delete 删除选区，Esc 取消当前工具"));
    return true;
}

void MainWindow::OptimizeSelectedMesh()
{
    QString error;
    OpengGlForm::MeshToolSnapshot snapshot;
    if (!m_openglwindows || !m_openglwindows->CaptureMeshToolSnapshot(m_openglwindows->rect().center(), snapshot, &error) ||
        !ApplyMeshTool(OpengGlForm::MeshTool::Select, snapshot, &error))
        statusBar()->showMessage(error.isEmpty() ? QStringLiteral("先启用网格工具") : error, 8000);
}

bool MainWindow::ApplyMeshTool(OpengGlForm::MeshTool tool, const OpengGlForm::MeshToolSnapshot& snapshot, QString* error)
{
    using Planner = live2dstruct::CMeshEditPlanner;
    using Builder = live2dstruct::CMocMeshStructureBuilder;
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!m_openglwindows || m_meshToolCommit || m_previewExport.IsBusy() || m_closingModel || m_batchExporting || m_psdBatch)
        return fail(QStringLiteral("当前不能提交网格工具操作"));
    if (!m_keyformEditor.IsActive() || m_keyformEditor.IsDragging() || m_keyformEditor.Type() != QLatin1String("artmesh") ||
        m_keyformEditor.Target() != snapshot.artMeshId || m_keyformEditor.Keyform() != snapshot.keyformIndex ||
        m_keyformEditor.Pose() != snapshot.parameterValues)
        return fail(QStringLiteral("关键形编辑姿态已改变，请重新操作"));
    if (!m_openglwindows->MeshToolSnapshotMatches(snapshot, error)) return false;
    const quint64 revision = m_document.Revision();
    QByteArray runtime;
    if (!m_document.BuildRuntimeMoc(runtime, error)) return false;
    if (QCryptographicHash::hash(runtime, QCryptographicHash::Sha256) != snapshot.previewDigest)
        return fail(QStringLiteral("文档与画布网格不同步，请重新载入模型"));
    live2dstruct::CKeyformMappingContext mapping;
    Planner::Input input;
    input.artMeshId = snapshot.artMeshId;
    input.positions = tool == OpengGlForm::MeshTool::Insert
        ? m_document.DrawableKeyformPositions(snapshot.artMeshId, snapshot.keyformIndex) : snapshot.coreVertices;
    input.indices = m_document.DrawableIndices(snapshot.artMeshId);
    Planner::Result plan;
    Planner::Diagnostic diagnostic;
    bool planned = false;
    if (tool == OpengGlForm::MeshTool::Insert) {
        if (!mapping.Build(m_document, snapshot.artMeshId, snapshot.keyformIndex, snapshot.parameterValues, error) ||
            !mapping.MatchesRuntime(snapshot.coreVertices, error)) return false;
        QVector<float> local;
        const QVector<float> click{static_cast<float>(snapshot.modelPosition.x()), static_cast<float>(snapshot.modelPosition.y())};
        if (!mapping.FromModel(click, local, error) || local.size() != 2) return false;
        planned = Planner::Insert(input, QPointF(local[0], local[1]), -1, plan, &diagnostic);
        if (planned) {
            auto visible = input;
            visible.positions = snapshot.coreVertices;
            Planner::Result visiblePlan;
            if (!Planner::Insert(visible, snapshot.modelPosition, -1, visiblePlan, &diagnostic)) planned = false;
            else if (visiblePlan.edit.indices != plan.edit.indices || visiblePlan.edit.vertices.size() != plan.edit.vertices.size())
                return fail(QStringLiteral("当前变形使显示三角形与局部网格不一致，请切换关键形姿态后插点"));
        }
    } else if (tool == OpengGlForm::MeshTool::Delete) {
        planned = Planner::Delete(input, snapshot.operationVertices, Planner::BoundaryPolicy::ReplaceWithChord, plan, &diagnostic);
        if (planned) {
            plan.selectedVertices.clear();
            for (int vertex : snapshot.selectedVertices)
                if (vertex >= 0 && vertex < plan.edit.oldToNew.size() && plan.edit.oldToNew[vertex] >= 0)
                    plan.selectedVertices.append(plan.edit.oldToNew[vertex]);
        }
    } else if (tool == OpengGlForm::MeshTool::Connect) {
        if (snapshot.operationVertices.size() != 2) return fail(QStringLiteral("依次选择两个顶点以连接"));
        planned = Planner::Connect(input, snapshot.operationVertices[0], snapshot.operationVertices[1],
                                   Planner::CrossingPolicy::RejectProtected, plan, &diagnostic);
    } else if (tool == OpengGlForm::MeshTool::Select) {
        QVector<int> triangles;
        const QSet<int> selected(snapshot.selectedVertices.cbegin(), snapshot.selectedVertices.cend());
        for (int triangle = 0; triangle < input.indices.size() / 3; ++triangle)
            if (selected.isEmpty() || selected.contains(input.indices[triangle * 3]) ||
                selected.contains(input.indices[triangle * 3 + 1]) || selected.contains(input.indices[triangle * 3 + 2]))
                triangles.append(triangle);
        planned = Planner::Retriangulate(input, triangles, plan, &diagnostic);
        plan.selectedVertices = snapshot.selectedVertices;
    }
    if (!planned) return fail(diagnostic.message.isEmpty() ? QStringLiteral("网格工具无法完成此次操作") : diagnostic.message);
    if (tool != OpengGlForm::MeshTool::Insert && std::any_of(plan.edit.vertices.cbegin(), plan.edit.vertices.cend(),
        [](const auto& vertex) { return vertex.indices.size() != 1 || vertex.weights != QVector<double>{1.0}; }))
        return fail(QStringLiteral("此连接操作需要插入顶点，请先使用插点工具"));
    Builder::Request request;
    request.meshes.append(plan.edit);
    Builder::Impact impact;
    if (!m_document.AnalyzeMeshStructureEdit(request, impact, error)) return false;
    if (!impact.blockingReasons.isEmpty()) {
        if (tool != OpengGlForm::MeshTool::Delete) return fail(impact.blockingReasons.join(QLatin1Char('\n')));
        auto unpaired = request;
        unpaired.gluePolicy = Builder::GlueEndpointPolicy::DropPair;
        Builder::Impact unpairedImpact;
        if (!m_document.AnalyzeMeshStructureEdit(unpaired, unpairedImpact, error)) return false;
        if (!unpairedImpact.blockingReasons.isEmpty()) return fail(unpairedImpact.blockingReasons.join(QLatin1Char('\n')));
        if (std::none_of(unpairedImpact.glues.cbegin(), unpairedImpact.glues.cend(),
                         [](const auto& glue) { return !glue.droppedPairs.isEmpty(); }))
            return fail(impact.blockingReasons.join(QLatin1Char('\n')));
        QMessageBox dialog(QMessageBox::Question, QStringLiteral("删除接缝顶点"),
            QStringLiteral("所选顶点参与了 Glue 接缝。删除时需要解除受影响的配对。"), QMessageBox::Yes | QMessageBox::Cancel, this);
        dialog.setTextFormat(Qt::PlainText);
        dialog.setDetailedText(impact.blockingReasons.join(QLatin1Char('\n')));
        dialog.button(QMessageBox::Yes)->setText(QStringLiteral("解除配对并删除"));
        dialog.button(QMessageBox::Cancel)->setText(QStringLiteral("保留顶点"));
        dialog.setDefaultButton(QMessageBox::Cancel);
        if (dialog.exec() != QMessageBox::Yes) return fail(QStringLiteral("已保留接缝顶点"));
        request = std::move(unpaired);
    }
    if (m_document.Revision() != revision || !m_openglwindows->MeshToolSnapshotMatches(snapshot, error))
        return fail(QStringLiteral("规划期间模型或画布已改变，请重新操作"));
    if (unchangedMesh(input, plan.edit)) {
        if (!m_openglwindows->SetMeshToolSelection(snapshot, plan.selectedVertices, error)) return false;
        RefreshMeshTools();
        statusBar()->showMessage(QStringLiteral("已更新顶点选区，网格无需改变"), 5000);
        return true;
    }
    live2dstruct::CModelEdit command;
    if (!m_document.MakeMeshStructureEdit(request, command, error)) return false;
    auto commit = std::make_shared<MeshToolCommitState>();
    commit->request = command.meshRequest;
    commit->snapshot = snapshot;
    commit->revision = revision;
    commit->selected = plan.selectedVertices;
    if (tool == OpengGlForm::MeshTool::Insert && !plan.insertedVertices.isEmpty()) {
        QPointF horizontal, vertical;
        if (!CCanvasCoordinates::ScreenToModel(snapshot.mvp, snapshot.viewportSize, snapshot.pixelPosition + QPointF(1, 0), horizontal) ||
            !CCanvasCoordinates::ScreenToModel(snapshot.mvp, snapshot.viewportSize, snapshot.pixelPosition + QPointF(0, 1), vertical))
            return fail(QStringLiteral("无法确定当前视图的插点精度"));
        const auto distance = [&](QPointF point) { return std::hypot(point.x() - snapshot.modelPosition.x(), point.y() - snapshot.modelPosition.y()); };
        const double tolerance = std::max(distance(horizontal), distance(vertical));
        if (!std::isfinite(tolerance) || tolerance <= 0) return fail(QStringLiteral("当前视图的插点精度无效"));
        commit->expectation = CMeshGeometryExpectation{};
        commit->expectation->tolerance = tolerance;
        for (int vertex : plan.insertedVertices)
            commit->expectation->points.append({snapshot.artMeshId, vertex, snapshot.modelPosition});
    }
    const QScopedValueRollback<std::shared_ptr<const MeshToolCommitState>> scope(m_meshToolCommit, commit);
    if (!SubmitInspectorEdit(command, error)) return false;
    ApplyDocumentEditToUi(command, true);
    RefreshMeshTools();
    statusBar()->showMessage(plan.boundaryChanged ? QStringLiteral("网格已更新，边界连接已调整，可撤销")
                                                : QStringLiteral("网格已更新，可撤销"), 5000);
    return true;
}

bool MainWindow::PrepareMeshMigration(const live2dstruct::CModelDocument& document,
                                      const live2dstruct::CModelEdit* command, bool forward,
                                      CMeshGeometryMigration& migration, QVariant* leaving, QString* error) const
{
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (!m_openglwindows || !m_openglwindows->IsModelReady()) return fail(QStringLiteral("网格预览尚未就绪"));
    CMeshGeometryMigration next;
    next.sourcePreviewDigest = m_openglwindows->CurrentPreviewDigest();
    CMeshEditorSnapshot current;
    if (!m_openglwindows->CaptureMeshEditorState(current, error)) return false;
    if (command) {
        if (command->field != live2dstruct::CModelEdit::MeshStructure || !command->runtime)
            return fail(QStringLiteral("网格历史缺少运行时状态"));
        for (const auto& remap : command->meshRemaps) {
            if (next.remaps.contains(remap.artMeshId)) return fail(QStringLiteral("网格顶点映射重复"));
            next.remaps.insert(remap.artMeshId, {forward ? remap.oldVertexCount : remap.newVertexCount,
                forward ? remap.newVertexCount : remap.oldVertexCount, forward ? remap.oldToNew : remap.newToOld});
        }
        const auto& archived = forward ? command->runtime->afterEditor : command->runtime->beforeEditor;
        if (archived.isValid()) {
            if (archived.metaType() != QMetaType::fromType<CMeshEditorSnapshot>())
                return fail(QStringLiteral("网格历史选区格式无效"));
            next.targetState = archived.value<CMeshEditorSnapshot>();
        }
    } else {
        CMeshEditorSnapshot target;
        for (const auto& mesh : document.Drawables()) {
            const auto old = current.constFind(mesh.id);
            if (old == current.cend() || target.contains(mesh.id)) return fail(QStringLiteral("会话网格目录与预览不匹配"));
            auto state = *old;
            state.vertexCount = mesh.vertexCount;
            state.selectedVertices.clear();
            target.insert(mesh.id, state);
            next.remaps.insert(mesh.id, {old->vertexCount, mesh.vertexCount, QVector<int>(old->vertexCount, -1)});
        }
        if (target.size() != current.size()) return fail(QStringLiteral("会话网格目录与预览数量不匹配"));
        next.targetState = std::move(target);
    }
    if (next.sourcePreviewDigest != m_openglwindows->CurrentPreviewDigest())
        return fail(QStringLiteral("捕获选区期间网格预览已改变"));
    QVariant snapshot;
    if (leaving) snapshot = QVariant::fromValue(current);
    migration = std::move(next);
    if (leaving) leaving->swap(snapshot);
    return true;
}

bool MainWindow::PublishMeshStructure(const live2dstruct::CModelDocument& document,
                                     const live2dstruct::CModelEdit& command, bool forward, QString* error)
{
    if (m_closingModel || m_batchExporting || m_psdBatch || m_previewExport.IsBusy() || !command.runtime) {
        if (error) *error = QStringLiteral("模型正在关闭或导出，请稍后编辑网格");
        return false;
    }
    if (!RuntimeMatchesDocument(error)) return false;
    QByteArray bytes;
    if (!document.BuildRuntimeMoc(bytes, error)) return false;
    if (!m_openglwindows || !m_openglwindows->IsModelReady()) return true;
    CMeshGeometryMigration migration;
    QVariant leaving;
    if (!PrepareMeshMigration(document, &command, forward, migration, &leaving, error)) return false;
    if (m_meshToolCommit) {
        if (!forward || m_meshToolCommit->request != command.meshRequest || m_meshToolCommit->revision != m_document.Revision()) {
            if (error) *error = QStringLiteral("网格提交上下文已改变，请重新操作");
            return false;
        }
        if (!m_openglwindows->MeshToolSnapshotMatches(m_meshToolCommit->snapshot, error)) return false;
        auto target = leaving.value<CMeshEditorSnapshot>();
        const auto found = target.find(m_meshToolCommit->snapshot.artMeshId);
        if (found == target.end() || command.meshRemaps.size() != 1) {
            if (error) *error = QStringLiteral("网格提交的选区映射不完整");
            return false;
        }
        found->vertexCount = command.meshRemaps.first().newVertexCount;
        found->selectedVertices = m_meshToolCommit->selected;
        migration.targetState = std::move(target);
        migration.expectation = m_meshToolCommit->expectation;
    }
    if (!m_openglwindows->ApplyMeshGeometryPreview(bytes, document.SourceDigest(), migration, error)) return false;
    (forward ? command.runtime->beforeEditor : command.runtime->afterEditor).swap(leaving);
    return true;
}
