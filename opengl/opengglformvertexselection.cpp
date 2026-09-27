#include "opengglform.h"
#include "ccanvascoordinates.h"
#include "cvertexselectionoverlay.h"
#include <QApplication>
#include <QLineF>
#include <QPointer>
#include <algorithm>
#include <limits>

namespace {
bool reject(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}
LAppModel* selectionModel()
{
    auto* manager = LAppLive2DManager::GetInstance();
    return manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
}
}

bool OpengGlForm::SetVertexSelectionSettings(const VertexSelection::Settings& settings, QString* error)
{
    if (error) error->clear();
    if (!VertexSelection::ValidateSettings(settings, error)) return false;
    if (settings.soft.enabled || settings.tool == VertexSelection::Tool::Brush) {
        const bool legacyWarp = m_deformerTarget.type == QLatin1String("warp") && !m_weightedDeformerBegin;
        const bool legacyMesh = m_deformerTarget.id.isEmpty() && m_meshEditEnabled && !m_meshEditName.isEmpty() && !m_weightedMeshBegin;
        if (legacyWarp || legacyMesh) return reject(error, QStringLiteral("当前编辑控制器不支持加权拖拽"));
    }
    if (settings == m_vertexSettings) return true;
    if (m_exportSuspended || m_meshEditCallbackActive || m_deformerCallbackActive || m_meshToolDispatchActive)
        return reject(error, QStringLiteral("当前不能切换顶点选择工具"));
    const QPointer<OpengGlForm> alive(this);
    if (!FinishKeyformDrag(true, error) || !alive) return false;
    m_vertexSettings = settings;
    NotifyVertexSelection();
    return true;
}

OpengGlForm::VertexSelection::Combine OpengGlForm::SelectionCombine(Qt::KeyboardModifiers modifiers) const
{
    if (modifiers & Qt::AltModifier) return VertexSelection::Combine::Subtract;
    if (modifiers & Qt::ShiftModifier) return VertexSelection::Combine::Add;
    return m_vertexSettings.combine;
}

bool OpengGlForm::CaptureVertexFrame(VertexFrame& frame, QString* error) const
{
    if (error) error->clear();
    if (m_exportSuspended || !m_runtimeReady) return false;
    auto* model = selectionModel();
    auto* view = LAppDelegate::GetInstance()->GetView();
    if (!model || !view || !model->HasEditorPreview()) return false;
    VertexFrame candidate;
    candidate.modelIdentity = model;
    candidate.viewport = size();
    candidate.mvp = view->GetMVP();
    candidate.digest = CurrentPreviewDigest();
    candidate.pose = model->GetParameterValues();
    if (!m_deformerTarget.id.isEmpty()) {
        const auto& target = m_deformerTarget;
        if (target.type != QLatin1String("warp") || target.previewDigest != candidate.digest || target.pose != candidate.pose ||
            target.rows < 1 || target.columns < 1 || qint64(target.rows) * target.columns != target.points.size() / 2) return false;
        candidate.id = target.id; candidate.type = target.type;
        candidate.warpLayout = QSize(target.columns, target.rows);
        if (!CCanvasCoordinates::ProjectVertices(candidate.mvp, candidate.viewport, target.points, candidate.geometry.points, error)) return false;
        const auto stored = m_warpSelections.constFind(target.id);
        if (stored != m_warpSelections.cend() && stored->rows == target.rows && stored->columns == target.columns)
            candidate.state = stored->selection;
        for (int row = 0; row < target.rows; ++row) for (int column = 0; column < target.columns; ++column) {
            const int index = row * target.columns + column;
            if (column + 1 < target.columns) candidate.geometry.edges.append({index, index + 1});
            if (row + 1 < target.rows) candidate.geometry.edges.append({index, index + target.columns});
        }
    } else {
        const QString target = m_meshToolTarget.isEmpty() ? m_meshEditName : m_meshToolTarget;
        if (!m_meshEditEnabled || target.isEmpty() || (!m_meshToolTarget.isEmpty() && m_meshTool != MeshTool::Select)) return false;
        MeshToolSnapshot snapshot;
        if (!CaptureMeshToolState(target, {}, true, snapshot, error)) return false;
        auto* mesh = model->GetModel()->GetMeshMap()->GetDrawMesh(target);
        CDrawMesh::EditorState state;
        if (!mesh || !mesh->CaptureEditorState(state, error)) return false;
        candidate.id = target; candidate.type = QStringLiteral("artmesh");
        candidate.state = {state.selectedVertices, state.paintedWeights};
        if (!CCanvasCoordinates::ProjectVertices(candidate.mvp, candidate.viewport, snapshot.coreVertices, candidate.geometry.points, error) ||
            !VertexSelection::EdgesFromTriangles(state.vertexCount, snapshot.indices, candidate.geometry.edges, error)) return false;
    }
    if (!VertexSelection::Validate(candidate.state, candidate.geometry.points.size(), error)) return false;
    frame = std::move(candidate);
    return true;
}

