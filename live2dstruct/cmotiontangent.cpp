#include "cmotiontimeline.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
using Timeline = CMotionTimeline;
using Mode = Timeline::TangentMode;
using Side = Timeline::HandleSide;
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool finite(double value) { return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max(); }
bool same(QPointF a, QPointF b) { return a.x() == b.x() && a.y() == b.y(); }
bool near(double a, double b) {
    return a == b || std::abs(a - b) <= 64 * std::numeric_limits<double>::epsilon() *
        std::max({std::abs(a), std::abs(b), std::numeric_limits<double>::min()});
}
bool fields(const QJsonObject& object, const QSet<QString>& expected) {
    const auto keys = object.keys(); return QSet<QString>(keys.cbegin(), keys.cend()) == expected;
}
double edgeSlope(double h0, double h1, double s0, double s1) {
    const double scale = std::max(h0, h1); h0 /= scale; h1 /= scale;
    double slope = ((2 * h0 + h1) * s0 - h0 * s1) / (h0 + h1);
    if (slope == 0 || s0 == 0 || std::signbit(slope) != std::signbit(s0)) return 0;
    if (std::signbit(s0) != std::signbit(s1) && std::abs(slope) > 3 * std::abs(s0)) slope = 3 * s0;
    return slope;
}
double automaticSlope(const QVector<QPointF>& points, const QVector<int>& types, int index) {
    const bool left = index > 0 && types.value(index) == 1;
    const bool right = index + 1 < points.size() && types.value(index + 1) == 1;
    const auto interval = [&](int end) { return points[end].x() - points[end - 1].x(); };
    const auto secant = [&](int end) { return (points[end].y() - points[end - 1].y()) / interval(end); };
    if (!left && !right) return 0;
    if (!left) return index + 2 < points.size() && types.value(index + 2) == 1
        ? edgeSlope(interval(index + 1), interval(index + 2), secant(index + 1), secant(index + 2)) : secant(index + 1);
    if (!right) return index > 1 && types.value(index - 1) == 1
        ? edgeSlope(interval(index), interval(index - 1), secant(index), secant(index - 1)) : secant(index);
    const double a = secant(index), b = secant(index + 1);
    if (a == 0 || b == 0 || std::signbit(a) != std::signbit(b)) return 0;
    const double h = std::max(interval(index), interval(index + 1));
    const double hL = interval(index) / h, hR = interval(index + 1) / h;
    const double w1 = 2 * hR + hL, w2 = hR + 2 * hL, minimum = std::min(std::abs(a), std::abs(b));
    return std::copysign(minimum * ((w1 + w2) / (w1 * (minimum / std::abs(a)) + w2 * (minimum / std::abs(b)))), a);
}
}

bool CMotionTimeline::TangentState::operator==(const TangentState& other) const
{ return mode == other.mode && slope == other.slope && incomingFraction == other.incomingFraction && outgoingFraction == other.outgoingFraction; }

bool CMotionTimeline::ValidateTangent(const TangentState& state, QString* error)
{
    if (state.mode != Mode::Broken && state.mode != Mode::Aligned && state.mode != Mode::Flat && state.mode != Mode::AutoClamped)
        return fail(error, QStringLiteral("切线模式无效"));
    if (!std::isfinite(state.slope) || !std::isfinite(state.incomingFraction) || !std::isfinite(state.outgoingFraction) ||
        state.incomingFraction < 0 || state.incomingFraction > 1 || state.outgoingFraction < 0 || state.outgoingFraction > 1)
        return fail(error, QStringLiteral("切线斜率或时间长度无效"));
    if ((state.mode == Mode::Aligned || state.mode == Mode::Flat) && (state.incomingFraction <= 0 || state.outgoingFraction <= 0))
        return fail(error, QStringLiteral("连续或水平切线必须保留非零时间长度"));
    if ((state.mode == Mode::Flat && state.slope != 0) || ((state.mode == Mode::Broken || state.mode == Mode::AutoClamped) &&
        (state.slope != 0 || state.incomingFraction != 1.0 / 3 || state.outgoingFraction != 1.0 / 3)))
        return fail(error, QStringLiteral("切线模式包含无效的手工状态"));
    if (error) error->clear(); return true;
}

