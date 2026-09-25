#ifndef CDRAWWARP_H
#define CDRAWWARP_H

#include "cdrawobject.h"
#include "cgridlines.h"
#include "live2dstruct/tree/cwarpnode.h"
#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPointer>
#include <QVector3D>

using namespace live2dstruct;

class CDrawWarp : public CDrawObject
{
public:
    CDrawWarp();
    ~CDrawWarp() override;
    void InitTest(int i, CWarpNode* node);
    void Init(CWarpNode* node);
    void UpdatePositions(CWarpNode* node);
    void SetColor(QVector3D color) { this->color = color; }
    void Draw(const QMatrix4x4& mv) override;
    void InitOpenGl() override;
    void SetDraw(bool flag) override;

private:
    bool EnsureOpenGl();
    CGridLines m_grid;
    bool m_IsDraw = false;
    bool m_buffersDirty = true;
    QOpenGLShaderProgram* shader = nullptr;
    QPointer<QOpenGLContext> m_context;
    QOpenGLVertexArrayObject m_vertexArray;
    QOpenGLBuffer m_vertexBuffer{QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer m_indexBuffer{QOpenGLBuffer::IndexBuffer};
    QVector3D color{0, 0, 1};
};

#endif
