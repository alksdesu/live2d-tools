#ifndef CCMO3XML_P_H
#define CCMO3XML_P_H

#include "std/cexportcontrol.h"
#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QVector>
#include <QXmlStreamReader>

namespace cmo3::detail {
inline constexpr qint64 MaximumXmlBytes = 64LL * 1024 * 1024;
inline constexpr qint64 MaximumMetadataBytes = 128LL * 1024 * 1024;
inline constexpr int MaximumDepth = 128;
struct ReadError { QString message; };
inline void require(bool condition, const QString& message)
{
    if (!condition) throw ReadError{message};
}
inline void checkpoint(CExportControl* control)
{
    if (control && !control->Checkpoint()) throw ReadError{control->Error()};
}
struct Node {
    QString type, value;
    QMap<QString, QString> attributes;
    QVector<int> children;
};
class Xml
{
public:
    QVector<Node> nodes;
    QHash<QString, int> definitions;
    int unrelatedInvalidReferences = 0;
    qint64 metadataBytes = 0;

    void parse(const QByteArray& bytes, CExportControl* control)
    {
        require(bytes.size() <= MaximumXmlBytes, QStringLiteral("CMO XML 超出读取预算"));
        QXmlStreamReader stream(bytes);
        QVector<int> stack;
        qint64 metadata = 0;
        while (!stream.atEnd()) {
            const auto token = stream.readNext();
            require(token != QXmlStreamReader::DTD && token != QXmlStreamReader::EntityReference,
                    QStringLiteral("CMO XML 不允许 DTD 或自定义实体"));
            if (token == QXmlStreamReader::StartElement) {
                require(nodes.size() < 1000000 && stack.size() < MaximumDepth,
                        QStringLiteral("CMO XML 节点数量或深度超出预算"));
                require(stream.qualifiedName().size() <= 256, QStringLiteral("CMO XML 类型名称过长"));
                Node node;
                node.type = stream.qualifiedName().toString();
                metadata += 128 + node.type.size() * 2;
                for (const auto& attribute : stream.attributes()) {
                    require(attribute.qualifiedName().size() <= 256 && attribute.value().size() <= 32768,
                            QStringLiteral("CMO XML 属性过长"));
                    const QString key = attribute.qualifiedName().toString();
                    const QString value = attribute.value().toString();
                    metadata += (key.size() + value.size()) * 2 + 64;
                    node.attributes.insert(key, value);
                }
                require(metadata <= MaximumMetadataBytes, QStringLiteral("CMO XML 元数据超出预算"));
                const int index = nodes.size();
                const auto id = node.attributes.value(QStringLiteral("xs.id"));
                if (!id.isEmpty()) {
                    require(!definitions.contains(id) && !node.attributes.contains(QStringLiteral("xs.ref")),
                            QStringLiteral("CMO XML 定义重复或同时包含引用：%1").arg(id));
                    definitions.insert(id, index);
                }
                if (!stack.isEmpty()) nodes[stack.last()].children.append(index);
                else require(nodes.isEmpty(), QStringLiteral("CMO XML 包含多个根节点"));
                nodes.append(std::move(node));
                stack.append(index);
                if ((index & 1023) == 0) checkpoint(control);
            } else if (token == QXmlStreamReader::EndElement) {
                require(!stack.isEmpty(), QStringLiteral("CMO XML 结束节点无效"));
                stack.removeLast();
            } else if (token == QXmlStreamReader::Characters && !stack.isEmpty()) {
                const auto value = stream.text();
                require(value.size() <= MaximumMetadataBytes / 2 - metadata / 2,
                        QStringLiteral("CMO XML 文本超出预算"));
                metadata += value.size() * 2;
                nodes[stack.last()].value.append(value);
            }
        }
        require(!stream.hasError() && !nodes.isEmpty() && nodes[0].type == QStringLiteral("root"),
                QStringLiteral("CMO XML 无效：%1").arg(stream.errorString()));
        metadataBytes = metadata;
        for (int index = 0; index < nodes.size(); ++index) {
            if ((index & 1023) == 0) checkpoint(control);
            if (!nodes[index].attributes.contains(QStringLiteral("xs.ref"))) continue;
            try { resolve(index); }
            catch (const ReadError&) { ++unrelatedInvalidReferences; }
        }
    }

    int resolve(int index) const
    {
        require(index >= 0 && index < nodes.size(), QStringLiteral("CMO XML 缺少必要节点"));
        const auto& node = nodes[index];
        const auto reference = node.attributes.constFind(QStringLiteral("xs.ref"));
        if (reference == node.attributes.cend()) return index;
        require(node.children.isEmpty() && node.value.trimmed().isEmpty(),
                QStringLiteral("CMO XML 引用混有内容"));
        const auto found = definitions.constFind(reference.value());
        require(found != definitions.cend(), QStringLiteral("CMO XML 引用未定义"));
        const int target = found.value();
        require(nodes[target].type == node.type && !nodes[target].attributes.contains(QStringLiteral("xs.ref")),
                QStringLiteral("CMO XML 引用类型不匹配或循环"));
        return target;
    }
    int field(int index, const QString& name, bool required = true) const
    {
        index = resolve(index);
        int result = -1;
        for (int child : nodes[index].children) {
            if (nodes[child].attributes.value(QStringLiteral("xs.n")) != name) continue;
            require(result < 0, QStringLiteral("CMO 字段重复：%1").arg(name));
            result = child;
        }
        require(!required || result >= 0, QStringLiteral("CMO 缺少字段：%1").arg(name));
        return result < 0 ? -1 : resolve(result);
    }
    int typed(int index, const QString& type) const
    {
        index = resolve(index);
        require(nodes[index].type == type, QStringLiteral("CMO 类型不匹配，预期 %1").arg(type));
        return index;
    }
    QString text(int index) const
    {
        index = resolve(index);
        require(nodes[index].children.isEmpty() && nodes[index].value.size() <= 32768 &&
                !nodes[index].value.contains(QChar::Null), QStringLiteral("CMO 文本字段无效"));
        return nodes[index].value;
    }
    qint64 number(const QString& value) const
    {
        bool valid = false;
        const qint64 result = value.toLongLong(&valid);
        require(valid, QStringLiteral("CMO 整数字段无效：%1").arg(value.left(80)));
        return result;
    }
    qint64 integer(int index, const QString& name) const { return number(text(field(index, name)).trimmed()); }
    bool boolean(int index, const QString& name) const
    {
        const QString value = text(field(index, name)).trimmed();
        require(value == QStringLiteral("true") || value == QStringLiteral("false"),
                QStringLiteral("CMO 布尔字段无效：%1").arg(name));
        return value == QStringLiteral("true");
    }
    QVector<int> array(int index) const
    {
        index = resolve(index);
        const auto& node = nodes[index];
        require(number(node.attributes.value(QStringLiteral("count"))) == node.children.size(),
                QStringLiteral("CMO 集合数量与实际内容不一致"));
        return node.children;
    }
    bool empty(int index) const
    {
        if (index < 0) return true;
        index = resolve(index);
        const auto& node = nodes[index];
        if (node.type == QStringLiteral("null")) return true;
        if (node.attributes.contains(QStringLiteral("count"))) return array(index).isEmpty();
        return node.children.isEmpty() && node.value.isEmpty();
    }
};

}

#endif
