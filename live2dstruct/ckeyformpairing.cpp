#include "ckeyformeditcontroller.h"
#include "cgeometrypairplanner.h"
#include <cstring>
#include <exception>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool sameFloat(float first, float second) { return std::memcmp(&first, &second, sizeof(float)) == 0; }
}

bool CKeyformEditController::PreparePairedEdit(const CModelDocument& document, const CModelEdit& edit,
    const QMap<QString, float>& pose, int driverKeyform, CModelEdit& result, QString* error) try
{
    using Geometry = CModelGeometryKeyform;
    Geometry::Type type;
    if (edit.field == CModelEdit::KeyformPositions) type = Geometry::ArtMesh;
    else if (edit.field == CModelEdit::WarpKeyforms) type = Geometry::Warp;
    else if (edit.field == CModelEdit::RotationKeyforms) type = Geometry::Rotation;
    else { result = edit; return true; }
    const CGeometryObjectPairs::Pair* pair = nullptr;
    bool forward = true;
    for (const auto& candidate : document.GeometryObjectPairs().pairs) {
        if (!candidate.enabled) continue;
        if (candidate.first.object.type == type && candidate.first.object.id == edit.name) { pair = &candidate; break; }
        if (candidate.second.object.type == type && candidate.second.object.id == edit.name) { pair = &candidate; forward = false; break; }
    }
    if (!pair) { result = edit; return true; }
    CModelDocument sourceAfter = document;
    if (!sourceAfter.ExecuteEdit(edit)) return fail(error, QStringLiteral("源对象修改无效，配对对象保持不变"));
    int count = 1;
    bool opacityChanged = false;
    if (type == Geometry::Warp) {
        const auto before = document.DeformerWarpKeyforms(edit.name), after = sourceAfter.DeformerWarpKeyforms(edit.name);
        if (before.size() != after.size()) return fail(error, QStringLiteral("同步编辑不能改变关键形数量"));
        count = before.size();
        for (int index = 0; index < count; ++index) opacityChanged = opacityChanged || !sameFloat(before[index].opacity, after[index].opacity);
    } else if (type == Geometry::Rotation) {
        const auto before = document.DeformerRotationKeyforms(edit.name), after = sourceAfter.DeformerRotationKeyforms(edit.name);
        if (before.size() != after.size()) return fail(error, QStringLiteral("同步编辑不能改变关键形数量"));
        count = before.size();
        for (int index = 0; index < count; ++index) opacityChanged = opacityChanged || !sameFloat(before[index].opacity, after[index].opacity);
    }
    CModelGeometryState changedBefore, changedAfter;
    for (int index = 0; index < count; ++index) {
        const int keyform = type == Geometry::ArtMesh ? edit.index : index;
        Geometry before, after;
        if (!document.CaptureGeometryKeyform(type, edit.name, keyform, before, error) ||
            !sourceAfter.CaptureGeometryKeyform(type, edit.name, keyform, after, error)) return false;
        if (before != after) { changedBefore.append(std::move(before)); changedAfter.append(std::move(after)); }
    }
    if (changedBefore.isEmpty()) { result = edit; return true; }
    if (opacityChanged) return fail(error, QStringLiteral("请分别修改透明度和几何形状，几何配对不改变透明度"));
    CModelGeometryState before, after;
    for (int index = 0; index < changedBefore.size(); ++index) {
        CGeometryPairPlanner::Result prepared;
        if (!CGeometryPairPlanner::PrepareSynchronized(document, *pair, forward, changedBefore[index], changedAfter[index], pose, prepared, error)) return false;
        if (!prepared.changed) continue;
        const auto source = prepared.edit.before.value<CModelGeometryState>();
        const auto target = prepared.edit.after.value<CModelGeometryState>();
        if (source.size() != target.size()) return fail(error, QStringLiteral("配对几何提交状态不完整"));
        for (int point = 0; point < source.size(); ++point) {
            bool duplicate = false;
            for (int existing = 0; existing < before.size(); ++existing) if (before[existing].SameEndpoint(source[point])) {
                if (before[existing] != source[point] || after[existing] != target[point])
                    return fail(error, QStringLiteral("多个关键形修改了同一个配对目标"));
                duplicate = true; break;
            }
            if (!duplicate) { before.append(source[point]); after.append(target[point]); }
        }
    }
    Geometry driver;
    if (!document.CaptureGeometryKeyform(type, edit.name, driverKeyform, driver, error)) return false;
    bool hasDriver = false;
    for (const auto& value : before) hasDriver = hasDriver || value.SameEndpoint(driver);
    if (!hasDriver) { before.append(driver); after.append(driver); }
    CModelEdit command;
    if (!document.MakeGeometryEdit(before, after, driver, command, error)) return false;
    result = std::move(command);
    return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备配对编辑，原状态保持不变")); }

bool CKeyformEditController::PreparePairSynchronization(const CModelDocument& document, const QString& id,
    const QString& type, int keyform, const QMap<QString, float>& pose,
    std::optional<CGeometryPairPlanner::SynchronizationContext>& result, QString* error) try
{
    for (const auto& pair : document.GeometryObjectPairs().pairs) {
        if (!pair.enabled) continue;
        const bool first = pair.first.object.id == id && CGeometryObjectPairs::TypeName(pair.first.object.type) == type;
        const bool second = pair.second.object.id == id && CGeometryObjectPairs::TypeName(pair.second.object.type) == type;
        if (!first && !second) continue;
        CGeometryPairPlanner::SynchronizationContext context;
        if (!CGeometryPairPlanner::PrepareSynchronization(document, pair, first, keyform, pose, context, error)) return false;
        result = std::move(context); return true;
    }
    result.reset(); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备配对拖拽，原状态保持不变")); }

bool CKeyformEditController::PreparePairedDrag(const CModelDocument& document, const CModelEdit& edit, int keyform,
    const CGeometryPairPlanner::SynchronizationContext& context, CModelEdit& result, QString* error) try
{
    using Geometry = CModelGeometryKeyform;
    Geometry after;
    if (edit.field == CModelEdit::KeyformPositions) {
        if (!document.CaptureGeometryKeyform(Geometry::ArtMesh, edit.name, keyform, after, error) ||
            edit.after.metaType() != QMetaType::fromType<QVector<float>>()) return false;
        after.value = edit.after.value<QVector<float>>();
    } else if (edit.field == CModelEdit::WarpKeyforms) {
        if (!document.CaptureGeometryKeyform(Geometry::Warp, edit.name, keyform, after, error) ||
            edit.after.metaType() != QMetaType::fromType<QVector<CModelWarpKeyform>>()) return false;
        const auto forms = edit.after.value<QVector<CModelWarpKeyform>>();
        if (keyform < 0 || keyform >= forms.size()) return fail(error, QStringLiteral("配对拖拽关键形不存在"));
        after.value = forms[keyform].positions;
    } else if (edit.field == CModelEdit::RotationKeyforms) {
        if (!document.CaptureGeometryKeyform(Geometry::Rotation, edit.name, keyform, after, error) ||
            edit.after.metaType() != QMetaType::fromType<QVector<CModelRotationKeyform>>()) return false;
        const auto forms = edit.after.value<QVector<CModelRotationKeyform>>();
        if (keyform < 0 || keyform >= forms.size()) return fail(error, QStringLiteral("配对拖拽关键形不存在"));
        const auto& form = forms[keyform];
        after.value = CModelRotationTransform{form.angle, form.originX, form.originY, form.scale, form.reflectX, form.reflectY};
    } else return fail(error, QStringLiteral("当前操作不属于配对几何拖拽"));
    CGeometryPairPlanner::Result prepared;
    if (!CGeometryPairPlanner::Synchronize(context, after, prepared, error)) return false;
    if (!prepared.changed) return fail(error, QStringLiteral("配对拖拽没有可提交的几何变化"));
    result = std::move(prepared.edit); return true;
}
catch (const std::exception&) { return fail(error, QStringLiteral("无法准备配对拖拽结果，原状态保持不变")); }

}
