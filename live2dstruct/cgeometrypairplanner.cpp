#include "cgeometrypairplanner.h"
#include "cdeformereditcontext.h"
#include "ckeyformmappingcontext.h"
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace live2dstruct {
namespace {
using Pairs = CGeometryObjectPairs;
using Geometry = CModelGeometryKeyform;
using Frame = CRotationFrameGeometry::Frame;
using Result = CGeometryPairPlanner::Result;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool storePoint(QPointF point, QVector<float>& values, QString* error)
{
    const double limit = std::numeric_limits<float>::max();
    if (!std::isfinite(point.x()) || !std::isfinite(point.y()) || std::abs(point.x()) > limit || std::abs(point.y()) > limit)
        return fail(error, QStringLiteral("配对几何坐标超出可表示范围"));
    values.append(float(point.x())); values.append(float(point.y()));
    return true;
}

QPointF point(const QVector<float>& values, int index)
{
    return {values[index * 2], values[index * 2 + 1]};
}

QPointF normal(const Pairs::Pair& pair)
{
    QPointF direction = pair.lineDirection;
    direction /= std::max(std::abs(direction.x()), std::abs(direction.y()));
    direction /= std::hypot(direction.x(), direction.y());
    return {-direction.y(), direction.x()};
}

QPointF reflectVector(QPointF value, QPointF axisNormal)
{
    return value - 2 * QPointF::dotProduct(value, axisNormal) * axisNormal;
}

QPointF reflectPoint(QPointF value, const Pairs::Pair& pair, QPointF axisNormal)
{
    const double distance = QPointF::dotProduct(value, axisNormal) - QPointF::dotProduct(pair.linePoint, axisNormal);
    return value - 2 * distance * axisNormal;
}

bool matchingPoints(const QVector<float>& actual, const QVector<float>& expected, QString* error)
{
    if (actual.size() != expected.size()) return fail(error, QStringLiteral("配对几何映射前后顶点数量不一致"));
    for (int index = 0; index < actual.size(); ++index) {
        const double a = actual[index], b = expected[index];
        const double tolerance = CKeyformMappingContext::AbsoluteTolerance +
            CKeyformMappingContext::RelativeTolerance * std::max(std::abs(a), std::abs(b));
        if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > tolerance)
            return fail(error, QStringLiteral("配对顶点 %1 无法准确反解到目标坐标系").arg(index / 2));
    }
    return true;
}

bool pose(const CModelDocument& document, const Pairs::Object& object, int key,
          const QMap<QString, float>& base, QMap<QString, float>& result, QString* error)
{
    QMap<QString, float> values, corner;
    for (const auto& parameter : document.Parameters()) {
        if (values.contains(parameter.id)) return fail(error, QStringLiteral("模型参数标识重复：%1").arg(parameter.id));
        const auto metadata = document.ParameterMetadata(parameter.id);
        const float value = base.value(parameter.id, document.ParameterValue(parameter.id));
        if (!std::isfinite(value) || value < metadata.minimum || value > metadata.maximum)
            return fail(error, QStringLiteral("配对基础姿态参数超出范围：%1").arg(parameter.id));
        values.insert(parameter.id, value);
    }
    for (auto it = base.cbegin(); it != base.cend(); ++it)
        if (!values.contains(it.key())) return fail(error, QStringLiteral("配对基础姿态含未知参数：%1").arg(it.key()));
    if (!document.KeyformParameterValues(object.id, Pairs::TypeName(object.type), key, corner, error)) return false;
    for (auto it = corner.cbegin(); it != corner.cend(); ++it) values[it.key()] = it.value();
    result = std::move(values);
    return true;
}

bool validPair(const CModelDocument& document, const Pairs::Pair& input, Pairs::Pair& pair, QString* error)
{
    Pairs::State normalized;
    if (!Pairs::Normalize({document.SourceDigest(), {input}}, normalized, error) ||
        !Pairs::Validate(normalized, document, error)) return false;
    pair = normalized.pairs.front();
    if (!pair.enabled || pair.requiresReview) return fail(error, QStringLiteral("几何配对已停用或等待重新确认"));
    if (pair.first.object.type == Geometry::ArtMesh) return true;
    const auto ancestor = [&](const Pairs::Object& object, const Pairs::Object& possibleAncestor) {
        int parent = document.DeformerParentIndex(object.id);
        QSet<int> seen;
        while (parent != -1) {
            if (parent < 0 || parent >= document.Deformers().size() || seen.contains(parent)) return true;
            seen.insert(parent);
            const auto& node = document.Deformers()[parent];
            if (node.id == possibleAncestor.id) return true;
            parent = document.DeformerParentIndex(node.id);
        }
        return false;
    };
    if (ancestor(pair.first.object, pair.second.object) || ancestor(pair.second.object, pair.first.object))
        return fail(error, QStringLiteral("不能同时配对祖先与后代变形器；需要按候选父坐标系顺序求解"));
    return true;
}

CModelRotationKeyform rotationForm(const CModelDocument& document, const Geometry& geometry)
{
    auto result = document.DeformerRotationKeyforms(geometry.id).at(geometry.keyform);
    const auto& transform = std::get<CModelRotationTransform>(geometry.value);
    result.angle = transform.angle; result.originX = transform.originX; result.originY = transform.originY;
    result.scale = transform.scale; result.reflectX = transform.reflectX; result.reflectY = transform.reflectY;
    return result;
}

bool framePoints(const Frame& frame, QVector<float>& result, QString* error)
{
    QVector<float> values;
    for (const QPointF value : {frame.origin, frame.origin + frame.xAxis * frame.scale, frame.origin + frame.yAxis * frame.scale})
        if (!storePoint(value, values, error)) return false;
    result = std::move(values);
    return true;
}

struct Mapping
{
    CKeyformMappingContext mesh;
    CDeformerEditContext deformer;
    Geometry::Type type = Geometry::ArtMesh;
    Frame rotation;
    bool Build(const CModelDocument& document, const Geometry& geometry, const QMap<QString, float>& values,
               QVector<float>& world, QString* error)
    {
        type = geometry.type;
        if (type == Geometry::ArtMesh) {
            return mesh.Build(document, geometry.id, geometry.keyform, values, error) &&
                mesh.ToModel(std::get<QVector<float>>(geometry.value), world, error);
        }
        if (!deformer.Build(document, geometry.id, geometry.TypeName(), geometry.keyform, values, error)) return false;
        if (type == Geometry::Rotation)
            return deformer.CurrentRotationFrame(rotation, error) && framePoints(rotation, world, error);
        return deformer.ToModel(std::get<QVector<float>>(geometry.value), world, error);
    }
    bool Inverse(const QVector<float>& desired, const QVector<float>& previousWorld, const Geometry& previous,
                 Geometry& result, QString* error) const
    {
        result = previous;
        if (desired == previousWorld) return true;
        QVector<float> local, restored;
        if (type == Geometry::ArtMesh) {
            if (!mesh.FromModel(desired, local, error) || !mesh.ToModel(local, restored, error)) return false;
        } else if (!deformer.FromModel(desired, local, error) || !deformer.ToModel(local, restored, error)) return false;
        if (!matchingPoints(restored, desired, error)) return false;
        result.value = std::move(local);
        return true;
    }
};

struct Prepared
{
    Pairs::Pair pair;
    Geometry source, target;
    QVector<int> vertices;
    Mapping sourceMapping, targetMapping;
    Result result;
    bool Build(const CModelDocument& document, const Pairs::Pair& input, bool forward, int sourceKey,
               const QMap<QString, float>& base, QString* error)
    {
        if (!validPair(document, input, pair, error)) return false;
        const auto& sourceEndpoint = forward ? pair.first : pair.second;
        const auto& targetEndpoint = forward ? pair.second : pair.first;
        int targetKey;
        if (!Pairs::ResolveKeyform(pair, sourceKey, forward, targetKey, error) ||
            !document.CaptureGeometryKeyform(sourceEndpoint.object.type, sourceEndpoint.object.id, sourceKey, source, error) ||
            !document.CaptureGeometryKeyform(targetEndpoint.object.type, targetEndpoint.object.id, targetKey, target, error) ||
            !pose(document, sourceEndpoint.object, sourceKey, base, result.sourcePose, error) ||
            !pose(document, targetEndpoint.object, targetKey, base, result.targetPose, error) ||
            !sourceMapping.Build(document, source, result.sourcePose, result.sourceModel, error) ||
            !targetMapping.Build(document, target, result.targetPose, result.targetModel, error)) return false;
        result.targetBeforeModel = result.targetModel;
        vertices = pair.firstVertexToSecondVertex;
        if (!forward) for (int first = 0; first < pair.firstVertexToSecondVertex.size(); ++first)
            vertices[pair.firstVertexToSecondVertex[first]] = first;
        return true;
    }
};

bool culling(const CModelDocument& document, const Geometry& target, const QVector<float>& before,
             const QVector<float>& after, bool windingMatches, QString* error)
{
    if (target.type != Geometry::ArtMesh || before == after) return true;
    const auto found = std::find_if(document.Drawables().cbegin(), document.Drawables().cend(),
        [&](const auto& mesh) { return mesh.id == target.id; });
    if (found == document.Drawables().cend() || !found->culling) return true;
    if (!windingMatches)
        return fail(error, QStringLiteral("目标 %1 启用了背面剔除，但顶点对应没有反向绕序；镜像会反转源三角面的朝向").arg(target.id));
    const auto indices = document.DrawableIndices(target.id);
    const auto area = [&](const QVector<float>& values, int triangle) {
        const QPointF a = point(values, indices[triangle]), b = point(values, indices[triangle + 1]), c = point(values, indices[triangle + 2]);
        return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
    };
    for (int index = 0; index < indices.size(); index += 3)
        if (area(before, index) * area(after, index) < 0)
            return fail(error, QStringLiteral("目标 %1 的三角形 %2 会翻转绕序并受到背面剔除；请检查顶点对应和反向绕序设置")
                        .arg(target.id).arg(index / 3));
    return true;
}

void warpDiagnostic(const CModelDocument& document, const Prepared& prepared, bool synchronized, QStringList& diagnostics)
{
    if (prepared.source.type != Geometry::Warp) return;
    bool identity = prepared.pair.first.rows == prepared.pair.second.rows && prepared.pair.first.columns == prepared.pair.second.columns;
    for (int index = 0; index < prepared.vertices.size(); ++index) identity = identity && prepared.vertices[index] == index;
    const auto deformer = [&](const QString& id) -> const CModelDeformer* {
        const auto found = std::find_if(document.Deformers().cbegin(), document.Deformers().cend(), [&](const auto& value) { return value.id == id; });
        return found == document.Deformers().cend() ? nullptr : &*found;
    };
    const auto* first = deformer(prepared.source.id);
    const auto* second = deformer(prepared.target.id);
    if (!synchronized && identity && first && second && first->quadTransform == second->quadTransform)
        diagnostics.append(QStringLiteral("当前关键形的 Warp 点映射使用相同局部坐标对应；子对象与其他参数姿态仍需独立验证"));
    else diagnostics.append(QStringLiteral("此次操作保证控制点对应；不同网格顺序或插值方式下，格内与域外变形不保证完全对称"));
    if (!identity && first && second && (!first->quadTransform || !second->quadTransform))
        diagnostics.append(QStringLiteral("非四边形 Warp 使用固定三角划分，单轴反序可能改变格内插值"));
}

bool finish(const CModelDocument& document, Prepared& prepared, const Geometry& sourceAfter,
            const Geometry& targetAfter, Result& result, QString* error)
{
    prepared.result.changed = sourceAfter != prepared.source || targetAfter != prepared.target;
    if (prepared.result.changed) {
        if (!document.MakeGeometryEdit({prepared.source, prepared.target}, {sourceAfter, targetAfter},
                                       prepared.source, prepared.result.edit, error)) return false;
        CModelDocument candidate = document;
        QByteArray bytes;
        if (!candidate.ExecuteEdit(prepared.result.edit) || !candidate.BuildPreviewMoc(bytes, error))
            return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("无法准备配对几何预览"));
    }
    result = std::move(prepared.result);
    if (error) error->clear();
    return true;
}