bool OpengGlForm::VertexFrameMatches(const VertexFrame& frame) const
{
    VertexFrame current;
    VertexSelection::State expected = frame.state;
    if (m_vertexGesture && m_vertexGesture->tool == VertexSelection::Tool::Brush &&
        !VertexSelection::BrushState(m_vertexGesture->stroke, expected)) return false;
    return CaptureVertexFrame(current) && current.state == expected && current.modelIdentity == frame.modelIdentity &&
        current.warpLayout == frame.warpLayout && current.id == frame.id && current.type == frame.type && current.digest == frame.digest &&
        current.pose == frame.pose && current.mvp == frame.mvp && current.viewport == frame.viewport &&
        current.geometry.points == frame.geometry.points && current.geometry.edges == frame.geometry.edges;
}

bool OpengGlForm::ApplyVertexSelection(const VertexFrame& frame, const VertexSelection::State& state, QString* error)
{
    if (!VertexSelection::Validate(state, frame.geometry.points.size(), error)) return false;
    if (frame.type == QLatin1String("warp")) {
        auto found = m_warpSelections.find(frame.id);
        if (found == m_warpSelections.end() || qint64(found->rows) * found->columns != frame.geometry.points.size())
            return reject(error, QStringLiteral("变形器控制点布局已改变"));
        found->selection = state;
        if (m_deformerOverlay && m_deformerTarget.id == frame.id) m_deformerOverlay->SetSelected(state.hard);
    } else {
        auto* model = selectionModel();
        auto* mesh = model && model->GetModel() ? model->GetModel()->GetMeshMap()->GetDrawMesh(frame.id) : nullptr;
        CDrawMesh::EditorState current;
        if (!mesh || !mesh->CaptureEditorState(current, error) || current.vertexCount != frame.geometry.points.size())
            return reject(error, QStringLiteral("网格顶点布局已改变"));
        current.selectedVertices = state.hard;
        current.paintedWeights = state.painted;
        if (!mesh->RestoreEditorState(current, error)) return false;
    }
    return true;
}

