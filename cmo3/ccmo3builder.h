#ifndef CCMO3BUILDER_H
#define CCMO3BUILDER_H

#include <QString>
#include <QSet>
#include <QHash>
#include <QMap>
#include <QVector>
#include <QJsonObject>
#include <QStringList>
#include <optional>
#include "live2dstruct/cmodeldocument.h"

namespace live2dstruct {
class CSectionTable;
}

namespace cmo3 {

// moc3 → cmo3 生成编排器。
// 数据源：live2dstruct 全段解析 + CTextureExtractor 图集素材裁片。
// 生成 MODEL_IMAGE 模式工程：单 CLayeredImage + N CLayer，drawable 挂 well-known 根。
class CCmo3Builder
{
public:
    enum class ParameterDefaultMode { ModelDefinition, CurrentValues };
    struct Options {
        QString mocPath;                 // 输入 .moc3
        QByteArray mocBytes;
        std::optional<QMap<int, QByteArray>> texturePngs;
        QString outPath;                 // 输出 .cmo3
        QString modelName;               // 模型显示名（默认取文件名）
        QString cdiPath;                 // 可选 .cdi3.json（显示名映射）
        QString physicsPath;             // 可选 .physics3.json
        std::optional<QByteArray> physicsBytes;
        bool allowPhysicsConversion = false;
        QString userDataPath;
        QHash<QString, float> parameterValues;
        QHash<QString, live2dstruct::CModelParameterMetadata> parameterMetadata;
        ParameterDefaultMode parameterDefaultMode = ParameterDefaultMode::CurrentValues;
        QSet<QString> hiddenIds;         // 当前会话中隐藏的 Part/Deformer/ArtMesh
        QMap<QString, bool> objectVisibility;
        QHash<QString, float> drawableOpacityOverrides;
        QHash<QString, int> drawableDrawOrderOverrides;
        QHash<QString, QVector<QVector<float>>> drawableKeyformPositionOverrides;
        QHash<QString, QVector<quint16>> drawableIndexOverrides;
        QHash<QString, QVector<int>> drawableMaskIndexOverrides;
        QHash<QString, int> deformerParentOverrides;
        QHash<QString, QVector<live2dstruct::CModelRotationKeyform>> deformerRotationKeyformOverrides;
        QHash<QString, QVector<live2dstruct::CModelWarpKeyform>> deformerWarpKeyformOverrides;
        QHash<QString, bool> drawableMaskInverted;
        std::optional<QMap<QString, QString>> deformerDisplayNames;
    };

    struct PhysicsExport {
        bool present = false;
        QJsonObject document;
        QStringList conversions;
    };

    static bool PreparePhysicsExport(const Options& options, PhysicsExport& result, QString* error = nullptr);

    // 全流程执行；失败时 outError 带原因
    static bool Build(const Options& options, QString* outError = nullptr);
};

} // namespace cmo3

#endif // CCMO3BUILDER_H
