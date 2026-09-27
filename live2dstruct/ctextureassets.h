#ifndef CTEXTUREASSETS_H
#define CTEXTUREASSETS_H

#include <QByteArray>
#include <QJsonArray>
#include <QMap>
#include <QSize>
#include <QString>

namespace live2dstruct {
class CTextureAssets
{
public:
    struct Source {
        QString path;
        QByteArray png, digest;
        QSize size;
        bool operator==(const Source& other) const
        { return path == other.path && png == other.png && digest == other.digest && size == other.size; }
    };
    struct DraftEntry {
        QString sourcePath;
        QByteArray sourceDigest;
        QSize size;
        QByteArray png;
        bool operator==(const DraftEntry& other) const
        { return sourcePath == other.sourcePath && sourceDigest == other.sourceDigest && size == other.size && png == other.png; }
    };
    using Catalog = QMap<int, Source>;
    using State = QMap<int, QByteArray>;
    using Draft = QMap<int, DraftEntry>;
    static constexpr int MaximumSlots = 10000;
    static constexpr qint64 MaximumBytes = 2LL * 1024 * 1024 * 1024 - 1;

    static bool MakeSource(const QString& path, const QByteArray& png, Source& result, QString* error = nullptr);
    static bool ValidateCatalog(const Catalog& sources, QString* error = nullptr);
    static bool ValidateState(const Catalog& sources, const State& state, QString* error = nullptr);
    static bool Replace(const Catalog& sources, const State& state, int slot, const QByteArray& png,
                        State& result, QString* error = nullptr);
    static State Effective(const Catalog& sources, const State& state);
    static bool CaptureDraft(const Catalog& sources, const State& state, Draft& result, QString* error = nullptr);
    static bool RestoreDraft(const Catalog& sources, const Draft& draft, State& result, QString* error = nullptr);
    static bool ValidateDraft(const Draft& draft, QString* error = nullptr);
    static bool ToJson(const Draft& draft, QJsonArray& result, QString* error = nullptr);
    static bool FromJson(const QJsonArray& array, Draft& result, QString* error = nullptr);
};
}

#endif
