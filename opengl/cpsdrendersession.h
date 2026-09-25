#ifndef CPSDRENDERSESSION_H
#define CPSDRENDERSESSION_H

#include "cpsdfiletask.h"
#include "coffscreenbuffer.h"
#include "framework/Math/CubismMatrix44.hpp"
#include <QObject>
#include <QPointer>
#include <QVector>

namespace Live2D { namespace Cubism { namespace Framework {
class CubismModel;
namespace Rendering { class CubismRenderer_OpenGLES2; }
}}}

class CPsdRenderSession
{
    friend class PsdCooperativeGlTests;
public:
    struct Entry {
        CPsdExportItem::Type type = CPsdExportItem::Type::Layer;
        int drawable = -1;
        QString name;
        float opacityOverride = -1;
    };
    using Validate = std::function<bool(QString*)>;
    CPsdRenderSession(QObject* owner, QOpenGLContext* context, Live2D::Cubism::Framework::CubismModel* model,
                      Live2D::Cubism::Framework::Rendering::CubismRenderer_OpenGLES2* renderer,
                      const QSize& canvas, const Live2D::Cubism::Framework::CubismMatrix44& matrix,
                      QVector<Entry> entries, std::function<bool()> makeCurrent, Validate validate, CExportControl* control);
    ~CPsdRenderSession();
    bool Begin(bool includeHidden, QString* error = nullptr);
    bool Next(CPsdExportItem& item, QString* error = nullptr);
    bool Finish(bool complete, QString* error = nullptr);
    void DetachControl();

private:
    bool Activate(QString* error);
    bool Fail(QString* error, const QString& reason);
    QPointer<QObject> m_owner;
    QPointer<QOpenGLContext> m_context;
    Live2D::Cubism::Framework::CubismModel* m_model;
    Live2D::Cubism::Framework::Rendering::CubismRenderer_OpenGLES2* m_renderer;
    QSize m_canvas;
    Live2D::Cubism::Framework::CubismMatrix44 m_matrix;
    QVector<Entry> m_entries;
    std::function<bool()> m_makeCurrent;
    Validate m_validate;
    CExportControl* m_control;
    COffscreenBuffer m_buffer;
    qsizetype m_index = 0;
    bool m_started = false;
    bool m_includeHidden = false;
};

#endif
