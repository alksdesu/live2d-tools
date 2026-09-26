#include "cparameterreferenceplan.h"
#include <QDir>
#include <exception>
#include <utility>

namespace {
using Edits = live2dstruct::CParameterResourceEdits;

bool fail(QString* error, const QString& reason)
{
    if (error) *error = reason;
    return false;
}

bool isSubset(const QSet<QString>& values, const QSet<QString>& available)
{
    for (const auto& value : values) if (!available.contains(value)) return false;
    return true;
}

bool checkResource(const QString& path, const QByteArray& digest, Edits::Kind kind,
                   const Edits::Prepared& resources, QString* error)
{
    const auto source = resources.resources.constFind(path);
    if (path.isEmpty() || path.contains(QChar::Null) || QDir::isAbsolutePath(path) || QDir::cleanPath(path) != path ||
        source == resources.resources.cend() || source->kind != kind)
        return fail(error, QStringLiteral("运行中的引用资源不在模型目录声明中：%1").arg(path));
    if (digest.size() != 32 || digest != source->sourceDigest)
        return fail(error, QStringLiteral("运行中的引用资源与磁盘计划不是同一版本：%1").arg(path));
    return true;
}
}

bool CParameterReferencePlan::Prepare(const QString& modelJsonPath, const Edits::State& current,
                                     const QSet<QString>& deletedIds, const ParameterReferenceCatalog& catalog,
                                     Prepared& prepared, QString* error)
{
    if (error) error->clear();
    try {
        Prepared candidate;
        if (!Edits::Prepare(modelJsonPath, current, deletedIds, candidate.resources, error)) return false;
        if (catalog.modelSettingsDigest.size() != 32 || catalog.modelSettingsDigest != candidate.resources.model.sourceDigest)
            return fail(error, QStringLiteral("运行中的模型设置与磁盘计划不是同一版本"));
        for (auto it = catalog.motions.cbegin(); it != catalog.motions.cend(); ++it)
            if (!checkResource(it.key(), it.value(), Edits::Kind::Motion, candidate.resources, error)) return false;
        for (auto it = catalog.expressions.cbegin(); it != catalog.expressions.cend(); ++it)
            if (!checkResource(it.key(), it.value(), Edits::Kind::Expression, candidate.resources, error)) return false;
        if ((!catalog.physicsPath.isEmpty() || !catalog.physicsDigest.isEmpty()) &&
            !checkResource(catalog.physicsPath, catalog.physicsDigest, Edits::Kind::Physics, candidate.resources, error)) return false;
        if (!isSubset(current.breath, catalog.breath) || !isSubset(current.mouseFollow, catalog.mouseFollow))
            return fail(error, QStringLiteral("已有的自动驱动移除状态与运行时能力不匹配"));
        if (!isSubset(candidate.resources.model.eyeBlink, catalog.eyeBlink) || !isSubset(candidate.resources.model.lipSync, catalog.lipSync))
            return fail(error, QStringLiteral("模型分组移除状态与运行时眨眼或口型能力不匹配"));
        auto& state = candidate.resources.state;
        state.breath = current.breath | (deletedIds & catalog.breath);
        state.mouseFollow = current.mouseFollow | (deletedIds & catalog.mouseFollow);
        const auto describe = [&](const QSet<QString>& added, const QString& label) {
            auto ids = added.values();
            ids.sort();
            for (const auto& id : ids) candidate.resources.removals.append(label + QStringLiteral(" · ") + id);
        };
        describe(state.breath - current.breath, QStringLiteral("呼吸驱动"));
        describe(state.mouseFollow - current.mouseFollow, QStringLiteral("鼠标跟随"));
        candidate.runtime.modelSettingsDigest = catalog.modelSettingsDigest;
        candidate.runtime.eyeBlink = candidate.resources.model.eyeBlink;
        candidate.runtime.lipSync = candidate.resources.model.lipSync;
        candidate.runtime.breath = state.breath;
        candidate.runtime.mouseFollow = state.mouseFollow;
        for (auto it = state.resources.cbegin(); it != state.resources.cend(); ++it) {
            const auto source = candidate.resources.resources.constFind(it.key());
            if (source == candidate.resources.resources.cend() || source->kind != it->kind)
                return fail(error, QStringLiteral("移除的参数引用缺少原始资源计划：%1").arg(it.key()));
            switch (it->kind) {
            case Edits::Kind::Motion:
                candidate.runtime.motions.insert(it.key(), {source->sourceDigest, source->parameterRows});
                break;
            case Edits::Kind::Expression:
                candidate.runtime.expressions.insert(it.key(), {source->sourceDigest, source->parameterRows});
                break;
            case Edits::Kind::Physics:
                candidate.runtime.physics = {it.key(), source->sourceDigest, source->inputRows, source->outputRows};
                break;
            default: return fail(error, QStringLiteral("参数引用资源类型不受支持"));
            }
        }
        if (state.modelSuppressedIds.isEmpty() && state.resources.isEmpty() && state.breath.isEmpty() && state.mouseFollow.isEmpty()) state = {};
        prepared = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法准备模型参数引用事务"));
    }
}