bool OpengGlForm::BeginVertexGesture(QMouseEvent* event, bool& consumed, QString* error)
{
    consumed = false;
    VertexFrame frame;
    if (!CaptureVertexFrame(frame, error)) return true;
    const auto tool = m_vertexSettings.tool;
    const auto combine = SelectionCombine(event->modifiers());
    int nearest = -1;
    if (frame.type == QLatin1String("warp")) {
        RefreshDeformerOverlay();
        if (m_deformerOverlay) {
            const auto hit = m_deformerOverlay->HitTest(event->position());
            if (hit.kind == CDeformerHandleOverlay::WarpPoint) nearest = hit.index;
        }
    } else {
        double distance = 10.0;
        for (int index = 0; index < frame.geometry.points.size(); ++index) {
            const double candidate = QLineF(frame.geometry.points[index], event->position()).length();
            if (candidate < distance || (nearest < 0 && candidate <= distance)) { nearest = index; distance = candidate; }
        }
    }
    if (tool == VertexSelection::Tool::Move && nearest >= 0 && combine == VertexSelection::Combine::Replace &&
        (!frame.state.painted || frame.state.painted->at(nearest) > 0)) return true;
    consumed = true;
    VertexGesture gesture;
    gesture.frame = std::move(frame); gesture.tool = tool; gesture.combine = combine;
    gesture.start = gesture.current = event->position(); gesture.path = {event->position()}; gesture.picked = nearest;
    if (tool == VertexSelection::Tool::Brush) {
        VertexSelection::State selected;
        if (!VertexSelection::BeginBrush(gesture.frame.geometry, gesture.frame.state, m_vertexSettings.soft,
                m_vertexSettings.brush, combine, event->position(), gesture.stroke, selected, error) ||
            !ApplyVertexSelection(gesture.frame, selected, error)) return false;
    }
    m_vertexGesture = std::move(gesture);
    NotifyVertexSelection();
    return true;
}

bool OpengGlForm::MoveVertexGesture(QMouseEvent* event, QString* error)
{
    if (!m_vertexGesture) return true;
    if (!(event->buttons() & Qt::LeftButton) || !VertexFrameMatches(m_vertexGesture->frame))
        return FinishVertexGesture(true, error);
    auto& gesture = *m_vertexGesture;
    gesture.current = event->position();
    gesture.dragged = gesture.dragged || QLineF(gesture.start, gesture.current).length() >= QApplication::startDragDistance();
    if (gesture.tool == VertexSelection::Tool::Brush) {
        VertexSelection::Stroke next;
        VertexSelection::State selected;
        if (!VertexSelection::AppendBrush(gesture.stroke, event->position(), next, selected, error) ||
            !ApplyVertexSelection(gesture.frame, selected, error)) return false;
        gesture.stroke = std::move(next);
    } else if (gesture.tool == VertexSelection::Tool::Lasso && gesture.path.last() != event->position()) {
        if (gesture.path.size() >= VertexSelection::Limits{}.maximumPathPoints)
            return reject(error, QStringLiteral("套索路径过长，请结束后继续选择"));
        gesture.path.append(event->position());
    }
    NotifyVertexSelection();
    return true;
}

bool OpengGlForm::FinishVertexGesture(bool cancel, QString* error)
{
    if (!m_vertexGesture) return true;
    const auto& gesture = *m_vertexGesture;
    const bool matches = VertexFrameMatches(gesture.frame);
    if (!matches) {
        VertexSelection::State current, applied;
        auto* model = selectionModel();
        bool sameIdentity = model && model == gesture.frame.modelIdentity && CurrentPreviewDigest() == gesture.frame.digest;
        if (sameIdentity && gesture.frame.type == QLatin1String("warp")) {
            const auto stored = m_warpSelections.constFind(gesture.frame.id);
            sameIdentity = stored != m_warpSelections.cend() &&
                QSize(stored->columns, stored->rows) == gesture.frame.warpLayout;
            if (sameIdentity) current = stored->selection;
        } else if (sameIdentity) {
            auto* mesh = model->GetModel() ? model->GetModel()->GetMeshMap()->GetDrawMesh(gesture.frame.id) : nullptr;
            CDrawMesh::EditorState state;
            sameIdentity = mesh && mesh->CaptureEditorState(state) && state.vertexCount == gesture.frame.geometry.points.size();
            if (sameIdentity) current = {state.selectedVertices, state.paintedWeights};
        }
        if (gesture.tool == VertexSelection::Tool::Brush && sameIdentity &&
            VertexSelection::BrushState(gesture.stroke, applied) && current == applied &&
            !ApplyVertexSelection(gesture.frame, gesture.frame.state, error)) return false;
        m_vertexGesture.reset();
        m_weightHistory = {};
        m_ceditstate.RemoveState(CEditState::MousePress);
        NotifyVertexSelection();
        return cancel || reject(error, QStringLiteral("选择期间模型或画布已改变，已放弃手势"));
    }
    VertexSelection::State selected = gesture.frame.state;
    if (!cancel && gesture.tool == VertexSelection::Tool::Brush) {
        if (!VertexSelection::BrushState(gesture.stroke, selected, error)) return false;
    } else if (!cancel) {
        if (!gesture.dragged) {
            const QVector<int> hits = gesture.picked >= 0 ? QVector<int>{gesture.picked} : QVector<int>{};
            if (!VertexSelection::SelectVertices(gesture.frame.geometry.points.size(), hits, gesture.frame.state, gesture.combine, selected, error)) return false;
        } else {
            auto polygon = gesture.path;
            if (gesture.tool != VertexSelection::Tool::Lasso) {
                const auto rect = QRectF(gesture.start, gesture.current).normalized();
                polygon = {rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()};
            }
            if (!VertexSelection::SelectPolygon(gesture.frame.geometry.points, polygon, gesture.frame.state, gesture.combine, selected, error)) return false;
        }
    }
    if (!ApplyVertexSelection(gesture.frame, selected, error)) return false;
    if (!cancel) {
        if (gesture.tool == VertexSelection::Tool::Brush) RememberWeightStroke(gesture.frame, selected);
        else m_weightHistory = {};
    }
    m_vertexGesture.reset();
    m_ceditstate.RemoveState(CEditState::MousePress);
    NotifyVertexSelection();
    return true;
}

