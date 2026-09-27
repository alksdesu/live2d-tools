#ifndef CTEXTUREWORKSPACE_H
#define CTEXTUREWORKSPACE_H

#include "ctextureassets.h"
#include <QJsonObject>
#include <QMetaType>

namespace live2dstruct {
class CTextureWorkspace
{
public:
    enum class Transition { PreserveIdentity, ReplaceGenerated };
    struct Atlas {
        QString id, name;
        int sourceSlot = -1;
        QSize size;
        QByteArray png;
        bool operator==(const Atlas& other) const;
        bool operator!=(const Atlas& other) const { return !(*this == other); }
    };
    struct State {
        QByteArray sourceFingerprint;
        QMap<int, Atlas> atlases;
        bool operator==(const State& other) const;
        bool operator!=(const State& other) const { return !(*this == other); }
    };
    struct Origin {
        QString path;
        QByteArray digest;
        QSize size;
        bool operator==(const Origin& other) const;
    };
    struct Draft {
        QMap<int, Origin> origins;
        State state;
        bool operator==(const Draft& other) const;
    };
    static constexpr qint64 MaximumDecodedBytes = CTextureAssets::MaximumBytes;
    static bool Initialize(const CTextureAssets::Catalog& sources, const CTextureAssets::State& legacy,
                           State& result, QString* error = nullptr);
    static bool Validate(const CTextureAssets::Catalog& sources, const State& state, QString* error = nullptr);
    static bool Freeze(const CTextureAssets::Catalog& sources, const State& state,
                       State& result, QString* error = nullptr);
    static bool ValidateTransition(const CTextureAssets::Catalog& sources, const State& before,
                                   const State& after, QString* error = nullptr);
    static bool ValidateTransition(const CTextureAssets::Catalog& sources, const State& before,
                                   const State& after, Transition transition, QString* error = nullptr);
    static bool Effective(const CTextureAssets::Catalog& sources, const State& state,
                          CTextureAssets::State& result, QString* error = nullptr);
    static bool Fingerprint(const CTextureAssets::Catalog& sources, const State& state,
                            QByteArray& result, QString* error = nullptr);
    static bool CaptureDraft(const CTextureAssets::Catalog& sources, const State& state,
                             Draft& result, QString* error = nullptr);
    static bool RestoreDraft(const CTextureAssets::Catalog& sources, const Draft& draft,
                             State& result, QString* error = nullptr);
    static bool ValidateDraft(const Draft& draft, QString* error = nullptr);
    static bool ToJson(const Draft& draft, QJsonObject& result, QString* error = nullptr);
    static bool FromJson(const QJsonObject& object, Draft& result, QString* error = nullptr);
};
}
Q_DECLARE_METATYPE(live2dstruct::CTextureWorkspace::State)
Q_DECLARE_METATYPE(live2dstruct::CTextureWorkspace::Draft)
#endif
