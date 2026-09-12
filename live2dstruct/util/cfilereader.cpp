#include "cfilereader.h"
namespace live2dstruct {
CReader::CReader(const QString& filename) {
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        m_valid = false;
        m_error = QString("无法打开文件：%1").arg(filename);
        m_pos = 0;
        return;
    }
    m_byte = file.readAll();
    m_pos = 0;
    file.close();
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
