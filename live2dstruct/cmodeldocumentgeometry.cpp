#include "cmodeldocument.h"
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>

namespace live2dstruct {
namespace {
bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

struct GeometryDirectory
{
    QHash<QString, int> meshes, deformers;
    explicit GeometryDirectory(const CModelDocument& document)
    {
        const auto add = [](auto& entries, const QString& id, int index) {
            const auto found = entries.find(id);
            if (found == entries.end()) entries.insert(id, index);
            else found.value() = -2;
        };
        for (int index = 0; index < document.Drawables().size(); ++index)
            add(meshes, document.Drawables()[index].id, index);
        for (int index = 0; index < document.Deformers().size(); ++index)
            add(deformers, document.Deformers()[index].id, index);
    }
};

bool capture(const CModelDocument& document, const GeometryDirectory& directory,
             CModelGeometryKeyform::Type type, const QString& id, int keyform,
             CModelGeometryKeyform& result, QString* error)
{
    using Frame = CModelGeometryKeyform;
    if (!document.IsLoaded() || id.isEmpty() || keyform < 0)
        return fail(error, QStringLiteral("几何对象或关键形无效"));
    Frame candidate{type, id, keyform, {}};
    if (type == Frame::ArtMesh) {
        const int index = directory.meshes.value(id, -1);
        if (index < 0) return fail(error, QStringLiteral("网格不存在或 ID 重复：%1").arg(id));
        const auto& mesh = document.Drawables()[index];
        if (keyform >= mesh.keyforms.size()) return fail(error, QStringLiteral("网格关键形不存在：%1").arg(id));
        const auto positions = document.DrawableKeyformPositions(id, keyform);
        if (mesh.vertexCount < 0 || qint64(positions.size()) != qint64(mesh.vertexCount) * 2)
            return fail(error, QStringLiteral("网格关键形顶点数量无效：%1").arg(id));
        candidate.value = positions;
    } else if (type == Frame::Warp || type == Frame::Rotation) {
        const int index = directory.deformers.value(id, -1);
        if (index < 0) return fail(error, QStringLiteral("变形器不存在或 ID 重复：%1").arg(id));
        const auto& deformer = document.Deformers()[index];
        if ((type == Frame::Warp) != (deformer.type == CModelDeformer::Warp))
            return fail(error, QStringLiteral("几何对象类型不匹配：%1").arg(id));
        if (type == Frame::Warp) {
            const auto forms = document.DeformerWarpKeyforms(id);
            if (keyform >= forms.size() || forms.size() != deformer.warpKeyforms.size())
                return fail(error, QStringLiteral("曲面变形器关键形不存在：%1").arg(id));
            const auto& form = forms[keyform];
            const qint64 points = (qint64(deformer.rows) + 1) * (qint64(deformer.columns) + 1);
            if (deformer.rows < 0 || deformer.columns < 0 || points <= 0 || points * 2 != form.positions.size())
                return fail(error, QStringLiteral("曲面变形器控制点数量无效：%1").arg(id));
            candidate.value = form.positions;
        } else {
            const auto forms = document.DeformerRotationKeyforms(id);
            if (keyform >= forms.size() || forms.size() != deformer.rotationKeyforms.size())
                return fail(error, QStringLiteral("旋转变形器关键形不存在：%1").arg(id));
            const auto& form = forms[keyform];
            candidate.value = CModelRotationTransform{form.angle, form.originX, form.originY, form.scale, form.reflectX, form.reflectY};
        }
    } else return fail(error, QStringLiteral("不支持的几何对象类型"));
    result = std::move(candidate);
    return true;
}

bool validValue(const CModelGeometryKeyform& frame, const CModelGeometryKeyform& current, QString* error)
{
    if (frame.value.valueless_by_exception() || frame.value.index() != current.value.index())
        return fail(error, QStringLiteral("几何数据类型不匹配：%1").arg(frame.id));
    if (const auto* positions = std::get_if<QVector<float>>(&frame.value)) {
        if (positions->size() != std::get<QVector<float>>(current.value).size())
            return fail(error, QStringLiteral("几何顶点数量不匹配：%1").arg(frame.id));
        for (float value : *positions)
            if (!std::isfinite(value)) return fail(error, QStringLiteral("几何坐标不是有限数值：%1").arg(frame.id));
    } else {
        const auto& value = std::get<CModelRotationTransform>(frame.value);
        for (float number : {value.angle, value.originX, value.originY, value.scale})
            if (!std::isfinite(number)) return fail(error, QStringLiteral("旋转几何数据不是有限数值：%1").arg(frame.id));
    }
    return true;
}

bool editable(const CModelDocument& document, const GeometryDirectory& directory,
              const CModelGeometryKeyform& frame, QString* error)
{
    if (document.IsLocked(frame.TypeName(), frame.id))
        return fail(error, QStringLiteral("几何对象已锁定：%1").arg(frame.id));
    const auto checkParts = [&](int index) {
        QSet<int> visited;
        while (index != -1) {
            if (index < 0 || index >= document.Parts().size() || visited.contains(index))
                return fail(error, QStringLiteral("几何对象的部件父链无效"));
            visited.insert(index);
            const auto& part = document.Parts()[index];
            if (document.IsLocked(QStringLiteral("part"), part.id))
                return fail(error, QStringLiteral("所属部件已锁定：%1").arg(part.id));
            index = part.parentIndex;
        }
        return true;
    };
    int parent = -1, part = -1;
    if (frame.type == CModelGeometryKeyform::ArtMesh) {
        const auto& mesh = document.Drawables()[directory.meshes.value(frame.id)];
        parent = mesh.parentDeformerIndex;
        part = mesh.parentPartIndex;
    } else {
        const auto& deformer = document.Deformers()[directory.deformers.value(frame.id)];
        parent = document.DeformerParentIndex(frame.id);
        part = deformer.parentPartIndex;
    }
    if (!checkParts(part)) return false;
    QSet<int> visited;
    while (parent != -1) {
        if (parent < 0 || parent >= document.Deformers().size() || visited.contains(parent))
            return fail(error, QStringLiteral("几何对象的变形器父链无效"));
        visited.insert(parent);
        const auto& deformer = document.Deformers()[parent];
        if (directory.deformers.value(deformer.id, -1) != parent)
            return fail(error, QStringLiteral("父变形器 ID 重复：%1").arg(deformer.id));
        const auto type = deformer.type == CModelDeformer::Warp ? QStringLiteral("warp") : QStringLiteral("rotation");
        if (document.IsLocked(type, deformer.id))
            return fail(error, QStringLiteral("父变形器已锁定：%1").arg(deformer.id));
        if (!checkParts(deformer.parentPartIndex)) return false;
        parent = document.DeformerParentIndex(deformer.id);
    }
    return true;
}

bool readStates(const CModelEdit& edit, CModelGeometryState& before, CModelGeometryState& after, QString* error)
{
    const auto stateType = QMetaType::fromType<CModelGeometryState>();
    if (edit.field != CModelEdit::GeometryKeyforms || edit.before.metaType() != stateType ||
        edit.after.metaType() != stateType || edit.coalesce || edit.parameterChange != CModelEdit::ParameterChange::None ||
        edit.freezeValue || edit.meshRequest || edit.bindingKeysRequest || edit.deformerRequest || !edit.meshRemaps.isEmpty())
        return fail(error, QStringLiteral("几何批量命令格式无效"));
    before = edit.before.value<CModelGeometryState>();
    after = edit.after.value<CModelGeometryState>();
    if (before.isEmpty() || before.size() != after.size())
        return fail(error, QStringLiteral("几何批量修改前后的对象数量不一致"));
    QMap<int, QMap<QString, QSet<int>>> seen;
    bool changed = false, driver = false;
    for (qsizetype index = 0; index < before.size(); ++index) {
        const auto& previous = before[index];
        const auto& next = after[index];
        if (!previous.SameEndpoint(next) || previous.id.isEmpty() || previous.keyform < 0 || previous.TypeName().isEmpty())
            return fail(error, QStringLiteral("几何批量修改前后的关键形不匹配"));
        auto& keys = seen[int(previous.type)][previous.id];
        if (keys.contains(previous.keyform)) return fail(error, QStringLiteral("几何批量修改含重复关键形：%1").arg(previous.id));
        keys.insert(previous.keyform);
        changed = changed || previous != next;
        driver = driver || (previous.id == edit.name && previous.TypeName() == edit.type && previous.keyform == edit.index);
    }
    if (!driver) return fail(error, QStringLiteral("预览关键形不属于本次几何修改"));
    if (!changed) return fail(error, QStringLiteral("几何批量修改没有变化"));
    return true;
}
}

bool CModelDocument::CanEditBlendShapeGeometry(const QString& meshId, QString* error) const
{
    const GeometryDirectory directory(*this);
    if (!IsLoaded() || directory.meshes.value(meshId, -1) < 0)
        return fail(error, QStringLiteral("融合目标网格不存在或 ID 重复：%1").arg(meshId));
    return editable(*this, directory, {CModelGeometryKeyform::ArtMesh, meshId, -1, {}}, error);
}

bool CModelDocument::CaptureGeometryKeyform(CModelGeometryKeyform::Type type, const QString& id, int keyform,
                                           CModelGeometryKeyform& result, QString* error) const
{
    try {
        CModelGeometryKeyform candidate;
        const GeometryDirectory directory(*this);
        if (!capture(*this, directory, type, id, keyform, candidate, error) || !validValue(candidate, candidate, error)) return false;
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法读取几何关键形，原结果保持不变")); }
}

bool CModelDocument::MakeGeometryEdit(const CModelGeometryState& before, const CModelGeometryState& after,
                                     const CModelGeometryKeyform& driver, CModelEdit& edit, QString* error) const
{
    try {
        CModelEdit command{driver.id, driver.TypeName(), CModelEdit::GeometryKeyforms,
            QVariant::fromValue(before), QVariant::fromValue(after), driver.keyform, false};
        CModelDocument candidate = *this;
        if (!candidate.ApplyGeometryEdit(command, true, error)) return false;
        edit = std::move(command);
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备几何批量修改，原结果保持不变")); }
}

bool CModelDocument::ApplyGeometryEdit(const CModelEdit& edit, bool forward, QString* error)
{
    CModelGeometryState before, after;
    if (!readStates(edit, before, after, error)) return false;
    const auto& expected = forward ? before : after;
    const auto& next = forward ? after : before;
    const GeometryDirectory directory(*this);
    for (qsizetype index = 0; index < expected.size(); ++index) {
        const auto& frame = expected[index];
        CModelGeometryKeyform current;
        if (!capture(*this, directory, frame.type, frame.id, frame.keyform, current, error) ||
            !validValue(frame, current, error) || !validValue(next[index], current, error)) return false;
        if (frame != current) return fail(error, QStringLiteral("几何关键形在操作期间已改变：%1").arg(frame.id));
        if (forward && frame != next[index] && !editable(*this, directory, frame, error)) return false;
    }
    QHash<QString, QVector<CModelWarpKeyform>> warps;
    QHash<QString, QVector<CModelRotationKeyform>> rotations;
    for (qsizetype index = 0; index < next.size(); ++index) {
        const auto& frame = next[index];
        if (frame == expected[index]) continue;
        if (frame.type == CModelGeometryKeyform::Warp) {
            auto found = warps.find(frame.id);
            if (found == warps.end()) found = warps.insert(frame.id, DeformerWarpKeyforms(frame.id));
            (*found)[frame.keyform].positions = std::get<QVector<float>>(frame.value);
        } else if (frame.type == CModelGeometryKeyform::Rotation) {
            auto found = rotations.find(frame.id);
            if (found == rotations.end()) found = rotations.insert(frame.id, DeformerRotationKeyforms(frame.id));
            auto& form = (*found)[frame.keyform];
            const auto& transform = std::get<CModelRotationTransform>(frame.value);
            form.angle = transform.angle; form.originX = transform.originX; form.originY = transform.originY;
            form.scale = transform.scale; form.reflectX = transform.reflectX; form.reflectY = transform.reflectY;
        }
    }
    for (qsizetype index = 0; index < next.size(); ++index) {
        const auto& frame = next[index];
        if (frame.type == CModelGeometryKeyform::ArtMesh && frame != expected[index] &&
            !SetDrawableKeyformPositions(frame.id, frame.keyform, std::get<QVector<float>>(frame.value)))
            return fail(error, QStringLiteral("无法写入网格关键形：%1").arg(frame.id));
    }
    for (auto it = warps.cbegin(); it != warps.cend(); ++it)
        if (!SetDeformerWarpKeyforms(it.key(), it.value())) return fail(error, QStringLiteral("无法写入曲面变形器关键形：%1").arg(it.key()));
    for (auto it = rotations.cbegin(); it != rotations.cend(); ++it)
        if (!SetDeformerRotationKeyforms(it.key(), it.value())) return fail(error, QStringLiteral("无法写入旋转变形器关键形：%1").arg(it.key()));
    return true;
}

bool CModelDocument::ValidateGeometryEditStorage(const CModelEdit& edit) const
{
    CModelGeometryState before, after, current;
    if (!readStates(edit, before, after, nullptr)) return false;
    const GeometryDirectory directory(*this);
    CModelDocument check = *this;
    QMap<int, QSet<QString>> seen;
    for (const auto& frame : before) {
        CModelGeometryKeyform value;
        if (!capture(*this, directory, frame.type, frame.id, frame.keyform, value, nullptr) || !validValue(value, value, nullptr)) return false;
        current.append(std::move(value));
        if (seen[int(frame.type)].contains(frame.id)) continue;
        seen[int(frame.type)].insert(frame.id);
        if (frame.type == CModelGeometryKeyform::ArtMesh) {
            const auto forms = m_drawableKeyformPositionOverrides.value(frame.id);
            for (int index = 0; index < forms.size(); ++index)
                if (!forms[index].isEmpty() && !check.SetDrawableKeyformPositions(frame.id, index, forms[index])) return false;
        } else if (frame.type == CModelGeometryKeyform::Warp) {
            if (m_deformerWarpKeyformOverrides.contains(frame.id) &&
                !check.SetDeformerWarpKeyforms(frame.id, m_deformerWarpKeyformOverrides.value(frame.id))) return false;
        } else if (m_deformerRotationKeyformOverrides.contains(frame.id) &&
            !check.SetDeformerRotationKeyforms(frame.id, m_deformerRotationKeyformOverrides.value(frame.id))) return false;
    }
    return current == before || current == after;
}
QByteArray CModelDocument::GeometryEditStorageFingerprint(const CModelEdit& edit) const
{
    QMap<int, QSet<QString>> owners;
    for (const auto& value : {edit.before, edit.after})
        for (const auto& frame : value.value<CModelGeometryState>()) owners[int(frame.type)].insert(frame.id);
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    const auto number = [&](float value) {
        quint32 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        stream << bits;
    };
    const auto positions = [&](const QVector<float>& values) {
        stream << qint64(values.size());
        for (float value : values) number(value);
    };
    for (auto type = owners.cbegin(); type != owners.cend(); ++type) {
        auto ids = type->values();
        std::sort(ids.begin(), ids.end());
        stream << qint32(type.key()) << qint64(ids.size());
        for (const auto& id : ids) {
            stream << id;
            if (type.key() == CModelGeometryKeyform::ArtMesh) {
                const auto found = m_drawableKeyformPositionOverrides.constFind(id);
                stream << (found != m_drawableKeyformPositionOverrides.cend());
                if (found == m_drawableKeyformPositionOverrides.cend()) continue;
                stream << qint64(found->size());
                for (const auto& form : *found) positions(form);
            } else if (type.key() == CModelGeometryKeyform::Warp) {
                const auto found = m_deformerWarpKeyformOverrides.constFind(id);
                stream << (found != m_deformerWarpKeyformOverrides.cend());
                if (found == m_deformerWarpKeyformOverrides.cend()) continue;
                stream << qint64(found->size());
                for (const auto& form : *found) { positions(form.positions); number(form.opacity); }
            } else if (type.key() == CModelGeometryKeyform::Rotation) {
                const auto found = m_deformerRotationKeyformOverrides.constFind(id);
                stream << (found != m_deformerRotationKeyformOverrides.cend());
                if (found == m_deformerRotationKeyformOverrides.cend()) continue;
                stream << qint64(found->size());
                for (const auto& form : *found) {
                    number(form.opacity); number(form.angle); number(form.originX); number(form.originY); number(form.scale);
                    stream << form.reflectX << form.reflectY;
                }
            }
        }
    }
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
}

}
