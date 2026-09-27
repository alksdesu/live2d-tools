#include "live2dstruct/calphameshplanner.h"
#include "live2dstruct/cmesheditplanner.h"
#include "std/cexportcontrol.h"
#include <QSet>
#include <QScopeGuard>
#include <QtTest>
#include <cmath>
#include <cstring>

using Planner = live2dstruct::CAlphaMeshPlanner;
using Mesh = live2dstruct::CMeshEditPlanner;
namespace {
Planner::Input square(int size = 32)
{
    Planner::Input input;
    input.artMeshId = "mesh";
    input.image = QImage(size, size, QImage::Format_RGBA8888);
    input.image.fill(Qt::transparent);
    input.pixelCoordinates = { { 0, 0 }, { double(size), 0 }, { double(size), double(size) }, { 0, double(size) } };
    input.indices = { 0, 1, 2, 0, 2, 3 };
    input.uvs = { 0, 0, 1, 0, 1, 1, 0, 1 };
    input.referenceSize = { 1024, 1024 };
    input.sourceToken = "owned-source";
    return input;
}
void fill(QImage& image, QRect rect, int alpha = 255)
{
    for (int y = rect.top(); y <= rect.bottom(); ++y)
        for (int x = rect.left(); x <= rect.right(); ++x)
            image.setPixelColor(x, y, QColor(120, 30, 70, alpha));
}
double area(const Planner::Plan& plan)
{
    double sum = 0;
    for (int i = 0; i < plan.edit.indices.size(); i += 3) {
        auto a = plan.pixelCoordinates[plan.edit.indices[i]], b = plan.pixelCoordinates[plan.edit.indices[i + 1]],
             c = plan.pixelCoordinates[plan.edit.indices[i + 2]];
        sum += std::abs((b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x())) / 2;
    }
    return sum;
}
Planner::Settings settings()
{
    auto s = Planner::Preset(Planner::Density::Standard);
    s.margin = 0;
    return s;
}
bool covers(const Planner::Plan& plan, QPointF p)
{
    for (int i = 0; i < plan.edit.indices.size(); i += 3) {
        double signs[3];
        for (int j = 0; j < 3; ++j) {
            auto a = plan.pixelCoordinates[plan.edit.indices[i + j]],
                 b = plan.pixelCoordinates[plan.edit.indices[i + (j + 1) % 3]];
            signs[j] = (b.x() - a.x()) * (p.y() - a.y()) - (b.y() - a.y()) * (p.x() - a.x());
        }
        if ((signs[0] >= 0 && signs[1] >= 0 && signs[2] >= 0) || (signs[0] <= 0 && signs[1] <= 0 && signs[2] <= 0))
            return true;
    }
    return false;
}
}
class AlphaMeshPlannerTests : public QObject
{
    Q_OBJECT
private slots:
    void highUnsignedSourceVerticesKeepTheirMigrationIdentity()
    {
        auto input = square(8);
        fill(input.image, { 0, 0, 8, 8 });
        const auto originalPoints = input.pixelCoordinates;
        const auto originalUvs = input.uvs;
        input.pixelCoordinates.resize(65536);
        input.uvs.resize(65536 * 2);
        const QVector<int> high { 32768, 65533, 65534, 65535 };
        for (int i = 0; i < high.size(); ++i) {
            input.pixelCoordinates[high[i]] = originalPoints[i];
            input.uvs[high[i] * 2] = originalUvs[i * 2];
            input.uvs[high[i] * 2 + 1] = originalUvs[i * 2 + 1];
        }
        input.indices = { 32768, 65533, 65534, 32768, 65534, 65535 };
        Planner::Plan result;
        QString error;
        QVERIFY2(Planner::Build(input, settings(), result, &error), qPrintable(error));
        QVERIFY2(Planner::Validate(input, result, &error), qPrintable(error));
        QCOMPARE(result.edit.oldToNew.size(), 65536);
        for (int vertex : high)
            QVERIFY(result.edit.oldToNew[vertex] >= 0);
        for (const auto& source : result.edit.vertices)
            for (int vertex : source.indices)
                QVERIFY(high.contains(vertex));
        const auto before = result.edit.indices;
        input.pixelCoordinates.append({ 1, 1 });
        input.uvs.append({ .125f, .125f });
        QVERIFY(!Planner::Build(input, settings(), result, &error));
        QCOMPARE(result.edit.indices, before);
    }

