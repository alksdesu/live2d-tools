#ifndef CONIONSKINRENDERER_H
#define CONIONSKINRENDERER_H

#include "live2dstruct/conionskin.h"
#include <QMatrix4x4>
#include <QRect>
#include <QVector4D>
#include <memory>

class LAppModel;
class COnionSkinRenderer
{
public:
    struct Statistics {
        quint64 modelBuilds = 0, plans = 0, samples = 0, composites = 0, cacheDraws = 0;
    };
    COnionSkinRenderer();
    ~COnionSkinRenderer();
    bool Render(LAppModel& model, const live2dstruct::COnionSkin::Request& request,
                const QMatrix4x4& matrix, const QRect& viewport, const QVector4D& background,
                quint64 generation, live2dstruct::COnionSkin::Status& status, QString* error = nullptr);
    void Invalidate() noexcept;
    Statistics Counters() const;
private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
#endif
