#include "cmesh.h"
#include <QOpenGLContext>

CMesh::CMesh() {
}

CMesh::~CMesh()
{
    delete textureMaskFrame;
    textureMaskFrame = nullptr;
    if (!owningContext() || !owningContext()->isValid() ||
        QOpenGLContext::currentContext() != owningContext() || !isInitialized()) {
        m_EBO = 0;
        m_VAO = 0;
        m_vboIdList.clear();
        return;
    }
    if (m_EBO != 0) glDeleteBuffers(1, &m_EBO);
    if (!m_vboIdList.isEmpty()) glDeleteBuffers(m_vboIdList.size(), m_vboIdList.constData());
    if (m_VAO != 0) glDeleteVertexArrays(1, &m_VAO);
}

namespace {
void clearMeshBuffers(QOpenGLFunctions_3_3_Core* functions, GLuint& vao, GLuint& ebo, QVector<unsigned int>& vbos)
{
    if (!QOpenGLContext::currentContext()) {
        ebo = 0;
        vao = 0;
        vbos.clear();
        return;
    }
    if (ebo != 0) functions->glDeleteBuffers(1, &ebo);
    if (!vbos.isEmpty()) functions->glDeleteBuffers(vbos.size(), vbos.constData());
    if (vao != 0) functions->glDeleteVertexArrays(1, &vao);
    ebo = 0;
    vao = 0;
    vbos.clear();
}
}



void CMesh::setGlOwningContext(const QOpenGLContext *context)
{
    if (!context || !context->isValid() || QOpenGLContext::currentContext() != context ||
        (owningContext() && owningContext() != context)) return;
    if (!owningContext() && (m_VAO || m_EBO || !m_vboIdList.isEmpty() || textureMaskFrame)) return;
    initializeOpenGLFunctions();
}

void CMesh::SetConstPoint(float *uvVertex, float *positionVertex, unsigned int *indices)
{
    if (!owningContext() || !owningContext()->isValid() || QOpenGLContext::currentContext() != owningContext() ||
        !isInitialized()) return;
    clearMeshBuffers(this, m_VAO, m_EBO, m_vboIdList);
    IsEbo =true;
    unsigned int VBO;
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &VBO);

    glBindVertexArray(m_VAO);//绑定VBO和VAO对象

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    m_vboIdList.push_back(VBO);
    glBufferData(GL_ARRAY_BUFFER,8 * sizeof(float), positionVertex, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);


    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    m_vboIdList.push_back(VBO);
    glBufferData(GL_ARRAY_BUFFER, 8 * sizeof(float), uvVertex, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);



    glGenBuffers(1, &m_EBO);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, 6 * sizeof(unsigned int), indices, GL_STATIC_DRAW);


    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);//解绑VAO
}

void CMesh::SetConstPointEBO(float *uvVertex, float *positionVertex)
{
    if (!owningContext() || !owningContext()->isValid() || QOpenGLContext::currentContext() != owningContext() ||
        !isInitialized()) return;
    clearMeshBuffers(this, m_VAO, m_EBO, m_vboIdList);
    IsEbo =false;
    unsigned int VBO;
    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &VBO);

    glBindVertexArray(m_VAO);//绑定VBO和VAO对象

    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    m_vboIdList.push_back(VBO);
    glBufferData(GL_ARRAY_BUFFER,8 * sizeof(float), positionVertex, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);


    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    m_vboIdList.push_back(VBO);
    glBufferData(GL_ARRAY_BUFFER, 8 * sizeof(float), uvVertex, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void*)0);



    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);//解绑VAO
}


void CMesh::SetImage(const QString &name)
{
    if (!owningContext() || !owningContext()->isValid() || QOpenGLContext::currentContext() != owningContext() ||
        !isInitialized()) return;
    delete textureMaskFrame;
    textureMaskFrame = nullptr;
    const QImage image(name);
    if (!image.isNull()) textureMaskFrame = new QOpenGLTexture(image.mirrored());
}

void CMesh::Draw()
{
    if (!owningContext() || !owningContext()->isValid() || QOpenGLContext::currentContext() != owningContext() ||
        !isInitialized()) return;
    if (m_VAO == 0 || !textureMaskFrame) return;
    glBindVertexArray(m_VAO);//
    glActiveTexture(GL_TEXTURE0);
    textureMaskFrame->bind(0);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);

}

