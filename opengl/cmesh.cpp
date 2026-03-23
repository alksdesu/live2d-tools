#include "cmesh.h"

CMesh::CMesh() {

    IsEbo =true;
}

CMesh::~CMesh()
{

}



void CMesh::setGlOwningContext(const QOpenGLContext *context)
{
    initializeOpenGLFunctions();
    setOwningContext(context);
}

void CMesh::SetConstPoint(float *uvVertex, float *positionVertex, unsigned int *indices)
{
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
    textureMaskFrame = new QOpenGLTexture(QImage(name).mirrored());
}

void CMesh::Draw()
{
    glBindVertexArray(m_VAO);//
    glActiveTexture(GL_TEXTURE0);
    textureMaskFrame->bind(0);
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, NULL);

}




