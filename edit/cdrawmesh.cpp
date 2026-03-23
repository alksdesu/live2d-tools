#include "cdrawmesh.h"

#include<opencv2/opencv.hpp>



CDrawMesh::CDrawMesh(int ebosize,const short* vetebo,int uvsize,const float* vetuv,const float* vetpos)
{
    initializeOpenGLFunctions();
    setOwningContext(QOpenGLContext::currentContext());
    outerColor = QVector4D(0.0f,0.0f,0.0f,1.0f);//黑色
    CreateShader();
    mesh = new CTrianglMesh(ebosize,vetebo,uvsize, vetuv, vetpos);
    IsDraw = false;
    IsOver =false;
}

CDrawMesh::~CDrawMesh()
{
    delete shader;
    delete mesh;
}



void CDrawMesh::CreateShader()
{
    shader = new QOpenGLShaderProgram();
    bool success = shader->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shader_meshPoint_circle.vert");
    if (!success) {
        qDebug() << "shaderProgram addShaderFromSourceFile failed!" << shader->log();
    }
    success = shader->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shader_meshPoint_circle.frag");
    if (!success) {
        qDebug() << "shaderProgram addShaderFromSourceFile failed!" <<shader->log();
    }

    success = shader->link();
    if(!success) {
        qDebug() << "shaderProgram link failed!" << shader->log();

    }







}

void CDrawMesh::Draw(const QMatrix4x4 &projection)
{
    //优化方案传入GUP中的数据最好是整块进行传输
    //qDebug()<< "CDrawMesh::Draw" <<projection;
    //mesh->normalizing();
    if(IsDraw || IsOver){
        this->projection = projection;
        const std::vector<CPoint*>& poins = mesh->GetWolrd();
        const std::vector<CEdge*>& line = mesh->GetEdge();
        for(int i = 0 ;i < poins.size();i++){
            if(poins[i]->GetIsSelect()){//被选择了就
                outerColor = QVector4D(1.0,0.0,0.0,1.0);
            }
            else if(IsOver && !IsDraw){
                outerColor = QVector4D(0.0,1.0,0.0,1.0);
            }
            else{
                outerColor = QVector4D(0.0,0.0,0.0,1.0);
            }
            DrawPoint(poins[i]->GetPoint());
        }
        if(IsOver && !IsDraw){
            outerColor = QVector4D(0.0,1.0,0.0,1.0);//绿色
        }
        else{
            outerColor = QVector4D(0.0,0.0,0.0,1.0);
        }

        for(int i = 0 ;i <line.size() ;i++){
            DrawLine(poins[line[i]->start]->GetPoint(),poins[line[i]->end]->GetPoint() );
        }

    }


}

//传入世界坐标系点
void CDrawMesh::MoveSelect(const QVector2D &vet)
{
    QMatrix4x4 move;
    move.setToIdentity();
    move.translate(QVector3D(vet,0));//世界坐标系的位置
    std::vector<CPoint*>& point = mesh->GetCpoint();
    for(int i = 0;i < point.size() ;i++){
        if(point[i]->GetIsSelect()){
            const QVector2D& p =  point[i]->GetPoint();
            QVector4D out = move * QVector4D(p,0,1.0f);//移动点

            point[i]->SetPoint(QVector2D(out));
        }
    }
}

//世界坐标系
void CDrawMesh::SetSelect(const QRectF &rect)
{
    m_vetSelectPoint.clear();
    std::vector<CPoint*>& point = mesh->GetCpoint();
    for(int i = 0 ;i < point.size() ;i++){
        if(rect.contains(point[i]->GetPoint().toPointF()) ){
            point[i]->SetIsSelect(true);
            m_vetSelectPoint.push_back(point[i]);//这里获取拷贝
        }else{
            if(point[i]->GetIsSelect()){
                point[i]->SetIsSelect(false);
            };
        }
    }
}

void CDrawMesh::CancelSelect()
{
    std::vector<CPoint*>& point = mesh->GetCpoint();
    for(int i = 0 ;i < point.size() ;i++){
        point[i]->SetIsSelect(false);
    }
    m_vetSelectPoint.clear();
}

const std::vector<CPoint*> &CDrawMesh::GetSelectPoint() const
{
    return m_vetSelectPoint;//应该是选择的点进行移动
}

void CDrawMesh::AddPoint(const QVector2D &vet)
{
    mesh->AddPoint(vet);
}

bool CDrawMesh::ClickMesh(const QPointF &point)
{
    //当前点选中了mesh吗？
    return mesh->GetContentPoint(point);//->GetRectF().contains(point);
}







void CDrawMesh::DrawPoint(const QVector2D &point)
{


    shader->bind();
    shader->setUniformValue("projection",projection);
    shader->setUniformValue("outerColor",outerColor);

    float vertices[2];
    vertices[0] =point.x();
    vertices[1] =point.y();

    glPointSize(4); // 设置点的大小
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), vertices);
    glDrawArrays(GL_POINTS, 0, 1);

}

void CDrawMesh::DrawLine(const QVector2D &pointA, const QVector2D &pointB)
{
    // 开始绘制
    shader->bind();
    shader->setUniformValue("projection",projection);
    shader->setUniformValue("outerColor",outerColor);
    float vertices[4];
    vertices[0] = pointA.x();
    vertices[1] = pointA.y();
    vertices[2] = pointB.x();
    vertices[3] = pointB.y();

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), vertices);
    glDrawArrays(GL_LINES, 0, 2);
}

void CDrawMesh::DrawMesh(const CTrianglMesh& mesh)
{
    // meshshader->bind();
    // meshshader->setUniformValue("image",0);//设置图像的通道


    // glActiveTexture(GL_TEXTURE0);
    // meshtexture->bind(0);

    // //pos
    // glEnableVertexAttribArray(0);
    // glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),mesh.GetPos() );

    // //tex
    // glEnableVertexAttribArray(1);
    // glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),mesh.GetUV());


    // glDrawElements(GL_TRIANGLES, mesh.GetEBOSize(),GL_UNSIGNED_SHORT ,mesh.GetEBOIndex());



}



