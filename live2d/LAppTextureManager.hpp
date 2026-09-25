/**
 * Copyright(c) Live2D Inc. All rights reserved.
 *
 * Use of this source code is governed by the Live2D Open Software license
 * that can be found at https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <QMap>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QPointer>
#include <QSet>
#include <QString>

class LAppTextureManager : public QOpenGLFunctions_3_3_Core
{
public:
    struct TextureInfo
    {
        GLuint id = 0;
        int width = 0;
        int height = 0;
        std::string fileName;
    };

    LAppTextureManager();
    ~LAppTextureManager();
    void setGlOwningContext(const QOpenGLContext* context);
    TextureInfo* CreateTextureFromPngFile(std::string fileName);
    void ReleaseTextures();
    void ReleaseTexture(GLuint textureId);
    void ReleaseTexture(std::string fileName);
    void ReleaseUnusedTextures(const QSet<QString>& retainedPaths);

    TextureInfo* GetTextureInfoById(GLuint textureId) const;
    TextureInfo* GetTextureInfoByName(const std::string& fileName) const;
    void SetModelTextures(const QMap<int, QString>& textures) { m_modelTex = textures; }
    void AddModelTex(int modelNumber, const QString& modelName) { m_modelTex.insert(modelNumber, modelName); }
    QMap<int, QString>& GetModelTex() { return m_modelTex; }

    static unsigned int Premultiply(unsigned char red, unsigned char green, unsigned char blue, unsigned char alpha)
    {
        return (red * (alpha + 1) >> 8) | ((green * (alpha + 1) >> 8) << 8) |
               ((blue * (alpha + 1) >> 8) << 16) | (static_cast<unsigned int>(alpha) << 24);
    }

private:
    bool CanUseContext();
    void RemoveModelTexture(const std::string& fileName);
    std::vector<std::unique_ptr<TextureInfo>> m_textures;
    QMap<int, QString> m_modelTex;
    QPointer<QOpenGLContext> m_context;
};
