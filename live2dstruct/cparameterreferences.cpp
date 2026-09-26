#include "cparameterreferences.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <utility>

namespace live2dstruct {
namespace {
constexpr qint64 MaximumFileBytes = 16 * 1024 * 1024;
constexpr qint64 MaximumTotalBytes = 64 * 1024 * 1024;
constexpr int MaximumFiles = 2048;
constexpr int MaximumReferences = 100000;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool validText(const QString& text)
{
    return !text.isEmpty() && !text.contains(QChar::Null);
}

bool textField(const QJsonObject& object, const QString& key, QString& value, const QString& location, QString* error)
{
    const auto field = object.value(key);
    if (!field.isString() || !validText(field.toString()))
        return fail(error, QStringLiteral("%1.%2 必须是非空且不含 NUL 的字符串").arg(location, key));
    value = field.toString();
    return true;
}

bool arrayField(const QJsonObject& object, const QString& key, QJsonArray& value, const QString& location, QString* error)
{
    if (!object.value(key).isArray()) return fail(error, QStringLiteral("%1.%2 必须是数组").arg(location, key));
    value = object.value(key).toArray();
    if (value.size() > MaximumReferences) return fail(error, QStringLiteral("%1.%2 超出依赖扫描数量限制").arg(location, key));
    return true;
}

struct ReferenceList
{
    QStringList values;
    qint64 characters = 0;
};

bool appendReference(ReferenceList& result, const QString& text, QString* error)
{
    constexpr qint64 MaximumResultCharacters = 4 * 1024 * 1024;
    if (result.values.size() >= MaximumReferences || text.size() > MaximumResultCharacters - result.characters)
        return fail(error, QStringLiteral("参数引用结果超出扫描大小或数量限制"));
    result.values.append(text);
    result.characters += text.size();
    return true;
}

bool scanParameterArray(const QJsonObject& root, const QString& id, bool motion, ReferenceList& result, QString* error)
{
    QJsonArray rows;
    const QString key = motion ? QStringLiteral("Curves") : QStringLiteral("Parameters");
    if (!arrayField(root, key, rows, QStringLiteral("JSON"), error)) return false;
    for (qsizetype i = 0; i < rows.size(); ++i) {
        const QString location = QStringLiteral("%1[%2]").arg(key).arg(i);
        if (!rows[i].isObject()) return fail(error, location + QStringLiteral(" 必须是对象"));
        const auto row = rows[i].toObject();
        QString parameter, target = QStringLiteral("Parameter");
        if (!textField(row, QStringLiteral("Id"), parameter, location, error)) return false;
        if (motion && !textField(row, QStringLiteral("Target"), target, location, error)) return false;
        if (motion && target != QStringLiteral("Parameter") && target != QStringLiteral("Model") && target != QStringLiteral("PartOpacity"))
            return fail(error, location + QStringLiteral(".Target 类型不受支持"));
        if (target == QStringLiteral("Parameter") && parameter == id &&
            !appendReference(result, QStringLiteral("%1 #%2").arg(motion ? QStringLiteral("参数曲线") : QStringLiteral("表情参数")).arg(i + 1), error)) return false;
    }
    return true;
}

class Scanner
{
public:
    explicit Scanner(QString id) : m_id(std::move(id)) {}

    bool read(const QString& path, QJsonObject& root, QString* error)
    {
        const QFileInfo info(path);
        if (!info.isFile()) return fail(error, QStringLiteral("关联文件不存在或不是普通文件：%1").arg(path));
        const QString canonical = info.canonicalFilePath();
        const auto cached = m_documents.constFind(canonical);
        if (cached != m_documents.cend()) { root = *cached; return true; }
        if (m_documents.size() >= MaximumFiles || info.size() > MaximumFileBytes || info.size() > MaximumTotalBytes - m_totalBytes)
            return fail(error, QStringLiteral("关联文件超出依赖扫描大小或数量限制：%1").arg(path));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("无法读取关联文件 %1：%2").arg(path, file.errorString()));
        const auto bytes = file.read(MaximumFileBytes + 1);
        if (file.error() != QFileDevice::NoError) return fail(error, QStringLiteral("读取关联文件 %1 失败：%2").arg(path, file.errorString()));
        if (bytes.size() > MaximumFileBytes || bytes.size() > MaximumTotalBytes - m_totalBytes)
            return fail(error, QStringLiteral("关联文件超出依赖扫描大小限制：%1").arg(path));
        QJsonParseError parse;
        const auto document = QJsonDocument::fromJson(bytes, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject())
            return fail(error, QStringLiteral("关联文件 %1 不是有效 JSON 对象：%2").arg(path, parse.errorString()));
        root = document.object();
        m_documents.insert(canonical, root);
        m_totalBytes += bytes.size();
        return true;
    }

