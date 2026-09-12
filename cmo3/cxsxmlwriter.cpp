#include "cxsxmlwriter.h"
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace cmo3 {

namespace {
const QString kAttrN = QStringLiteral("xs.n");
const QString kAttrId = QStringLiteral("xs.id");
const QString kAttrIdx = QStringLiteral("xs.idx");
const QString kAttrRef = QStringLiteral("xs.ref");
} // namespace

CXsXmlWriter::CXsXmlWriter()
    : m_xml(&m_buffer)
{
    m_xml.setAutoFormatting(true);
    m_xml.setAutoFormattingIndent(0);
}

void CXsXmlWriter::BeginDocument(const QStringList& versionPIs, const QStringList& importClasses,
                                 const QString& fileFormatVersion)
{
    m_xml.writeStartDocument(QStringLiteral("1.0"));
    for (const QString& pi : versionPIs) {
        m_xml.writeProcessingInstruction(QStringLiteral("version"), pi);
    }
    for (const QString& cls : importClasses) {
        m_xml.writeProcessingInstruction(QStringLiteral("import"), cls);
    }
    m_xml.writeStartElement(QStringLiteral("root"));
    m_xml.writeAttribute(QStringLiteral("fileFormatVersion"), fileFormatVersion);
}

void CXsXmlWriter::EndDocument()
{
    m_xml.writeEndElement(); // root
    m_xml.writeEndDocument();
}

void CXsXmlWriter::BeginShared()
{
    m_xml.writeStartElement(QStringLiteral("shared"));
}

void CXsXmlWriter::EndShared()
{
    m_xml.writeEndElement();
}

void CXsXmlWriter::BeginMain()
{
    m_xml.writeStartElement(QStringLiteral("main"));
}

void CXsXmlWriter::EndMain()
{
    m_xml.writeEndElement();
}

void CXsXmlWriter::BeginObject(const QString& className, const QString& fieldName, const QString& id)
{
    m_xml.writeStartElement(className);
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    if (!id.isEmpty()) {
        m_xml.writeAttribute(kAttrId, id);
        m_xml.writeAttribute(kAttrIdx, QStringLiteral("0"));
    }
}

void CXsXmlWriter::EndObject()
{
    m_xml.writeEndElement();
}

void CXsXmlWriter::Attribute(const QString& name, const QString& value)
{
    m_xml.writeAttribute(name, value);
}

void CXsXmlWriter::Ref(const QString& className, const QString& fieldName, const QString& id)
{
    m_xml.writeStartElement(className);
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeAttribute(kAttrRef, id);
    m_xml.writeEndElement();
}

void CXsXmlWriter::Null(const QString& fieldName)
{
    m_xml.writeStartElement(QStringLiteral("null"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeEndElement();
}

void CXsXmlWriter::S(const QString& fieldName, const QString& value)
{
    m_xml.writeStartElement(QStringLiteral("s"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    if (!value.isEmpty()) m_xml.writeCharacters(value);
    m_xml.writeEndElement();
}

void CXsXmlWriter::I(const QString& fieldName, int value)
{
    m_xml.writeStartElement(QStringLiteral("i"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeCharacters(QString::number(value));
    m_xml.writeEndElement();
}

void CXsXmlWriter::L(const QString& fieldName, qint64 value)
{
    m_xml.writeStartElement(QStringLiteral("l"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeCharacters(QString::number(value));
    m_xml.writeEndElement();
}

void CXsXmlWriter::F(const QString& fieldName, float value)
{
    m_xml.writeStartElement(QStringLiteral("f"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeCharacters(FormatFloat(value));
    m_xml.writeEndElement();
}

void CXsXmlWriter::B(const QString& fieldName, bool value)
{
    m_xml.writeStartElement(QStringLiteral("b"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeCharacters(value ? QStringLiteral("true") : QStringLiteral("false"));
    m_xml.writeEndElement();
}

void CXsXmlWriter::File(const QString& fieldName, const QString& path)
{
    m_xml.writeStartElement(QStringLiteral("file"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeAttribute(QStringLiteral("path"), path);
    m_xml.writeEndElement();
}

void CXsXmlWriter::Enum(const QString& className, const QString& fieldName, const QString& value)
{
    m_xml.writeStartElement(className);
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeAttribute(QStringLiteral("v"), value);
    m_xml.writeEndElement();
}

void CXsXmlWriter::FloatArray(const QString& fieldName, const QVector<float>& values)
{
    m_xml.writeStartElement(QStringLiteral("float-array"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeAttribute(QStringLiteral("count"), QString::number(values.size()));
    QStringList parts;
    parts.reserve(values.size());
    for (float v : values) parts.append(FormatFloat(v));
    m_xml.writeCharacters(parts.join(QLatin1Char(' ')));
    m_xml.writeEndElement();
}

void CXsXmlWriter::IntArray(const QString& fieldName, const QVector<int>& values)
{
    m_xml.writeStartElement(QStringLiteral("int-array"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeAttribute(QStringLiteral("count"), QString::number(values.size()));
    QStringList parts;
    parts.reserve(values.size());
    for (int v : values) parts.append(QString::number(v));
    m_xml.writeCharacters(parts.join(QLatin1Char(' ')));
    m_xml.writeEndElement();
}

void CXsXmlWriter::LongArray(const QString& fieldName, const QVector<qint64>& values)
{
    m_xml.writeStartElement(QStringLiteral("long-array"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeAttribute(QStringLiteral("count"), QString::number(values.size()));
    QStringList parts;
    parts.reserve(values.size());
    for (qint64 v : values) parts.append(QString::number(v));
    m_xml.writeCharacters(parts.join(QLatin1Char(' ')));
    m_xml.writeEndElement();
}

void CXsXmlWriter::ShortArray(const QString& fieldName, const QVector<short>& values)
{
    m_xml.writeStartElement(QStringLiteral("short-array"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeAttribute(QStringLiteral("count"), QString::number(values.size()));
    QStringList parts;
    parts.reserve(values.size());
    for (short v : values) parts.append(QString::number(v));
    m_xml.writeCharacters(parts.join(QLatin1Char(' ')));
    m_xml.writeEndElement();
}

void CXsXmlWriter::ByteArray(const QString& fieldName, const QVector<quint8>& values)
{
    m_xml.writeStartElement(QStringLiteral("byte-array"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeAttribute(QStringLiteral("count"), QString::number(values.size()));
    QStringList parts;
    parts.reserve(values.size());
    for (quint8 v : values) parts.append(QString::number(v));
    m_xml.writeCharacters(parts.join(QLatin1Char(' ')));
    m_xml.writeEndElement();
}

void CXsXmlWriter::BeginCArrayList(const QString& fieldName, int count)
{
    m_xml.writeStartElement(QStringLiteral("carray_list"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeAttribute(QStringLiteral("count"), QString::number(count));
}

void CXsXmlWriter::BeginArrayList(const QString& fieldName, int count)
{
    m_xml.writeStartElement(QStringLiteral("array_list"));
    if (!fieldName.isEmpty()) m_xml.writeAttribute(kAttrN, fieldName);
    m_xml.writeAttribute(QStringLiteral("count"), QString::number(count));
}

void CXsXmlWriter::EndList()
{
    m_xml.writeEndElement();
}

void CXsXmlWriter::GuidObject(const QString& className, const QString& uuid, const QString& id,
                              const QString& note)
{
    m_xml.writeStartElement(className);
    m_xml.writeAttribute(QStringLiteral("uuid"), uuid);
    m_xml.writeAttribute(QStringLiteral("note"), note);
    m_xml.writeAttribute(kAttrId, id);
    m_xml.writeAttribute(kAttrIdx, QStringLiteral("0"));
    m_xml.writeEndElement();
}

QString CXsXmlWriter::FormatFloat(float value)
{
    // 保持 float 往返精度；整数值补 ".0" 对齐 Java Float.toString 风格
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    QString s = QString::fromStdString(stream.str());
    if (!s.contains(QLatin1Char('.')) && !s.contains(QLatin1Char('e')) &&
        !s.contains(QLatin1Char('n')) && !s.contains(QLatin1Char('i'))) {
        s += QLatin1String(".0");
    }
    return s;
}

} // namespace cmo3
