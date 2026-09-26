#include "cmodeljsonwriter.h"
#include "cmodeljson.h"
#include "live2dstruct/cmodeluserdata.h"
#include <QDir>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCollator>
#include <QSaveFile>
#include <QTemporaryFile>

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

bool CModelJsonWriter::CreateUserData(const QString& modelJsonPath, const live2dstruct::CModelUserData& data,
                                     QString* savedPath, QString* error, const QByteArray& expectedModelDigest)
{
    if (!data.Validate(error)) return false;
    if (!expectedModelDigest.isEmpty() && expectedModelDigest.size() != 32) {
        if (error) *error = QStringLiteral("模型设置的加载摘要无效");
        return false;
    }
    QFile source(modelJsonPath);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) *error = source.errorString();
        return false;
    }
    const QByteArray sourceBytes = source.readAll();
    if (source.error() != QFileDevice::NoError) {
        if (error) *error = source.errorString();
        return false;
    }
    source.close();
    if (!expectedModelDigest.isEmpty() &&
        QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256) != expectedModelDigest) {
        if (error) *error = QStringLiteral("模型设置自加载后已改变，请重新打开模型后保存标签");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(sourceBytes, &parseError);
    QJsonObject root = document.object();
    QJsonObject references = root.value(QStringLiteral("FileReferences")).toObject();
    if (parseError.error != QJsonParseError::NoError ||
        root.value(QStringLiteral("Version")).toDouble(-1) != 3.0 ||
        references.value(QStringLiteral("Moc")).toString().isEmpty()) {
        if (error) *error = QStringLiteral("无法向无效的 model3.json 登记 UserData");
        return false;
    }
    const QJsonValue existing = references.value(QStringLiteral("UserData"));
    if (!existing.isUndefined() && (!existing.isString() || !existing.toString().isEmpty())) {
        if (error) *error = QStringLiteral("模型的 UserData 引用已变化，请重新打开模型后保存");
        return false;
    }

    const QDir directory = QFileInfo(modelJsonPath).absoluteDir();
    const QString base = QFileInfo(references.value(QStringLiteral("Moc")).toString()).completeBaseName();
    QTemporaryFile output(directory.filePath(base + QStringLiteral(".XXXXXX.userdata3.json")));
    if (!output.open()) {
        if (error) *error = output.errorString();
        return false;
    }
    const QString path = output.fileName();
    output.close();
    if (!data.Save(path, error)) return false;
    references.insert(QStringLiteral("UserData"), directory.relativeFilePath(path));
    root.insert(QStringLiteral("FileReferences"), references);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!source.open(QIODevice::ReadOnly)) {
        if (error) *error = source.errorString();
        return false;
    }
    const QByteArray latestSource = source.readAll();
    if (source.error() != QFileDevice::NoError) {
        if (error) *error = source.errorString();
        return false;
    }
    source.close();
    if (latestSource != sourceBytes) {
        if (error) *error = QStringLiteral("模型设置在保存标签期间已改变，请重新打开模型后重试");
        return false;
    }
    QSaveFile modelFile(modelJsonPath);
    if (!modelFile.open(QIODevice::WriteOnly) || modelFile.write(bytes) != bytes.size() || !modelFile.commit()) {
        if (error) *error = modelFile.errorString();
        return false;
    }
    output.setAutoRemove(false);
    if (savedPath) *savedPath = path;
    return true;
}
