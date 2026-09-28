#ifndef CPSDREADDOCUMENT_H
#define CPSDREADDOCUMENT_H

#include "cpsddirectory.h"
#include <memory>

class CExportControl;
class CPsdReader;

class CPsdReadDocument
{
public:
    bool IsValid() const { return bool(m_data); }
    const CPsdDirectory* Directory() const;
    bool ReadLayer(int recordIndex, QImage& result, QString* error = nullptr, CExportControl* control = nullptr) const;

private:
    struct Data;
    std::shared_ptr<const Data> m_data;
    friend class CPsdReader;
};

#endif
