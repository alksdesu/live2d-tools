#include "cdrawwarp.h"



void CDrawWarp::ClaVetIndex()
{
    // Live2D 网格布局：columns 是 X 方向，rows 是 Y 方向
    // 顶点按行优先存储：index = x + y * (columns + 1)
    // 网格有 columns * rows 个 quad
    int stride = columns + 1; // 每行顶点数
    vetIndex.resize(columns * rows * 4, 0);
    int num = 0;
    for(int y = 0; y < rows; y++){
        for(int x = 0; x < columns; x++){
            vetIndex[num++] = x     + y     * stride;
            vetIndex[num++] = x     + (y+1) * stride;
            vetIndex[num++] = (x+1) + (y+1) * stride;
            vetIndex[num++] = (x+1) + y     * stride;
        }
    }
}

CDrawWarp::CDrawWarp()
{
    initializeOpenGLFunctions();
    setOwningContext(QOpenGLContext::currentContext());

    m_IsDraw =false;
    shader = new QOpenGLShaderProgram();
    bool success = shader->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shader_warp.vert");
    if (!success) {
        qDebug() << "shaderProgram addShaderFromSourceFile failed!" << shader->log();
    }
    success = shader->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shader_warp.frag");
    if (!success) {
        qDebug() << "shaderProgram addShaderFromSourceFile failed!" <<shader->log();
    }

    success = shader->link();
    if(!success) {
        qDebug() << "shaderProgram link failed!" << shader->log();

    }
    color = QVector3D(0,0,1);
    m_IsDraw = false;
}

CDrawWarp::~CDrawWarp()
{
    delete shader;
}

void CDrawWarp::Init(CWarpNode* node)
{
    this->rows = node->GetRows();
    this->columns = node->GetColumns();
    this->curWarp = node->GetCurKeyForm();
    ClaVetIndex();
}

void CDrawWarp::UpdatePositions(CWarpNode* node)
{
    this->curWarp = node->GetCurKeyForm();
}
void CDrawWarp::InitTest(int i,CWarpNode* node)
{
    this->rows = node->GetRows();
    this->columns = node->GetColumns();
    this->curWarp = node->GetKeyFormS()[i];
    ClaVetIndex();
}


void CDrawWarp::Draw(const QMatrix4x4 &mv)
{
    if(m_IsDraw){
        // 开始绘制
        shader->bind();
        shader->setUniformValue("projection",mv);
        shader->setUniformValue("spriteColor",color);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        // 设置线的宽度为5.0像素
        glLineWidth(1.0f);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), curWarp.GetPosition().data());


        glDrawElements(GL_QUADS, vetIndex.size(), GL_UNSIGNED_SHORT,vetIndex.data());
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    }


}

void CDrawWarp::InitOpenGl()
{
    initializeOpenGLFunctions();
    setOwningContext(QOpenGLContext::currentContext());

}

void CDrawWarp::SetDraw(bool flag)
{
    m_IsDraw = flag;
}
