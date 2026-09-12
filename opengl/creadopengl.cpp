#include "creadopengl.h"
#include "cmeshbounds.h"

CReadOpengl::CReadOpengl() {


}

void CReadOpengl::setGlOwningContext(const QOpenGLContext *context)
{
    initializeOpenGLFunctions();
    setOwningContext(context);

    // kmesh.setGlOwningContext(owningContext());
    // kmesh.SetConstPointEBO(m_uvVertex,m_positionVertex);


    bool success;
    shaderProgramOffset.addShaderFromSourceFile(QOpenGLShader::Vertex,":/atr.vert");
    shaderProgramOffset.addShaderFromSourceFile(QOpenGLShader::Fragment,":/atr.frag");
    success=shaderProgramOffset.link();
    if(!success)
        qDebug()<<"ERR:"<<shaderProgramOffset.log();

}

void CReadOpengl::UpdateFrame(GLuint tex,int w,int h)
{
    glViewport(0,0,w, h);
    // //这套代码封装起来，比如说
    glDisable(GL_BLEND);//关闭混合，用原来的图层直接覆盖只是单纯去修改帧缓冲区的不透明度
    glUseProgram(shaderProgramOffset.programId());
    //shaderProgramOffset.bind(); //绑定shader
    shaderProgramOffset.setUniformValue("textureMaskFrame",0);//设置图像的通道
    GLuint aPos = shaderProgramOffset.attributeLocation("aPos");
    GLuint aTexCord = shaderProgramOffset.attributeLocation("aTexCord");


    glEnableVertexAttribArray(aPos);
    glVertexAttribPointer(aPos, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), m_positionVertex);

    glEnableVertexAttribArray(aTexCord);
    glVertexAttribPointer(aTexCord, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), m_uvVertex);
    //glBindVertexArray(kmesh.getVAO());

    glActiveTexture(GL_TEXTURE0);//绑定一个cmesh
    glBindTexture(GL_TEXTURE_2D, tex);//绑定tex
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT,m_indices);
    glEnable(GL_BLEND);//因为背景颜色是黑色，导致和屏幕混合的时候产生黑边
    glUseProgram(0);
}


bool CReadOpengl::IsWrite =false;
int CReadOpengl::m_sIndex = 0;
void CReadOpengl::WriteONE(const QString& strFileName,int width ,int height)
{
    if(IsWrite){
        return;
    }
    IsWrite =true;
    Write(strFileName,width,height);
}

void CReadOpengl::Write(const QString &strFileName, int width, int height)
{
    Q_UNUSED(strFileName);
    Q_UNUSED(width);
    Q_UNUSED(height);
}




QImage* CReadOpengl::GetImage(int x, int y, int width, int height)
{
    // QImage 自持像素内存，避免外部缓冲的所有权问题；
    // RGBA8888 每像素 4 字节，scanline 无 padding，glReadPixels 可直写 bits()
    QImage* image = new QImage();
    if (width <= 0 || height <= 0) {
        return image; // 空图像，下游按 zero-size 跳过
    }

    *image = QImage(width, height, QImage::Format_RGBA8888);
    if (image->isNull()) {
        qWarning("CReadOpengl::GetImage: 分配 %dx%d 图像失败", width, height);
        return image;
    }

    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(x, y, width, height, GLenum(GL_RGBA), GLenum(GL_UNSIGNED_BYTE), image->bits());

    return image;
}


void CReadOpengl::PrintImage(int width, int height)
{
    Q_UNUSED(width);
    Q_UNUSED(height);
}

COffScreenNode CReadOpengl::GetScreenNode(CubismModel &model, const int index, const float *mvp, int width, int height, bool IsPrint)
{
    QMatrix4x4 transform;
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            transform(row, column) = mvp[column * 4 + row];
        }
    }
    const QRect rect = MeshBounds::Viewport(model.GetDrawableVertices(index),
        model.GetDrawableVertexCount(index), [&](float x, float y) {
            const QVector3D point = transform.map(QVector3D(x, y, 0));
            return QPointF(point.x(), point.y());
        }, QSize(width, height));
    if (IsPrint) PrintImage(width, height);
    return COffScreenNode(GetImage(rect.x(), rect.y(), rect.width(), rect.height()), rect);
}

