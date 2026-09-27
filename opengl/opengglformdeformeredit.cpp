#include "opengglform.h"
#include "ccanvascoordinates.h"
#include <QApplication>
#include <QPointer>
#include <QScopeGuard>
#include <QLineF>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <exception>

namespace {
bool reject(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

LAppModel* currentModel()
{
    auto* manager = LAppLive2DManager::GetInstance();
    return manager && manager->GetModelNum() ? manager->GetModel(0) : nullptr;
}
}

void OpengGlForm::SetDeformerEditHandlers(DeformerBegin begin, DeformerMove move, MeshFinish finish)
{
    if (m_exportSuspended || m_deformerCallbackActive || m_meshEditCallbackActive) return;
    const QPointer<OpengGlForm> alive(this);
    QString error;
    if (!FinishKeyformDrag(true, &error)) { if (alive) emit MeshEditRejected(error); return; }
    if (!alive) return;
    m_deformerBegin = std::move(begin);
    m_weightedDeformerBegin = {};
    m_deformerMove = std::move(move);
    m_deformerFinish = std::move(finish);
}

bool OpengGlForm::SetDeformerEditTarget(const DeformerEditTarget& target, QString* error)
{
    if (error) error->clear();
    if (target.id.isEmpty() || target.keyform < 0 || (target.type != "warp" && target.type != "rotation") ||
        target.previewDigest.size() != 32 || target.points.isEmpty() || target.points.size() % 2 ||
        target.points.size() > 2000000 ||
        !std::all_of(target.points.cbegin(), target.points.cend(), [](float value) { return std::isfinite(value); }) ||
        !std::all_of(target.pose.cbegin(), target.pose.cend(), [](float value) { return std::isfinite(value); }) ||
        (target.type == "rotation" && (target.points.size() != 2 || target.direction.isNull() ||
            !std::isfinite(target.direction.x()) || !std::isfinite(target.direction.y()))))
        return reject(error, QStringLiteral("变形器编辑快照无效"));
    if (target.type == QLatin1String("warp") && m_weightedDeformerBegin &&
        (target.rows < 1 || target.columns < 1 || qint64(target.rows) * target.columns != target.points.size() / 2))
        return reject(error, QStringLiteral("变形器控制点行列数无效"));
    const QPointer<OpengGlForm> alive(this);
    if (m_vertexGesture && !m_deformerCallbackActive &&
        (target.id != m_deformerTarget.id || target.keyform != m_deformerTarget.keyform ||
         target.previewDigest != m_deformerTarget.previewDigest || target.pose != m_deformerTarget.pose ||
         target.rows != m_deformerTarget.rows || target.columns != m_deformerTarget.columns) &&
        !FinishVertexGesture(true, error)) return false;
    if (!alive) return false;
    const bool same = target.id == m_deformerTarget.id && target.type == m_deformerTarget.type &&
        target.keyform == m_deformerTarget.keyform && target.pose == m_deformerTarget.pose;
    if (m_deformerGesture && !m_deformerCallbackActive &&
        (!same || target.previewDigest != m_deformerGesture->acceptedDigest) && !FinishDeformerGesture(true, error)) return false;
    if (!alive) return false;
    if (m_deformerCallbackActive && !same) return reject(error, QStringLiteral("提交期间不能更换变形器编辑目标"));
    if (!m_deformerOverlay) m_deformerOverlay = new CDeformerHandleOverlay(this);
    if (!same) m_deformerOverlay->Clear();
    if (target.type == QLatin1String("warp") && target.rows > 0 && target.columns > 0 &&
        qint64(target.rows) * target.columns == target.points.size() / 2) {
        auto found = m_warpSelections.find(target.id);
        if (found == m_warpSelections.end() || found->rows != target.rows || found->columns != target.columns)
            m_warpSelections.insert(target.id, WarpState{target.rows, target.columns, {}});
    }
    m_deformerTarget = target;
    m_deformerOverlayDirty = true;
    RefreshDeformerOverlay();
    return true;
}

bool OpengGlForm::ClearDeformerEditTarget(QString* error)
{
    if (m_deformerCallbackActive) return reject(error, QStringLiteral("正在提交变形器拖拽"));
    const QPointer<OpengGlForm> alive(this);
    if (!FinishVertexGesture(true, error) || !alive || !FinishDeformerGesture(true, error)) return false;
    if (!alive) return false;
    auto* model = currentModel();
    if (m_deformerTarget.type == QLatin1String("rotation") && !CurrentPreviewDigest().isEmpty() &&
        model && !model->HasEditorPreview()) {
        if (auto* view = LAppDelegate::GetInstance()->GetView()) view->ShowWarps({m_deformerTarget.id});
    }
    m_deformerTarget = {};
    m_deformerOverlayDirty = true;
    if (m_deformerOverlay) { m_deformerOverlay->Clear(); m_deformerOverlay->hide(); }
    return true;
}

void OpengGlForm::RefreshDeformerOverlay()
{
    if (!m_deformerOverlay) return;
    auto* model = currentModel();
    auto* view = m_runtimeReady ? LAppDelegate::GetInstance()->GetView() : nullptr;
    if (m_exportSuspended || !model || !model->HasEditorPreview() || !view || m_deformerTarget.id.isEmpty() ||
        CurrentPreviewDigest() != m_deformerTarget.previewDigest || model->GetParameterValues() != m_deformerTarget.pose) {
        m_deformerOverlay->hide();
        return;
    }
    const auto mvp = view->GetMVP();
    if (m_deformerOverlayDirty || mvp != m_deformerOverlayMvp || size() != m_deformerOverlaySize) {
        const bool valid = m_deformerTarget.type == "warp"
            ? m_deformerOverlay->SetWarp(m_deformerTarget.points, mvp, size())
            : m_deformerOverlay->SetRotation(QPointF(m_deformerTarget.points[0], m_deformerTarget.points[1]),
                                            m_deformerTarget.direction, mvp, size());
        if (!valid) { m_deformerOverlay->hide(); return; }
        m_deformerOverlayMvp = mvp;
        m_deformerOverlaySize = size();
        m_deformerOverlayDirty = false;
    }
    if (m_deformerTarget.type == QLatin1String("warp")) {
        const auto state = m_warpSelections.constFind(m_deformerTarget.id);
        if (state != m_warpSelections.cend()) m_deformerOverlay->SetSelected(state->selection.hard);
    }
    m_deformerOverlay->show();
    m_deformerOverlay->raise();
}

bool OpengGlForm::DeformerFrameMatches() const
{
    auto* model = currentModel();
    auto* view = m_runtimeReady ? LAppDelegate::GetInstance()->GetView() : nullptr;
    return m_deformerGesture && view && model && model->HasEditorPreview() &&
        m_deformerGesture->viewport == size() && m_deformerGesture->mvp == view->GetMVP() &&
        m_deformerGesture->acceptedDigest == CurrentPreviewDigest() && model->GetParameterValues() == m_deformerTarget.pose;
}

bool OpengGlForm::BeginDeformerGesture(QMouseEvent* event, QString* error)
{
    if (m_deformerCallbackActive || m_meshEditCallbackActive || IsKeyformDragging())
        return reject(error, QStringLiteral("请先结束当前画布拖拽"));
    RefreshDeformerOverlay();
    if (!m_deformerOverlay || m_deformerOverlay->isHidden()) return false;
    const auto hit = m_deformerOverlay->HitTest(event->position());
    if (hit.kind == CDeformerHandleOverlay::None && m_deformerTarget.type != "warp") return false;
    event->accept();
    if ((!m_deformerBegin && !m_weightedDeformerBegin) || !m_deformerMove || !m_deformerFinish)
        return reject(error, QStringLiteral("变形器拖拽控制器尚未连接"));
    DeformerGesture gesture;
    gesture.handle = hit.kind; gesture.mvp = m_deformerOverlayMvp; gesture.viewport = size();
    gesture.acceptedDigest = CurrentPreviewDigest();
    gesture.startPixel = gesture.currentPixel = event->position(); gesture.modifiers = event->modifiers();
    gesture.previousSelection = m_deformerOverlay->Selected();
    const auto stored = m_warpSelections.constFind(m_deformerTarget.id);
    gesture.previousVertexSelection = stored != m_warpSelections.cend() ? stored->selection : VertexSelection::State{gesture.previousSelection, {}};
    if (!CCanvasCoordinates::ScreenToModel(gesture.mvp, gesture.viewport, event->position(), gesture.startModel))
        return reject(error, QStringLiteral("画布无法反解鼠标位置"));
    auto selected = gesture.previousSelection;
    if (m_deformerTarget.type == QLatin1String("warp") && !m_weightedDeformerBegin &&
        (gesture.previousVertexSelection.painted || m_vertexSettings.soft.enabled || m_vertexSettings.tool == VertexSelection::Tool::Brush))
        return reject(error, QStringLiteral("当前编辑控制器不支持加权拖拽"));
    if (hit.kind == CDeformerHandleOverlay::None) {
        m_deformerGesture = std::move(gesture);
        return true;
    }
    VertexSelection::State pointSelection = gesture.previousVertexSelection;
    QVector<double> weights;
    if (hit.kind == CDeformerHandleOverlay::WarpPoint) {
        if (m_weightedDeformerBegin) {
            if (!(pointSelection.painted && pointSelection.painted->at(hit.index) > 0) && !pointSelection.hard.contains(hit.index)) {
                if (!VertexSelection::SelectVertices(m_deformerTarget.points.size() / 2, {hit.index}, pointSelection,
                        VertexSelection::Combine::Replace, pointSelection, error)) return false;
            }
            VertexFrame frame;
            VertexSelection::Influence influence;
            if (!CaptureVertexFrame(frame, error) ||
                !VertexSelection::ComputeInfluence(frame.geometry, pointSelection, m_vertexSettings.soft, influence, error)) return false;
            weights = std::move(influence.weights);
            gesture.weights = weights;
            selected = pointSelection.hard;
        } else {
            if (event->modifiers() & Qt::AltModifier) {
                selected.removeAll(hit.index); m_deformerOverlay->SetSelected(selected);
                m_ceditstate.RemoveState(CEditState::MousePress);
                return true;
            }
            if (event->modifiers() & Qt::ShiftModifier) {
                if (!selected.contains(hit.index)) selected.append(hit.index);
            } else if (!selected.contains(hit.index)) selected = {hit.index};
        }
    } else selected.clear();
    if (hit.kind == CDeformerHandleOverlay::RotationAngle) {
        const QPointF origin(m_deformerTarget.points[0], m_deformerTarget.points[1]);
        gesture.lastRay = gesture.startModel - origin;
        if (gesture.lastRay.isNull()) return reject(error, QStringLiteral("旋转射线过短"));
    }
    const QPointer<OpengGlForm> alive(this);
    m_deformerCallbackActive = true;
    const auto release = qScopeGuard([alive] { if (alive) alive->m_deformerCallbackActive = false; });
    try {
        const auto begin = m_deformerBegin;
        const auto weighted = m_weightedDeformerBegin;
        if (!(weighted ? weighted(hit.kind, weights, error) : begin(hit.kind, selected, error)) || !alive) return false;
    } catch (const std::exception&) { return reject(error, QStringLiteral("无法开始变形器拖拽")); }
    m_deformerGesture = std::move(gesture);
    if (!DeformerFrameMatches()) {
        m_deformerCallbackActive = false;
        FinishDeformerGesture(true, error);
        return reject(error, QStringLiteral("开始拖拽时画布已改变"));
    }
    m_deformerOverlay->SetSelected(selected);
    if (m_weightedDeformerBegin && hit.kind == CDeformerHandleOverlay::WarpPoint) {
        auto state = m_warpSelections.find(m_deformerTarget.id);
        if (state != m_warpSelections.end()) state->selection = std::move(pointSelection);
    }
    NotifyVertexSelection();
    return true;
}

bool OpengGlForm::MoveDeformerGesture(QMouseEvent* event, QString* error)
{
    if (m_deformerCallbackActive || m_meshEditCallbackActive)
        return reject(error, QStringLiteral("正在提交画布拖拽"));
    if (!m_deformerGesture) return false;
    if (!DeformerFrameMatches() || !(event->buttons() & Qt::LeftButton)) return FinishDeformerGesture(true, error);
    auto& gesture = *m_deformerGesture;
    gesture.currentPixel = event->position();
    if (gesture.handle == CDeformerHandleOverlay::None) {
        gesture.rectangle = gesture.rectangle || QLineF(gesture.startPixel, event->position()).length() >= QApplication::startDragDistance();
        if (gesture.rectangle) m_deformerOverlay->SetSelectionRect(QRectF(gesture.startPixel, gesture.currentPixel).normalized());
        return true;
    }
    QPointF delta;
    double angle = 0;
    QPointF pixel = event->position(), model;
    if (gesture.handle != CDeformerHandleOverlay::RotationAngle && (event->modifiers() & Qt::ShiftModifier)) {
        const auto displacement = pixel - gesture.startPixel;
        if (std::abs(displacement.x()) >= std::abs(displacement.y())) pixel.setY(gesture.startPixel.y());
        else pixel.setX(gesture.startPixel.x());
    }
    if (!CCanvasCoordinates::ScreenToModel(gesture.mvp, gesture.viewport, pixel, model))
        return reject(error, QStringLiteral("画布无法反解拖拽位置"));
    if (gesture.handle == CDeformerHandleOverlay::RotationAngle) {
        const QPointF ray = model - QPointF(m_deformerTarget.points[0], m_deformerTarget.points[1]);
        if (QLineF(event->position(), m_deformerOverlay->OriginPixel()).length() < QApplication::startDragDistance())
            return reject(error, QStringLiteral("请在旋转中心外拖动角度手柄"));
        const double cross = gesture.lastRay.x() * ray.y() - gesture.lastRay.y() * ray.x();
        const double dot = gesture.lastRay.x() * ray.x() + gesture.lastRay.y() * ray.y();
        const double step = -qRadiansToDegrees(std::atan2(cross, dot));
        if (!std::isfinite(step)) return reject(error, QStringLiteral("旋转角度无效"));
        gesture.angle += step; gesture.lastRay = ray; angle = gesture.angle;
        if (event->modifiers() & Qt::ShiftModifier) angle = std::round(angle / 15.0) * 15.0;
    } else delta = model - gesture.startModel;
    const QPointer<OpengGlForm> alive(this);
    m_deformerCallbackActive = true;
    const auto release = qScopeGuard([alive] { if (alive) alive->m_deformerCallbackActive = false; });
    try {
        const auto move = m_deformerMove;
        if (!move(delta, angle, error) || !alive) return false;
    } catch (const std::exception&) { return reject(error, QStringLiteral("变形器预览被拒绝，保留最后接受的状态")); }
    gesture.acceptedDigest = CurrentPreviewDigest();
    return true;
}

bool OpengGlForm::FinishDeformerGesture(bool cancel, QString* error)
{
    if (error) error->clear();
    if (m_deformerCallbackActive) return reject(error, QStringLiteral("正在提交变形器拖拽"));
    if (!m_deformerGesture) return true;
    if (m_deformerGesture->handle != CDeformerHandleOverlay::None) {
        if (!m_deformerFinish) return reject(error, QStringLiteral("变形器拖拽提交器尚未连接"));
        const QPointer<OpengGlForm> alive(this);
        m_deformerCallbackActive = true;
        const auto release = qScopeGuard([alive] { if (alive) alive->m_deformerCallbackActive = false; });
        try {
            const auto finish = m_deformerFinish;
            if (!finish(cancel, error) || !alive) return false;
        } catch (const std::exception&) { return reject(error, QStringLiteral("无法结束变形器拖拽，保留最后接受的状态")); }
    }
    auto selected = m_deformerGesture->previousSelection;
    if (!cancel && m_deformerGesture->handle == CDeformerHandleOverlay::None) {
        const auto& gesture = *m_deformerGesture;
        const auto inside = gesture.rectangle ? m_deformerOverlay->PointsInRect(QRectF(gesture.startPixel, gesture.currentPixel)) : QVector<int>{};
        if (gesture.modifiers & Qt::AltModifier) { for (int point : inside) selected.removeAll(point); }
        else if (gesture.modifiers & Qt::ShiftModifier) { for (int point : inside) if (!selected.contains(point)) selected.append(point); }
        else selected = inside;
    } else if (!cancel) selected = m_deformerOverlay->Selected();
    if (m_deformerTarget.type == QLatin1String("warp")) {
        auto state = m_warpSelections.find(m_deformerTarget.id);
        if (state != m_warpSelections.end()) {
            if (cancel) state->selection = m_deformerGesture->previousVertexSelection;
            else if (!m_weightedDeformerBegin) state->selection = {selected, {}};
            selected = state->selection.hard;
        }
    }
    m_deformerOverlay->SetSelected(selected);
    m_deformerOverlay->SetSelectionRect(std::nullopt);
    m_deformerGesture.reset();
    m_ceditstate.RemoveState(CEditState::MousePress);
    RefreshDeformerOverlay();
    NotifyVertexSelection();
    return true;
}

bool OpengGlForm::FinishKeyformDrag(bool cancel, QString* error)
{
    const QPointer<OpengGlForm> alive(this);
    return FinishVertexGesture(cancel, error) && alive && FinishDeformerGesture(cancel, error) && alive && FinishArtMeshDrag(cancel, error);
}
