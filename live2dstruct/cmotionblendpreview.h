#ifndef CMOTIONBLENDPREVIEW_H
#define CMOTIONBLENDPREVIEW_H

#include "cmotiontimeline.h"

class CMotionBlendPreview
{
public:
    static CMotionPreviewFrame Evaluate(const CMotionTimeline& timeline, double time,
                                       const QMap<QString, float>& baseline,
                                       const QSet<QString>& eyeBlinkIds,
                                       const QSet<QString>& lipSyncIds,
                                       bool loop = false);
};

#endif
