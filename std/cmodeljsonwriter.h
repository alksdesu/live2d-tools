#ifndef CMODELJSONWRITER_H
#define CMODELJSONWRITER_H
#include <QString>

// 为裸 .moc3 文件（如 Unity 提取物）扫描同目录资源并合成标准 model3.json，
// 使其能走现有的 model3.json 加载链打开。
class CModelJsonWriter
{
public:
    // 返回可用的 model3.json 绝对路径，失败返回空并填充 outError。
    // 同目录已有引用该 moc 的 json 时直接复用；否则生成 <mocBaseName>.model3.json。
    // overwrite 为 true 时允许覆盖已存在但不匹配的同名文件。
    static QString GenerateFor(const QString& mocPath, QString* outError, bool overwrite = false);

    // 查找同目录下已引用该 moc 的 model3.json（无则返回空）
    static QString FindExistingJson(const QString& mocPath);

    // 目标生成路径 <mocDir>/<mocBaseName>.model3.json
    static QString TargetJsonPath(const QString& mocPath);
};

#endif // CMODELJSONWRITER_H
