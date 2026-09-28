#ifndef CMODELEXPORTSNAPSHOT_H
#define CMODELEXPORTSNAPSHOT_H

#include "cmodeldisplaynames.h"
#include <QByteArray>
#include <memory>

namespace live2dstruct {
class CModelExportSnapshot
{
public:
    static bool Create(const QByteArray& sourceDigest, const QByteArray& runtimeBytes,
                       const CModelDisplayNames::State& names, CModelExportSnapshot& result, QString* error = nullptr);
    bool IsValid() const noexcept { return bool(m_data); }
    bool Matches(const QByteArray& sourceDigest, const QByteArray& runtimeDigest, QString* error = nullptr) const;
    QString DisplayName(const QString& type, const QString& id) const;
    const CModelDisplayNames::State& DisplayNames() const;
    const QByteArray& SourceDigest() const;
    const QByteArray& RuntimeDigest() const;

private:
    struct Data;
    std::shared_ptr<const Data> m_data;
};
}

#endif
