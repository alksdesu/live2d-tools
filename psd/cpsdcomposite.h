#ifndef CPSDCOMPOSITE_H
#define CPSDCOMPOSITE_H

#include "psd/psdSamples/Exporter/PsdExport.h"
#include <QString>
#include <memory>

class CExportControl;

class CPsdComposite
{
public:
    static constexpr quint64 MaximumWorkingBytes = 32ULL * 1024 * 1024;
    static constexpr quint64 MaximumSamples = 1024ULL * 1024 * 1024;
    CPsdComposite();
    ~CPsdComposite();
    // Borrow encoded channels until Write completes to avoid full-image copies.
    bool Prepare(const psd::ExportDocument& document, CExportControl* control, QString* error = nullptr);
    bool IsSupported() const;
    bool Write(const psd::MergedImageSink& sink, QString* error = nullptr);

private:
    struct State;
    std::unique_ptr<State> m_state;
    bool m_busy = false;
};

#endif
