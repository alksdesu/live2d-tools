#include "LAppModel.hpp"
#include "framework/Model/CubismParameterReferenceMask.hpp"
#include "framework/Motion/CubismMotion.hpp"
#include "framework/Motion/CubismMotionQueueEntry.hpp"
#include "framework/Effect/CubismBreath.hpp"
#include "framework/Effect/CubismEyeBlink.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <memory>
#include <type_traits>

using namespace Live2D::Cubism::Framework;

namespace {
bool fail(QString* error, const QString& reason)
{
    if (error) *error = reason;
    return false;
}

QString parameterId(CubismIdHandle id)
{
    return id ? QString::fromUtf8(id->GetString().GetRawString()) : QString();
}

struct PreparedReferences
{
    ParameterReferenceState state;
    QVector<QPair<CubismMotion*, CubismMotion::ReferenceState>> motions;
    QVector<QPair<CubismExpressionMotion*, QVector<quint8>>> expressions;
    QVector<quint8> eyes, breath, lips, mouse;
    QSet<CubismIdHandle> expressionIds;
};

bool verifyResource(const QString& path, const ParameterResourceReferences& references, bool motion, qint64& bytesRead, QString* error)
{
    QFile file(path);
    if (references.sourceDigest.size() != 32 || !QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly) ||
        file.size() > 16 * 1024 * 1024 || file.size() > 64 * 1024 * 1024 - bytesRead)
        return fail(error, QStringLiteral("无法核对参数引用资源：%1").arg(path));
    const auto bytes = file.read(16 * 1024 * 1024 + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > 16 * 1024 * 1024 ||
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != references.sourceDigest)
        return fail(error, QStringLiteral("参数引用资源内容已改变：%1").arg(path));
    bytesRead += bytes.size();
    QJsonParseError parse;
    const auto json = QJsonDocument::fromJson(bytes, &parse);
    const QString key = motion ? QStringLiteral("Curves") : QStringLiteral("Parameters");
    if (parse.error != QJsonParseError::NoError || !json.isObject() || !json.object().value(key).isArray())
        return fail(error, QStringLiteral("参数引用资源格式无效：%1").arg(path));
    const auto rows = json.object().value(key).toArray();
    QVector<quint8> checked;
    return PrepareParameterReferenceMask(references.sourceDigest, references, rows.size(), [&](int index) {
        const auto row = rows[index].toObject();
        if (motion && row.value(QStringLiteral("Target")).toString() != QStringLiteral("Parameter")) return QString();
        return row.value(QStringLiteral("Id")).toString();
    }, checked, error);
}
}

QString LAppModel::ReferenceSourcePath(const QString& path) const
{
    if (QDir::isAbsolutePath(path))
        return QDir::cleanPath(QDir(QString::fromUtf8(_modelHomeDir.GetRawString())).relativeFilePath(path));
    return QDir::cleanPath(path);
}

bool LAppModel::ConfigureMotionReferences(CubismMotion* motion, const QString& path, QString* error)
{
    if (!motion) return false;
    const auto key = ReferenceSourcePath(path);
    CubismMotion::ReferenceState prepared;
    if (!motion->PrepareParameterReferences(m_parameterReferences.motions.value(key), m_parameterReferences.eyeBlink,
                                           m_parameterReferences.lipSync, prepared, error)) return false;
    motion->SetReferenceSourcePath(key);
    motion->CommitParameterReferences(prepared);
    return true;
}

bool LAppModel::ConfigureExpressionReferences(CubismExpressionMotion* expression, const QString& path, QString* error)
{
    if (!expression) return false;
    const auto key = ReferenceSourcePath(path);
    QVector<quint8> prepared;
    if (!expression->PrepareParameterReferences(m_parameterReferences.expressions.value(key), prepared, error)) return false;
    expression->SetReferenceSourcePath(key);
    expression->CommitParameterReferences(prepared);
    return true;
}

