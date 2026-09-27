#ifndef CMOCRUNTIMESNAPSHOT_H
#define CMOCRUNTIMESNAPSHOT_H

#include <QByteArray>
#include <QString>
#include <memory>

namespace live2dstruct {
class CMocRuntimeSnapshot
{
public:
    static bool Create(const QByteArray& bytes, CMocRuntimeSnapshot& result, QString* error = nullptr);
    bool IsValid() const noexcept;
    const QByteArray& Bytes() const noexcept;
    const QByteArray& Digest() const noexcept;

private:
    struct Data;
    std::shared_ptr<const Data> m_data;
};
}

#endif
