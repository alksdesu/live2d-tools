#include "cglmeshbuffer.h"

#include <QOpenGLVersionFunctionsFactory>
#include <QScopeGuard>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace {
template<typename T>
bool Equal(const std::vector<T>& previous, const T* values, size_t count)
{
    return previous.size() == count && (count == 0 || std::memcmp(previous.data(), values, count * sizeof(T)) == 0);
}

bool ValidMode(GLenum mode)
{
    return mode == GL_POINTS || mode == GL_LINES || mode == GL_LINE_LOOP || mode == GL_LINE_STRIP
        || mode == GL_TRIANGLES || mode == GL_TRIANGLE_STRIP || mode == GL_TRIANGLE_FAN;
}

bool WriteBuffer(QOpenGLFunctions_3_3_Core& gl, GLenum target, GLint bytes,
                 const void* data, GLint& capacity)
{
    if (bytes == 0) return true;
    if (capacity < bytes) gl.glBufferData(target, bytes, data, GL_DYNAMIC_DRAW);
    else gl.glBufferSubData(target, 0, bytes, data);
    GLint actualSize = 0;
    gl.glGetBufferParameteriv(target, GL_BUFFER_SIZE, &actualSize);
    if (gl.glGetError() != GL_NO_ERROR || actualSize < bytes) return false;
    capacity = actualSize;
    return true;
}
}

CGlMeshBuffer::~CGlMeshBuffer()
{
    Clear();
}

QOpenGLFunctions_3_3_Core* CGlMeshBuffer::Functions() const
{
    if (!m_context || !m_context->isValid() || QOpenGLContext::currentContext() != m_context)
        return nullptr;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(m_context);
    return gl && gl->initializeOpenGLFunctions() ? gl : nullptr;
}

bool CGlMeshBuffer::SetContext(const QOpenGLContext* context)
{
    if (!context || !context->isValid() || QOpenGLContext::currentContext() != context
        || (m_context && m_context != context))
        return false;
    auto* gl = QOpenGLVersionFunctionsFactory::get<QOpenGLFunctions_3_3_Core>(const_cast<QOpenGLContext*>(context));
    if (!gl || !gl->initializeOpenGLFunctions()) return false;
    if (!m_context) Reset();
    m_context = const_cast<QOpenGLContext*>(context);
    return true;
}

