#ifndef CCMO3BUILDER_H
#define CCMO3BUILDER_H

#include <QString>
#include <QSet>
#include <QHash>
#include <QVector>

namespace live2dstruct {
class CSectionTable;
}

namespace cmo3 {

// moc3 → cmo3 生成编排器。
// 数据源：live2dstruct 全段解析 + CTextureExtractor 每 mesh 光栅化贴图。
// 生成 MODEL_IMAGE 模式工程：单 CLayeredImage + N CLayer，drawable 挂 well-known 根。
class CCmo3Builder
{
public:
    struct Options {
        QString mocPath;                 // 输入 .moc3
        QString outPath;                 // 输出 .cmo3
        QString modelName;               // 模型显示名（默认取文件名）
        QString cdiPath;                 // 可选 .cdi3.json（显示名映射）
        QString physicsPath;             // 可选 .physics3.json
        QSet<QString> hiddenIds;         // 当前会话中隐藏的 Part/Deformer/ArtMesh
        QHash<QString, float> drawableOpacityOverrides;
        QHash<QString, int> drawableDrawOrderOverrides;
        QHash<QString, QVector<QVector<float>>> drawableKeyformPositionOverrides;
    };

    // 全流程执行；失败时 outError 带原因
    static bool Build(const Options& options, QString* outError = nullptr);
};

} // namespace cmo3

#endif // CCMO3BUILDER_H
