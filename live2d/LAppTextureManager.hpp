/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <string>
#include <framework/Type/csmVector.hpp>
#include <QOpenGLFunctions_3_3_Core>
#include "LAppDelegate.hpp"
#include <QMap>

class LAppTextureManager : public QOpenGLFunctions_3_3_Core
{
public:

    void setGlOwningContext(const QOpenGLContext *context){
        initializeOpenGLFunctions();
        setOwningContext(context);
    }

    struct TextureInfo
    {
        GLuint id;              ///< テクスチャID
        int width;              ///< 横幅
        int height;             ///< 高さ
        std::string fileName;   ///< ファイル名
    };
    LAppTextureManager();
    ~LAppTextureManager();
    inline unsigned int Premultiply(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha)
    {
        return static_cast<unsigned>(\
            (red * (alpha + 1) >> 8) | \
            ((green * (alpha + 1) >> 8) << 8) | \
            ((blue * (alpha + 1) >> 8) << 16) | \
            (((alpha)) << 24)   \
            );
    }

    TextureInfo* CreateTextureFromPngFile(std::string fileName);
    void ReleaseTextures();
    void ReleaseTexture(Csm::csmUint32 textureId);
    void ReleaseTexture(std::string fileName);

    TextureInfo* GetTextureInfoById(GLuint textureId) const;
    void AddModelTex(int modlenum,const QString& modelname){
        m_modelTex.insert(modlenum,modelname);
    }
    QMap<int,QString>& GetModelTex(){
        return m_modelTex;
    }
private:
    Csm::csmVector<TextureInfo*> _textures;//这里包含了所有的tex
    QMap<int,QString> m_modelTex;
};
