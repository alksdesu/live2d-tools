#ifndef CPSDORIGINAL_H
#define CPSDORIGINAL_H

#include "cpsddirectory.h"
#include "cpsdreader.h"
#include <memory>

class CPsdOriginal
{
public:
    static bool Prepare(const QByteArray& bytes, const QByteArray& expectedDigest, CPsdOriginal& result,
                        QString* error = nullptr, CExportControl* control = nullptr,
                        const CPsdReadLimits& limits = CPsdReadLimits::Directory());
    bool IsValid() const { return bool(m_directory); }
    const CPsdDirectory* Directory() const { return m_directory.get(); }
    bool Write(const QString& path, CExportControl* control = nullptr, QString* error = nullptr) const;

private:
    std::shared_ptr<const CPsdDirectory> m_directory;
};

#endif
