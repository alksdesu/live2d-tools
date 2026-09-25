#ifndef CPSDLIST_H
#define CPSDLIST_H
#include <QString>
#include <QImage>
#include "psd/psdSamples/PsdDocumentation.h"
#include "psd/psdSamples/Interfaces/PsdMallocAllocator.h"
#include "psd/psdSamples/Interfaces/PsdFile.h"
#include "psd/psdSamples/Exporter/PsdExport.h"
using namespace   psd;
class CExportControl;

class CPsdList
{
public:
    CPsdList();
    ~CPsdList();
    void SetControl(CExportControl* control) { m_control = control; }
    CExportControl* Control() const { return m_control; }
    bool SetPsd(const QString& name,int w,int h,bool cubismCompat = false);
    bool AddLayer(const QString& name,const QImage& image,const QRect& rect);
    bool AddLayer(const QString& name, const QImage& image, const QRect& rect,
                  uint8_t opacity, uint32_t blendModeKey, bool isClipped,
                  bool isPremultiplied = false);
    bool AddGroupStart();
    bool AddGroupEnd(const char* name);
    bool CreatePsd();

private:
    bool CanAppendLayer();
    bool AddGroupChannels(unsigned int index);
    bool Fail(const QString& error);
    CExportControl* m_control = nullptr;
    bool m_failed = false;
    unsigned int m_canvasWidth;
    unsigned int m_canvasHeight;
    unsigned int m_bitsPerChannel;
    exportColorMode::Enum m_colorMode;
    QString m_docname;
    MallocAllocator allocator;
    ExportDocument *document;
};

#endif // CPSDLIST_H