bool CGlMeshBuffer::Upload(const float* positions, int vertexCount, const float* uvs,
                          const quint16* indices, int indexCount)
{
    if (vertexCount < 0 || vertexCount > MaxVertices || indexCount < 0 || indexCount > MaxIndices
        || vertexCount > std::numeric_limits<GLsizei>::max() || indexCount > std::numeric_limits<GLsizei>::max()
        || (vertexCount > 0 && !positions) || (indexCount > 0 && !indices)
        || (vertexCount == 0 && (positions || uvs || indices || indexCount))
        || (indexCount == 0 && indices))
        return false;
    const size_t values = static_cast<size_t>(vertexCount) * 2;
    const size_t vertexBytes = values * sizeof(float) * (uvs ? 2 : 1);
    const size_t indexBytes = static_cast<size_t>(indexCount) * sizeof(quint16);
    if (vertexBytes > static_cast<size_t>(std::numeric_limits<GLint>::max())
        || indexBytes > static_cast<size_t>(std::numeric_limits<GLint>::max())
        || vertexBytes > static_cast<size_t>(std::numeric_limits<GLsizeiptr>::max())
        || indexBytes > static_cast<size_t>(std::numeric_limits<GLsizeiptr>::max()))
        return false;
    for (size_t i = 0; i < values; ++i)
        if (!std::isfinite(positions[i]) || (uvs && !std::isfinite(uvs[i]))) return false;
    for (int i = 0; i < indexCount; ++i)
        if (indices[i] >= vertexCount) return false;
    auto* gl = Functions();
    if (!gl || gl->glGetError() != GL_NO_ERROR) return false;
    if (vertexCount == 0) return Clear();
    if (Equal(m_positions, positions, values) && Equal(m_uvs, uvs, uvs ? values : 0)
        && Equal(m_indices, indices, static_cast<size_t>(indexCount)))
        return true;

    try {
        std::vector<float> nextPositions(positions, positions + values);
        std::vector<float> nextUvs;
        std::vector<quint16> nextIndices;
        if (uvs) nextUvs.assign(uvs, uvs + values);
        if (indexCount) nextIndices.assign(indices, indices + indexCount);
        std::vector<float> packed;
        const float* vertexData = nextPositions.data();
        if (uvs) {
            packed.resize(values * 2);
            for (int vertex = 0; vertex < vertexCount; ++vertex) {
                const size_t input = static_cast<size_t>(vertex) * 2, output = input * 2;
                packed[output] = positions[input];
                packed[output + 1] = positions[input + 1];
                packed[output + 2] = uvs[input];
                packed[output + 3] = uvs[input + 1];
            }
            vertexData = packed.data();
        }

        GLint previousVertexArray = 0, previousArrayBuffer = 0;
        gl->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
        gl->glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &previousArrayBuffer);
        const auto restore = qScopeGuard([&] {
            gl->glBindVertexArray(static_cast<GLuint>(previousVertexArray));
            gl->glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(previousArrayBuffer));
        });
        const int next = m_active == 0 ? 1 : 0;
        Buffers& buffers = m_buffers[static_cast<size_t>(next)];
        if (!buffers.vertexArray) gl->glGenVertexArrays(1, &buffers.vertexArray);
        if (!buffers.vertices) gl->glGenBuffers(1, &buffers.vertices);
        if (!buffers.indices) gl->glGenBuffers(1, &buffers.indices);
        const GLenum creationError = gl->glGetError();
        if (!buffers.vertexArray || !buffers.vertices || !buffers.indices || creationError != GL_NO_ERROR)
            return false;
        gl->glBindVertexArray(buffers.vertexArray);
        gl->glBindBuffer(GL_ARRAY_BUFFER, buffers.vertices);
        if (!WriteBuffer(*gl, GL_ARRAY_BUFFER, static_cast<GLint>(vertexBytes), vertexData, buffers.vertexCapacity))
            return false;
        gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers.indices);
        if (!WriteBuffer(*gl, GL_ELEMENT_ARRAY_BUFFER, static_cast<GLint>(indexBytes),
                         nextIndices.data(), buffers.indexCapacity))
            return false;
        const GLsizei stride = static_cast<GLsizei>((uvs ? 4 : 2) * sizeof(float));
        gl->glEnableVertexAttribArray(0);
        gl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, nullptr);
        if (uvs) {
            gl->glEnableVertexAttribArray(1);
            gl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                                     reinterpret_cast<const void*>(2 * sizeof(float)));
        } else {
            gl->glDisableVertexAttribArray(1);
        }
        if (gl->glGetError() != GL_NO_ERROR) return false;
        m_active = next;
        m_positions.swap(nextPositions);
        m_uvs.swap(nextUvs);
        m_indices.swap(nextIndices);
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    } catch (const std::length_error&) {
        return false;
    }
}

bool CGlMeshBuffer::Draw(GLenum mode)
{
    auto* gl = Functions();
    if (!gl || !ValidMode(mode) || gl->glGetError() != GL_NO_ERROR) return false;
    if (m_positions.empty()) return true;
    if (m_active < 0) return false;
    GLint previousVertexArray = 0;
    gl->glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVertexArray);
    const auto restore = qScopeGuard([&] { gl->glBindVertexArray(static_cast<GLuint>(previousVertexArray)); });
    gl->glBindVertexArray(m_buffers[static_cast<size_t>(m_active)].vertexArray);
    if (m_indices.empty()) gl->glDrawArrays(mode, 0, static_cast<GLsizei>(m_positions.size() / 2));
    else gl->glDrawElements(mode, static_cast<GLsizei>(m_indices.size()), GL_UNSIGNED_SHORT, nullptr);
    return gl->glGetError() == GL_NO_ERROR;
}

bool CGlMeshBuffer::Clear()
{
    if (!m_context) {
        Reset();
        return true;
    }
    auto* gl = Functions();
    if (!gl) return false;
    for (const Buffers& buffers : m_buffers) {
        if (buffers.vertexArray) gl->glDeleteVertexArrays(1, &buffers.vertexArray);
        if (buffers.vertices) gl->glDeleteBuffers(1, &buffers.vertices);
        if (buffers.indices) gl->glDeleteBuffers(1, &buffers.indices);
    }
    Reset();
    return true;
}

GLuint CGlMeshBuffer::GetVertexArray() const
{
    return m_active < 0 ? 0 : m_buffers[static_cast<size_t>(m_active)].vertexArray;
}

void CGlMeshBuffer::Reset()
{
    m_buffers = {};
    m_active = -1;
    m_positions.clear();
    m_uvs.clear();
    m_indices.clear();
}
