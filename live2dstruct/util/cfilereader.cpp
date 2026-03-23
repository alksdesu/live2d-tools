#include "cfilereader.h"
namespace live2dstruct {
CReader::CReader(const QString& filename) {
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug("无法打开文件");
    }
    m_byte = file.readAll();
    m_pos = 0;
    file.close();
}

CReader::~CReader()
{
    // if(file->isOpen()){
    //     file->close();
    // }
    // delete file;
}
}
