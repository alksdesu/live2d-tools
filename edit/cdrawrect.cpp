#include "cdrawrect.h"



CDrawRect::CDrawRect(const QVector3D &color)
{
    initializeOpenGLFunctions();
    setOwningContext(QOpenGLContext::currentContext());
    shader = new QOpenGLShaderProgram();
    bool success = shader->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/drawshape.vert");
    if (!success) {
        qDebug() << "shaderProgram addShaderFromSourceFile failed!" << shader->log();
    }
    success = shader->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/drawshape.frag");
    if (!success) {
        qDebug() << "shaderProgram addShaderFromSourceFile failed!" <<shader->log();
    }

    success = shader->link();
    if(!success) {
        qDebug() << "shaderProgram link failed!" << shader->log();

    }
    shader->bind();
    shader->setUniformValue("spriteColor",color);
    m_IsDraw = false;
    m_rect.setRect(0,0,0,0);

}

CDrawRect::~CDrawRect()
{
    delete shader;
}

void CDrawRect::InitOpenGl()
{
    initializeOpenGLFunctions();
    setOwningContext(QOpenGLContext::currentContext());
}

void CDrawRect::SetDraw(bool flag)
{
    m_IsDraw = flag;
}

//如何修改这个点
void CDrawRect::Draw(const QMatrix4x4 &mv)
{
    if(m_IsDraw){
        // 开始绘制
        shader->bind();
        shader->setUniformValue("projection",mv);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        // 设置线的宽度为5.0像素
        glLineWidth(1.0f);
        float vertices[8];

        vertices[0] = m_rect.x();
        vertices[1] = m_rect.y();
        vertices[2] =m_rect.x() ;
        vertices[3] =m_rect.y() + m_rect.height();

        vertices[4] = m_rect.x()+ m_rect.width();
        vertices[5] = m_rect.y() + m_rect.height();
        vertices[6] =m_rect.x() + m_rect.width();
        vertices[7] =m_rect.y() ;

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), vertices);
        glDrawArrays(GL_QUADS, 0, 4);

        // 恢复到填充模式
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);


    }


}

void CDrawRect::SetStartAndEndPos(const QVector2D &startPos, const QVector2D &endPos)
{
    if(startPos.x() == 0 && startPos.y() == 0 && endPos.x() == 0 && endPos.y() == 0){
        m_IsDraw = false;
    }else{
        m_IsDraw = true;
    }

    this->startPos = startPos;
    this->endPos = endPos;
    SetQRectF();
}

void CDrawRect::SetSelectRect(const QRectF &rect)
{
    if(rect.width() == 0 && rect.height() == 0 ){
        m_IsDraw = false;
    }else{
        m_IsDraw = true;
    }

    m_rect = rect;
}

void CDrawRect::SetQRectF()
{
    float x = startPos.x() < endPos.x() ? startPos.x() : endPos.x();
    float y = startPos.y() < endPos.y() ? startPos.y() : endPos.y();
    float w = abs(startPos.x() - endPos.x());
    float h = abs(startPos.y() - endPos.y());
    m_rect = QRectF(x,y,w,h);
}

bool CDrawRect::IsContains(const QPointF &point)
{
    if(m_rect.contains(point)){
        return true;
    }
    return false;

};

//传入的是世界坐标系上的二点
void CDrawRect::Move(const QVector2D &vet)
{
    QMatrix4x4 move;
    move.setToIdentity();
    move.translate(QVector3D(vet,0));//世界坐标系的位置
    QVector4D start2 =move * QVector4D(this->startPos,0,1.0f);
    QVector4D end2 = move * QVector4D(this->endPos,0,1.0f);
    this->startPos = QVector2D(start2);
    this->endPos = QVector2D(end2);
    SetQRectF();
}

const QRectF &CDrawRect::GetQQRectF()
{
    return m_rect;
}
