#include "cdeformereditcontext.h"
#include "ckeyformbindinggrid.h"
#include "ckeyformmappingcontext.h"
#include "cmodeldocument.h"
#include "csectiontable.h"
#include "tree/c2dtree.h"

#include <QSet>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool sameSlice(const std::vector<float>& pool, qint64 begin, const QVector<float>& values)
{
    return begin >= 0 && begin <= qint64(pool.size()) && values.size() <= qint64(pool.size()) - begin &&
        std::equal(values.cbegin(), values.cend(), pool.cbegin() + begin) &&
        std::all_of(values.cbegin(), values.cend(), [](float value) { return std::isfinite(value); });
}

bool sameDeformer(const CSectionTable& table, const CDeformers& source,
                  const CModelDeformer& definition, const CModelDocument& document)
{
    if (source.parent_part_indices != definition.parentPartIndex ||
        source.parent_deformer_indices != document.DeformerParentIndex(source.ids)) return false;
    const int specific = source.specific_sources_indices;
    if (source.types == 0) {
        if (definition.type != CModelDeformer::Warp || specific < 0 || specific >= table.warp_deformers.size()) return false;
        const auto* warp = table.warp_deformers[specific];
        const auto forms = document.DeformerWarpKeyforms(source.ids);
        const qint64 begin = warp->keyform_sources_begin_indices;
        if (warp->rows != definition.rows || warp->columns != definition.columns ||
            warp->is_quad_source != definition.quadTransform ||
            qint64(warp->vertex_counts) != (qint64(warp->rows) + 1) * (qint64(warp->columns) + 1) ||
            forms.size() != warp->keyform_sources_counts || begin < 0 || begin > table.warp_deformer_keyforms.size() ||
            forms.size() > table.warp_deformer_keyforms.size() - begin || !table.keyform_positions) return false;
        for (int index = 0; index < forms.size(); ++index) {
            const auto* form = table.warp_deformer_keyforms[begin + index];
            if (!std::isfinite(forms[index].opacity) || forms[index].opacity != form->opacities ||
                forms[index].positions.size() != qint64(warp->vertex_counts) * 2 ||
                !sameSlice(table.keyform_positions->xys, form->keyform_position_sources_begin_indices, forms[index].positions))
                return false;
        }
        return true;
    }
    if (source.types != 1 || definition.type != CModelDeformer::Rotation ||
        specific < 0 || specific >= table.rotation_deformers.size()) return false;
    const auto* rotation = table.rotation_deformers[specific];
    const auto forms = document.DeformerRotationKeyforms(source.ids);
    const qint64 begin = rotation->keyform_sources_begin_indices;
    if (!std::isfinite(definition.baseAngle) || definition.baseAngle != rotation->base_angles ||
        forms.size() != rotation->keyform_sources_counts || begin < 0 || begin > table.rotation_deformer_keyforms.size() ||
        forms.size() > table.rotation_deformer_keyforms.size() - begin) return false;
    for (int index = 0; index < forms.size(); ++index) {
        const auto* form = table.rotation_deformer_keyforms[begin + index];
        const CModelRotationKeyform actual{form->opacities, form->angles, form->origin_x, form->origin_y,
                                          form->scales, form->is_reflect_x, form->is_reflect_y};
        if (!forms[index].IsValid() || !(forms[index] == actual)) return false;
    }
    return true;
}

bool map(const C2DTreeNode& node, const QVector<float>& input, QVector<float>& output,
         bool inverse, QString* error)
{
    if (input.size() > 2000000 || input.size() % 2 != 0 ||
        !std::all_of(input.cbegin(), input.cend(), [](float value) { return std::isfinite(value); }))
        return fail(error, QStringLiteral("变形器映射坐标数量或数值无效"));
    const std::vector<float> values(input.cbegin(), input.cend());
    std::vector<float> mapped;
    if (!(inverse ? node.MapFromCanvas(values, mapped) : node.MapToCanvas(values, mapped)))
        return fail(error, inverse ? QStringLiteral("父坐标系无法唯一反解所选点，可能存在退化或折叠")
                                   : QStringLiteral("父坐标系映射失败或坐标超出范围"));
    QVector<float> candidate(mapped.cbegin(), mapped.cend());
    output.swap(candidate);
    return true;
}

