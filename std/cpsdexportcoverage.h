#ifndef CPSDEXPORTCOVERAGE_H
#define CPSDEXPORTCOVERAGE_H

#include <QByteArray>
#include <QJsonObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <memory>

class CPsdExportCoverage
{
public:
    enum class Method { Gpu, Cpu };
    enum class State { Pending, Raster, Empty, Excluded };
    enum class Reason { None, RuntimeHidden, EditorHidden, NoVertices, NoTriangles, DegenerateBounds, OutsideCanvas };
    struct Context {
        QByteArray sourceMocDigest, runtimeMocDigest;
        QSize canvas;
        Method method = Method::Gpu;
        bool includeHidden = false;
        int runtimeCount = 0;
    };
    struct Record {
        int runtimeIndex = -1;
        QString id, displayName, partId;
        quint32 psdLayerId = 0;
        State state = State::Pending;
        Reason reason = Reason::None;
        QRect bounds;
    };
    struct Counts {
        int source = 0, requested = 0, raster = 0, empty = 0, excluded = 0, pending = 0;
    };
    static constexpr int MaximumRecords = 1000000;
    static constexpr qsizetype MaximumCharacters = 16 * 1024 * 1024;
    static bool Capture(const Context& context, const QVector<Record>& records, bool committed,
                        CPsdExportCoverage& result, QString* error = nullptr);
    bool IsEmpty() const { return !m_data; }
    bool IsValid() const { return true; }
    bool IsCommitted() const;
    const Context& GetContext() const;
    const QVector<Record>& Records() const;
    Counts Statistics() const;
    QJsonObject ToJson() const;
    QString Summary() const;
    QString Text(int maximumDetails = 100) const;
    static QString StateText(State state);
    static QString ReasonText(Reason reason);
    bool operator==(const CPsdExportCoverage& other) const;
    bool operator!=(const CPsdExportCoverage& other) const { return !(*this == other); }

private:
    struct Data;
    std::shared_ptr<const Data> m_data;
};

#endif