    bool fileReferences(const QString& path, int kind, const QString& label, ReferenceList& result, QString* error)
    {
        if (++m_entries > MaximumFiles) return fail(error, QStringLiteral("关联文件条目超过依赖扫描数量限制"));
        const QFileInfo info(path);
        const QString canonical = info.canonicalFilePath();
        const QString key = (canonical.isEmpty() ? info.absoluteFilePath() : canonical) + QChar::Null + QString::number(kind);
        auto cached = m_matches.constFind(key);
        if (cached == m_matches.cend()) {
            QJsonObject document;
            if (!read(path, document, error)) return false;
            QStringList matches;
            QString detail;
            ReferenceList scanned;
            const bool valid = kind == 0 ? CParameterReferences::ScanPhysics(document, m_id, matches, &detail)
                                        : scanParameterArray(document, m_id, kind == 2, scanned, &detail);
            if (kind != 0) matches.swap(scanned.values);
            if (!valid) return fail(error, QStringLiteral("%1：%2").arg(path, detail));
            m_matches.insert(key, matches);
            cached = m_matches.constFind(key);
        }
        for (const auto& match : *cached)
            if (!appendReference(result, QStringLiteral("%1 · %2 · %3").arg(label, path, match), error)) return false;
        return true;
    }

private:
    QString m_id;
    qint64 m_totalBytes = 0;
    int m_entries = 0;
    QHash<QString, QJsonObject> m_documents;
    QHash<QString, QStringList> m_matches;
};
}

bool CParameterReferences::ScanPhysics(const QJsonObject& root, const QString& parameterId, QStringList& references, QString* error)
{
    if (error) error->clear();
    if (!validText(parameterId)) return fail(error, QStringLiteral("参数 ID 无效"));
    QJsonArray settings;
    if (!arrayField(root, QStringLiteral("PhysicsSettings"), settings, QStringLiteral("物理配置"), error)) return false;
    ReferenceList candidate;
    qint64 count = 0;
    for (qsizetype i = 0; i < settings.size(); ++i) {
        const QString location = QStringLiteral("PhysicsSettings[%1]").arg(i);
        if (!settings[i].isObject()) return fail(error, location + QStringLiteral(" 必须是对象"));
        const auto setting = settings[i].toObject();
        QString name = QString::number(i + 1);
        if (setting.contains(QStringLiteral("Id")) && !textField(setting, QStringLiteral("Id"), name, location, error)) return false;
        for (const auto& direction : {QStringLiteral("Input"), QStringLiteral("Output")}) {
            QJsonArray rows;
            if (!arrayField(setting, direction, rows, location, error)) return false;
            count += rows.size();
            if (count > MaximumReferences) return fail(error, QStringLiteral("物理引用条目超出扫描限制"));
            const QString endpoint = direction == QStringLiteral("Input") ? QStringLiteral("Source") : QStringLiteral("Destination");
            for (qsizetype j = 0; j < rows.size(); ++j) {
                const QString field = QStringLiteral("%1.%2[%3]").arg(location, direction).arg(j);
                if (!rows[j].isObject() || !rows[j].toObject().value(endpoint).isObject())
                    return fail(error, field + QStringLiteral(".%1 必须是对象").arg(endpoint));
                const auto reference = rows[j].toObject().value(endpoint).toObject();
                QString id, target;
                if (!textField(reference, QStringLiteral("Id"), id, field, error) ||
                    !textField(reference, QStringLiteral("Target"), target, field, error)) return false;
                if (target != QStringLiteral("Parameter")) return fail(error, field + QStringLiteral(".Target 必须是 Parameter"));
                if (id == parameterId && !appendReference(candidate, QStringLiteral("物理组 %1 · %2 #%3").arg(name,
                    direction == QStringLiteral("Input") ? QStringLiteral("输入") : QStringLiteral("输出")).arg(j + 1), error)) return false;
            }
        }
    }
    candidate.values.removeDuplicates();
    candidate.values.sort();
    references.swap(candidate.values);
    return true;
}