bool roundTripMatches(const QVector<float>& actual, const QVector<float>& expected, QString* error)
{
    if (actual.size() != expected.size()) return fail(error, QStringLiteral("变形器映射坐标数量不一致"));
    for (qsizetype index = 0; index < actual.size(); ++index) {
        const double a = actual[index], b = expected[index];
        const double tolerance = CKeyformMappingContext::AbsoluteTolerance +
            CKeyformMappingContext::RelativeTolerance * std::max(std::abs(a), std::abs(b));
        if (!std::isfinite(a) || !std::isfinite(b) || std::abs(a - b) > tolerance)
            return fail(error, QStringLiteral("所选点反解后无法准确回到模型坐标：%1").arg(index / 2));
    }
    return true;
}

void flipY(QVector<float>& points)
{
    for (qsizetype index = 1; index < points.size(); index += 2) points[index] = -points[index];
}
}

struct CDeformerEditContext::State
{
    explicit State(const QByteArray& bytes) : reader(CReader::FromBytes(bytes)), table(reader) {}
    CReader reader;
    CSectionTable table;
    std::unique_ptr<C2DTree> tree;
    const C2DTreeNode* target = nullptr;
    QVector<float> points;
    QPointF direction;
    std::shared_ptr<const CCoordinateFrame> parentFrame;
    CModelRotationKeyform rotation;
    float baseAngle = 0;
    bool isRotation = false;
};