    void constrainedGraphKeepsCrossings()
    {
        Mesh::ConstrainedInput input;
        input.points = { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 }, { 0, 5 }, { 10, 5 } };
        input.constraints = { { 0, 2 }, { 4, 5 } };
        Mesh::ConstrainedResult result;
        Mesh::Diagnostic error;
        QVERIFY2(Mesh::TriangulateConstraints(input, result, &error), qPrintable(error.message));
        QVERIFY(result.points.contains(QPointF(5, 5)));
        QVERIFY(!result.indices.isEmpty());
    }
    void rationalCrossingKeepsConstraintIdentity()
    {
        Mesh::ConstrainedInput input;
        input.points = { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 }, { 0, 3 }, { 10, 4 } };
        input.constraints = { { 0, 2 }, { 4, 5 } };
        Mesh::ConstrainedResult result;
        Mesh::Diagnostic diagnostic;
        QVERIFY2(Mesh::TriangulateConstraints(input, result, &diagnostic), qPrintable(diagnostic.message));
        int crossing = -1;
        for (int i = 0; i < result.points.size(); ++i)
            if (std::hypot(result.points[i].x() - 10.0 / 3, result.points[i].y() - 10.0 / 3) < 1e-12)
                crossing = i;
        QVERIFY(crossing >= 0);
        QSet<QPair<int, int>> edges;
        for (int i = 0; i < result.indices.size(); i += 3)
            for (int j = 0; j < 3; ++j) {
                const int a = result.indices[i + j], b = result.indices[i + (j + 1) % 3];
                edges.insert({ std::min(a, b), std::max(a, b) });
            }
        for (int endpoint : { 0, 2, 4, 5 })
            QVERIFY(edges.contains({ std::min(endpoint, crossing), std::max(endpoint, crossing) }));
    }
    void manualConnectHandlesRepeatedCavityBoundaryVertex()
    {
        Mesh::Input input;
        input.artMeshId = "mesh";
        input.positions
            = { -74, 127, -25, 22, -36, 35, -3, 29, -24, 56, -40, 82, -32, 69, -13, 41, 13, -13, 30, -36, -8, 11 };
        input.indices
            = { 9, 0, 8, 7, 9, 3, 2, 9, 10, 4, 9, 7, 0, 2, 10, 9, 2, 1, 0, 10, 8, 0, 9, 5, 5, 9, 6, 6, 9, 4, 10, 9, 8 };
        input.protectedEdges = { { 9, 8 } };
        Mesh::Result result;
        Mesh::Diagnostic diagnostic;
        QVERIFY2(Mesh::Connect(input, 9, 8, Mesh::CrossingPolicy::RejectProtected, result, &diagnostic),
            qPrintable(diagnostic.message));
        QVERIFY2(Mesh::Connect(input, 1, 3, Mesh::CrossingPolicy::RejectProtected, result, &diagnostic),
            qPrintable(diagnostic.message));
        QCOMPARE(result.edit.vertices.size(), 11);
        for (int index = 0; index < 11; ++index) {
            QCOMPARE(result.edit.oldToNew[index], index);
            QCOMPARE(result.edit.vertices[index].indices, QVector<int> { index });
            QCOMPARE(result.edit.vertices[index].weights, QVector<double> { 1 });
        }
        QVERIFY(!result.affectedTriangles.isEmpty());
        QVERIFY(result.protectedEdges.contains({ 8, 9 }));
        QVERIFY(result.protectedEdges.contains({ 1, 3 }));
        Mesh::Input after { input.artMeshId, input.positions, result.edit.indices, result.protectedEdges };
        Mesh::Result checked;
        QVERIFY2(Mesh::Connect(after, 1, 3, Mesh::CrossingPolicy::RejectProtected, checked, &diagnostic),
            qPrintable(diagnostic.message));
        QCOMPARE(checked.edit.indices, result.edit.indices);
    }
    void rectangleUsesWholeKeyformWeightsAndOriginalTriangleDomain()
    {
        auto input = square();
        fill(input.image, { 4, 7, 20, 14 });
        Planner::Plan result;
        QString error;
        QVERIFY2(Planner::Build(input, settings(), result, &error), qPrintable(error));
        QVERIFY2(Planner::Validate(input, result, &error), qPrintable(error));
        QVERIFY(std::abs(area(result) - 280) < 1e-7);
        for (int i = 0; i < result.edit.vertices.size(); ++i) {
            const auto& v = result.edit.vertices[i];
            double sum = 0;
            QPointF p;
            for (int j = 0; j < v.indices.size(); ++j) {
                QVERIFY(v.weights[j] >= 0);
                sum += v.weights[j];
                p += input.pixelCoordinates[v.indices[j]] * v.weights[j];
            }
            QVERIFY(std::abs(sum - 1) < 1e-12);
            QVERIFY(std::hypot(p.x() - result.pixelCoordinates[i].x(), p.y() - result.pixelCoordinates[i].y()) < 1e-5);
        }
    }
    void holesAndMultipleIslandsStayEmpty()
    {
        auto input = square();
        fill(input.image, { 2, 2, 14, 14 });
        fill(input.image, { 6, 6, 6, 6 }, 0);
        fill(input.image, { 23, 23, 3, 4 });
        Planner::Plan result;
        QString error;
        QVERIFY2(Planner::Build(input, settings(), result, &error), qPrintable(error));
        QVERIFY(!covers(result, { 8.5, 8.5 }));
        QVERIFY(!covers(result, { 19, 19 }));
        QVERIFY(covers(result, { 24, 24 }));
        QVERIFY(std::abs(area(result) - (196 - 36 + 12)) < 1e-7);
        QCOMPARE(result.statistics.islands, 2);
        QCOMPARE(result.statistics.holes, 1);
    }
    void singlePixelAndDiagonalContactAreNotDropped()
    {
        auto input = square();
        fill(input.image, { 5, 5, 1, 1 });
        fill(input.image, { 6, 6, 1, 1 });
        fill(input.image, { 20, 4, 1, 8 });
        Planner::Plan result;
        QString error;
        QVERIFY2(Planner::Build(input, settings(), result, &error), qPrintable(error));
        QVERIFY(std::abs(area(result) - 10) < 1e-7);
        QCOMPARE(result.statistics.islands, 3);
        QVERIFY(covers(result, { 5.5, 5.5 }));
        QVERIFY(covers(result, { 6.5, 6.5 }));
        QVERIFY(!covers(result, { 5.5, 6.5 }));
    }
    void alphaThresholdAndBlankAreExplicit()
    {
        auto input = square();
        fill(input.image, { 4, 4, 4, 4 }, 1);
        Planner::Plan result;
        QString error;
        auto config = settings();
        QVERIFY2(Planner::Build(input, config, result, &error), qPrintable(error));
        const auto saved = result.edit.indices;
        config.alphaThreshold = 1;
        QVERIFY(!Planner::Build(input, config, result, &error));
        QCOMPARE(result.edit.indices, saved);
        config.alphaThreshold = 255;
        QVERIFY(!Planner::Build(input, config, result, &error));
    }
    void clippedDomainDoesNotExposeOtherAtlasPixels()
    {
        auto input = square();
        input.pixelCoordinates = { { 0, 0 }, { 32, 0 }, { 0, 32 } };
        input.uvs = { 0, 0, 1, 0, 0, 1 };
        input.indices = { 0, 1, 2 };
        input.image.fill(Qt::white);
        Planner::Plan result;
        QString error;
        QVERIFY2(Planner::Build(input, settings(), result, &error), qPrintable(error));
        QVERIFY(std::abs(area(result) - 512) < 1e-7);
        QVERIFY(!covers(result, { 25, 25 }));
    }
    void repeatAndTransparentHaveDifferentDomains()
    {
        auto input = square(8);
        input.pixelCoordinates = { { -8, -8 }, { 16, -8 }, { 16, 16 }, { -8, 16 } };
        input.uvs = { -1, -1, 2, -1, 2, 2, -1, 2 };
        input.image.fill(Qt::white);
        Planner::Plan repeat, transparent;
        QString error;
        QVERIFY2(Planner::Build(input, settings(), transparent, &error), qPrintable(error));
        input.repeat = true;
        QVERIFY2(Planner::Build(input, settings(), repeat, &error), qPrintable(error));
        QVERIFY(std::abs(area(transparent) - 64) < 1e-7);
        QVERIFY(std::abs(area(repeat) - 576) < 1e-7);
    }
    void gluePinsKeepExactOldIdentityOutsideAlpha()
    {
        auto input = square();
        fill(input.image, { 10, 10, 4, 4 });
        input.pinnedVertices = { 0, 2 };
        Planner::Plan result;
        QString error;
        QVERIFY2(Planner::Build(input, settings(), result, &error), qPrintable(error));
        for (int pin : input.pinnedVertices) {
            QVERIFY(result.edit.oldToNew[pin] >= 0);
            const auto& v = result.edit.vertices[result.edit.oldToNew[pin]];
            QCOMPARE(v.indices, QVector<int> { pin });
            QCOMPARE(v.weights, QVector<double> { 1 });
        }
    }
    void invalidOverlapAndDegeneracyAreAtomic_data()
    {
        QTest::addColumn<int>("fault");
        for (int i = 0; i < 6; ++i)
            QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidOverlapAndDegeneracyAreAtomic()
    {
        QFETCH(int, fault);
        auto input = square();
        input.image.fill(Qt::white);
        if (fault == 0)
            input.pixelCoordinates[1] = input.pixelCoordinates[0];
        if (fault == 1)
            input.indices = { 0, 0, 1 };
        if (fault == 2)
            input.indices = { 0, 1, 2, 0, 1, 2 };
        if (fault == 3)
            input.indices[0] = 100;
        if (fault == 4) {
            input.pixelCoordinates.append({ 5, 5 });
            input.uvs += { 5.f / 32, 5.f / 32 };
            input.indices += { 0, 1, 4 };
        }
        if (fault == 5)
            input.pixelCoordinates[0].setX(std::numeric_limits<double>::infinity());
        Planner::Plan result;
        result.edit.artMeshId = "keep";
        QString error;
        QVERIFY(!Planner::Build(input, settings(), result, &error));
        QCOMPARE(result.edit.artMeshId, QString("keep"));
        QVERIFY(!error.isEmpty());
    }
    void densityIsDeterministicAndDoesNotDiscardBoundary()
    {
        auto input = square(128);
        fill(input.image, { 4, 4, 120, 120 });
        Planner::Plan coarse, fine, again;
        QString error;
        auto c = Planner::Preset(Planner::Density::Coarse), f = Planner::Preset(Planner::Density::Fine);
        c.margin = f.margin = 0;
        QVERIFY2(Planner::Build(input, c, coarse, &error), qPrintable(error));
        QVERIFY2(Planner::Build(input, f, fine, &error), qPrintable(error));
        QVERIFY2(Planner::Build(input, f, again, &error), qPrintable(error));
        QVERIFY(fine.statistics.newVertices > coarse.statistics.newVertices);
        QVERIFY(std::abs(area(coarse) - 14400) < 1e-7);
        QVERIFY(std::abs(area(fine) - 14400) < 1e-7);
        QCOMPARE(fine.edit.indices, again.edit.indices);
        QCOMPARE(fine.pixelCoordinates, again.pixelCoordinates);
    }
    void hashSeedDoesNotChangeMeshOrOperationBudget()
    {
        const auto restoreSeed = qScopeGuard([] { QHashSeed::resetRandomGlobalSeed(); });
        auto input = square(128);
        fill(input.image, {4, 4, 120, 120});
        auto config = Planner::Preset(Planner::Density::Fine);
        config.margin = 0;
        QHashSeed::setDeterministicGlobalSeed();
        Planner::Plan baseline;
        QString error;
        QVERIFY2(Planner::Build(input, config, baseline, &error), qPrintable(error));
        QSet<size_t> observedSeeds{size_t(QHashSeed::globalSeed())};
        for (int iteration = 0; iteration < 4; ++iteration) {
            QHashSeed::resetRandomGlobalSeed();
            observedSeeds.insert(size_t(QHashSeed::globalSeed()));
            Planner::Plan candidate;
            QVERIFY2(Planner::Build(input, config, candidate, &error), qPrintable(error));
            QCOMPARE(candidate.statistics.operations, baseline.statistics.operations);
            QCOMPARE(candidate.edit.oldToNew, baseline.edit.oldToNew);
            QCOMPARE(candidate.edit.indices, baseline.edit.indices);
            QCOMPARE(candidate.pixelCoordinates, baseline.pixelCoordinates);
            QCOMPARE(candidate.edit.vertices.size(), baseline.edit.vertices.size());
            for (int index = 0; index < candidate.edit.vertices.size(); ++index) {
                QCOMPARE(candidate.edit.vertices[index].indices, baseline.edit.vertices[index].indices);
                QCOMPARE(candidate.edit.vertices[index].weights, baseline.edit.vertices[index].weights);
            }
            QCOMPARE(candidate.contours.size(), baseline.contours.size());
            for (int index = 0; index < candidate.contours.size(); ++index) {
                QCOMPARE(candidate.contours[index].points, baseline.contours[index].points);
                QCOMPARE(candidate.contours[index].hole, baseline.contours[index].hole);
            }
            auto bounded = input;
            bounded.limits.maximumOperations = baseline.statistics.operations;
            QVERIFY2(Planner::Build(bounded, config, candidate, &error), qPrintable(error));
            QCOMPARE(candidate.statistics.operations, baseline.statistics.operations);
            QCOMPARE(candidate.edit.indices, baseline.edit.indices);
            --bounded.limits.maximumOperations;
            candidate.edit.artMeshId = "unchanged";
            QVERIFY(!Planner::Build(bounded, config, candidate, &error));
            QCOMPARE(candidate.edit.artMeshId, QString("unchanged"));
        }
        QVERIFY(observedSeeds.size() > 1);
    }
    void marginIsBoundedAndReportedInRealPixels()
    {
        auto input = square();
        fill(input.image, { 10, 10, 2, 2 });
        input.referenceSize = { 2048, 1024 };
        auto config = settings();
        config.margin = 1;
        Planner::Plan result;
        QString error;
        QVERIFY2(Planner::Build(input, config, result, &error), qPrintable(error));
        QCOMPARE(result.statistics.marginPixels, 2.0);
        QCOMPARE(result.statistics.boundarySpacingPixels, 48.0);
        QVERIFY(std::abs(area(result) - 36) < 1e-7);
    }
    void cancellationAndLimitsNeverReplaceResult()
    {
        auto input = square();
        input.image.fill(Qt::white);
        Planner::Plan result;
        result.edit.artMeshId = "keep";
        QString error;
        CExportControl control;
        control.SetHandler([](const auto&, QString*) { return false; });
        QVERIFY(!Planner::Build(input, settings(), result, &error, &control));
        QCOMPARE(result.edit.artMeshId, QString("keep"));
        input.limits.maximumOperations = 10;
        QVERIFY(!Planner::Build(input, settings(), result, &error));
        QCOMPARE(result.edit.artMeshId, QString("keep"));
        input.limits.maximumOperations = 256LL * 1024 * 1024;
        input.limits.maximumPixels = 10;
        QVERIFY(!Planner::Build(input, settings(), result, &error));
    }
    void smallWorkingBudgetIncludesContourAndTriangulationStorage()
    {
        auto input = square();
        input.image.fill(Qt::white);
        input.limits.maximumWorkingBytes = 12000;
        Planner::Plan result;
        result.edit.artMeshId = "keep";
        QString error;
        QVERIFY(!Planner::Build(input, settings(), result, &error));
        QCOMPARE(result.edit.artMeshId, QString("keep"));
        QVERIFY(error.contains("预算"));
        Mesh::ConstrainedInput domain;
        domain.points = { { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } };
        domain.constraints = { { 0, 2 }, { 1, 3 } };
        domain.maximumWorkingBytes = 2048;
        Mesh::ConstrainedResult output;
        output.operations = 99;
        Mesh::Diagnostic diagnostic;
        QVERIFY(!Mesh::TriangulateConstraints(domain, output, &diagnostic));
        QCOMPARE(output.operations, 99);
    }
    void cancellationDuringGeometryKeepsPublishedOutput()
    {
        auto input = square(128);
        input.image.fill(Qt::white);
        Planner::Plan result;
        result.edit.artMeshId = "keep";
        QString error;
        CExportControl control;
        int calls = 0;
        control.SetHandler([&](const auto& progress, QString*) {
            if (progress.detail == QStringLiteral("生成轮廓与原三角约束") && ++calls > 250)
                return false;
            return true;
        });
        QVERIFY(!Planner::Build(input, settings(), result, &error, &control));
        QVERIFY(calls > 250);
        QCOMPARE(result.edit.artMeshId, QString("keep"));
    }
    void borrowedImageAndTokenAreFrozenBeforeCallbacks()
    {
        auto input = square();
        QByteArray bytes(32 * 32 * 4, char(255));
        QByteArray token = "borrowed";
        input.image = QImage(reinterpret_cast<const uchar*>(bytes.constData()), 32, 32, 128, QImage::Format_RGBA8888);
        input.sourceToken = QByteArray::fromRawData(token.constData(), token.size());
        auto expected = input;
        expected.image = input.image.copy();
        expected.sourceToken = QByteArray(input.sourceToken.constData(), input.sourceToken.size());
        Planner::Plan result;
        QString error;
        CExportControl control;
        bool changed = false;
        control.SetHandler([&](const auto&, QString*) {
            if (!changed) {
                changed = true;
                std::memset(bytes.data(), 0, bytes.size());
                std::memset(token.data(), 'x', token.size());
            }
            return true;
        });
        QVERIFY2(Planner::Build(input, settings(), result, &error, &control), qPrintable(error));
        QVERIFY(Planner::Validate(expected, result, &error));
        QVERIFY(!Planner::Validate(input, result, &error));
    }
    void callbacksCannotMutateFrozenInputAndPlanTamperingFails()
    {
        auto input = square();
        input.image.fill(Qt::white);
        const auto original = input;
        Planner::Plan result;
        QString error;
        CExportControl control;
        bool changed = false;
        control.SetHandler([&](const auto&, QString*) {
            if (!changed) {
                changed = true;
                input.image.fill(Qt::transparent);
                input.pixelCoordinates.clear();
                input.sourceToken = "changed";
            }
            return true;
        });
        QVERIFY2(Planner::Build(input, settings(), result, &error, &control), qPrintable(error));
        QVERIFY(Planner::Validate(original, result, &error));
        QVERIFY(!Planner::Validate(input, result, &error));
        result.edit.vertices[0].weights[0] = .5;
        QVERIFY(!Planner::Validate(original, result, &error));
    }
    void pointContactResultCanBeGeneratedAgain()
    {
        auto input = square();
        fill(input.image, { 5, 5, 1, 1 });
        fill(input.image, { 6, 6, 1, 1 });
        Planner::Plan first, second;
        QString error;
        QVERIFY2(Planner::Build(input, settings(), first, &error), qPrintable(error));
        auto after = input;
        after.pixelCoordinates = first.pixelCoordinates;
        after.indices = first.edit.indices;
        after.uvs.clear();
        for (const auto& source : first.edit.vertices)
            for (int axis = 0; axis < 2; ++axis) {
                double value = 0, sum = 0;
                for (double w : source.weights)
                    sum += w;
                for (int i = 0; i < source.indices.size(); ++i)
                    value += input.uvs[source.indices[i] * 2 + axis] * (source.weights[i] / sum);
                after.uvs.append(float(value));
            }
        QVERIFY2(Planner::Build(after, settings(), second, &error), qPrintable(error));
        QVERIFY(std::abs(area(second) - 2) < 1e-7);
        QVERIFY(!covers(second, { 5.5, 6.5 }));
    }
    void atlasNeighborOutsideUvCannotBleedIntoMargin()
    {
        auto input = square();
        input.pixelCoordinates = { { 0, 0 }, { 8, 0 }, { 8, 8 }, { 0, 8 } };
        input.uvs = { 0, 0, .25f, 0, .25f, .25f, 0, .25f };
        fill(input.image, { 8, 0, 1, 8 });
        Planner::Plan result;
        result.edit.artMeshId = "keep";
        QString error;
        auto config = settings();
        config.margin = 4;
        QVERIFY(!Planner::Build(input, config, result, &error));
        QCOMPARE(result.edit.artMeshId, QString("keep"));
    }
    void floatUvCollapseIsRejectedBeforeCommit()
    {
        auto input = square();
        fill(input.image, { 10, 10, 2, 2 });
        const float next = std::nextafter(1.f, 2.f);
        input.uvs = { 1, 1, next, 1, next, next, 1, next };
        Planner::Plan result;
        QString error;
        QVERIFY(!Planner::Build(input, settings(), result, &error));
        QVERIFY(error.contains("float UV"));
    }
    void gluePinAtPointContactCannotBeDuplicated()
    {
        auto input = square();
        input.pixelCoordinates.append({ 6, 6 });
        input.uvs += { 6.f / 32, 6.f / 32 };
        input.indices = { 0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4 };
        input.pinnedVertices = { 4 };
        fill(input.image, { 5, 5, 1, 1 });
        fill(input.image, { 6, 6, 1, 1 });
        Planner::Plan result;
        result.edit.artMeshId = "keep";
        QString error;
        QVERIFY(!Planner::Build(input, settings(), result, &error));
        QVERIFY(error.contains("Glue"));
        QCOMPARE(result.edit.artMeshId, QString("keep"));
    }
    void curvedContoursSimplifyWithoutLosingOpaqueCells()
    {
        auto input = square(128);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x)
                if (std::hypot(x + .5 - 64, y + .5 - 64) < 45)
                    input.image.setPixelColor(x, y, Qt::white);
        Planner::Plan result;
        QString error;
        auto config = Planner::Preset(Planner::Density::Coarse);
        QVERIFY2(Planner::Build(input, config, result, &error), qPrintable(error));
        int count = 0;
        for (const auto& contour : result.contours)
            count += contour.points.size();
        QVERIFY2(count < 100, qPrintable(QString::number(count)));
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x)
                if (input.image.pixelColor(x, y).alpha())
                    QVERIFY(covers(result, { x + .5, y + .5 }));
    }
};
QTEST_GUILESS_MAIN(AlphaMeshPlannerTests)
#include "alpha_mesh_planner_tests.moc"