void OpengGlForm::RememberWeightStroke(const VertexFrame& frame, const VertexSelection::State& state)
{
    if (state == frame.state) return;
    auto& history = m_weightHistory;
    if (history.id != frame.id || history.type != frame.type || history.vertexCount != frame.geometry.points.size() ||
        history.edges != frame.geometry.edges || history.warpLayout != (frame.type == QLatin1String("warp") ? QSize(m_deformerTarget.columns, m_deformerTarget.rows) : QSize()) || history.index < 0 || history.states.value(history.index) != frame.state) {
        history = {}; history.id = frame.id; history.type = frame.type;
        history.vertexCount = frame.geometry.points.size(); history.edges = frame.geometry.edges;
        if (frame.type == QLatin1String("warp")) history.warpLayout = QSize(m_deformerTarget.columns, m_deformerTarget.rows);
        history.states.append(frame.state); history.index = 0;
    }
    history.states.resize(history.index + 1);
    history.states.append(state);
    const int maxStates = std::max(2, std::min(33, int((32LL * 1024 * 1024) / std::max<qint64>(1, qint64(history.vertexCount) * sizeof(double)))));
    while (history.states.size() > maxStates) history.states.removeFirst();
    history.index = history.states.size() - 1;
}

OpengGlForm::VertexSelectionStatus OpengGlForm::SelectionStatus() const
{
    VertexSelectionStatus status;
    VertexFrame frame;
    status.busy = IsKeyformDragging() || m_meshToolDispatchActive;
    if (!CaptureVertexFrame(frame)) return status;
    status.target = frame.id; status.type = frame.type;
    status.painted = frame.state.painted.has_value(); status.hardCount = frame.state.hard.size();
    VertexSelection::Influence influence;
    if (!CaptureInfluence(frame, influence)) return status;
    status.available = true; status.affectedCount = influence.affected.size();
    const auto& history = m_weightHistory;
    if (!status.busy && history.id == frame.id && history.type == frame.type && history.edges == frame.geometry.edges &&
        history.warpLayout == (frame.type == QLatin1String("warp") ? QSize(m_deformerTarget.columns, m_deformerTarget.rows) : QSize()) &&
        history.vertexCount == frame.geometry.points.size() && history.index >= 0 && history.states.value(history.index) == frame.state) {
        status.canUndoStroke = history.index > 0;
        status.canRedoStroke = history.index + 1 < history.states.size();
    }
    return status;
}