bool LAppModel::PrepareParameterReferences(const ParameterReferenceState& state, std::function<void()>& commit, QString* error)
{
    if (state.motions.size() + state.expressions.size() > 2048)
        return fail(error, QStringLiteral("参数引用资源数量超出限制"));
    const bool used = !state.motions.isEmpty() || !state.expressions.isEmpty() || !state.physics.sourcePath.isEmpty() ||
        !state.physics.sourceDigest.isEmpty() || !state.physics.suppressedInputs.isEmpty() || !state.physics.suppressedOutputs.isEmpty() ||
        !state.eyeBlink.isEmpty() || !state.lipSync.isEmpty() || !state.breath.isEmpty() || !state.mouseFollow.isEmpty();
    if ((used || !state.modelSettingsDigest.isEmpty()) &&
        (state.modelSettingsDigest.size() != 32 || state.modelSettingsDigest != m_modelSettingsDigest))
        return fail(error, QStringLiteral("参数引用与模型设置的加载快照不匹配"));
    auto prepared = std::make_shared<PreparedReferences>();
    prepared->state = state;
    const QDir directory(QString::fromUtf8(_modelHomeDir.GetRawString()));
    QSet<QString> motionPaths, expressionPaths;
    if (_modelSetting) {
        for (int group = 0; group < _modelSetting->GetMotionGroupCount(); ++group) {
            const auto name = _modelSetting->GetMotionGroupName(group);
            for (int index = 0; index < _modelSetting->GetMotionCount(name); ++index)
                motionPaths.insert(ReferenceSourcePath(QString::fromUtf8(_modelSetting->GetMotionFileName(name, index))));
        }
        for (int index = 0; index < _modelSetting->GetExpressionCount(); ++index)
            expressionPaths.insert(ReferenceSourcePath(QString::fromUtf8(_modelSetting->GetExpressionFileName(index))));
    }
    QSet<CubismMotion*> motions;
    for (auto it = _motions.Begin(); it != _motions.End(); ++it)
        if (auto* motion = dynamic_cast<CubismMotion*>(it->Second)) motions.insert(motion);
    if (_motionManager) {
        const auto* entries = _motionManager->GetCubismMotionQueueEntries();
        for (int index = 0; index < entries->GetSize(); ++index)
            if ((*entries)[index]) if (auto* motion = dynamic_cast<CubismMotion*>((*entries)[index]->GetCubismMotion())) motions.insert(motion);
    }
    for (auto* motion : motions) {
        motionPaths.insert(motion->ReferenceSourcePath());
        CubismMotion::ReferenceState mask;
        if (!motion->PrepareParameterReferences(state.motions.value(motion->ReferenceSourcePath()), state.eyeBlink, state.lipSync, mask, error)) return false;
        prepared->motions.append(qMakePair(motion, std::move(mask)));
    }
    QSet<CubismExpressionMotion*> expressions;
    for (auto it = _expressions.Begin(); it != _expressions.End(); ++it)
        if (auto* expression = dynamic_cast<CubismExpressionMotion*>(it->Second)) expressions.insert(expression);
    if (_expressionManager) {
        const auto* entries = _expressionManager->GetCubismMotionQueueEntries();
        for (int index = 0; index < entries->GetSize(); ++index)
            if ((*entries)[index]) if (auto* expression = dynamic_cast<CubismExpressionMotion*>((*entries)[index]->GetCubismMotion())) expressions.insert(expression);
    }
    for (auto* expression : expressions) {
        expressionPaths.insert(expression->ReferenceSourcePath());
        QVector<quint8> mask;
        if (!expression->PrepareParameterReferences(state.expressions.value(expression->ReferenceSourcePath()), mask, error)) return false;
        prepared->expressions.append(qMakePair(expression, std::move(mask)));
    }
    qint64 bytesRead = 0;
    for (auto it = state.motions.cbegin(); it != state.motions.cend(); ++it)
        if (it.key().isEmpty() || ReferenceSourcePath(it.key()) != it.key() || !motionPaths.contains(it.key()) ||
            !verifyResource(directory.filePath(it.key()), it.value(), true, bytesRead, error))
            return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("动作引用资源不存在：%1").arg(it.key()));
    for (auto it = state.expressions.cbegin(); it != state.expressions.cend(); ++it) {
        if (it.key().isEmpty() || ReferenceSourcePath(it.key()) != it.key() || !expressionPaths.contains(it.key()) ||
            !verifyResource(directory.filePath(it.key()), it.value(), false, bytesRead, error))
            return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("表情引用资源不存在：%1").arg(it.key()));
        for (const auto& id : it->suppressedParameters) prepared->expressionIds.insert(CubismFramework::GetIdManager()->GetId(id.toUtf8().constData()));
    }
    if (!_physics && (!state.physics.sourcePath.isEmpty() || !state.physics.sourceDigest.isEmpty() ||
                     !state.physics.suppressedInputs.isEmpty() || !state.physics.suppressedOutputs.isEmpty()))
        return fail(error, QStringLiteral("物理引用资源尚未加载"));
    if (_physics && (!state.physics.sourcePath.isEmpty() || !state.physics.sourceDigest.isEmpty() ||
                     !state.physics.suppressedInputs.isEmpty() || !state.physics.suppressedOutputs.isEmpty())) {
        QFile file(directory.filePath(state.physics.sourcePath));
        if (state.physics.sourcePath != m_physicsReferencePath || state.physics.sourceDigest != _physics->SourceDigest() ||
            !QFileInfo(file).isFile() || !file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024 ||
            file.size() > 64 * 1024 * 1024 - bytesRead)
            return fail(error, QStringLiteral("物理引用与加载快照不匹配"));
        const auto bytes = file.read(16 * 1024 * 1024 + 1);
        if (file.error() != QFileDevice::NoError || bytes.size() > 16 * 1024 * 1024 ||
            QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != state.physics.sourceDigest)
            return fail(error, QStringLiteral("物理引用资源内容已改变"));
    }
    if ((_eyeBlink && !_eyeBlink->PrepareParameterReferences(state.eyeBlink, prepared->eyes, error)) ||
        (!_eyeBlink && !state.eyeBlink.isEmpty()) ||
        (_breath && !_breath->PrepareParameterReferences(state.breath, prepared->breath, error)) ||
        (!_breath && !state.breath.isEmpty())) return fail(error, error && !error->isEmpty() ? *error : QStringLiteral("自动参数驱动尚未加载"));
    if (!PrepareParameterGroupMask(state.lipSync, _lipSyncIds.GetSize(), [&](int index) { return parameterId(_lipSyncIds[index]); }, prepared->lips, error)) return false;
    const CubismIdHandle mouse[] = {_idParamAngleX, _idParamAngleY, _idParamAngleZ, _idParamBodyAngleX, _idParamEyeBallX, _idParamEyeBallY};
    if (!PrepareParameterGroupMask(state.mouseFollow, 6, [&](int index) { return parameterId(mouse[index]); }, prepared->mouse, error)) return false;
    commit = [this, prepared]() noexcept {
        for (auto& entry : prepared->motions) entry.first->CommitParameterReferences(entry.second);
        for (auto& entry : prepared->expressions) entry.first->CommitParameterReferences(entry.second);
        if (_expressionManager) _expressionManager->CommitParameterReferenceSuppression(prepared->expressionIds);
        if (_eyeBlink) _eyeBlink->CommitParameterReferences(prepared->eyes);
        if (_breath) _breath->CommitParameterReferences(prepared->breath);
        m_suppressedLipSync.swap(prepared->lips);
        m_suppressedMouseFollow.swap(prepared->mouse);
        static_assert(std::is_nothrow_swappable_v<ParameterReferenceState>);
        std::swap(m_parameterReferences, prepared->state);
    };
    return true;
}