bool CDeformerEditContext::Build(const CModelDocument& document, const QString& id, const QString& type,
                                 int keyform, const QMap<QString, float>& fullPose, QString* error)
{
    if (error) error->clear();
    if (!document.IsLoaded() || document.SourceBytes().isEmpty())
        return fail(error, QStringLiteral("变形器编辑需要已加载的模型快照"));
    if (type != QStringLiteral("warp") && type != QStringLiteral("rotation"))
        return fail(error, QStringLiteral("变形器编辑类型无效：%1").arg(type));
    try {
        QMap<QString, float> corner;
        if (!document.KeyformParameterValues(id, type, keyform, corner, error)) return false;
        if (fullPose.size() != document.Parameters().size())
            return fail(error, QStringLiteral("变形器编辑姿态必须包含全部参数"));
        QSet<QString> parameters;
        for (const auto& parameter : document.Parameters()) {
            const auto value = fullPose.constFind(parameter.id);
            const auto metadata = document.ParameterMetadata(parameter.id);
            if (parameter.id.isEmpty() || parameters.contains(parameter.id) || value == fullPose.cend() ||
                !std::isfinite(value.value()) || !std::isfinite(metadata.minimum) || !std::isfinite(metadata.maximum) ||
                value.value() < metadata.minimum || value.value() > metadata.maximum)
                return fail(error, QStringLiteral("编辑姿态参数缺失、重复或超出范围：%1").arg(parameter.id));
            parameters.insert(parameter.id);
        }
        for (auto value = corner.cbegin(); value != corner.cend(); ++value)
            if (fullPose.value(value.key()) != value.value())
                return fail(error, QStringLiteral("编辑姿态未处于目标关键形角点：%1").arg(value.key()));

        QByteArray bytes;
        if (!document.BuildPreviewMoc(bytes, error)) return false;
        auto candidate = std::make_shared<State>(bytes);
        const auto& table = candidate->table;
        if (!table.IsValid()) return fail(error, candidate->reader.Error());
        CKeyformBindingGrid bindings;
        if (!bindings.Build(table, error)) return false;
        const auto targetType = type == QStringLiteral("warp") ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation;
        QMap<QString, float> patchedCorner;
        if (!bindings.Pose(targetType, id, keyform, patchedCorner, error)) return false;
        if (patchedCorner != corner)
            return fail(error, QStringLiteral("预览补丁改变了目标关键形的参数绑定"));

        QHash<QString, int> idCounts;
        int targetIndex = -1;
        for (int index = 0; index < table.deformers.size(); ++index) {
            const auto* deformer = table.deformers[index];
            ++idCounts[deformer->ids];
            if (deformer->ids == id) targetIndex = index;
        }
        if (targetIndex < 0) return fail(error, QStringLiteral("预览快照缺少目标变形器：%1").arg(id));
        if (table.deformers[targetIndex]->types != (targetType == CKeyformBindingGrid::Warp ? 0 : 1))
            return fail(error, QStringLiteral("目标变形器类型与预览快照不一致：%1").arg(id));
        QSet<int> blendWarps, blendRotations;
        for (const auto* shape : table.blend_shapes_warp_deformers) if (shape) blendWarps.insert(shape->target_indices);
        for (const auto* shape : table.blend_shapes_rotation_deformers) if (shape) blendRotations.insert(shape->target_indices);
        QSet<int> chain;
        for (int index = targetIndex; index != -1; index = table.deformers[index]->parent_deformer_indices) {
            if (index < 0 || index >= table.deformers.size() || chain.contains(index))
                return fail(error, QStringLiteral("变形器父链缺失或形成循环"));
            chain.insert(index);
            const auto* deformer = table.deformers[index];
            if (deformer->ids.isEmpty() || idCounts.value(deformer->ids) != 1 || index >= document.Deformers().size() ||
                document.Deformers()[index].id != deformer->ids || (deformer->types != 0 && deformer->types != 1))
                return fail(error, QStringLiteral("变形器标识、类型或所属源不一致：%1").arg(deformer->ids));
            if ((deformer->types == 0 ? blendWarps : blendRotations).contains(deformer->specific_sources_indices))
                return fail(error, QStringLiteral("目标或父变形器包含尚不支持编辑映射的 BlendShape：%1").arg(deformer->ids));
            const auto gridType = deformer->types == 0 ? CKeyformBindingGrid::Warp : CKeyformBindingGrid::Rotation;
            const QString diagnostic = bindings.Diagnostic(gridType, deformer->ids);
            if (!diagnostic.isEmpty()) return fail(error, QStringLiteral("变形器 %1：%2").arg(deformer->ids, diagnostic));
            for (const auto& axis : bindings.Axes(gridType, deformer->ids)) {
                const auto value = fullPose.constFind(axis.parameterId);
                if (axis.parameterType != 0 || axis.keys.isEmpty() || value == fullPose.cend() ||
                    value.value() < axis.keys.first() || value.value() > axis.keys.last())
                    return fail(error, QStringLiteral("变形器 %1 的参数 %2 不在普通轴关键值覆盖范围内")
                                .arg(deformer->ids, axis.parameterId));
            }
            if (!sameDeformer(table, *deformer, document.Deformers()[index], document))
                return fail(error, QStringLiteral("变形器数据与文档有效关键形不一致：%1").arg(deformer->ids));
        }

        QVector<float> local;
        if (targetType == CKeyformBindingGrid::Warp) local = document.DeformerWarpKeyforms(id).at(keyform).positions;
        else {
            const auto form = document.DeformerRotationKeyforms(id).at(keyform);
            local = {form.originX, form.originY};
        }
        if (local.isEmpty()) return fail(error, QStringLiteral("目标变形器没有可编辑坐标"));
        candidate->tree = std::make_unique<C2DTree>(table);
        for (auto* node : candidate->tree->getParametersTree())
            if (auto* parameter = dynamic_cast<CParameterNode*>(node)) parameter->SetCurValue(fullPose.value(parameter->getName()));
        const auto nodes = candidate->tree->GetWarpKeys();
        for (const auto* node : nodes) {
            if (!node || node->getName() != id) continue;
            if (candidate->target) return fail(error, QStringLiteral("求值树包含重复的目标变形器：%1").arg(id));
            candidate->target = node;
            if (targetType == CKeyformBindingGrid::Rotation) {
                const auto* rotation = dynamic_cast<const CRotationNode*>(node);
                if (!rotation || !rotation->GetCanvasBasis())
                    return fail(error, QStringLiteral("旋转变形器的父方向退化或无法求值：%1").arg(id));
                const QPointF y = rotation->GetCanvasBasis()->yAxis;
                candidate->direction = QPointF(-y.x(), y.y());
                candidate->parentFrame = node->ParentFrame();
                candidate->rotation = document.DeformerRotationKeyforms(id).at(keyform);
                candidate->baseAngle = document.Deformers()[targetIndex].baseAngle;
                candidate->isRotation = true;
            } else if (!dynamic_cast<const CWarpNode*>(node))
                return fail(error, QStringLiteral("求值树目标类型不一致：%1").arg(id));
        }
        if (!candidate->target) return fail(error, QStringLiteral("求值树缺少目标变形器：%1").arg(id));
        if (!map(*candidate->target, local, candidate->points, false, error)) return false;
        if (!sameSlice(candidate->target->GetCanvasPositions(), 0, candidate->points) ||
            candidate->target->GetCanvasPositions().size() != size_t(candidate->points.size()))
            return fail(error, QStringLiteral("目标变形器的求值坐标与关键形角点不一致：%1").arg(id));
        flipY(candidate->points);
        m_state = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("变形器编辑上下文构建失败，原上下文保持不变"));
    }
}