bool CParameterReferences::ScanModel(const QString& modelJsonPath, const QString& parameterId, QStringList& references, QString* error)
{
    if (error) error->clear();
    if (!validText(modelJsonPath) || !validText(parameterId)) return fail(error, QStringLiteral("模型路径或参数 ID 无效"));
    Scanner scanner(parameterId);
    QJsonObject root;
    if (!scanner.read(modelJsonPath, root, error)) return false;
    if (!root.value(QStringLiteral("FileReferences")).isObject()) return fail(error, QStringLiteral("model3.json.FileReferences 必须是对象"));
    const auto files = root.value(QStringLiteral("FileReferences")).toObject();
    const QDir directory = QFileInfo(modelJsonPath).absoluteDir();
    ReferenceList candidate;
    if (root.contains(QStringLiteral("Groups"))) {
        QJsonArray groups;
        if (!arrayField(root, QStringLiteral("Groups"), groups, QStringLiteral("model3.json"), error)) return false;
        for (qsizetype i = 0; i < groups.size(); ++i) {
            const QString location = QStringLiteral("Groups[%1]").arg(i);
            if (!groups[i].isObject()) return fail(error, location + QStringLiteral(" 必须是对象"));
            const auto group = groups[i].toObject();
            QString name, target;
            QJsonArray ids;
            if (!textField(group, QStringLiteral("Name"), name, location, error) ||
                !textField(group, QStringLiteral("Target"), target, location, error) ||
                !arrayField(group, QStringLiteral("Ids"), ids, location, error)) return false;
            if (target != QStringLiteral("Parameter") && target != QStringLiteral("Part"))
                return fail(error, location + QStringLiteral(".Target 类型不受支持"));
            if (target != QStringLiteral("Parameter") && (name == QStringLiteral("EyeBlink") || name == QStringLiteral("LipSync")))
                return fail(error, QStringLiteral("模型分组 %1 的 Target 必须是 Parameter").arg(name));
            for (const auto& id : ids) {
                if (!id.isString() || !validText(id.toString())) return fail(error, location + QStringLiteral(".Ids 含无效 ID"));
                if (target == QStringLiteral("Parameter") && id.toString() == parameterId &&
                    !appendReference(candidate, QStringLiteral("模型分组 %1 · %2").arg(name, modelJsonPath), error)) return false;
            }
        }
    }
    if (files.contains(QStringLiteral("Physics"))) {
        const auto file = files.value(QStringLiteral("Physics"));
        if (!file.isString() || file.toString().contains(QChar::Null)) return fail(error, QStringLiteral("FileReferences.Physics 必须是字符串"));
        if (!file.toString().isEmpty() && !scanner.fileReferences(directory.filePath(file.toString()), 0, QStringLiteral("物理"), candidate, error)) return false;
    }
    if (files.contains(QStringLiteral("Expressions"))) {
        QJsonArray expressions;
        if (!arrayField(files, QStringLiteral("Expressions"), expressions, QStringLiteral("FileReferences"), error)) return false;
        for (qsizetype i = 0; i < expressions.size(); ++i) {
            const QString location = QStringLiteral("Expressions[%1]").arg(i);
            if (!expressions[i].isObject()) return fail(error, location + QStringLiteral(" 必须是对象"));
            QString file;
            if (!textField(expressions[i].toObject(), QStringLiteral("File"), file, location, error)) return false;
            QString label = QStringLiteral("表情 #%1").arg(i + 1);
            if (expressions[i].toObject().contains(QStringLiteral("Name"))) {
                QString name;
                if (!textField(expressions[i].toObject(), QStringLiteral("Name"), name, location, error)) return false;
                label = QStringLiteral("表情 %1").arg(name);
            }
            if (!scanner.fileReferences(directory.filePath(file), 1, label, candidate, error)) return false;
        }
    }
    if (files.contains(QStringLiteral("Motions"))) {
        if (!files.value(QStringLiteral("Motions")).isObject()) return fail(error, QStringLiteral("FileReferences.Motions 必须是对象"));
        const auto motions = files.value(QStringLiteral("Motions")).toObject();
        for (auto group = motions.constBegin(); group != motions.constEnd(); ++group) {
            if (!group.value().isArray()) return fail(error, QStringLiteral("动作组 %1 必须是数组").arg(group.key()));
            const auto rows = group.value().toArray();
            if (rows.size() > MaximumFiles) return fail(error, QStringLiteral("动作文件条目超出扫描数量限制"));
            for (qsizetype i = 0; i < rows.size(); ++i) {
                const QString location = QStringLiteral("Motions.%1[%2]").arg(group.key()).arg(i);
                if (!rows[i].isObject()) return fail(error, location + QStringLiteral(" 必须是对象"));
                QString file;
                if (!textField(rows[i].toObject(), QStringLiteral("File"), file, location, error) ||
                    !scanner.fileReferences(directory.filePath(file), 2, QStringLiteral("动作组 %1 #%2").arg(group.key()).arg(i + 1), candidate, error)) return false;
            }
        }
    }
    candidate.values.removeDuplicates();
    candidate.values.sort();
    references.swap(candidate.values);
    return true;
}
}
