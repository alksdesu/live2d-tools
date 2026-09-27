#ifndef CVERTEXSELECTION_H
#define CVERTEXSELECTION_H

#include <QJsonObject>
#include <QMetaType>
#include <QPointF>
#include <QString>
#include <QVector>
#include <memory>
#include <optional>

class CExportControl;

namespace live2dstruct {
class CVertexSelection
{
public:
    enum class Tool { Move, Rectangle, Lasso, Brush };
    enum class Combine { Replace, Add, Subtract };
    enum class Distance { AlongEdges, ConnectedEuclidean, AllEuclidean };
    enum class Falloff { Linear, Smooth };
    enum class Source { Hard, Distance, Painted };
    struct Soft
    {
        bool enabled = false;
        double radius = 80;
        Distance distance = Distance::AlongEdges;
        Falloff falloff = Falloff::Smooth;
        bool operator==(const Soft& other) const;
    };
    struct Brush
    {
        double radius = 50;
        double strength = 1;
        double hardness = .5;
        bool operator==(const Brush& other) const;
    };
    struct Settings
    {
        Tool tool = Tool::Move;
        Combine combine = Combine::Replace;
        Soft soft;
        Brush brush;
        bool operator==(const Settings& other) const;
    };
    struct State
    {
        QVector<int> hard;
        std::optional<QVector<double>> painted;
        bool operator==(const State& other) const;
        bool operator!=(const State& other) const { return !(*this == other); }
    };
    struct Edge
    {
        int a = -1;
        int b = -1;
        bool operator==(const Edge& other) const { return a == other.a && b == other.b; }
    };
    struct Geometry
    {
        QVector<QPointF> points;
        QVector<Edge> edges;
    };
    struct Influence
    {
        QVector<double> weights;
        QVector<int> affected;
        Source source = Source::Hard;
        qint64 operations = 0;
    };
    struct Limits
    {
        int maximumVertices = 1000000;
        int maximumEdges = 3000000;
        int maximumPathPoints = 4096;
        int maximumStrokeSegments = 65536;
        qint64 maximumOperations = 64LL * 1024 * 1024;
        qint64 maximumBytes = 256LL * 1024 * 1024;
    };
    class Stroke
    {
    public:
        bool IsValid() const { return bool(m_data); }

    private:
        struct Data;
        std::shared_ptr<const Data> m_data;
        friend class CVertexSelection;
    };

    static bool Validate(const State&, int vertexCount, QString* error = nullptr);
    static bool ValidateSettings(const Settings&, QString* error = nullptr);
    static bool Remap(const State&, int oldCount, int newCount, const QVector<int>& oldToNew, State& result,
        QString* error = nullptr);
    static bool SelectVertices(
        int vertexCount, const QVector<int>& hits, const State&, Combine, State& result, QString* error = nullptr);
    static bool EdgesFromTriangles(
        int vertexCount, const QVector<quint16>& triangles, QVector<Edge>& result, QString* error = nullptr);
    static bool SelectPolygon(const QVector<QPointF>& points, const QVector<QPointF>& polygon, const State&, Combine,
        State& result, QString* error = nullptr, CExportControl* control = nullptr);
    static bool SelectPolygon(const QVector<QPointF>& points, const QVector<QPointF>& polygon, const State&, Combine,
        State& result, const Limits&, QString* error = nullptr, CExportControl* control = nullptr);
    static bool ComputeInfluence(const Geometry&, const State&, const Soft&, Influence& result,
        QString* error = nullptr, CExportControl* control = nullptr);
    static bool ComputeInfluence(const Geometry&, const State&, const Soft&, Influence& result, const Limits&,
        QString* error = nullptr, CExportControl* control = nullptr);
    static bool Translate(const QVector<float>& baselineModel, const QVector<double>& weights, QPointF totalModelDelta,
        QVector<float>& result, QString* error = nullptr);
    static bool BeginBrush(const Geometry&, const State&, const Soft&, const Brush&, Combine, QPointF start,
        Stroke& result, State& selection, QString* error = nullptr, CExportControl* control = nullptr);
    static bool BeginBrush(const Geometry&, const State&, const Soft&, const Brush&, Combine, QPointF start,
        Stroke& result, State& selection, const Limits&, QString* error = nullptr, CExportControl* control = nullptr);
    static bool AppendBrush(const Stroke&, QPointF point, Stroke& result, State& selection, QString* error = nullptr,
        CExportControl* control = nullptr);
    static bool BrushState(const Stroke&, State& result, QString* error = nullptr);
    static bool CancelBrush(const Stroke&, State& result, QString* error = nullptr);
    static QJsonObject ToJson(const Settings&);
    static bool FromJson(const QJsonObject&, Settings& result, QString* error = nullptr);
};
}

Q_DECLARE_METATYPE(live2dstruct::CVertexSelection::State)
Q_DECLARE_METATYPE(live2dstruct::CVertexSelection::Settings)
Q_DECLARE_METATYPE(live2dstruct::CVertexSelection::Tool)
Q_DECLARE_METATYPE(live2dstruct::CVertexSelection::Combine)

#endif
