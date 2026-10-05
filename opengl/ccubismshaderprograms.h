#ifndef CCUBISMSHADERPROGRAMS_H
#define CCUBISMSHADERPROGRAMS_H

#include <QByteArray>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QPointer>
#include <QString>
#include <array>
#include <memory>

class QOpenGLShaderProgram;

class CCubismShaderPrograms
{
public:
    enum class Variant {
        SetupMask, Straight, StraightMasked, StraightInverted,
        Premultiplied, PremultipliedMasked, PremultipliedInverted,
        OffscreenMask, CompositeMasked, BackdropComposite, Count
    };
    enum class BlendMode { Normal, Additive, Multiplicative };
    static constexpr std::size_t ProgramCount = static_cast<std::size_t>(Variant::Count);
    struct Source { QByteArray vertex, fragment; };
    using SourceSet = std::array<Source, ProgramCount>;
    struct Program {
        GLuint id = 0;
        GLint matrix = -1, clipMatrix = -1, sampler0 = -1, sampler1 = -1, sampler2 = -1;
        GLint baseColor = -1, multiplyColor = -1, screenColor = -1, channelFlag = -1;
        GLint resolution = -1, maskInvert = -1, maskRect = -1, colorMode = -1, alphaMode = -1, useMask = -1;
    };

    CCubismShaderPrograms();
    ~CCubismShaderPrograms();
    CCubismShaderPrograms(const CCubismShaderPrograms&) = delete;
    CCubismShaderPrograms& operator=(const CCubismShaderPrograms&) = delete;

    bool Initialize(const QOpenGLContext* context);
    bool Initialize(const QOpenGLContext* context, const SourceSet& sources);
    const Program* Get(Variant variant) const;
    bool Bind(Variant variant, BlendMode blendMode = BlendMode::Normal);
    bool Release();
    const QOpenGLContext* Context() const { return m_context.data(); }
    const QString& Error() const { return m_error; }
    static const SourceSet& Sources();

private:
    QPointer<QOpenGLContext> m_context;
    std::array<std::unique_ptr<QOpenGLShaderProgram>, ProgramCount> m_programs;
    std::array<Program, ProgramCount> m_locations{};
    QString m_error;
};

#endif
