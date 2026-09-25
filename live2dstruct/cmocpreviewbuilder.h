#ifndef CMOCPREVIEWBUILDER_H
#define CMOCPREVIEWBUILDER_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>
#include <memory>

namespace live2dstruct {
class CMocPreviewBuilder
{
public:
    bool Initialize(const QByteArray& baseline, QString* error = nullptr);
    QByteArray SourceDigest() const;
    bool Build(const QHash<QString, QVector<QVector<float>>>& overrides,
               QByteArray& result, QString* error = nullptr) const;

private:
    struct Snapshot;
    std::shared_ptr<const Snapshot> m_snapshot;
};
}

#endif
