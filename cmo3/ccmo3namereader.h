#ifndef CCMO3NAMEREADER_H
#define CCMO3NAMEREADER_H

#include <QByteArray>
#include <QSize>
#include <QStringList>
#include <QVector>
#include <memory>

class CExportControl;

namespace cmo3 {
class CCmo3NameReader
{
public:
    enum class ObjectType { Part, ArtMesh, Warp, Rotation };
    struct Object {
        ObjectType type = ObjectType::Part;
        QString id, name, guid, parentPart, parentDeformer;
        int vertexCount = 0, indexCount = 0, rows = 0, columns = 0;
        bool quadTransform = false;
        QByteArray topologyDigest;
        QStringList diagnostics;
    };
    class Catalog {
    public:
        bool IsValid() const { return bool(m_data); }
        const QVector<Object>& Objects() const;
        const QStringList& Diagnostics() const;
        QString SourcePath() const;
        QByteArray SourceDigest() const;
        QSize CanvasSize() const;
    private:
        struct Data;
        std::shared_ptr<const Data> m_data;
        friend class CCmo3NameReader;
    };
    enum class MatchStatus { Compatible, MissingTarget, MissingSource, Conflict, InvalidName };
    struct Match {
        ObjectType type = ObjectType::Part;
        QString id, name;
        int sourceIndex = -1;
        MatchStatus status = MatchStatus::Conflict;
        QStringList evidence, diagnostics;
    };
    struct Proposal {
        QByteArray sourceCmoDigest, sourceMocDigest;
        QVector<Match> matches;
        QStringList diagnostics;
        int compatibleCount = 0;
        bool partial = false;
    };

    static QString TypeName(ObjectType type);
    static bool ReadFile(const QString& path, Catalog& result,
                         QString* error = nullptr, CExportControl* control = nullptr);
    static bool ReadBytes(const QByteArray& bytes, const QString& sourcePath, Catalog& result,
                          QString* error = nullptr, CExportControl* control = nullptr);
    static bool BuildProposal(const Catalog& catalog, const QByteArray& sourceMocBytes, Proposal& result,
                              QString* error = nullptr, CExportControl* control = nullptr);
};
}

#endif
