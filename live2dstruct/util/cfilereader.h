#ifndef CFILEREADER_H
#define CFILEREADER_H
#include <QFile>
#include <QDataStream>
namespace live2dstruct {
class CReader
{
public:
    CReader(const QString& filename );
    ~CReader();
    //QFile* file;
    QByteArray m_byte;
    int m_pos;

    const void seek(int pos) {
        m_pos = pos;
        //file->seek(pos);
    }
    const float readFloat() {

        uint32_t uint_num = 0;
        for (int i = 3; i >= 0; i--) {
            uint_num <<= 8;
            uint_num |= (m_byte[m_pos+i] & 0xFF);
        }
        m_pos+=4;
        float value = *(reinterpret_cast<float*>(&uint_num));
        return  value;
    }
    const int readInt(){
        uint32_t uint_num = 0;
        for (int i = 3; i >= 0; i--) {
            uint_num <<= 8;
            if(m_pos+i >= m_byte.size()){
                 uint_num |= (0 & 0xFF);
            }else{
                uint_num |= (m_byte[m_pos+i] & 0xFF);
            }

        }
        m_pos+=4;
        int value = *(reinterpret_cast<int*>(&uint_num));
        return  value;
    }
    const short readShort() {
        uint32_t uint_num = 0;
        for (int i = 1; i >= 0; i--) {
            uint_num <<= 8;
            uint_num |= (m_byte[m_pos+i] & 0xFF);
        }
        m_pos+=2;
        short value = *(reinterpret_cast<short*>(&uint_num));
        return  value;
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
        QByteArray bytes = m_byte.mid(m_pos,0x40);//file->read(2);
        m_pos+=0x40;
        const char* data = bytes.data();
        // 使用指向数据的指针创建一个QString对象
        QString string = QString::fromUtf8(data);
        return string;

    }

};
}
#endif // CFILEREADER_H