bool CDeformerEditContext::ToModel(const QVector<float>& local, QVector<float>& world, QString* error) const
{
    if (error) error->clear();
    if (!m_state) return fail(error, QStringLiteral("变形器编辑上下文尚未构建"));
    try {
        QVector<float> candidate;
        if (!map(*m_state->target, local, candidate, false, error)) return false;
        flipY(candidate);
        world.swap(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("变形器坐标映射失败，原输出保持不变"));
    }
}

bool CDeformerEditContext::FromModel(const QVector<float>& world, QVector<float>& local, QString* error) const
{
    if (error) error->clear();
    if (!m_state) return fail(error, QStringLiteral("变形器编辑上下文尚未构建"));
    try {
        QVector<float> canvas = world, candidate, reprojected;
        flipY(canvas);
        if (!map(*m_state->target, canvas, candidate, true, error) ||
            !map(*m_state->target, candidate, reprojected, false, error) ||
            !roundTripMatches(reprojected, canvas, error)) return false;
        local.swap(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("变形器坐标反解失败，原输出保持不变"));
    }
}

const QVector<float>& CDeformerEditContext::ModelPoints() const
{
    static const QVector<float> empty;
    return m_state ? m_state->points : empty;
}

QPointF CDeformerEditContext::RotationDirection() const
{
    return m_state ? m_state->direction : QPointF();
}

bool CDeformerEditContext::CurrentRotationFrame(CRotationFrameGeometry::Frame& result, QString* error) const
{
    if (!m_state || !m_state->isRotation)
        return fail(error, QStringLiteral("当前上下文不是旋转变形器"));
    return ResolveRotation(m_state->rotation, result, error);
}

bool CDeformerEditContext::ResolveRotation(const CModelRotationKeyform& keyform,
                                          CRotationFrameGeometry::Frame& result, QString* error) const
{
    if (error) error->clear();
    if (!m_state || !m_state->isRotation)
        return fail(error, QStringLiteral("当前上下文不是旋转变形器"));
    if (!keyform.IsValid()) return fail(error, QStringLiteral("旋转关键形数值无效"));
    try {
        CRotationNodeKeyform key(keyform.opacity, keyform.angle, keyform.originX, keyform.originY,
                                keyform.scale, keyform.reflectX, keyform.reflectY);
        CCoordinateFrame::ResolvedRotation resolved;
        if (!CCoordinateFrame::ResolveRotation(m_state->baseAngle, key, m_state->parentFrame, resolved))
            return fail(error, QStringLiteral("旋转关键形的父方向退化或坐标超出范围"));
        const auto model = [](QPointF point) { return QPointF(point.x(), -point.y()); };
        CRotationFrameGeometry::Frame candidate{model(resolved.basis.origin), model(resolved.basis.xAxis),
                                               model(resolved.basis.yAxis), std::abs(double(resolved.signedScale))};
        if (!CRotationFrameGeometry::Validate(candidate, error)) return false;
        result = candidate;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("旋转坐标系求值失败，原输出保持不变"));
    }
}

