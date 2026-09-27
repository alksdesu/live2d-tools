#ifndef CMOTIONKEYCLIPBOARD_H
#define CMOTIONKEYCLIPBOARD_H

#include "cmotiontimeline.h"

class CMotionKeyClipboard
{
public:
    static constexpr qint64 MaximumBytes = 16LL * 1024 * 1024;
    static constexpr const char* MimeType = "application/x-live2d-motion-keyframes+json";
    struct Result {
        CMotionTimeline timeline;
        CMotionPointSelection selection;
        bool changed = false;
        bool convertedToUnrestricted = false;
    };
    static bool Capture(const CMotionTimeline& timeline, const CMotionPointSelection& selection,
                        QByteArray& result, QString* error = nullptr);
    static bool Paste(const CMotionTimeline& timeline, const QByteArray& bytes, double pasteTime,
                      const std::optional<CMotionTrack>& destination, bool replaceCoincident,
                      Result& result, QString* error = nullptr);
};

#endif
