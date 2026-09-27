#include "cmotionkeyclipboard.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <limits>
#include <exception>

namespace {
constexpr auto Format = "live2d-motion-keyframes";
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool validTrack(const CMotionTrack& track)
{
    return !track.TargetName().isEmpty() && !track.id.trimmed().isEmpty() && !track.id.contains(QChar::Null);
}
bool finitePoint(const QPointF& point)
{
    return std::isfinite(point.x()) && std::isfinite(point.y()) && point.x() >= 0 &&
        point.x() <= std::numeric_limits<float>::max() && std::abs(point.y()) <= std::numeric_limits<float>::max();
}
bool keys(const QJsonObject& object, const QSet<QString>& required, const QSet<QString>& optional = {})
{
    for (const auto& key : required) if (!object.contains(key)) return false;
    for (auto it = object.begin(); it != object.end(); ++it) if (!required.contains(it.key()) && !optional.contains(it.key())) return false;
    return true;
}
bool point(const QJsonValue& value, QPointF& result)
{
    if (!value.isArray()) return false;
    const auto array = value.toArray();
    if (array.size() != 2 || !array[0].isDouble() || !array[1].isDouble()) return false;
    const QPointF candidate(array[0].toDouble(), array[1].toDouble());
    if (!finitePoint(candidate)) return false;
    result = candidate; return true;
}
QJsonArray encodedPoint(const QPointF& value) { return {value.x(), value.y()}; }
QPointF shifted(const QPointF& value, double offset) { return {value.x() + offset, value.y()}; }
struct Clipboard {
    bool restricted = false;
    QVector<CMotionTimeline::PointInsertion> points;
    QMap<CMotionTrack, bool> tracks;
};

bool decode(const QByteArray& bytes, Clipboard& output, QString* error)
{
    if (bytes.isEmpty() || bytes.size() > CMotionKeyClipboard::MaximumBytes)
        return fail(error, QStringLiteral("关键帧剪贴板为空或超过 16 MiB"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return fail(error, QStringLiteral("关键帧剪贴板不是有效 JSON"));
    const auto root = document.object();
    if (!keys(root, {"format", "version", "restricted", "tracks"}) || root.value("format") != QJsonValue(Format) ||
        !root.value("version").isDouble() || (root.value("version").toDouble() != 1 && root.value("version").toDouble() != 2) ||
        !root.value("restricted").isBool() || !root.value("tracks").isArray())
        return fail(error, QStringLiteral("关键帧剪贴板格式或版本不受支持"));
    const bool authoring = root.value("version").toDouble() == 2;
    const auto tracks = root.value("tracks").toArray();
    if (tracks.isEmpty() || tracks.size() > CMotionTimeline::MaximumPointCount)
        return fail(error, QStringLiteral("关键帧剪贴板的轨道数量无效"));
    Clipboard candidate; candidate.restricted = root.value("restricted").toBool();
    double earliest = std::numeric_limits<double>::infinity();
    for (const auto& value : tracks) {
        if (!value.isObject()) return fail(error, QStringLiteral("关键帧剪贴板轨道不是对象"));
        const auto object = value.toObject();
        if (!keys(object, {"target", "id", "points"}) || !object.value("target").isString() ||
            !object.value("id").isString() || !object.value("points").isArray())
            return fail(error, QStringLiteral("关键帧剪贴板轨道字段无效"));
        CMotionTrack track; track.id = object.value("id").toString(); const auto target = object.value("target").toString();
        if (target == QLatin1String("Parameter")) track.target = CMotionTarget::Parameter;
        else if (target == QLatin1String("Model")) track.target = CMotionTarget::Model;
        else if (target == QLatin1String("PartOpacity")) track.target = CMotionTarget::PartOpacity;
        else return fail(error, QStringLiteral("关键帧剪贴板的轨道类型未知"));
        if (!validTrack(track) || candidate.tracks.contains(track)) return fail(error, QStringLiteral("关键帧剪贴板轨道重复或 ID 无效"));
        candidate.tracks.insert(track, true);
        const auto points = object.value("points").toArray();
        if (points.isEmpty() || points.size() > CMotionTimeline::MaximumPointCount - candidate.points.size())
            return fail(error, QStringLiteral("关键帧剪贴板的点数为空或超过 100000"));
        std::optional<QPointF> previous;
        for (const auto& entry : points) {
            if (!entry.isObject()) return fail(error, QStringLiteral("剪贴板关键帧不是对象"));
            const auto p = entry.toObject();
            const QSet<QString> required = authoring ? QSet<QString>{"time", "value", "segment", "tangent"} : QSet<QString>{"time", "value", "segment"};
            if (!keys(p, required, {"start", "controls"}) || !p.value("time").isDouble() ||
                !p.value("value").isDouble() || !p.value("segment").isDouble()) return fail(error, QStringLiteral("剪贴板关键帧字段无效"));
            const double segment = p.value("segment").toDouble();
            CMotionTimeline::PointInsertion insertion; insertion.track = track;
            insertion.point = {p.value("time").toDouble(), p.value("value").toDouble()};
            if (!finitePoint(insertion.point) || (previous && insertion.point.x() <= previous->x()) ||
                segment < 0 || segment > 3 || std::floor(segment) != segment)
                return fail(error, QStringLiteral("剪贴板关键帧的时间、数值或插值无效"));
            insertion.segmentType = int(segment);
            if (authoring) {
                CMotionTimeline::TangentState tangent;
                if (!p.value("tangent").isObject() || !CMotionTimeline::DecodeTangent(p.value("tangent").toObject(), tangent, error)) return false;
                insertion.tangent = tangent;
            }
            if (p.contains("start")) {
                QPointF start;
                if (!point(p.value("start"), start) || !previous || start != *previous)
                    return fail(error, QStringLiteral("剪贴板曲线段缺少相邻起点"));
                insertion.segmentStart = start;
            }
            if (insertion.segmentType != 0 && !insertion.segmentStart)
                return fail(error, QStringLiteral("非线性入段必须同时包含原相邻起点"));
            if (insertion.segmentType == 1) {
                if (!p.value("controls").isArray() || p.value("controls").toArray().size() != 2)
                    return fail(error, QStringLiteral("Bézier 入段需要两个控制柄"));
                for (const auto& c : p.value("controls").toArray()) {
                    QPointF handle;
                    if (!point(c, handle) || handle.x() < insertion.segmentStart->x() || handle.x() > insertion.point.x())
                        return fail(error, QStringLiteral("剪贴板 Bézier 控制柄超出曲线段"));
                    insertion.controls.append(handle);
                }
                if (insertion.controls[0].x() > insertion.controls[1].x())
                    return fail(error, QStringLiteral("剪贴板 Bézier 控制柄时间倒序"));
            } else if (p.contains("controls")) return fail(error, QStringLiteral("非 Bézier 入段不能包含控制柄"));
            earliest = std::min(earliest, insertion.point.x()); previous = insertion.point;
            candidate.points.append(std::move(insertion));
        }
    }
    if (earliest != 0) return fail(error, QStringLiteral("关键帧剪贴板必须以最早关键点为零时刻"));
    output = std::move(candidate); return true;
}
}

bool CMotionKeyClipboard::Capture(const CMotionTimeline& timeline, const CMotionPointSelection& selection,
                                  QByteArray& result, QString* error)
{
    if (error) error->clear();
    if (selection.isEmpty() || selection.size() > CMotionTimeline::MaximumPointCount)
        return fail(error, QStringLiteral("请选择 1 至 100000 个关键点"));
    try {
        QMap<CMotionTrack, QVector<int>> selected;
        QSet<CMotionPointRef> seen;
        double earliest = std::numeric_limits<double>::infinity();
        qint64 labels = 0;
        for (const auto& ref : selection) {
            const auto curve = timeline.Tracks().constFind(ref.track);
            if (!validTrack(ref.track) || seen.contains(ref) || curve == timeline.Tracks().cend() ||
                ref.index < 0 || ref.index >= curve->size() || !finitePoint(curve->at(ref.index)))
                return fail(error, QStringLiteral("所选关键点重复、已失效或数值无效"));
            if (!selected.contains(ref.track)) {
                labels += ref.track.id.size();
                if (labels > MaximumBytes) return fail(error, QStringLiteral("关键帧剪贴板的轨道名称超过大小限制"));
            }
            seen.insert(ref); selected[ref.track].append(ref.index); earliest = std::min(earliest, curve->at(ref.index).x());
        }
        bool authoring = false;
        for (const auto& ref : selection) authoring = authoring || timeline.Tangent(ref.track, ref.index) != CMotionTimeline::TangentState{};
        QJsonArray tracks;
        for (auto it = selected.begin(); it != selected.end(); ++it) {
            std::sort(it->begin(), it->end());
            const auto curve = timeline.Tracks().constFind(it.key()); QJsonArray points;
            int previous = -2;
            for (int index : *it) {
                const auto value = shifted(curve->at(index), -earliest);
                const bool connected = previous == index - 1;
                const int segment = connected ? timeline.SegmentType(it.key(), index) : 0;
                QJsonObject point{{"time", value.x()}, {"value", value.y()}, {"segment", segment}};
                if (authoring) {
                    auto tangent = timeline.Tangent(it.key(), index);
                    if (timeline.AreBeziersRestricted() && (tangent.mode == CMotionTimeline::TangentMode::Aligned || tangent.mode == CMotionTimeline::TangentMode::Flat)) {
                        const auto capabilities = timeline.Capabilities({it.key(), index});
                        if (capabilities.incoming.available) tangent.incomingFraction = 1.0 / 3;
                        if (capabilities.outgoing.available) tangent.outgoingFraction = 1.0 / 3;
                    }
                    point.insert("tangent", CMotionTimeline::EncodeTangent(tangent));
                }
                if (connected) point.insert("start", encodedPoint(shifted(curve->at(index - 1), -earliest)));
                if (segment == 1) {
                    const auto controls = timeline.BezierControls(it.key(), index);
                    if (controls.size() != 2) return fail(error, QStringLiteral("所选 Bézier 入段的控制柄不完整"));
                    point.insert("controls", QJsonArray{encodedPoint(shifted(controls[0], -earliest)), encodedPoint(shifted(controls[1], -earliest))});
                }
                points.append(point); previous = index;
            }
            tracks.append(QJsonObject{{"target", it.key().TargetName()}, {"id", it.key().id}, {"points", points}});
        }
        const auto bytes = QJsonDocument(QJsonObject{{"format", Format}, {"version", authoring ? 2 : 1},
            {"restricted", timeline.AreBeziersRestricted()}, {"tracks", tracks}}).toJson(QJsonDocument::Compact);
        Clipboard checked;
        if (!decode(bytes, checked, error)) return false;
        result = bytes; return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法复制关键帧，原剪贴板数据保持不变")); }
}

bool CMotionKeyClipboard::Paste(const CMotionTimeline& timeline, const QByteArray& bytes, double pasteTime,
                                const std::optional<CMotionTrack>& destination, bool replaceCoincident,
                                Result& result, QString* error)
{
    if (error) error->clear();
    if (!std::isfinite(pasteTime) || pasteTime < 0 || pasteTime > std::numeric_limits<float>::max() ||
        (destination && !validTrack(*destination))) return fail(error, QStringLiteral("粘贴目标轨道或时间无效"));
    try {
        Clipboard clipboard;
        if (!decode(bytes, clipboard, error)) return false;
        if (destination && clipboard.tracks.size() != 1) return fail(error, QStringLiteral("只有单轨剪贴板可以重定向到另一条轨道"));
        Result candidate; candidate.timeline = timeline;
        if (!clipboard.restricted && candidate.timeline.AreBeziersRestricted()) {
            if (!candidate.timeline.ConvertToUnrestricted(error)) return false;
            candidate.convertedToUnrestricted = true;
        }
        CMotionTimeline::BatchEdit edit;
        QSet<CMotionPointRef> removed;
        QMap<CMotionTrack, QMap<double, int>> existing;
        for (auto& insertion : clipboard.points) {
            if (destination) insertion.track = *destination;
            insertion.point = shifted(insertion.point, pasteTime);
            if (!finitePoint(insertion.point)) return fail(error, QStringLiteral("粘贴后的关键帧超出有限数值范围"));
            if (insertion.segmentStart) insertion.segmentStart = shifted(*insertion.segmentStart, pasteTime);
            if (clipboard.restricted && insertion.segmentType == 1) {
                const double start = insertion.segmentStart->x(), span = insertion.point.x() - start;
                insertion.controls[0].setX(start + span / 3);
                insertion.controls[1].setX(start + span * 2 / 3);
            } else for (auto& control : insertion.controls) control = shifted(control, pasteTime);
            auto found = existing.find(insertion.track);
            if (found == existing.end()) {
                QMap<double, int> times;
                const auto curve = timeline.Tracks().constFind(insertion.track);
                if (curve != timeline.Tracks().cend()) for (int index = 0; index < curve->size(); ++index) times.insert(curve->at(index).x(), index);
                found = existing.insert(insertion.track, std::move(times));
            }
            const auto coincident = found->constFind(insertion.point.x());
            if (coincident != found->cend()) {
                if (!replaceCoincident) return fail(error, QStringLiteral("目标轨道 %1 已有相同时刻的关键帧").arg(insertion.track.id));
                removed.insert({insertion.track, *coincident});
            }
            edit.insertions.append(std::move(insertion));
        }
        edit.removes = removed.values();
        std::sort(edit.removes.begin(), edit.removes.end());
        CMotionTimeline::BatchResult applied;
        if (!candidate.timeline.EditPoints(edit, applied, error)) return false;
        candidate.selection = std::move(applied.inserted);
        candidate.changed = !candidate.timeline.HasSameContent(timeline);
        result = std::move(candidate); return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法粘贴关键帧，原时间轴与输出保持不变")); }
}