QJsonObject CMotionTimeline::EncodeTangent(const TangentState& state)
{
    return {{"mode", int(state.mode)}, {"slope", state.slope}, {"incomingFraction", state.incomingFraction}, {"outgoingFraction", state.outgoingFraction}};
}

bool CMotionTimeline::DecodeTangent(const QJsonObject& object, TangentState& state, QString* error)
{
    if (!fields(object, {"mode", "slope", "incomingFraction", "outgoingFraction"})) return fail(error, QStringLiteral("切线存储字段无效"));
    for (const auto& name : {"mode", "slope", "incomingFraction", "outgoingFraction"})
        if (!object[name].isDouble() || !std::isfinite(object[name].toDouble())) return fail(error, QStringLiteral("切线存储数值无效"));
    const double mode = object["mode"].toDouble();
    if (mode < 0 || mode > 3 || std::floor(mode) != mode) return fail(error, QStringLiteral("切线存储模式无效"));
    TangentState candidate{Mode(int(mode)), object["slope"].toDouble(), object["incomingFraction"].toDouble(), object["outgoingFraction"].toDouble()};
    if (!ValidateTangent(candidate, error)) return false; state = candidate; return true;
}

CMotionTimeline::TangentState CMotionTimeline::Tangent(const CMotionTrack& track, int index) const
{ return m_tangents.value(track).value(index); }

bool CMotionTimeline::HasAuthoringTangents() const
{
    for (const auto& states : m_tangents) for (const auto& state : states) if (state != TangentState{}) return true;
    return false;
}

CMotionTimeline::TangentCapabilities CMotionTimeline::Capabilities(const CMotionPointRef& key) const
{
    TangentCapabilities result; const auto points = m_curves.constFind(key.track);
    if (points == m_curves.cend() || key.index < 0 || key.index >= points->size()) return result;
    result.valid = true; result.restricted = AreBeziersRestricted(); result.state = Tangent(key.track, key.index);
    const auto side = [&](bool available) {
        HandleCapability capability; capability.available = available;
        capability.moveTime = available && !result.restricted;
        capability.moveValue = available && result.state.mode != Mode::Flat;
        if (!available) capability.reason = QStringLiteral("此侧没有 Bézier 曲线段");
        else if (!capability.moveTime && !capability.moveValue) capability.reason = QStringLiteral("受限曲线的水平切线不可拖动；请先转换为自由 Bézier");
        else if (result.state.mode == Mode::AutoClamped) capability.reason = QStringLiteral("手动拖动后将转为连续联动切线");
        else if (result.restricted) capability.reason = QStringLiteral("受限曲线只能上下调整手柄");
        return capability;
    };
    result.incoming = side(key.index > 0 && SegmentType(key.track, key.index) == 1);
    result.outgoing = side(key.index + 1 < points->size() && SegmentType(key.track, key.index + 1) == 1);
    return result;
}

std::optional<QPointF> CMotionTimeline::TangentHandle(const CMotionPointRef& key, HandleSide side) const
{
    const auto capability = Capabilities(key);
    if (side == Side::Incoming && capability.incoming.available) return BezierControls(key.track, key.index).value(1);
    if (side == Side::Outgoing && capability.outgoing.available) return BezierControls(key.track, key.index + 1).value(0);
    return {};
}

