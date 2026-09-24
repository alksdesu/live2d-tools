#ifndef CXSXMLWRITER_H
#define CXSXMLWRITER_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QXmlStreamWriter>

namespace cmo3 {

// Cubism main.xml 的 xs 序列化写出器。
// shared 池对象带 xs.id="#N"，其余位置用 xs.ref="#N" 引用；xs.idx 读取器忽略，统一写 0。
class CXsXmlWriter
{
public:
    CXsXmlWriter();

    // ---- 文档骨架 ----
    void BeginDocument(const QStringList& versionPIs, const QStringList& importClasses,
                       const QString& fileFormatVersion);
    void EndDocument();
    void BeginShared();
    void EndShared();
    void BeginMain();
    void EndMain();

    // ---- 对象池 ----
    QString AllocId() { return QString("#%1").arg(m_nextId++); }

    // ---- 对象元素 ----
    // id 非空时输出 xs.id + xs.idx="0"（shared 池对象）
    void BeginObject(const QString& className, const QString& fieldName = QString(),
                     const QString& id = QString());
    void EndObject();
    void Attribute(const QString& name, const QString& value);

    // ---- 引用与原生字段（fieldName 为空则不输出 xs.n） ----
    void Ref(const QString& className, const QString& fieldName, const QString& id);
    void Null(const QString& fieldName);
    void S(const QString& fieldName, const QString& value);
    void I(const QString& fieldName, int value);
    void L(const QString& fieldName, qint64 value);
    void F(const QString& fieldName, float value);
    void B(const QString& fieldName, bool value);
    void File(const QString& fieldName, const QString& path);
    void Enum(const QString& className, const QString& fieldName, const QString& value);

    // ---- 数组 ----
    void FloatArray(const QString& fieldName, const QVector<float>& values);
    void IntArray(const QString& fieldName, const QVector<int>& values);
    void LongArray(const QString& fieldName, const QVector<qint64>& values);
    void ShortArray(const QString& fieldName, const QVector<short>& values);
    void ByteArray(const QString& fieldName, const QVector<quint8>& values);

    // ---- 容器（内容由调用方写入，Begin/End 之间） ----
    void BeginCArrayList(const QString& fieldName, int count);
    void BeginArrayList(const QString& fieldName, int count);
    void EndList();

    // ---- GUID 便捷（shared 池身份对象，如 CDrawableGuid/CFormGuid） ----
    void GuidObject(const QString& className, const QString& uuid, const QString& id,
                    const QString& note = QStringLiteral("(no debug info)"));

    QByteArray Result() const { return m_buffer; }
    bool HasError() const { return m_xml.hasError(); }

    // Java float 风格格式化（最短往返表示，如 "1.0"、"0.14434664"）
    static QString FormatFloat(float value);

private:
    QByteArray m_buffer;
    QXmlStreamWriter m_xml;
    int m_nextId = 0;
};

} // namespace cmo3

#endif // CXSXMLWRITER_H