bool OpengGlForm::StepWeightStroke(int direction, QString* error)
{
    const auto status = SelectionStatus();
    if (direction < 0 ? !status.canUndoStroke : !status.canRedoStroke)
        return reject(error, QStringLiteral("当前没有可撤回或重做的权重笔触"));
    VertexFrame frame;
    if (!CaptureVertexFrame(frame, error)) return false;
    const int next = m_weightHistory.index + direction;
    if (!ApplyVertexSelection(frame, m_weightHistory.states[next], error)) return false;
    m_weightHistory.index = next;
    NotifyVertexSelection();
    return true;
}

bool OpengGlForm::UndoWeightStroke(QString* error) { return StepWeightStroke(-1, error); }
bool OpengGlForm::RedoWeightStroke(QString* error) { return StepWeightStroke(1, error); }

void OpengGlForm::NotifyVertexSelection()
{
    RefreshVertexOverlay();
    update();
    const QPointer<OpengGlForm> alive(this);
    NotifyMeshSelection();
    if (alive) emit VertexSelectionChanged();
}

void OpengGlForm::RefreshVertexOverlay()
{
    VertexFrame frame;
    if (!CaptureVertexFrame(frame)) { if (m_vertexOverlay) m_vertexOverlay->hide(); return; }
    VertexSelection::Influence influence;
    if (!CaptureInfluence(frame, influence)) {
        if (m_vertexOverlay) m_vertexOverlay->hide();
        return;
    }
    if (!m_vertexOverlay) m_vertexOverlay = new CVertexSelectionOverlay(this);
    CVertexSelectionOverlay::Frame overlay;
    overlay.viewport = size(); overlay.points = frame.geometry.points;
    overlay.hard = frame.state.hard; overlay.weights = influence.weights;
    if (m_vertexGesture) {
        const auto& gesture = *m_vertexGesture;
        if (gesture.tool == VertexSelection::Tool::Lasso) overlay.lasso = gesture.path;
        else if (gesture.tool != VertexSelection::Tool::Brush && gesture.dragged)
            overlay.rectangle = QRectF(gesture.start, gesture.current).normalized();
    }
    if (m_vertexSettings.tool == VertexSelection::Tool::Brush && m_vertexHover) {
        overlay.brushCenter = m_vertexHover; overlay.brushRadius = m_vertexSettings.brush.radius;
        overlay.brushHardness = m_vertexSettings.brush.hardness;
        overlay.brushOperation = m_vertexGesture ? m_vertexGesture->combine : m_vertexSettings.combine;
    }
    if (!m_vertexOverlay->SetFrame(overlay)) { m_vertexOverlay->hide(); return; }
    m_vertexOverlay->show(); m_vertexOverlay->raise();
}

bool OpengGlForm::PrepareWarpSelectionState(const QMap<QString, QSize>& layouts, const WarpSelectionSnapshot* requested,
                                           WarpSelectionSnapshot& prepared, QString* error) const
{
    if (error) error->clear();
    WarpSelectionSnapshot candidate;
    const auto& source = requested ? *requested : m_warpSelections;
    if (requested) for (auto it = source.cbegin(); it != source.cend(); ++it)
        if (!layouts.contains(it.key())) return reject(error, QStringLiteral("变形器选区引用了不存在的对象"));
    for (auto it = layouts.cbegin(); it != layouts.cend(); ++it) {
        const auto layout = it.value();
        const qint64 count = qint64(layout.width()) * layout.height();
        if (it.key().isEmpty() || layout.width() < 1 || layout.height() < 1 || count > 1000000)
            return reject(error, QStringLiteral("变形器选区布局无效"));
        WarpState state{layout.height(), layout.width(), {}};
        const auto found = source.constFind(it.key());
        if (found != source.cend()) {
            if (found->rows == state.rows && found->columns == state.columns) state.selection = found->selection;
            else if (requested) return reject(error, QStringLiteral("变形器选区行列数不一致"));
        }
        if (!VertexSelection::Validate(state.selection, int(count), error)) return false;
        if (state.selection.painted) state.selection.painted = QVector<double>(state.selection.painted->cbegin(), state.selection.painted->cend());
        state.selection.hard = QVector<int>(state.selection.hard.cbegin(), state.selection.hard.cend());
        candidate.insert(QString(it.key().constData(), it.key().size()), std::move(state));
    }
    prepared.swap(candidate);
    return true;
}

