#include "cdrawrotation.h"

CDrawRotation::CDrawRotation() {

    m_IsDraw =false;
    initializeOpenGLFunctions();
    setOwningContext(QOpenGLContext::currentContext());
    shader = new QOpenGLShaderProgram();
    bool success = shader->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shader_rotation.vert");
    if (!success) {
        qDebug() << "shaderProgram addShaderFromSourceFile failed!" << shader->log();
    }
    success = shader->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shader_rotation.frag");
    if (!success) {
        qDebug() << "shaderProgram addShaderFromSourceFile failed!" <<shader->log();
    }

    success = shader->link();
    if(!success) {
        qDebug() << "shaderProgram link failed!" << shader->log();
    }

    m_IsDraw = false;
    m_rclTex = new QOpenGLTexture(QImage(":/awesomeface.png").mirrored());
    m_dirTex = new QOpenGLTexture(QImage(":/read.png").mirrored());

    m_IsDraw =false;

}

CDrawRotation::~CDrawRotation()
{
    delete shader;
    delete m_rclTex;
    delete m_dirTex;
}

void CDrawRotation::Init(CRotationNode *node)
{
    this->base_angles =node->GetBaseAngles();
    this->cur =node->GetCurKeyForm();
    ClaRect();//计算当前rect
}


void CDrawRotation::Draw(const QMatrix4x4 &mv)
{

    if(m_IsDraw){
        // 开始绘制
        //开启混合
        shader->bind();
        shader->setUniformValue("projection",mv);
        shader->setUniformValue("image",0);

        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),vetrclPos.data() );
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),vetTex.data() );

        glActiveTexture(GL_TEXTURE0);
        m_rclTex->bind(0);
        glDrawElements(GL_TRIANGLES, vetIndex.size(), GL_UNSIGNED_SHORT,vetIndex.data());






        shader->bind();
        shader->setUniformValue("projection",mv);
        shader->setUniformValue("image",0);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), vetdirPos.data());

        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),vetTex.data() );

        glActiveTexture(GL_TEXTURE0);
        m_dirTex->bind(0);
        glDrawElements(GL_TRIANGLES, vetIndex.size(), GL_UNSIGNED_SHORT,vetIndex.data());




    }









}

void CDrawRotation::SetDraw(bool flag)
{
    m_IsDraw = flag;
}

void CDrawRotation::InitOpenGl()
{

    initializeOpenGLFunctions();
    setOwningContext(QOpenGLContext::currentContext());


}



void CDrawRotation::ClaRect()
{

    float x = cur.Getorigin_x();
    float y = cur.Getorigin_y();//知道坐标点
    float sideLength = 0.05;
    float halfSide = sideLength / 2.0;
    vetrclPos.resize(8,0.0f);

    vetrclPos[0] = x - halfSide;vetrclPos[1] = y + halfSide;
    vetrclPos[2] = x - halfSide;vetrclPos[3] = y - halfSide;
    vetrclPos[4] = x + halfSide;vetrclPos[5] = y - halfSide;
    vetrclPos[6] = x + halfSide;vetrclPos[7] = y + halfSide;





    //--------------------------------------------
    float wid = 0.01;
    float hid = 0.1;
    std::vector<QPointF> vetPoint;
    vetPoint.push_back(QPointF(x - wid,y));
    vetPoint.push_back(QPointF(x - wid,y - hid));
    vetPoint.push_back(QPointF(x + wid,y - hid));
    vetPoint.push_back(QPointF(x + wid,y));

    vetdirPos.resize(8,0.0f);

    double  d = 0.017453292F;//弧度
    double  angle = (base_angles + cur.Getangles()) /** d*/;
    QTransform transform;
    transform.translate(x, y);  // 将中心点移动到原点
    transform.rotate(angle);  // 旋转角度
    transform.translate(-x, -y);  // 将中心点移回原位置
    for(int i = 0 ;i < vetPoint.size() ;i++){
        QPointF temp = transform.map(vetPoint[i]);
        vetdirPos[i*2] =temp.x();
        vetdirPos[i*2+1] =temp.y();
    }







    //------------------------------------

    vetIndex.resize(6,0);
    //求解这四个值
    vetIndex[0] = 0;vetIndex[1] = 1;vetIndex[2] = 3;
    vetIndex[3] = 1;vetIndex[4] = 2;vetIndex[5] = 3;

    vetTex.resize(8,0.0f);
    vetTex[0] = 0.0f; vetTex[1] = 0.0f;
    vetTex[2] = 1.0f; vetTex[3] = 0.0f;
    vetTex[4] = 1.0f; vetTex[5] = 1.0f;
    vetTex[6] = 0.0f; vetTex[7] = 1.0f;








}
