#include "cfilereader.h"
#include <limits>
namespace live2dstruct {
CReader CReader::FromBytes(QByteArray bytes)
{
    CReader reader;
    if (bytes.size() > std::numeric_limits<int>::max()) {
        reader.Fail(QStringLiteral("moc3 字节快照超出可读取范围"));
        return reader;
    }
    reader.m_byte = QByteArray(bytes.constData(), bytes.size());
    return reader;
}

CReader::CReader(const QString& filename) {
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        Fail(QString("无法打开文件：%1").arg(filename));
        return;
    }
    if (file.size() < 0 || file.size() > std::numeric_limits<int>::max()) {
        Fail(QString("moc3 文件大小超出可读取范围：%1").arg(file.size()));
        return;
    }
    m_byte = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        Fail(QString("无法读取文件：%1（%2）").arg(filename, file.errorString()));
        return;
    }
    if (m_byte.size() > std::numeric_limits<int>::max())
        Fail(QString("moc3 文件大小超出可读取范围：%1").arg(m_byte.size()));
}

bool CReader::Fail(const QString& message)
{
    if (m_valid) {
        m_valid = false;
        m_error = message;
    }
    return false;
}

bool CReader::RequireSpan(const QString& label, qint64 offset, qint64 count, qint64 width)
{
    if (!m_valid) return false;
    const qint64 size = m_byte.size();
    if (offset < 0 || offset > size || count < 0 || width <= 0 ||
        count > (size - offset) / width) {
        return Fail(QString("moc3 区段越界：%1（offset %2，count %3，width %4，file %5）")
                        .arg(label).arg(offset).arg(count).arg(width).arg(size));
    }
    return true;
}

bool CReader::IsMoc3() const
{
    if (m_byte.size() < 8 || m_byte.left(4) != QByteArrayLiteral("MOC3")) return false;
    const quint32 version = quint8(m_byte[4]);
    return version >= 1 && version <= 5;
}

CReader::~CReader()
{
    // if(file->isOpen()){
    //     file->close();
    // }
    // delete file;
}
}
