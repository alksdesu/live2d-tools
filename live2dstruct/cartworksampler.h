#ifndef CARTWORKSAMPLER_H
#define CARTWORKSAMPLER_H

#include "cmodelartwork.h"
#include <QImage>

class CExportControl;

namespace live2dstruct {
struct CArtworkSampleLimits {
    qint64 maximumSourcePixels = 128LL * 1024 * 1024;
    qint64 maximumDestinationPixels = 128LL * 1024 * 1024;
    qint64 maximumTotalPixels = 256LL * 1024 * 1024;
    qint64 maximumSamples = 512LL * 1024 * 1024;
    int maximumDimension = 32768;
};

class CArtworkSampler
{
public:
    static bool Render(const QImage& source, const QTransform& destinationToSource, const QSize& destination,
                       CModelArtwork::Boundary boundary, CModelArtwork::Filter filter, quint8 layerOpacity,
                       QImage& result, QString* error = nullptr, CExportControl* control = nullptr,
                       const CArtworkSampleLimits& limits = {});
};
}

#endif
