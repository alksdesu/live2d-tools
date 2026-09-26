#include "cparameterresourceedits.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <exception>

namespace live2dstruct {
namespace {
using Edits = CParameterResourceEdits;
using Editor = CParameterReferenceEditor;
constexpr qint64 MaximumFileBytes = 16 * 1024 * 1024;
constexpr qint64 MaximumTotalBytes = 64 * 1024 * 1024;
constexpr int MaximumFiles = 2048;
constexpr int MaximumIds = 100000;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

bool validText(const QString& text) { return !text.isEmpty() && !text.contains(QChar::Null); }

bool validIds(const QSet<QString>& ids)
{
    if (ids.size() > MaximumIds) return false;
    for (const auto& id : ids) if (!validText(id)) return false;
    return true;
}

QString relativePath(const QString& path)
{
    if (!validText(path) || QDir::isAbsolutePath(path)) return {};
    const auto clean = QDir::cleanPath(path);
    return clean == QLatin1String(".") ? QString() : clean;
}

bool catalog(const QJsonObject& model, const QDir& directory, QMap<QString, Edits::Kind>& result, QString* error)
{
    const auto files = model.value(QStringLiteral("FileReferences")).toObject();
    int count = 0;
    const auto add = [&](const QJsonValue& value, Edits::Kind kind) {
        if (++count > MaximumFiles) return fail(error, QStringLiteral("关联资源数量超出限制"));
        const auto path = value.isString() && validText(value.toString())
            ? relativePath(directory.relativeFilePath(QFileInfo(directory.filePath(value.toString())).absoluteFilePath())) : QString();
        if (path.isEmpty()) return fail(error, QStringLiteral("关联资源路径无效"));
        const auto previous = result.constFind(path);
        if (previous != result.cend() && *previous != kind) return fail(error, QStringLiteral("同一文件被声明为不同资源类型：%1").arg(path));
        result.insert(path, kind);
        return true;
    };
    if (files.contains(QStringLiteral("Physics"))) {
        const auto physics = files.value(QStringLiteral("Physics"));
        if ((!physics.isString() || !physics.toString().isEmpty()) && !add(physics, Edits::Kind::Physics)) return false;
    }
    if (files.contains(QStringLiteral("Expressions"))) {
        if (!files.value(QStringLiteral("Expressions")).isArray()) return fail(error, QStringLiteral("表情资源列表无效"));
        for (const auto& value : files.value(QStringLiteral("Expressions")).toArray())
            if (!value.isObject() || !add(value.toObject().value(QStringLiteral("File")), Edits::Kind::Expression))
                return fail(error, QStringLiteral("表情资源条目无效"));
    }
    if (files.contains(QStringLiteral("Motions"))) {
        if (!files.value(QStringLiteral("Motions")).isObject()) return fail(error, QStringLiteral("动作资源列表无效"));
        const auto groups = files.value(QStringLiteral("Motions")).toObject();
        for (auto it = groups.begin(); it != groups.end(); ++it) {
            if (!it.value().isArray()) return fail(error, QStringLiteral("动作资源分组无效：%1").arg(it.key()));
            for (const auto& value : it.value().toArray())
                if (!value.isObject() || !add(value.toObject().value(QStringLiteral("File")), Edits::Kind::Motion))
                    return fail(error, QStringLiteral("动作资源条目无效：%1").arg(it.key()));
        }
    }
    return true;
}

QSet<QString> referencedIds(const Editor::Prepared& prepared)
{
    QSet<QString> result;
    for (const auto& reference : prepared.references) result.insert(reference.parameterId);
    return result;
}

bool read(const QString& path, QByteArray& bytes, qint64& total, QString* error)
{
    const QFileInfo info(path);
    if (!info.isFile()) return fail(error, QStringLiteral("关联资源不存在：%1").arg(path));
    if (info.size() > MaximumFileBytes || info.size() > MaximumTotalBytes - total)
        return fail(error, QStringLiteral("关联资源大小超出限制：%1").arg(path));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return fail(error, QStringLiteral("无法读取关联资源：%1").arg(path));
    bytes = file.read(MaximumFileBytes + 1);
    if (file.error() != QFileDevice::NoError) return fail(error, QStringLiteral("读取关联资源失败：%1").arg(path));
    if (bytes.size() > MaximumFileBytes || bytes.size() > MaximumTotalBytes - total)
        return fail(error, QStringLiteral("关联资源大小超出限制：%1").arg(path));
    total += bytes.size();
    return true;
}

QJsonArray encodeIds(const QSet<QString>& ids)
{
    auto sorted = ids.values();
    sorted.sort();
    QJsonArray result;
    for (const auto& id : sorted) result.append(id);
    return result;
}

bool decodeIds(const QJsonValue& value, QSet<QString>& result, QString* error)
{
    if (!value.isArray() || value.toArray().size() > MaximumIds) return fail(error, QStringLiteral("资源移除引用列表无效"));
    for (const auto& id : value.toArray()) {
        if (!id.isString() || !validText(id.toString()) || result.contains(id.toString()))
            return fail(error, QStringLiteral("资源移除引用 ID 无效或重复"));
        result.insert(id.toString());
    }
    return true;
}

bool decodeSource(const QJsonValue& value, QByteArray& result, qint64& total, QString* error)
{
    if (!value.isString() || value.toString().size() > (MaximumFileBytes + 2) / 3 * 4)
        return fail(error, QStringLiteral("资源原始数据编码或大小无效"));
    const auto encoded = value.toString().toLatin1();
    const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
    if (QString::fromLatin1(encoded) != value.toString() || !decoded || decoded.decoded.toBase64() != encoded ||
        decoded.decoded.size() > MaximumFileBytes || decoded.decoded.size() > MaximumTotalBytes - total)
        return fail(error, QStringLiteral("资源原始数据编码或总大小无效"));
    result = decoded.decoded;
    total += result.size();
    return true;
}

QString kindName(Edits::Kind kind)
{
    switch (kind) {
    case Edits::Kind::Motion: return QStringLiteral("motion");
    case Edits::Kind::Expression: return QStringLiteral("expression");
    case Edits::Kind::Physics: return QStringLiteral("physics");
    default: return {};
    }
}
}

bool CParameterResourceEdits::State::IsEmpty() const
{
    return modelPath.isEmpty() && modelSource.isEmpty() && modelSuppressedIds.isEmpty() &&
           breath.isEmpty() && mouseFollow.isEmpty() && resources.isEmpty();
}

bool CParameterResourceEdits::Validate(const State& state, QString* error)
{
    if (error) error->clear();
    if (state.IsEmpty()) return true;
    if (!validText(state.modelPath) || !QDir::isAbsolutePath(state.modelPath) || QDir::cleanPath(state.modelPath) != state.modelPath ||
        QFileInfo(state.modelPath).fileName().isEmpty() ||
        !validIds(state.breath) || !validIds(state.mouseFollow) || state.resources.size() > MaximumFiles)
        return fail(error, QStringLiteral("参数引用项目状态无效"));
    Editor::Prepared model;
    if (!Editor::Prepare(Kind::ModelSettings, state.modelSource, state.modelSuppressedIds, model, error)) return false;
    if (referencedIds(model) != state.modelSuppressedIds)
        return fail(error, QStringLiteral("模型分组包含不存在的待移除引用"));
    QMap<QString, Kind> files;
    if (!catalog(model.source, QFileInfo(state.modelPath).absoluteDir(), files, error)) return false;
    qint64 total = state.modelSource.size();
    for (auto it = state.resources.cbegin(); it != state.resources.cend(); ++it) {
        if (relativePath(it.key()) != it.key() || !files.contains(it.key()) || files.value(it.key()) != it->kind || it->suppressedIds.isEmpty())
            return fail(error, QStringLiteral("资源引用状态与模型资源目录不一致：%1").arg(it.key()));
        if (it->source.size() > MaximumTotalBytes - total) return fail(error, QStringLiteral("资源状态总大小超出限制"));
        total += it->source.size();
        Editor::Prepared resource;
        if (!Editor::Prepare(it->kind, it->source, it->suppressedIds, resource, error)) return false;
        if (referencedIds(resource) != it->suppressedIds)
            return fail(error, QStringLiteral("资源中不存在待移除的参数引用：%1").arg(it.key()));
    }
    return true;
}

bool CParameterResourceEdits::Prepare(const QString& modelPath, const State& current, const QSet<QString>& deletedIds,
                                     Prepared& prepared, QString* error)
{
    if (error) error->clear();
    if (!validIds(deletedIds) || !validText(modelPath)) return fail(error, QStringLiteral("参数引用删除请求无效"));
    if (!Validate(current, error)) return false;
    try {
        Prepared candidate;
        candidate.state = current;
        qint64 total = 0;
        QByteArray modelBytes;
        if (!read(modelPath, modelBytes, total, error)) return false;
        const QFileInfo modelInfo(modelPath);
        if (!current.IsEmpty() && (current.modelPath != QDir::cleanPath(modelInfo.absoluteFilePath()) || current.modelSource != modelBytes))
            return fail(error, QStringLiteral("模型设置已改变，无法沿用参数引用修改：%1").arg(modelPath));
        candidate.state.modelPath = QDir::cleanPath(modelInfo.absoluteFilePath());
        candidate.state.modelSource = modelBytes;
        if (!Editor::Prepare(Kind::ModelSettings, modelBytes, current.modelSuppressedIds | deletedIds, candidate.model, error)) return false;
        candidate.state.modelSuppressedIds = referencedIds(candidate.model);
        candidate.model.suppressedIds = candidate.state.modelSuppressedIds;
        QMap<QString, Kind> files;
        const QDir directory = modelInfo.absoluteDir();
        if (!catalog(candidate.model.source, directory, files, error)) return false;
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QByteArray source;
            if (!read(directory.filePath(it.key()), source, total, error)) return false;
            const auto previous = current.resources.constFind(it.key());
            if (previous != current.resources.cend() && previous->source != source)
                return fail(error, QStringLiteral("关联资源已改变，无法恢复原引用：%1").arg(it.key()));
            const QSet<QString> previousIds = previous == current.resources.cend() ? QSet<QString>() : previous->suppressedIds;
            Editor::Prepared resource;
            if (!Editor::Prepare(it.value(), source, previousIds | deletedIds, resource, error))
                return fail(error, QStringLiteral("资源 %1：%2").arg(it.key(), error ? *error : QString()));
            const auto removed = referencedIds(resource);
            resource.suppressedIds = removed;
            if (!removed.isEmpty()) candidate.state.resources.insert(it.key(), {it.value(), source, removed});
            for (const auto& reference : resource.references) if (!previousIds.contains(reference.parameterId)) {
                const auto label = reference.kind == QLatin1String("motion") ? QStringLiteral("动作参数曲线")
                    : reference.kind == QLatin1String("expression") ? QStringLiteral("表情参数")
                    : reference.kind == QLatin1String("physicsInput") ? QStringLiteral("物理输入") : QStringLiteral("物理输出");
                const auto location = reference.owner.isEmpty() ? QStringLiteral("第 %1 项").arg(reference.row + 1)
                    : QStringLiteral("组 %1").arg(reference.owner);
                candidate.removals.append(QStringLiteral("%1：%2，%3（%4）").arg(it.key(), reference.parameterId, label, location));
            }
            candidate.resources.insert(it.key(), std::move(resource));
        }
        for (const auto& reference : candidate.model.references)
            if (!current.modelSuppressedIds.contains(reference.parameterId)) candidate.removals.append(QStringLiteral("模型分组 %1 · %2").arg(reference.owner, reference.parameterId));
        prepared = std::move(candidate);
        return true;
    } catch (const std::exception&) { return fail(error, QStringLiteral("无法准备项目资源引用状态")); }
}

