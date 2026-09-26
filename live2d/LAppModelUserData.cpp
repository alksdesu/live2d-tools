/**
 * Copyright(c) Live2D Inc. All rights reserved.
 * Use of this source code is governed by the Live2D Open Software license: https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */
#include "LAppModel.hpp"
#include "framework/CubismModelSettingJson.hpp"
#include "framework/Model/CubismModelUserData.hpp"
#include "live2dstruct/cmodeluserdata.h"
#include "std/cmodeljson.h"
#include "std/cmodelresourcepath.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <exception>
#include <limits>
#include <memory>
#include <utility>

using namespace Live2D::Cubism::Framework;

bool LAppModel::ReloadUserDataFromFile(const QString& path, const QByteArray& expectedSourceDigest, QString* error)
{
    if (error) error->clear();
    const auto fail = [&](const QString& message) { if (error) *error = message; return false; };
    if (HasExportPreview() || (_initialized && _updating) || !_model || !_modelSetting || m_modelSettingsPath.isEmpty())
        return fail(QStringLiteral("当前模型状态不允许更新网格标签"));
    if (!expectedSourceDigest.isEmpty() && expectedSourceDigest.size() != 32)
        return fail(QStringLiteral("网格标签的来源摘要无效"));
    try {
        const auto home = QString::fromUtf8(_modelHomeDir.GetRawString());
        const auto resolved = CModelResourcePath::Resolve(home, path);
        if (resolved.isEmpty()) return fail(QStringLiteral("网格标签路径为空或无效"));
        live2dstruct::CModelUserData document;
        QByteArray userDataDigest;
        if (!document.Load(resolved, error, &userDataDigest)) return false;
        if (!expectedSourceDigest.isEmpty() && userDataDigest != expectedSourceDigest)
            return fail(QStringLiteral("网格标签与刚保存的内容不一致：%1").arg(resolved));
        QSet<QString> drawableIds;
        for (int index = 0; index < _model->GetDrawableCount(); ++index)
            drawableIds.insert(QString::fromUtf8(_model->GetDrawableId(index)->GetString().GetRawString()));
        for (const auto& entry : document.Entries())
            if (!drawableIds.contains(entry.id)) return fail(QStringLiteral("网格标签引用不存在的 ArtMesh：%1").arg(entry.id));

        QFile file(m_modelSettingsPath);
        constexpr qint64 maximumSettingsBytes = 16 * 1024 * 1024;
        if (!file.open(QIODevice::ReadOnly)) return fail(QStringLiteral("无法读取模型设置：%1").arg(file.errorString()));
        if (file.size() > maximumSettingsBytes) return fail(QStringLiteral("模型设置大小超出限制"));
        QByteArray settingsBytes = file.read(maximumSettingsBytes + 1);
        if (file.error() != QFileDevice::NoError || settingsBytes.size() > maximumSettingsBytes)
            return fail(QStringLiteral("无法完整读取模型设置"));
        if (!CModelJson::ValidateRuntimeSettings(settingsBytes, error)) return false;
        const bool changed = settingsBytes != m_modelSettingsBytes;
        if (changed && !CModelJson::IsUserDataRegistration(m_modelSettingsBytes, settingsBytes))
            return fail(QStringLiteral("模型配置包含网格标签登记以外的变化，请重新打开模型"));
        const auto files = QJsonDocument::fromJson(settingsBytes).object().value(QStringLiteral("FileReferences")).toObject();
        if (CModelResourcePath::Resolve(home, files.value(QStringLiteral("UserData")).toString()) != resolved)
            return fail(QStringLiteral("网格标签路径与当前模型声明不一致"));
        if (m_modelSettingsDigest.size() != 32 ||
            QCryptographicHash::hash(m_modelSettingsBytes, QCryptographicHash::Sha256) != m_modelSettingsDigest ||
            (!m_parameterReferences.modelSettingsDigest.isEmpty() && m_parameterReferences.modelSettingsDigest != m_modelSettingsDigest))
            return fail(QStringLiteral("当前模型的参数引用来源不一致"));
        QByteArray settingsDigest = QCryptographicHash::hash(settingsBytes, QCryptographicHash::Sha256);
        QByteArray referenceDigest = m_parameterReferences.modelSettingsDigest.isEmpty() ? QByteArray() : settingsDigest;

        const auto bytes = document.ToJson();
        if (bytes.size() > std::numeric_limits<csmSizeInt>::max()) return fail(QStringLiteral("网格标签大小超出限制"));
        auto* replacement = CubismModelUserData::Create(reinterpret_cast<const csmByte*>(bytes.constData()), csmSizeInt(bytes.size()));
        const auto cleanup = qScopeGuard([&] { CubismModelUserData::Delete(replacement); });
        if (!replacement || replacement->GetArtMeshUserDatas().GetSize() != document.Entries().size())
            return fail(QStringLiteral("运行时未接受网格标签"));
        std::unique_ptr<CubismModelSettingJson> settings;
        if (changed) {
            settings = std::make_unique<CubismModelSettingJson>(reinterpret_cast<const csmByte*>(settingsBytes.constData()), csmSizeInt(settingsBytes.size()));
            if (!settings->GetJsonPointer()) return fail(QStringLiteral("运行时未接受新的模型设置"));
        }
        std::unique_ptr<ICubismModelSetting> previous;
        if (settings) { previous.reset(_modelSetting); _modelSetting = settings.release(); }
        std::swap(_modelUserData, replacement);
        m_modelSettingsBytes.swap(settingsBytes);
        m_modelSettingsDigest.swap(settingsDigest);
        m_parameterReferences.modelSettingsDigest.swap(referenceDigest);
        return true;
    } catch (const std::exception&) {
        return fail(QStringLiteral("无法准备网格标签更新，原运行时保持不变"));
    }
}