void OpengGlForm::CommitWarpSelectionState(WarpSelectionSnapshot&& prepared) noexcept
{
    if (m_warpSelections == prepared) return;
    if (m_weightHistory.type == QLatin1String("warp") &&
        !(m_warpSelections.value(m_weightHistory.id) == prepared.value(m_weightHistory.id))) ClearWeightStrokeHistory();
    m_warpSelections.swap(prepared);
}

void OpengGlForm::ResetVertexSelections()
{
    m_warpSelections.clear(); m_weightHistory = {}; m_vertexGesture.reset();
    if (m_vertexOverlay) m_vertexOverlay->Clear();
    auto* model = selectionModel();
    if (model && model->GetModel()) for (auto* mesh : model->GetModel()->GetMeshMap()->GetMapMesh())
        if (mesh) mesh->CancelSelect();
}

void OpengGlForm::SetWeightedMeshEditHandlers(MeshActivate activate, WeightedMeshBegin begin, MeshMove move, MeshFinish finish)
{
    const QPointer<OpengGlForm> alive(this);
    SetMeshEditHandlers(std::move(activate), {}, std::move(move), std::move(finish));
    if (alive && !IsKeyformDragging() && !m_meshEditCallbackActive && !m_deformerCallbackActive && !m_exportSuspended)
        m_weightedMeshBegin = std::move(begin);
}

void OpengGlForm::SetWeightedDeformerEditHandlers(WeightedDeformerBegin begin, DeformerMove move, MeshFinish finish)
{
    const QPointer<OpengGlForm> alive(this);
    SetDeformerEditHandlers({}, std::move(move), std::move(finish));
    if (alive && !IsKeyformDragging() && !m_meshEditCallbackActive && !m_deformerCallbackActive && !m_exportSuspended)
        m_weightedDeformerBegin = std::move(begin);
}

bool OpengGlForm::CaptureInfluence(const VertexFrame& frame, VertexSelection::Influence& influence, QString* error) const
{
    const QVector<double>* frozen = nullptr;
    if (m_meshDrag && m_meshDrag->name == frame.id && !m_meshDrag->weights.isEmpty()) frozen = &m_meshDrag->weights;
    if (m_deformerGesture && m_deformerTarget.id == frame.id && !m_deformerGesture->weights.isEmpty()) frozen = &m_deformerGesture->weights;
    if (frozen && frozen->size() == frame.geometry.points.size()) {
        influence = {}; influence.weights = *frozen;
        for (int index = 0; index < frozen->size(); ++index) if (frozen->at(index) > 0) influence.affected.append(index);
        return true;
    }
    if (!m_influenceFrame || m_influenceFrame->id != frame.id || m_influenceFrame->type != frame.type ||
        m_influenceFrame->state != frame.state || m_influenceFrame->geometry.points != frame.geometry.points ||
        m_influenceFrame->geometry.edges != frame.geometry.edges || !(m_influenceSoft == m_vertexSettings.soft)) {
        VertexSelection::Influence candidate;
        if (!VertexSelection::ComputeInfluence(frame.geometry, frame.state, m_vertexSettings.soft, candidate, error)) return false;
        m_influence = std::move(candidate); m_influenceFrame = frame; m_influenceSoft = m_vertexSettings.soft;
    }
    influence = m_influence;
    return true;
}

void OpengGlForm::ClearWeightStrokeHistory() noexcept
{
    m_weightHistory = {};
}