void CReadOpengl::BeginDraw(GLint restoreFBO){

    if (_renderTexture == 0)
    {
        return;
    }

    // バックバッファのサーフェイスを記憶しておく
    if (restoreFBO < 0)
    {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &_oldFBO);//封装了，用于获取上一个帧缓冲区
    }
    else
    {
        _oldFBO = restoreFBO;
    }
    // マスク用RenderTextureをactiveにセット
    glBindFramebuffer(GL_FRAMEBUFFER, _renderTexture);//绘制之前先绑定
}

void CReadOpengl::EndDraw()
{
    if (_renderTexture == 0)
    {
        return;
    }

    // 描画対象を戻す
    glBindFramebuffer(GL_FRAMEBUFFER, _oldFBO);
}

bool CReadOpengl::CreateOffscreenSurface(int displayBufferWidth, int displayBufferHeight, GLuint colorBuffer)
{


    // 一旦削除
    DestroyOffscreenSurface();
    //进行离屏幕渲染
    do
    {
        GLuint ret = 0;

        // 新しく生成する
        if (colorBuffer == 0)
        {
            glGenTextures(1, &_colorBuffer);

            glBindTexture(GL_TEXTURE_2D, _colorBuffer);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, displayBufferWidth, displayBufferHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glBindTexture(GL_TEXTURE_2D, 0);

            _isColorBufferInherited = false;
        }
        else
        {// 指定されたものを使用
            _colorBuffer = colorBuffer;

            _isColorBufferInherited = true;
        }

        GLint tmpFramebufferObject;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &tmpFramebufferObject);//获得默认的帧缓冲

        glGenFramebuffers(1, &ret);
        glBindFramebuffer(GL_FRAMEBUFFER, ret);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _colorBuffer, 0);//绑定帧缓冲区作为附件
        glBindFramebuffer(GL_FRAMEBUFFER, tmpFramebufferObject);

        _renderTexture = ret;//在什么时候进行绑定

        _bufferWidth = displayBufferWidth;
        _bufferHeight = displayBufferHeight;
//使用的时候使用颜色附件就行
        // 成功
        return true;//帧缓冲区怎么使用

    } while (0);

    // 失敗したので削除
    DestroyOffscreenSurface();


    return false;
}

void CReadOpengl::DestroyOffscreenSurface()
{
    if (!_isColorBufferInherited && (_colorBuffer != 0))
    {
        glDeleteTextures(1, &_colorBuffer);
        _colorBuffer = 0;
    }

    if (_renderTexture!=0)
    {
        glDeleteFramebuffers(1, &_renderTexture);
        _renderTexture = 0;
    }
    _colorBuffer = 0;
}
GLuint CReadOpengl::GetRenderTexture() const
{
    return _renderTexture;
}

GLuint CReadOpengl::GetColorBuffer() const
{
    return _colorBuffer;
}

int CReadOpengl::GetBufferWidth() const
{
    return _bufferWidth;
}

int CReadOpengl::GetBufferHeight() const
{
    return _bufferHeight;
}

bool CReadOpengl::IsValid() const
{
    return _renderTexture != 0;
}

void CReadOpengl::PreDraw()
{
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_DEPTH_TEST);

    glEnable(GL_BLEND);//因为背景颜色是黑色，导致和屏幕混合的时候产生黑边
    glClearColor(1.0f, 1.0f, 1.0f, 0.0f);//没有深度测试
    //glClearColor(0.0f, 0.0f, 0.0f, 1.0f);//没有深度测试

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glColorMask(1, 1, 1, 1);
#ifdef CSM_TARGET_IPHONE_ES2
    glBindVertexArrayOES(0);
#endif

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ARRAY_BUFFER, 0); //前にバッファがバインドされていたら破棄する必要がある


}

void CReadOpengl::Clear(float r, float g, float b, float a)
{
    // マスクをクリアする
    glClearColor(r,g,b,a);
    glClear(GL_COLOR_BUFFER_BIT);
}