bool CDeformerEditContext::SolveRotation(const CRotationFrameGeometry::Frame& desired,
                                        const CModelRotationKeyform& reference, CModelRotationKeyform& result,
                                        QString* error, ReflectionPolicy reflection) const
{
    if (error) error->clear();
    if (!m_state || !m_state->isRotation)
        return fail(error, QStringLiteral("当前上下文不是旋转变形器"));
    if (!reference.IsValid() || (reflection != ReflectionPolicy::Preserve && reflection != ReflectionPolicy::AllowChange))
        return fail(error, QStringLiteral("旋转求解参考关键形或反射策略无效"));
    if (!CRotationFrameGeometry::Validate(desired, error)) return false;
    try {
        QVector<float> local;
        if (!FromModel({float(desired.origin.x()), float(desired.origin.y())}, local, error)) return false;
        CRotationFrameGeometry::Frame referenceFrame;
        const auto exact = [](QPointF first, QPointF second) { return first.x() == second.x() && first.y() == second.y(); };
        if (ResolveRotation(reference, referenceFrame) && exact(referenceFrame.origin, desired.origin) &&
            exact(referenceFrame.xAxis, desired.xAxis) && exact(referenceFrame.yAxis, desired.yAxis) && referenceFrame.scale == desired.scale) {
            result = reference;
            return true;
        }
        CModelRotationKeyform candidate = reference;
        candidate.originX = local[0]; candidate.originY = local[1];
        CRotationNodeKeyform neutral(reference.opacity, 0, candidate.originX, candidate.originY, 1, false, false);
        CCoordinateFrame::ResolvedRotation parent;
        if (!CCoordinateFrame::ResolveRotation(m_state->baseAngle, neutral, m_state->parentFrame, parent) ||
            parent.parentSignedScale == 0)
            return fail(error, QStringLiteral("目标父坐标系的方向或尺度无法反解"));
        const double magnitude = desired.scale / std::abs(double(parent.parentSignedScale));
        if (!std::isfinite(magnitude) || magnitude > std::numeric_limits<float>::max() ||
            (magnitude != 0 && float(magnitude) == 0))
            return fail(error, QStringLiteral("目标旋转尺度超出可表示范围"));
        candidate.scale = float(magnitude) * (std::signbit(reference.scale) ? -1.0f : 1.0f);
        const QPointF x(desired.xAxis.x(), -desired.xAxis.y());
        const QPointF y(desired.yAxis.x(), -desired.yAxis.y());
        const bool negativeParity = x.x() * y.y() - x.y() * y.x() < 0;
        candidate.reflectY = negativeParity ? !candidate.reflectX : candidate.reflectX;
        if (reflection == ReflectionPolicy::Preserve && candidate.reflectY != reference.reflectY)
            return fail(error, QStringLiteral("目标方向需要改变反射标记，请先确认全部关联关键形的反射一致性"));
        const double totalScale = double(candidate.scale) * parent.parentSignedScale;
        if (desired.scale != 0 && float(totalScale) == 0)
            return fail(error, QStringLiteral("目标旋转总尺度低于可表示精度"));
        const double sign = (totalScale < 0 ? -1.0 : 1.0) * (candidate.reflectX ? -1.0 : 1.0);
        const double principal = qRadiansToDegrees(std::atan2(x.y() * sign, x.x() * sign)) -
                                 parent.parentAngle - m_state->baseAngle;
        const double angle = double(reference.angle) +
            std::remainder(principal - std::remainder(double(reference.angle), 360.0), 360.0);
        if (!std::isfinite(angle) || std::abs(angle) > std::numeric_limits<float>::max())
            return fail(error, QStringLiteral("目标旋转角度超出可表示范围"));
        candidate.angle = float(angle);
        CRotationFrameGeometry::Frame actual;
        if (!ResolveRotation(candidate, actual, error) || !CRotationFrameGeometry::Matches(actual, desired, error)) return false;
        result = candidate;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("旋转坐标系反解失败，原输出保持不变"));
    }
}
}