bool LAppModel::CaptureParameterReferenceCatalog(ParameterReferenceCatalog& result, QString* error) const
{
    if (error) error->clear();
    if (!_initialized || _updating || !_model || m_modelSettingsDigest.size() != 32)
        return fail(error, QStringLiteral("当前模型状态不能读取参数引用目录"));
    try {
        ParameterReferenceCatalog candidate;
        candidate.modelSettingsDigest = m_modelSettingsDigest;
        const auto add = [&](const QString& path, const QByteArray& digest, QMap<QString, QByteArray>& values) {
            if (path.isEmpty() || digest.size() != 32 || (values.contains(path) && values.value(path) != digest))
                return fail(error, QStringLiteral("运行中的引用资源包含不同版本或缺少来源：%1").arg(path));
            values.insert(path, digest);
            return true;
        };
        const auto motion = [&](ACubismMotion* value) {
            if (!value) return true;
            const auto* item = dynamic_cast<const CubismMotion*>(value);
            return item && add(item->ReferenceSourcePath(), item->SourceDigest(), candidate.motions);
        };
        const auto expression = [&](ACubismMotion* value) {
            if (!value) return true;
            const auto* item = dynamic_cast<const CubismExpressionMotion*>(value);
            return item && add(item->ReferenceSourcePath(), item->SourceDigest(), candidate.expressions);
        };
        for (auto it = _motions.Begin(); it != _motions.End(); ++it) if (!motion(it->Second)) return false;
        for (auto it = _expressions.Begin(); it != _expressions.End(); ++it) if (!expression(it->Second)) return false;
        if (_motionManager) {
            const auto* entries = _motionManager->GetCubismMotionQueueEntries();
            for (int index = 0; index < entries->GetSize(); ++index)
                if ((*entries)[index] && !motion((*entries)[index]->GetCubismMotion())) return false;
        }
        if (_expressionManager) {
            const auto* entries = _expressionManager->GetCubismMotionQueueEntries();
            for (int index = 0; index < entries->GetSize(); ++index)
                if ((*entries)[index] && !expression((*entries)[index]->GetCubismMotion())) return false;
        }
        if (_physics) {
            candidate.physicsPath = m_physicsReferencePath;
            candidate.physicsDigest = _physics->SourceDigest();
            if (candidate.physicsPath.isEmpty() || candidate.physicsDigest.size() != 32)
                return fail(error, QStringLiteral("运行中的物理资源缺少来源"));
        }
        for (int index = 0; index < _eyeBlinkIds.GetSize(); ++index) candidate.eyeBlink.insert(parameterId(_eyeBlinkIds[index]));
        for (int index = 0; index < _lipSyncIds.GetSize(); ++index) candidate.lipSync.insert(parameterId(_lipSyncIds[index]));
        if (_breath) {
            const auto& parameters = _breath->GetParameters();
            for (int index = 0; index < parameters.GetSize(); ++index) candidate.breath.insert(parameterId(parameters[index].ParameterId));
        }
        for (const auto id : {_idParamAngleX, _idParamAngleY, _idParamAngleZ, _idParamBodyAngleX, _idParamEyeBallX, _idParamEyeBallY})
            if (id) candidate.mouseFollow.insert(parameterId(id));
        candidate.eyeBlink.remove(QString()); candidate.lipSync.remove(QString()); candidate.breath.remove(QString());
        result = std::move(candidate);
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法读取参数引用目录"));
    }
}