bool CMotionTimeline::ApplyTangentConstraints(const QSet<CMotionTrack>& tracks, QString* error)
{
    const bool restricted = AreBeziersRestricted();
    for (const auto& track : tracks) {
        const auto points = m_curves.constFind(track); if (points == m_curves.cend()) continue;
        auto& states = m_tangents[track]; if (states.size() != points->size()) return fail(error, QStringLiteral("切线状态与关键点数量不匹配"));
        if (std::all_of(states.cbegin(), states.cend(), [](const TangentState& state) { return state == TangentState{}; })) continue;
        auto& controls = m_bezierControls[track]; controls.resize(points->size());
        for (int index = 0; index < points->size(); ++index) {
            const auto& state = states[index]; if (!ValidateTangent(state, error)) return false;
            if (state.mode == Mode::Broken) continue;
            const auto key = points->at(index); const double slope = state.mode == Mode::AutoClamped ? automaticSlope(*points, m_segmentTypes.value(track), index) : state.slope;
            if (!std::isfinite(slope)) return fail(error, QStringLiteral("自动切线斜率超出数值范围"));
            for (const auto side : {Side::Incoming, Side::Outgoing}) {
                const int segment = side == Side::Incoming ? index : index + 1;
                if (segment <= 0 || segment >= points->size() || SegmentType(track, segment) != 1) continue;
                const double span = points->at(segment).x() - points->at(segment - 1).x();
                const double fraction = restricted || state.mode == Mode::AutoClamped ? 1.0 / 3 : side == Side::Incoming ? state.incomingFraction : state.outgoingFraction;
                const double delta = fraction * span * (side == Side::Incoming ? -1 : 1);
                const QPointF target(key.x() + delta, key.y() + slope * delta);
                if (!finite(target.x()) || !finite(target.y()) || target.x() < 0 ||
                    ((state.mode == Mode::Aligned || state.mode == Mode::Flat || state.mode == Mode::AutoClamped) &&
                     (fraction <= 0 || (!restricted && float(target.x()) == float(key.x())))))
                    return fail(error, QStringLiteral("关键点间隔不足或切线控制点超出运行时数值范围"));
                if (controls[segment].size() != 2) controls[segment] = BezierControls(track, segment);
                const int handle = side == Side::Incoming ? 1 : 0;
                if (!restricted) controls[segment][handle].setX(target.x());
                controls[segment][handle].setY(target.y());
            }
        }
    }
    return true;
}

bool CMotionTimeline::EditTangent(const TangentModeEdit& edit, QString* error)
{
    auto capability = Capabilities(edit.ref);
    if (!capability.valid) return fail(error, QStringLiteral("待编辑切线的关键点已失效"));
    TangentState state; state.mode = edit.mode;
    if (!ValidateTangent(state, error)) return false;
    if (edit.referenceSide && *edit.referenceSide != Side::Incoming && *edit.referenceSide != Side::Outgoing) return fail(error, QStringLiteral("切线参考侧无效"));
    if (edit.mode == capability.state.mode) return true;
    if (edit.mode == Mode::Aligned || edit.mode == Mode::Flat) {
        const auto& points = m_curves[edit.ref.track]; const auto key = points[edit.ref.index]; std::optional<double> incoming, outgoing;
        for (auto side : {Side::Incoming, Side::Outgoing}) if (const auto handle = TangentHandle(edit.ref, side)) {
            const int segment = side == Side::Incoming ? edit.ref.index : edit.ref.index + 1;
            const double span = points[segment].x() - points[segment - 1].x();
            const double length = std::abs(handle->x() - key.x()); const double ratio = length / span;
            if (side == Side::Incoming) state.incomingFraction = ratio > 0 ? ratio : 1.0 / 3;
            else state.outgoingFraction = ratio > 0 ? ratio : 1.0 / 3;
            if (length > 0) {
                const double slope = (handle->y() - key.y()) / (handle->x() - key.x());
                if (std::isfinite(slope)) (side == Side::Incoming ? incoming : outgoing) = slope;
            }
        }
        if (edit.mode == Mode::Aligned) state.slope = edit.referenceSide == Side::Outgoing ? outgoing.value_or(incoming.value_or(0)) : incoming.value_or(outgoing.value_or(0));
    }
    if (!ValidateTangent(state, error)) return false;
    m_tangents[edit.ref.track][edit.ref.index] = state; return true;
}

