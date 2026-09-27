#include "live2dstruct/cpsdlayermatcher.h"
#include "std/cexportcontrol.h"
#include <QtTest>
#include <limits>
using namespace live2dstruct;
namespace {
using Artwork = CModelArtwork; using Matcher = CPsdLayerMatcher;
Artwork::Layer layer(const QString& id, const QString& name, std::optional<quint32> external = {}) {
    Artwork::Layer value; value.id = id; value.name = name; value.externalId = external;
    value.bounds = QRect(-12, -8, 20, 30); value.imageDigest = QByteArray(32, 'p'); return value;
}
Artwork::Revision revision(const QString& id, const QVector<Artwork::Layer>& layers, const QString& family = "source") {
    Artwork::Revision value; value.id = id; value.familyId = family; value.layers = layers; return value;
}
Matcher::Result compare(const Artwork::Revision& old, const Artwork::Revision& next) {
    Matcher::Result result; QString error;
    if (!Matcher::Compare(old, next, result, &error)) qFatal("%s", qPrintable(error)); return result;
}
}
class PsdLayerMatcherTests : public QObject {
    Q_OBJECT
private slots:
    void uniqueIdsSurviveRenameReorderAndHiddenState() {
        auto first = layer("record:0", "Eye left", 10), second = layer("record:1", "Eye right", 11);
        auto renamed = first; renamed.id = "record:1"; renamed.name = "Renamed"; renamed.visible = false; renamed.bounds.translate(99, -17);
        auto moved = second; moved.id = "record:0";
        const auto result = compare(revision("old", {first, second}), revision("new", {moved, renamed}));
        QCOMPARE(result.matches.size(), 2); QCOMPARE(result.matches[0].status, Matcher::Status::Matched);
        QCOMPARE(result.matches[0].matchedLayerId, QString("record:1")); QCOMPARE(result.matches[1].matchedLayerId, QString("record:0"));
        QVERIFY(result.unmatchedNewLayerIds.isEmpty());
    }
    void identicalExternalIdsAcrossFamiliesNeverAutoLink() {
        const auto source = layer("record:0", "Layer", 7);
        Matcher::Result output; output.unmatchedNewLayerIds = {"baseline"}; QString error;
        QVERIFY(!Matcher::Compare(revision("one", {source}, "family-one"), revision("two", {source}, "family-two"), output, &error));
        QVERIFY(error.contains(QStringLiteral("不同素材来源"))); QCOMPARE(output.unmatchedNewLayerIds, QStringList{"baseline"});
    }
    void recordIdentifiersNeverBecomeCrossRevisionIdentity() {
        auto old = layer("record:0", "Old"), next = layer("record:0", "New");
        next.bounds = QRect(0, 0, 17, 19); next.imageDigest = QByteArray(32, 'x');
        auto result = compare(revision("old", {old}), revision("new", {next}));
        QCOMPARE(result.matches[0].status, Matcher::Status::Missing); QVERIFY(result.matches[0].matchedLayerId.isEmpty());
        next.bounds = old.bounds; result = compare(revision("old", {old}), revision("new", {next}));
        QCOMPARE(result.matches[0].status, Matcher::Status::NeedsConfirmation); QVERIFY(result.matches[0].matchedLayerId.isEmpty());
    }
    void duplicatesAndMissingIdsRequireConfirmationWithoutConsumingTargets() {
        auto a = layer("a", "Layer", 7), b = layer("b", "Layer", 7), next = layer("new", "Layer", 7);
        const auto previous = revision("old", {a, b}), candidate = revision("new", {next});
        const auto result = compare(previous, candidate);
        for (const auto& value : result.matches) {
            QCOMPARE(value.status, Matcher::Status::NeedsConfirmation); QCOMPARE(value.candidates.size(), 1);
            QCOMPARE(value.candidates[0].layerId, QString("new")); QVERIFY(value.matchedLayerId.isEmpty());
        }
        QCOMPARE(result.unmatchedNewLayerIds, QStringList{"new"});
        const auto again = compare(previous, candidate); QCOMPARE(again.matches[1].candidates[0].layerId, result.matches[0].candidates[0].layerId);
        a.externalId.reset(); auto missing = compare(revision("old", {a}), candidate);
        QCOMPARE(missing.matches[0].status, Matcher::Status::NeedsConfirmation);
        a.externalId = 7; next.id = "new1"; b.id = "new2";
        auto duplicatedNew = compare(revision("old", {a}), revision("new", {next, b}));
        QCOMPARE(duplicatedNew.matches[0].status, Matcher::Status::NeedsConfirmation); QCOMPARE(duplicatedNew.matches[0].candidates.size(), 2);
    }
    void typeChangesAndUnsupportedImagesAreIncompatible() {
        auto old = layer("old", "Layer", 7), next = layer("new", "Layer", 7);
        next.kind = Artwork::LayerKind::Group; next.imageDigest.clear();
        auto result = compare(revision("old", {old}), revision("new", {next}));
        QCOMPARE(result.matches[0].status, Matcher::Status::Incompatible); QVERIFY(!result.matches[0].candidates[0].compatible);
        next.kind = Artwork::LayerKind::Raster; next.pixelEquivalent = false;
        result = compare(revision("old", {old}), revision("new", {next}));
        QCOMPARE(result.matches[0].status, Matcher::Status::Incompatible); QVERIFY(result.matches[0].matchedLayerId.isEmpty());
        QCOMPARE(result.unmatchedNewLayerIds, QStringList{"new"});
    }
    void hierarchyAndContentAreSuggestionsOnly() {
        auto parent = layer("g", "Face"); parent.kind = Artwork::LayerKind::Group; parent.imageDigest.clear();
        auto old = layer("old", "Name before"); old.parentIndex = 0;
        auto next = old; next.id = "new"; next.name = "Name after"; next.bounds = QRect(0, 0, 11, 12); next.imageDigest = QByteArray(32, 'x');
        auto result = compare(revision("old", {parent, old}), revision("new", {parent, next}));
        QCOMPARE(result.matches.size(), 1); QCOMPARE(result.matches[0].status, Matcher::Status::NeedsConfirmation);
        QCOMPARE(result.matches[0].candidates.size(), 1); QVERIFY(result.matches[0].candidates[0].reasons.join('\n').contains("hierarchy:"));
        next.parentIndex = -1; next.imageDigest = old.imageDigest;
        result = compare(revision("old", {parent, old}), revision("new", {next}));
        QCOMPARE(result.matches[0].status, Matcher::Status::NeedsConfirmation); QVERIFY(result.matches[0].candidates[0].reasons.join('\n').contains("pixels:"));
    }
    void emptyAndHiddenLayersRemainMatchableAndNewLayersAreListed() {
        auto empty = layer("empty", "Empty", 8); empty.bounds = {}; empty.imageDigest.clear(); empty.visible = false;
        auto next = empty; next.id = "new-empty"; auto added = layer("added", "New art", 9);
        const auto result = compare(revision("old", {empty}), revision("new", {next, added}));
        QCOMPARE(result.matches[0].status, Matcher::Status::Matched); QCOMPARE(result.matches[0].matchedLayerId, QString("new-empty"));
        QCOMPARE(result.unmatchedNewLayerIds, QStringList{"added"});
    }
    void cancellationAndCallbackMutationCannotChangeFrozenMatchInputs() {
        auto old = revision("old", {layer("old-layer", "Layer", 7)}), next = revision("new", {layer("new-layer", "Layer", 7)});
        CExportControl control; control.SetHandler([&](const auto&, QString*) { next.familyId = "foreign"; next.layers.clear(); return true; });
        Matcher::Result result; QString error; QVERIFY2(Matcher::Compare(old, next, result, &error, &control), qPrintable(error));
        QCOMPARE(result.matches[0].matchedLayerId, QString("new-layer"));
        result.unmatchedNewLayerIds = {"baseline"}; CExportControl canceled; canceled.Cancel();
        QVERIFY(!Matcher::Compare(old, next, result, &error, &canceled)); QCOMPARE(result.unmatchedNewLayerIds, QStringList{"baseline"});
    }
    void cyclesDeepTreesAndCandidateExplosionFailAtomically() {
        auto g = layer("g", "Group"); g.kind = Artwork::LayerKind::Group; g.parentIndex = 0; g.imageDigest.clear();
        Matcher::Result result; result.unmatchedNewLayerIds = {"baseline"}; QString error;
        QVERIFY(!Matcher::Compare(revision("old", {g}), revision("new", {}), result, &error));
        QVector<Artwork::Layer> deep;
        for (int index = 0; index < Artwork::MaximumDepth + 2; ++index) { auto group = g; group.id = QString::number(index); group.parentIndex = index - 1; deep.append(group); }
        QVERIFY(!Matcher::Compare(revision("old", deep), revision("new", {}), result, &error)); QVERIFY(error.contains(QStringLiteral("深度")));
        QVector<Artwork::Layer> many;
        for (int index = 0; index < 320; ++index) many.append(layer(QString::number(index), "same"));
        QVERIFY(!Matcher::Compare(revision("old", many), revision("new", many), result, &error)); QVERIFY(error.contains(QStringLiteral("预算")));
        QCOMPARE(result.unmatchedNewLayerIds, QStringList{"baseline"});
    }
    void borrowedMetadataIsFrozenBeforeProgressCallbacks() {
        QChar name[]{QChar('a'), QChar('b'), QChar('c')}; QByteArray pixels(32, 'p');
        auto first = layer("old", QString::fromRawData(name, 3)); first.imageDigest = QByteArray::fromRawData(pixels.constData(), pixels.size());
        auto next = layer("new", "abc"); next.bounds = QRect(0, 0, 11, 12);
        const auto old = revision("old", {first}), newer = revision("new", {next});
        CExportControl control; control.SetHandler([&](const auto&, QString*) { name[0] = QChar('x'); pixels.fill('z'); return true; });
        Matcher::Result result; QString error; QVERIFY2(Matcher::Compare(old, newer, result, &error, &control), qPrintable(error));
        QCOMPARE(result.matches[0].status, Matcher::Status::NeedsConfirmation); QCOMPARE(result.matches[0].candidates.size(), 1);
        const auto reasons = result.matches[0].candidates[0].reasons.join('\n'); QVERIFY(reasons.contains("name:")); QVERIFY(reasons.contains("pixels:"));
    }
};
QTEST_GUILESS_MAIN(PsdLayerMatcherTests)
#include "psd_layer_matcher_tests.moc"
