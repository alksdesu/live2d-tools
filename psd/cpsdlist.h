#ifndef CPSDLIST_H
#define CPSDLIST_H
#include <QString>
#include <QImage>
#include <optional>
#include "cpsdpreview.h"
#include "psd/psdSamples/PsdDocumentation.h"
#include "psd/psdSamples/Interfaces/PsdMallocAllocator.h"
#include "psd/psdSamples/Interfaces/PsdFile.h"
#include "psd/psdSamples/Exporter/PsdExport.h"
using namespace   psd;
class CExportControl;
class CPsdSourceWriter;
struct CPsdSourceLayer;

class CPsdList
{
public:
    CPsdList();
    ~CPsdList();
    void SetControl(CExportControl* control);
    CExportControl* Control() const { return m_control; }
    bool SetPsd(const QString& name,int w,int h,bool cubismCompat = false);
    bool AddLayer(const QString& name,const QImage& image,const QRect& rect);
    bool AddLayer(const QString& name, const QImage& image, const QRect& rect,
                  uint8_t opacity, uint32_t blendModeKey, bool isClipped,
                  bool isPremultiplied = false, std::optional<quint32> layerId = {});
    bool AddEmptyLayer(const QString& name, quint8 opacity = 255, quint32 blendModeKey = 0x6e6f726d,
                       bool isClipped = false, std::optional<quint32> layerId = {});
    bool AddGroupStart();
    bool AddGroupEnd(const char* name);
    bool CreatePsd();
    CPsdPreviewKind Preview() const { return m_preview; }

private:
    friend class CPsdSourceWriter;
    bool SetSourcePsd(const QString& path, const QSize& size, const QByteArray& iccProfile);
    bool AddSourceLayer(const CPsdSourceLayer& layer, const QImage& pixels);
    bool AddSourceDivider();
    bool CanAppendLayer();
    bool AddGroupChannels(unsigned int index);
    bool Fail(const QString& error);
    CExportControl* m_control = nullptr;
    bool m_busy = false;
    bool m_failed = false;
    CPsdPreviewKind m_preview = CPsdPreviewKind::Unknown;
    unsigned int m_canvasWidth;
    unsigned int m_canvasHeight;
    unsigned int m_bitsPerChannel;
    exportColorMode::Enum m_colorMode;
    QString m_docname;
    MallocAllocator allocator;
    ExportDocument *document;
};

#endif // CPSDLIST_H