bool solveRotation(const CModelDocument& document, Prepared& prepared, const Frame& desired,
                   Geometry& targetAfter, bool allowSingleReflection, QString* error)
{
    const auto reference = rotationForm(document, prepared.target);
    const bool single = document.DeformerRotationKeyforms(prepared.target.id).size() == 1;
    CModelRotationKeyform solved;
    const auto policy = single && allowSingleReflection ? CDeformerEditContext::ReflectionPolicy::AllowChange :
                                                         CDeformerEditContext::ReflectionPolicy::Preserve;
    if (!prepared.targetMapping.deformer.SolveRotation(desired, reference, solved, error, policy)) return false;
    targetAfter = prepared.target;
    targetAfter.value = CModelRotationTransform{solved.angle, solved.originX, solved.originY, solved.scale, solved.reflectX, solved.reflectY};
    return framePoints(desired, prepared.result.targetModel, error);
}
}

struct CGeometryPairPlanner::SynchronizationContext::State
{
    CModelDocument document;
    Prepared prepared;
};

bool CGeometryPairPlanner::PrepareMirror(const CModelDocument& document, const Pairs::Pair& pair,
                                         bool firstToSecond, int sourceKey, const QMap<QString, float>& base,
                                         Result& result, QString* error) try
{
    if (error) error->clear();
    Prepared prepared;
    if (!prepared.Build(document, pair, firstToSecond, sourceKey, base, error)) return false;
    const QPointF axisNormal = normal(prepared.pair);
    Geometry targetAfter;
    if (prepared.source.type == Geometry::Rotation) {
        Frame desired;
        if (!CRotationFrameGeometry::Mirror(prepared.sourceMapping.rotation, prepared.pair.linePoint,
                                           axisNormal, desired, error)) return false;
        QPointF x = prepared.pair.rotationLocalX, y = prepared.pair.rotationLocalY;
        if (!firstToSecond) {
            x = {prepared.pair.rotationLocalX.x(), prepared.pair.rotationLocalY.x()};
            y = {prepared.pair.rotationLocalX.y(), prepared.pair.rotationLocalY.y()};
        }
        const auto basisX = desired.xAxis, basisY = desired.yAxis;
        desired.xAxis = basisX * x.x() + basisY * x.y();
        desired.yAxis = basisX * y.x() + basisY * y.y();
        if (!solveRotation(document, prepared, desired, targetAfter, true, error)) return false;
    } else {
        QVector<float> reflected, desired(prepared.result.targetModel.size());
        reflected.reserve(prepared.result.sourceModel.size());
        for (int vertex = 0; vertex < prepared.vertices.size(); ++vertex)
            if (!storePoint(reflectPoint(point(prepared.result.sourceModel, vertex), prepared.pair, axisNormal), reflected, error)) return false;
        for (int source = 0; source < prepared.vertices.size(); ++source) {
            const int target = prepared.vertices[source];
            desired[target * 2] = reflected[source * 2]; desired[target * 2 + 1] = reflected[source * 2 + 1];
        }
        if (!culling(document, prepared.target, prepared.result.targetModel, desired, prepared.pair.reversedWinding, error) ||
            !prepared.targetMapping.Inverse(desired, prepared.result.targetModel, prepared.target, targetAfter, error)) return false;
        prepared.result.targetModel = std::move(desired);
        warpDiagnostic(document, prepared, false, prepared.result.diagnostics);
    }
    return finish(document, prepared, prepared.source, targetAfter, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备镜像复制，原结果保持不变")); }

bool CGeometryPairPlanner::PrepareSynchronized(const CModelDocument& document, const Pairs::Pair& pair,
                                               bool firstToSecond, const Geometry& sourceBefore, const Geometry& sourceAfter,
                                               const QMap<QString, float>& base, Result& result, QString* error) try
{
    if (error) error->clear();
    SynchronizationContext context;
    if (!PrepareSynchronization(document, pair, firstToSecond, sourceBefore.keyform, base, context, error)) return false;
    if (sourceBefore != context.m_state->prepared.source)
        return fail(error, QStringLiteral("同步编辑的源关键形已经改变或不属于此配对"));
    return Synchronize(context, sourceAfter, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备对称同步，原结果保持不变")); }

bool CGeometryPairPlanner::PrepareSynchronization(const CModelDocument& document, const Pairs::Pair& pair,
                                                  bool firstToSecond, int sourceKey, const QMap<QString, float>& base,
                                                  SynchronizationContext& result, QString* error) try
{
    if (error) error->clear();
    auto candidate = std::make_shared<SynchronizationContext::State>();
    candidate->document = document;
    if (!candidate->prepared.Build(candidate->document, pair, firstToSecond, sourceKey, base, error)) return false;
    result.m_state = std::move(candidate);
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法建立对称同步上下文，原上下文保持不变")); }

bool CGeometryPairPlanner::Synchronize(const SynchronizationContext& context, const Geometry& sourceAfter,
                                      Result& result, QString* error) try
{
    if (error) error->clear();
    if (!context.m_state) return fail(error, QStringLiteral("对称同步上下文尚未建立"));
    const auto& document = context.m_state->document;
    auto prepared = context.m_state->prepared;
    const auto& sourceBefore = prepared.source;
    if (!sourceBefore.SameEndpoint(sourceAfter)) return fail(error, QStringLiteral("同步编辑的源关键形不属于此上下文"));
    if (sourceBefore.type == Geometry::Rotation && std::holds_alternative<CModelRotationTransform>(sourceAfter.value)) {
        const auto& before = std::get<CModelRotationTransform>(sourceBefore.value);
        const auto& after = std::get<CModelRotationTransform>(sourceAfter.value);
        const auto forms = document.DeformerRotationKeyforms(sourceBefore.id);
        if (forms.size() > 1 && (before.reflectX != after.reflectX || before.reflectY != after.reflectY)) {
            for (int index = 0; index < forms.size(); ++index)
                if (index != sourceBefore.keyform && (forms[index].reflectX != after.reflectX || forms[index].reflectY != after.reflectY))
                    return fail(error, QStringLiteral("单个源关键形的反射修改会破坏相邻插值，请先统一整组反射标记"));
        }
    }
    CModelDocument changed = document;
    if (sourceBefore != sourceAfter) {
        CModelEdit edit;
        if (!document.MakeGeometryEdit({sourceBefore}, {sourceAfter}, sourceBefore, edit, error) || !changed.ExecuteEdit(edit))
            return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("源几何修改无效"));
    }
    Mapping changedMapping = prepared.sourceMapping;
    QVector<float> changedWorld;
    if (sourceAfter.type == Geometry::Rotation) {
        if (!changedMapping.deformer.ResolveRotation(rotationForm(changed, sourceAfter), changedMapping.rotation, error) ||
            !framePoints(changedMapping.rotation, changedWorld, error)) return false;
    } else if (sourceAfter.type == Geometry::ArtMesh) {
        if (!changedMapping.mesh.ToModel(std::get<QVector<float>>(sourceAfter.value), changedWorld, error)) return false;
    } else if (!changedMapping.deformer.ToModel(std::get<QVector<float>>(sourceAfter.value), changedWorld, error)) return false;
    const QPointF axisNormal = normal(prepared.pair);
    Geometry targetAfter;
    if (prepared.source.type == Geometry::Rotation) {
        const auto& beforeFrame = prepared.sourceMapping.rotation;
        const auto& afterFrame = changedMapping.rotation;
        Frame desired = prepared.targetMapping.rotation;
        desired.origin += reflectVector(afterFrame.origin - beforeFrame.origin, axisNormal);
        const auto rotate = [&](QPointF value) {
            const auto reflected = reflectVector(value, axisNormal);
            return reflectVector(afterFrame.xAxis * QPointF::dotProduct(beforeFrame.xAxis, reflected) +
                                 afterFrame.yAxis * QPointF::dotProduct(beforeFrame.yAxis, reflected), axisNormal);
        };
        desired.xAxis = rotate(desired.xAxis); desired.yAxis = rotate(desired.yAxis);
        if (beforeFrame.scale == 0 && afterFrame.scale != 0)
            return fail(error, QStringLiteral("源起始尺度为零，无法确定同步缩放比例；请使用镜像复制"));
        if (beforeFrame.scale != 0) desired.scale *= afterFrame.scale / beforeFrame.scale;
        if (sourceBefore == sourceAfter) {
            targetAfter = prepared.target;
        } else if (!solveRotation(document, prepared, desired, targetAfter, false, error)) return false;
    } else {
        QVector<float> updates, desired = prepared.result.targetModel;
        updates.reserve(changedWorld.size());
        for (int source = 0; source < prepared.vertices.size(); ++source) {
            const int target = prepared.vertices[source];
            const QPointF delta = point(changedWorld, source) - point(prepared.result.sourceModel, source);
            if (!storePoint(point(prepared.result.targetModel, target) + reflectVector(delta, axisNormal), updates, error)) return false;
            desired[target * 2] = updates[source * 2]; desired[target * 2 + 1] = updates[source * 2 + 1];
        }
        if (!culling(document, prepared.target, prepared.result.targetModel, desired, true, error) ||
            !prepared.targetMapping.Inverse(desired, prepared.result.targetModel, prepared.target, targetAfter, error)) return false;
        prepared.result.targetModel = std::move(desired);
        warpDiagnostic(document, prepared, true, prepared.result.diagnostics);
    }
    prepared.result.sourceModel = std::move(changedWorld);
    return finish(document, prepared, sourceAfter, targetAfter, result, error);
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备对称同步，原结果保持不变")); }

bool CGeometryPairPlanner::PrepareRotationCorrespondence(const CModelDocument& document, const Pairs::Pair& input,
                                                         int firstKey, int secondKey, const QMap<QString, float>& base,
                                                         Pairs::Pair& result, QString* error) try
{
    if (error) error->clear();
    Pairs::Pair candidate;
    if (!validPair(document, input, candidate, error)) return false;
    if (candidate.first.object.type != Geometry::Rotation)
        return fail(error, QStringLiteral("局部基对应只适用于 Rotation 配对"));
    QMap<QString, float> firstPose, secondPose;
    Geometry first, second;
    if (!pose(document, candidate.first.object, firstKey, base, firstPose, error) ||
        !pose(document, candidate.second.object, secondKey, base, secondPose, error) ||
        !document.CaptureGeometryKeyform(Geometry::Rotation, candidate.first.object.id, firstKey, first, error) ||
        !document.CaptureGeometryKeyform(Geometry::Rotation, candidate.second.object.id, secondKey, second, error)) return false;
    Mapping firstMapping, secondMapping;
    QVector<float> ignored;
    if (!firstMapping.Build(document, first, firstPose, ignored, error) ||
        !secondMapping.Build(document, second, secondPose, ignored, error)) return false;
    const QPointF axisNormal = normal(candidate);
    const auto column = [&](QPointF direction) {
        const auto reflected = reflectVector(direction, axisNormal);
        return QPointF(QPointF::dotProduct(firstMapping.rotation.xAxis, reflected),
                       QPointF::dotProduct(firstMapping.rotation.yAxis, reflected));
    };
    candidate.rotationLocalX = column(secondMapping.rotation.xAxis);
    candidate.rotationLocalY = column(secondMapping.rotation.yAxis);
    Pairs::State normalized;
    if (!Pairs::Normalize({document.SourceDigest(), {candidate}}, normalized, error)) return false;
    result = normalized.pairs.front();
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备 Rotation 局部基对应，原结果保持不变")); }
}