bool CMotionTimeline::EditHandle(const HandleEdit& edit, QString* error)
{
    const auto current = TangentHandle(edit.ref, edit.side); const auto capabilities = Capabilities(edit.ref);
    if (!current || !finite(edit.position.x()) || !finite(edit.position.y())) return fail(error, QStringLiteral("Bézier 手柄或位置无效"));
    const auto& points = m_curves[edit.ref.track]; const int index = edit.ref.index;
    const int segment = edit.side == Side::Incoming ? index : index + 1, control = edit.side == Side::Incoming ? 1 : 0;
    QPointF target = edit.position;
    if (capabilities.restricted && target.x() != current->x()) return fail(error, QStringLiteral("请先转换为自由 Bézier，再水平移动手柄"));
    if (target.x() < points[segment - 1].x() || target.x() > points[segment].x()) return fail(error, QStringLiteral("Bézier 手柄时间必须位于所属曲线段内"));
    auto state = Tangent(edit.ref.track, index);
    if (state.mode == Mode::Flat) target.setY(points[index].y());
    if (same(target, *current)) return true;
    const bool transient = state.mode == Mode::Broken && edit.linkForGesture;
    if (state.mode == Mode::AutoClamped || transient) {
        if (!EditTangent({edit.ref, Mode::Aligned, edit.side}, error)) return false;
        state = Tangent(edit.ref.track, index);
    }
    if (state.mode == Mode::Broken) {
        auto controls = m_bezierControls[edit.ref.track].value(segment);
        if (controls.size() != 2) controls = BezierControls(edit.ref.track, segment);
        if (!capabilities.restricted) controls[control].setX(target.x()); controls[control].setY(target.y());
        m_bezierControls[edit.ref.track][segment] = controls; return true;
    }
    const double delta = target.x() - points[index].x(), span = points[segment].x() - points[segment - 1].x();
    if ((state.mode == Mode::Aligned || state.mode == Mode::Flat) &&
        (delta == 0 || (!capabilities.restricted && float(target.x()) == float(points[index].x()))))
        return fail(error, QStringLiteral("连续或水平切线需要可表示的时间长度；可切换为断开模式"));
    if (state.mode == Mode::Aligned) state.slope = (target.y() - points[index].y()) / delta;
    if (edit.side == Side::Incoming) state.incomingFraction = std::abs(delta) / span; else state.outgoingFraction = std::abs(delta) / span;
    m_tangents[edit.ref.track][index] = state;
    if (!ApplyTangentConstraints({edit.ref.track}, error)) return false;
    if (transient) m_tangents[edit.ref.track][index] = TangentState{};
    return true;
}

bool CMotionTimeline::ValidateTangentState(QString* error) const
{
    if (m_tangents.size() != m_curves.size()) return fail(error, QStringLiteral("切线轨道目录不匹配"));
    QSet<CMotionTrack> tracks;
    for (auto it = m_curves.cbegin(); it != m_curves.cend(); ++it) {
        if (!m_tangents.contains(it.key()) || m_tangents[it.key()].size() != it->size()) return fail(error, QStringLiteral("切线与关键点数量不匹配"));
        tracks.insert(it.key());
    }
    if (!HasAuthoringTangents()) { if (error) error->clear(); return true; }
    CMotionTimeline candidate;
    candidate.m_source = m_source; candidate.m_curves = m_curves; candidate.m_segmentTypes = m_segmentTypes;
    candidate.m_bezierControls = m_bezierControls; candidate.m_tangents = m_tangents;
    if (!candidate.ApplyTangentConstraints(tracks, error)) return false;
    for (const auto& track : tracks) for (int index = 0; index < m_curves[track].size(); ++index) {
        if (Tangent(track, index).mode == Mode::Broken) continue;
        for (auto side : {Side::Incoming, Side::Outgoing}) if (const auto actual = TangentHandle({track, index}, side)) {
            const auto expected = candidate.TangentHandle({track, index}, side);
            if (!expected || !near(actual->x(), expected->x()) || !near(actual->y(), expected->y())) return fail(error, QStringLiteral("保存的切线模式与 Bézier 控制点不一致"));
        }
    }
    return true;
}

bool CMotionTimeline::TransformPoints(const PointTransform& transform, BatchResult& result, QString* error)
{ BatchEdit edit; edit.transform = transform; return EditPoints(edit, result, error); }

bool CMotionTimeline::ValidateEditableState(QString* error) const
{
    QJsonObject state;
    return HasAuthoringTangents() ? SaveState(&state, Loop(), 0, error) : BuildJson(nullptr, Timing().fps, Loop(), true, error);
}
