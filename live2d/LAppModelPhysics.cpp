/**
 * Copyright(c) Live2D Inc. All rights reserved.
 * Use of this source code is governed by the Live2D Open Software license: https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */
#include "LAppModel.hpp"
#include "std/cmodelresourcepath.h"
#include <QFile>
#include <QScopedValueRollback>
#include <exception>

using namespace Live2D::Cubism::Framework;

namespace {
bool hasReferences(const ParameterPhysicsReferences& references)
{
    return !references.sourcePath.isEmpty() || !references.sourceDigest.isEmpty() ||
        !references.suppressedInputs.isEmpty() || !references.suppressedOutputs.isEmpty();
}
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
}

bool LAppModel::PreparePhysicsCandidate(const CubismModel& previous, const CubismModel& replacement,
                                       const ParameterPhysicsReferences& references, bool draftActive,
                                       const QByteArray& bytes, bool stabilize, bool keepOptions,
                                       PhysicsOwner& result, QString* error) const
{
    if (bytes.size() > 16 * 1024 * 1024) return fail(error, QStringLiteral("物理草稿大小超出限制"));
    if (hasReferences(references) && (m_physicsSourceBytes.isEmpty() || references.sourcePath != m_physicsReferencePath ||
        references.sourceDigest != m_physicsSourceDigest))
        return fail(error, QStringLiteral("物理引用与原文件来源不匹配"));
    PhysicsOwner candidate;
    if (!m_physicsSourceBytes.isEmpty()) {
        PhysicsOwner source(CubismPhysics::Create(reinterpret_cast<const csmByte*>(m_physicsSourceBytes.constData()),
            csmSizeInt(m_physicsSourceBytes.size()), error));
        if (!source) return false;
        CubismPhysics::ParameterDefinitionState binding;
        if (!source->PrepareModelBinding(replacement, references, binding, error)) return false;
        if (!draftActive) candidate = std::move(source);
    }
    if (draftActive && !bytes.isEmpty()) {
        candidate.reset(CubismPhysics::Create(reinterpret_cast<const csmByte*>(bytes.constData()), csmSizeInt(bytes.size()), error));
        if (!candidate) return false;
    }
    if (candidate) {
        if (keepOptions && _physics) candidate->SetOptions(_physics->GetOptions());
        CubismPhysics::PreparedRuntimeState state;
        if (!candidate->PrepareRuntimeState(_physics, previous, replacement, draftActive ? ParameterPhysicsReferences{} : references,
            state, stabilize ? CubismPhysics::StatePolicy::Stabilize : CubismPhysics::StatePolicy::PreserveCompatible, error)) return false;
        if (!candidate->CommitRuntimeState(state)) return fail(error, QStringLiteral("物理模拟候选在提交前失效"));
    }
    result.swap(candidate);
    return true;
}

bool LAppModel::ApplyPhysicsDraft(const PhysicsDraft& draft, QString* error)
{
    if (error) error->clear();
    if (HasEditorPreview() || _snapshotMode)
        return fail(error, QStringLiteral("请先退出固定姿态编辑，再修改物理预览"));
    if (!_initialized || _updating || m_physicsDragActive || !_model || HasExportPreview())
        return fail(error, QStringLiteral("当前模型状态不允许更新物理预览"));
    if (draft.bytes.size() > 16 * 1024 * 1024) return fail(error, QStringLiteral("物理草稿大小超出限制"));
    try {
        const QScopedValueRollback<csmBool> updating(_updating, true);
        auto bytes = draft.bytes;
        PhysicsOwner candidate;
        if (!PreparePhysicsCandidate(*_model, *_model, m_parameterReferences.physics, true, bytes,
            draft.stabilize, false, candidate, error)) return false;
        auto* retired = _physics;
        _physics = candidate.release();
        candidate.reset(retired);
        m_physicsDraftBytes.swap(bytes);
        m_hasPhysicsDraft = true;
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("无法准备物理草稿，原预览保持不变"));
    }
}

bool LAppModel::ReloadPhysicsFromFile(const QString& path, QString* error)
{
    if (error) error->clear();
    if (HasExportPreview() || m_physicsDragActive || (_initialized && _updating) || !_model || !_modelSetting)
        return fail(error, QStringLiteral("当前模型状态不允许重载物理配置"));
    try {
        const auto home = QString::fromUtf8(_modelHomeDir.GetRawString());
        const auto resolved = CModelResourcePath::Resolve(home, path);
        if (resolved.isEmpty() || resolved != CModelResourcePath::Resolve(home, QString::fromUtf8(_modelSetting->GetPhysicsFileName())))
            return fail(error, QStringLiteral("物理文件与模型声明不一致"));
        QFile file(resolved);
        if (!file.open(QIODevice::ReadOnly)) return fail(error, file.errorString());
        if (file.size() > 16 * 1024 * 1024) return fail(error, QStringLiteral("物理文件大小超出限制"));
        QByteArray data = file.read(16 * 1024 * 1024 + 1);
        if (file.error() != QFileDevice::NoError || data.isEmpty() || data.size() > 16 * 1024 * 1024)
            return fail(error, QStringLiteral("无法完整读取物理文件"));
        PhysicsOwner candidate(CubismPhysics::Create(reinterpret_cast<const csmByte*>(data.constData()), csmSizeInt(data.size()), error));
        if (!candidate) return false;
        auto pathKey = ReferenceSourcePath(resolved);
        auto digest = candidate->SourceDigest();
        const auto& references = m_parameterReferences.physics;
        if (hasReferences(references) && (references.sourcePath != pathKey || references.sourceDigest != digest))
            return fail(error, QStringLiteral("物理文件与当前参数引用历史不一致"));
        CubismPhysics::PreparedRuntimeState state;
        if (!candidate->PrepareRuntimeState(_physics, *_model, *_model, references, state,
            CubismPhysics::StatePolicy::PreserveCompatible, error)) return false;
        if (!candidate->CommitRuntimeState(state)) return fail(error, QStringLiteral("物理模拟候选在提交前失效"));
        auto* retired = _physics;
        _physics = candidate.release();
        candidate.reset(retired);
        m_physicsSourceBytes.swap(data);
        m_physicsSourceDigest.swap(digest);
        m_physicsReferencePath.swap(pathKey);
        m_hasPhysicsDraft = false;
        m_physicsDraftBytes.clear();
        return true;
    } catch (const std::exception&) {
        return fail(error, QStringLiteral("物理重载失败，原预览保持不变"));
    }
}
