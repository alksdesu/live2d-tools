#include "cmodeljsonwriter.h"
#include "cmodeljson.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCollator>

namespace {

// 按名称自然序排序（texture_00 < texture_01 < texture_10）
void sortNatural(QStringList& list)
{
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(list.begin(), list.end(), collator);
}

// 在目录中查找指定后缀的文件：优先 <mocBaseName><suffix>，唯一候选次之，否则返回空
QString findCompanion(const QDir& dir, const QString& mocBaseName, const QString& suffix)
{
    const QString preferred = mocBaseName + suffix;
    if (dir.exists(preferred)) {
        return preferred;
    }
    QStringList candidates = dir.entryList(QStringList() << ("*" + suffix), QDir::Files);
    if (candidates.size() == 1) {
        return candidates.first();
    }
    return QString();
}

// 收集纹理相对路径：优先 textures/ 子目录，否则同目录 *.png
QStringList collectTextures(const QDir& dir)
{
    QStringList result;
    if (dir.exists("textures")) {
        QDir texDir(dir.filePath("textures"));
        QStringList pngs = texDir.entryList(QStringList() << "*.png", QDir::Files);
        sortNatural(pngs);
        for (const QString& p : pngs) {
            result << ("textures/" + p);
        }
        if (!result.isEmpty()) {
            return result;
        }
    }
    QStringList pngs = dir.entryList(QStringList() << "*.png", QDir::Files);
    sortNatural(pngs);
    for (const QString& p : pngs) {
        result << p;
    }
    return result;
}

// 收集 motions/ 或 motion/ 目录下的动作相对路径
QStringList collectMotions(const QDir& dir)
{
    QStringList result;
    for (const QString& sub : {QStringLiteral("motions"), QStringLiteral("motion")}) {
        if (!dir.exists(sub)) {
            continue;
        }
        QDir motionDir(dir.filePath(sub));
        QStringList files = motionDir.entryList(QStringList() << "*.motion3.json", QDir::Files);
        sortNatural(files);
        for (const QString& f : files) {
            result << (sub + "/" + f);
        }
        if (!result.isEmpty()) {
            break;
        }
    }
    return result;
}

// 收集表情相对路径（同目录 + expressions/ 子目录）
QStringList collectExpressions(const QDir& dir)
{
    QStringList result;
    QStringList files = dir.entryList(QStringList() << "*.exp3.json", QDir::Files);
    sortNatural(files);
    result = files;
    if (dir.exists("expressions")) {
        QDir expDir(dir.filePath("expressions"));
        QStringList subFiles = expDir.entryList(QStringList() << "*.exp3.json", QDir::Files);
        sortNatural(subFiles);
        for (const QString& f : subFiles) {
            result << ("expressions/" + f);
        }
    }
    return result;
}

} // namespace

QString CModelJsonWriter::TargetJsonPath(const QString& mocPath)
{
    QFileInfo info(mocPath);
    return info.absolutePath() + "/" + info.completeBaseName() + ".model3.json";
}

QString CModelJsonWriter::FindExistingJson(const QString& mocPath)
{
    QFileInfo mocInfo(mocPath);
    QDir dir(mocInfo.absolutePath());
    const QString mocFileName = mocInfo.fileName();

    QStringList jsons = dir.entryList(QStringList() << "*.model3.json", QDir::Files);
    for (const QString& j : jsons) {
        CModelJson parsed;
        parsed.Parse(dir.filePath(j));
        if (parsed.IsOpen() && QFileInfo(parsed.m_moc).fileName() == mocFileName) {
            return dir.filePath(j);
        }
    }
    return QString();
}

QString CModelJsonWriter::GenerateFor(const QString& mocPath, QString* outError, bool overwrite)
{
    QFileInfo mocInfo(mocPath);
    if (!mocInfo.isFile()) {
        if (outError) *outError = QString("moc3 文件不存在：%1").arg(mocPath);
        return QString();
    }

    QDir dir(mocInfo.absolutePath());
    const QString mocBaseName = mocInfo.completeBaseName();

    const QString existing = FindExistingJson(mocPath);
    if (!existing.isEmpty()) {
        return existing;
    }

    const QString targetPath = TargetJsonPath(mocPath);
    if (QFileInfo::exists(targetPath) && !overwrite) {
        if (outError) *outError = QString("目标文件已存在且未引用该 moc3：%1").arg(targetPath);
        return QString();
    }

    QStringList textures = collectTextures(dir);
    if (textures.isEmpty()) {
        if (outError) *outError = "未在 moc3 同目录或 textures/ 子目录找到任何 PNG 纹理";
        return QString();
    }

    QJsonObject fileRefs;
    fileRefs["Moc"] = mocInfo.fileName();

    QJsonArray texArray;
    for (const QString& t : textures) {
        texArray.append(t);
    }
    fileRefs["Textures"] = texArray;

    const QString physics = findCompanion(dir, mocBaseName, ".physics3.json");
    if (!physics.isEmpty()) fileRefs["Physics"] = physics;

    const QString pose = findCompanion(dir, mocBaseName, ".pose3.json");
    if (!pose.isEmpty()) fileRefs["Pose"] = pose;

    const QString cdi = findCompanion(dir, mocBaseName, ".cdi3.json");
    if (!cdi.isEmpty()) fileRefs["DisplayInfo"] = cdi;

    const QString userData = findCompanion(dir, mocBaseName, ".userdata3.json");
    if (!userData.isEmpty()) fileRefs["UserData"] = userData;

    const QStringList motions = collectMotions(dir);
    if (!motions.isEmpty()) {
        QJsonArray idleArray;
        for (const QString& m : motions) {
            QJsonObject entry;
            entry["File"] = m;
            idleArray.append(entry);
        }
        QJsonObject motionsObj;
        motionsObj["Idle"] = idleArray;
        fileRefs["Motions"] = motionsObj;
    }

    const QStringList expressions = collectExpressions(dir);
    if (!expressions.isEmpty()) {
        QJsonArray expArray;
        for (const QString& e : expressions) {
            QJsonObject entry;
            entry["Name"] = QFileInfo(e).fileName();
            entry["File"] = e;
            expArray.append(entry);
        }
        fileRefs["Expressions"] = expArray;
    }

    QJsonObject root;
    root["Version"] = 3;
    root["FileReferences"] = fileRefs;

    QFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (outError) *outError = QString("无法写入文件：%1").arg(targetPath);
        return QString();
    }
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.close();

    return targetPath;
}
