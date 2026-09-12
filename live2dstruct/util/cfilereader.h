#ifndef CFILEREADER_H
#define CFILEREADER_H
#include <QFile>
#include <QDataStream>
#include <QString>
#include <cstring>
#include <cstdint>
namespace live2dstruct {
class CReader
{
public:
    CReader(const QString& filename );
    ~CReader();
    //QFile* file;
    QByteArray m_byte;
    int m_pos;
    bool IsValid() const { return m_valid; }
    bool IsMoc3() const;
    const QString& Error() const { return m_error; }

    void seek(int pos) {
        if (pos < 0 || pos > m_byte.size()) {
            m_valid = false;
            m_error = QString("moc3 offset 越界: %1 (size %2)").arg(pos).arg(m_byte.size());
            return;
        }
        m_pos = pos;
        //file->seek(pos);
    }
    const float readFloat() {

        uint32_t uint_num = 0;
        if (!canRead(4)) return 0.0f;
        for (int i = 3; i >= 0; i--) {
            uint_num <<= 8;
            uint_num |= (m_byte[m_pos+i] & 0xFF);
        }
        m_pos+=4;
        float value = 0.0f;
        std::memcpy(&value, &uint_num, sizeof(value));
        return  value;
    }
    const int readInt(){
        uint32_t uint_num = 0;
        if (!canRead(4)) return 0;
        for (int i = 3; i >= 0; i--) {
            uint_num <<= 8;
            if(m_pos+i >= m_byte.size()){
                 uint_num |= (0 & 0xFF);
            }else{
                uint_num |= (m_byte[m_pos+i] & 0xFF);
            }

        }
        m_pos+=4;
        int value = 0;
        std::memcpy(&value, &uint_num, sizeof(value));
        return  value;
    }
    const short readShort() {
        uint32_t uint_num = 0;
        if (!canRead(2)) return 0;
        for (int i = 1; i >= 0; i--) {
            uint_num <<= 8;
            uint_num |= (m_byte[m_pos+i] & 0xFF);
        }
        m_pos+=2;
        short value = 0;
        std::memcpy(&value, &uint_num, sizeof(value));
        return  value;
    }
    const uint16_t readUShort() {
        if (!canRead(2)) return 0;
        const uint16_t value = static_cast<uint16_t>(static_cast<uint8_t>(m_byte[m_pos]) |
                                     (static_cast<uint16_t>(static_cast<uint8_t>(m_byte[m_pos + 1])) << 8));
        m_pos += 2;
        return value;
    }
    const bool readBoolean() {
        int value =readInt();
        if(value == 0){
            return false;
        }else{
            return true;
        }
    }
    const QString readString()  {
        //QByteArray bytes = file->read(0x40);
        if (!canRead(0x40)) return QString();
        QByteArray bytes = m_byte.mid(m_pos,0x40);//file->read(2);
        m_pos+=0x40;
        const char* data = bytes.data();
        // 使用指向数据的指针创建一个QString对象
        QString string = QString::fromUtf8(data);
        return string;

    }

private:
    bool canRead(int size) {
        if (size < 0 || m_pos < 0 || m_pos > m_byte.size() - size) {
            m_valid = false;
            m_error = QString("moc3 数据截断: pos %1 size %2 (file %3)")
                          .arg(m_pos).arg(size).arg(m_byte.size());
            return false;
        }
        return true;
    }
    bool m_valid = true;
    QString m_error;

};
}
#endif // CFILEREADER_H