bool CParameterResourceEdits::ToJson(const State& state, QJsonObject& object, QString* error)
{
    if (!Validate(state, error)) return false;
    if (state.IsEmpty()) { object = {}; return true; }
    QJsonObject resources;
    for (auto it = state.resources.cbegin(); it != state.resources.cend(); ++it)
        resources.insert(it.key(), QJsonObject{{QStringLiteral("kind"), kindName(it->kind)},
            {QStringLiteral("source"), QString::fromLatin1(it->source.toBase64())},
            {QStringLiteral("suppressedIds"), encodeIds(it->suppressedIds)}});
    object = {{QStringLiteral("version"), 1}, {QStringLiteral("modelPath"), state.modelPath},
        {QStringLiteral("modelSource"), QString::fromLatin1(state.modelSource.toBase64())},
        {QStringLiteral("modelSuppressedIds"), encodeIds(state.modelSuppressedIds)}, {QStringLiteral("resources"), resources},
        {QStringLiteral("breath"), encodeIds(state.breath)}, {QStringLiteral("mouseFollow"), encodeIds(state.mouseFollow)}};
    return true;
}

bool CParameterResourceEdits::FromJson(const QJsonObject& object, State& state, QString* error)
{
    if (error) error->clear();
    if (object.isEmpty()) { state = {}; return true; }
    if (object.value(QStringLiteral("version")).toDouble(-1) != 1 || !object.value(QStringLiteral("modelPath")).isString() ||
        !object.value(QStringLiteral("resources")).isObject() || object.value(QStringLiteral("resources")).toObject().size() > MaximumFiles)
        return fail(error, QStringLiteral("参数引用项目格式无效"));
    State candidate;
    candidate.modelPath = object.value(QStringLiteral("modelPath")).toString();
    qint64 total = 0;
    if (!decodeSource(object.value(QStringLiteral("modelSource")), candidate.modelSource, total, error) ||
        !decodeIds(object.value(QStringLiteral("modelSuppressedIds")), candidate.modelSuppressedIds, error) ||
        !decodeIds(object.value(QStringLiteral("breath")), candidate.breath, error) ||
        !decodeIds(object.value(QStringLiteral("mouseFollow")), candidate.mouseFollow, error)) return false;
    const auto resources = object.value(QStringLiteral("resources")).toObject();
    for (auto it = resources.begin(); it != resources.end(); ++it) {
        if (!it.value().isObject()) return fail(error, QStringLiteral("资源状态不是对象：%1").arg(it.key()));
        const auto value = it.value().toObject();
        Resource resource;
        const auto kind = value.value(QStringLiteral("kind")).toString();
        if (kind == QLatin1String("motion")) resource.kind = Kind::Motion;
        else if (kind == QLatin1String("expression")) resource.kind = Kind::Expression;
        else if (kind == QLatin1String("physics")) resource.kind = Kind::Physics;
        else return fail(error, QStringLiteral("资源类型无效：%1").arg(it.key()));
        if (!decodeSource(value.value(QStringLiteral("source")), resource.source, total, error) ||
            !decodeIds(value.value(QStringLiteral("suppressedIds")), resource.suppressedIds, error)) return false;
        candidate.resources.insert(it.key(), std::move(resource));
    }
    if (!Validate(candidate, error)) return false;
    state = std::move(candidate);
    return true;
}
}
