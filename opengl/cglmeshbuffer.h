#ifndef CGLMESHBUFFER_H
#define CGLMESHBUFFER_H

#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QPointer>
#include <array>
#include <vector>

class CGlMeshBuffer
{
public:
    static constexpr int MaxVertices = 1000000;
    static constexpr int MaxIndices = 3000000;

    CGlMeshBuffer() = default;
    ~CGlMeshBuffer();
    CGlMeshBuffer(const CGlMeshBuffer&) = delete;
    CGlMeshBuffer& operator=(const CGlMeshBuffer&) = delete;

    bool SetContext(const QOpenGLContext* context);
    bool Upload(const float* positions, int vertexCount, const float* uvs,
                const quint16* indices, int indexCount);
    bool Draw(GLenum mode);
    bool Clear();
    GLuint GetVertexArray() const;

private:
    struct Buffers
    {
        GLuint vertexArray = 0;
        GLuint vertices = 0;
        GLuint indices = 0;
        GLint vertexCapacity = 0;
        GLint indexCapacity = 0;
    };

    QOpenGLFunctions_3_3_Core* Functions() const;
    void Reset();

    QPointer<QOpenGLContext> m_context;
    std::array<Buffers, 2> m_buffers{};
    int m_active = -1;
    std::vector<float> m_positions;
    std::vector<float> m_uvs;
    std::vector<quint16> m_indices;
};

#endif
