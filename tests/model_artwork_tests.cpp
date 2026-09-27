#include "live2dstruct/cmodelartwork.h"
#include "std/cpngimage.h"
#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <cmath>
#include <cstring>
#include <limits>

using namespace live2dstruct;
using Artwork = CModelArtwork;
namespace {
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256); }
QByteArray png()
{
    QImage image(3, 2, QImage::Format_ARGB32);
    image.fill(qRgba(19, 71, 139, 0)); image.setPixel(1, 0, qRgba(93, 211, 7, 123));
    QBuffer buffer; if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
    return buffer.data();
}
Artwork::State state()
{
    Artwork::State result; const auto pixels = png(); const auto image = digest(pixels);
    result.images.insert(image, {{3, 2}, pixels});
    result.families.insert("source", {"source", "原稿", Artwork::SourceKind::Psd, "revision"});
    Artwork::Revision revision;
    revision.id = "revision"; revision.familyId = "source"; revision.path = "/tmp/original.psd"; revision.fileName = "original.psd";
    revision.sourceBytes = QByteArray("8BPS\0immutable-source", 21); revision.sourceDigest = digest(revision.sourceBytes);
    revision.iccProfile = QByteArray("ICC\0bytes", 9); revision.canvasSize = {100, 200};
    Artwork::Layer group; group.id = "group"; group.name = "组"; group.kind = Artwork::LayerKind::Group; group.sourceIndex = 0; group.bounds = {0, 0, 0, 0};
    Artwork::Layer layer; layer.id = "record:1"; layer.name = "图层"; layer.parentIndex = 0; layer.sourceIndex = 1;
    layer.externalId = std::numeric_limits<quint32>::max(); layer.bounds = {-7, -11, 3, 2}; layer.imageDigest = image;
    layer.diagnostics = {"保留原始像素"}; layer.opacity = 173;
    revision.layers = {group, layer}; result.revisions.insert(revision.id, revision);
    result.materials.insert("material", {"material", "材质", "source", "revision", "record:1", Artwork::Boundary::Transparent, true, {}});
    result.placements.insert("placement", {"placement", "material", "atlas-id", QTransform(1, 0, 0, 1, 15, 23), {-7, -11, 3, 2}, 2, Artwork::Filter::ExactTexels});
    Artwork::Binding binding; binding.meshId = "mesh"; binding.placementId = "placement";
    binding.familyCoordinates = {{-7, -11}, {-4, -11}, {-7, -9}};
    binding.sampledMaterial = {"mesh", 3, {0.25f, 0.5f, 0.75f, 0.5f, 0.25f, 0.75f}};
    binding.topologyDigest = digest("triangles"); result.bindings.insert(binding.meshId, binding); return result;
}
QJsonObject json(const Artwork::State& value)
{ QJsonObject result; Artwork::ToJson(value, result); return result; }
}

class ModelArtworkTests : public QObject
{
    Q_OBJECT
private slots:
    void emptyStateIsRoundTrippable()
    {
        const Artwork::State empty; QVERIFY(empty.IsEmpty()); QString error; Artwork::Usage usage{7, 9, 11, 13};
        QVERIFY2(Artwork::Validate(empty, &usage, &error), qPrintable(error)); QCOMPARE(usage.encodedBytes, 0); QCOMPARE(usage.decodedBytes, 0); QCOMPARE(usage.coordinateCount, 0); QCOMPARE(usage.metadataBytes, 0);
        QJsonObject object; QVERIFY(Artwork::ToJson(empty, object, &error)); Artwork::State restored = state();
        QVERIFY(Artwork::FromJson(object, restored, &error)); QVERIFY(restored.IsEmpty());
        QByteArray hash; QVERIFY(Artwork::Fingerprint(empty, hash, &error)); QCOMPARE(hash.size(), 32);
    }
    void completeStateRoundTripsWithoutOriginalFile()
    {
        const auto original = state(); QString error; Artwork::Usage usage;
        QVERIFY2(Artwork::Validate(original, &usage, &error), qPrintable(error)); QCOMPARE(usage.decodedBytes, 24); QCOMPARE(usage.coordinateCount, 3);
        QCOMPARE(usage.encodedBytes, original.images.first().png.size() + original.revisions.first().sourceBytes.size() + original.revisions.first().iccProfile.size());
        QJsonObject object; QVERIFY(Artwork::ToJson(original, object, &error));
        const auto serialized = QJsonDocument(object).toJson(QJsonDocument::Compact);
        Artwork::State parsed; QVERIFY2(Artwork::FromJson(QJsonDocument::fromJson(serialized).object(), parsed, &error), qPrintable(error));
        QVERIFY(parsed == original); QCOMPARE(parsed.revisions.first().sourceBytes, original.revisions.first().sourceBytes);
        QCOMPARE(parsed.revisions.first().layers[1].bounds.topLeft(), QPoint(-7, -11));
        QCOMPARE(*parsed.revisions.first().layers[1].externalId, std::numeric_limits<quint32>::max());
        QImage image; QVERIFY(CPngImage::Decode(parsed.images.first().png, image)); QCOMPARE(image.pixel(0, 0), qRgba(19, 71, 139, 0));
        QByteArray before, after; QVERIFY(Artwork::Fingerprint(original, before)); QVERIFY(Artwork::Fingerprint(parsed, after)); QCOMPARE(before, after);
    }
    void preservesExactDoubleAndFloatBits()
    {
        auto original = state(); const double value = std::nextafter(0.125, 1.0);
        original.revisions["revision"].documentToFamily = QTransform(value, -0.0, 0.0, 0.0, 0.25, -0.0, -0.0, 2.0, 1.0);
        original.placements["placement"].sourceDomain = QRectF(-0.0, value, 3.0, 2.0);
        original.bindings["mesh"].familyCoordinates[0] = {-0.0, value}; original.bindings["mesh"].sampledMaterial.uvs[0] = -0.0f;
        Artwork::State parsed; QString error; QVERIFY2(Artwork::FromJson(json(original), parsed, &error), qPrintable(error)); QVERIFY(parsed == original);
        auto modified = original; modified.bindings["mesh"].familyCoordinates[0].setX(0.0); QVERIFY(modified != original);
        QByteArray a, b; QVERIFY(Artwork::Fingerprint(original, a)); QVERIFY(Artwork::Fingerprint(modified, b)); QVERIFY(a != b);
    }
    void sourcesAreTypedAndOldRevisionsRemainPinned()
    {
        auto original = state(); auto next = original.revisions["revision"]; next.id = "new";
        next.sourceBytes = "new PSD"; next.sourceDigest = digest(next.sourceBytes); next.layers[1].externalId = 17; next.layers[1].name = "新图层";
        original.revisions.insert(next.id, next); original.families["source"].currentRevision = next.id;
        original.materials["material"].missingInRevision = next.id;
        QVERIFY(Artwork::Validate(original)); Artwork::State parsed; QVERIFY(Artwork::FromJson(json(original), parsed));
        QCOMPARE(parsed.materials["material"].revisionId, QString("revision")); QCOMPARE(parsed.materials["material"].missingInRevision, QString("new"));
        QCOMPARE(parsed.revisions["new"].layers[1].id, parsed.revisions["revision"].layers[1].id);
        for (auto kind : {Artwork::SourceKind::Psd, Artwork::SourceKind::Png, Artwork::SourceKind::AtlasSnapshot}) {
            original.families["source"].kind = kind; QVERIFY(Artwork::FromJson(json(original), parsed)); QCOMPARE(parsed.families["source"].kind, kind);
        }
    }
    void duplicateExternalIdsAndUnsupportedUnboundLayersRemainInspectable()
    {
        auto original = state(); auto layer = original.revisions["revision"].layers[1];
        layer.id = "record:2"; layer.sourceIndex = 2; layer.pixelEquivalent = false; layer.blendMode = "mul "; layer.clipping = true;
        layer.diagnostics = {"存在未栅格化的图层效果"}; original.revisions["revision"].layers.append(layer);
        QVERIFY(Artwork::Validate(original)); Artwork::State parsed; QVERIFY(Artwork::FromJson(json(original), parsed)); QVERIFY(parsed == original);
        original.materials["material"].layerId = "record:2"; QVERIFY(!Artwork::Validate(original));
    }
    void emptyRasterAndDetachedBindingPreserveHistory()
    {
        auto original = state(); auto& layer = original.revisions["revision"].layers[1]; layer.bounds = {-1, -2, 0, 5}; layer.imageDigest.clear();
        auto& binding = original.bindings["mesh"]; binding.status = Artwork::BindingStatus::Detached; binding.detachedReason = "PNG 已手动替换";
        QVERIFY(Artwork::Validate(original)); Artwork::State parsed; QVERIFY(Artwork::FromJson(json(original), parsed)); QVERIFY(parsed == original);
        QCOMPARE(parsed.bindings["mesh"].sampledMaterial, original.bindings["mesh"].sampledMaterial);
    }
    void unsupportedLayersWithoutRgbKeepTheirBoundsAndOriginalSource()
    {
        auto original = state(); auto layer = original.revisions["revision"].layers[1];
        layer.id = "adjustment"; layer.sourceIndex = 2; layer.pixelEquivalent = false; layer.imageDigest.clear();
        layer.diagnostics = {"调整图层没有可直接使用的 RGB 像素"}; original.revisions["revision"].layers.append(layer);
        Artwork::Usage usage; QString error; QVERIFY2(Artwork::Validate(original, &usage, &error), qPrintable(error));
        QCOMPARE(usage.decodedBytes, 24); Artwork::State frozen, restored;
        QVERIFY(Artwork::Freeze(original, frozen)); QVERIFY(Artwork::FromJson(json(frozen), restored)); QVERIFY(restored == original);
        QCOMPARE(restored.images.size(), 1); QVERIFY(restored.revisions["revision"].layers[2].imageDigest.isEmpty());
        QCOMPARE(restored.revisions["revision"].layers[2].bounds, QRect(-7, -11, 3, 2));
        original.materials["material"].layerId = "adjustment"; QVERIFY(!Artwork::Validate(original));
        original.materials["material"].layerId = "record:1"; original.revisions["revision"].layers[2].pixelEquivalent = true;
        QVERIFY(!Artwork::Validate(original));
        original.revisions["revision"].layers[2].pixelEquivalent = false;
        original.revisions["revision"].layers[2].imageDigest = QByteArray(32, 'x'); QVERIFY(!Artwork::Validate(original));
    }
    void metadataUsageIncludesTextCoordinatesAndDescriptors()
    {
        const auto original = state(); Artwork::Usage baseline; QVERIFY(Artwork::Validate(original, &baseline));
        QVERIFY(baseline.metadataBytes > qint64(sizeof(Artwork::Binding) + sizeof(Artwork::Revision)));
        auto moreText = original; moreText.families["source"].name += QString(100, 'x'); Artwork::Usage textUsage;
        QVERIFY(Artwork::Validate(moreText, &textUsage)); QCOMPARE(textUsage.metadataBytes - baseline.metadataBytes, 200);
        QCOMPARE(textUsage.encodedBytes, baseline.encodedBytes); QCOMPARE(textUsage.decodedBytes, baseline.decodedBytes);
        auto morePoints = original; morePoints.bindings["mesh"].familyCoordinates.append({1, 2}); morePoints.bindings["mesh"].sampledMaterial.uvs << 0.1f << 0.2f;
        Artwork::Usage pointUsage; QVERIFY(Artwork::Validate(morePoints, &pointUsage)); QCOMPARE(pointUsage.coordinateCount, baseline.coordinateCount + 1);
        QCOMPARE(pointUsage.metadataBytes - baseline.metadataBytes, qint64(sizeof(QPointF) + 2 * sizeof(float)));
        auto moreRecords = original; auto material = moreRecords.materials["material"]; material.id = "material2"; moreRecords.materials.insert(material.id, material);
        Artwork::Usage recordUsage; QVERIFY(Artwork::Validate(moreRecords, &recordUsage)); QVERIFY(recordUsage.metadataBytes > baseline.metadataBytes + qint64(sizeof(Artwork::Material)));
        Artwork::State restored; QVERIFY(Artwork::FromJson(json(original), restored)); Artwork::Usage restoredUsage;
        QVERIFY(Artwork::Validate(restored, &restoredUsage)); QCOMPARE(restoredUsage.metadataBytes, baseline.metadataBytes);
    }
    void invalidState_data()
    {
        QTest::addColumn<int>("fault"); for (int fault = 0; fault < 39; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void invalidState()
    {
        QFETCH(int, fault); auto broken = state();
        switch (fault) {
        case 0: broken.images.first().png[10] ^= 1; break;
        case 1: broken.images.first().size = {9, 9}; break;
        case 2: broken.families["source"].id = "other"; break;
        case 3: broken.families["source"].kind = Artwork::SourceKind(99); break;
        case 4: broken.families["source"].currentRevision = "missing"; break;
        case 5: broken.revisions["revision"].sourceBytes = "changed"; break;
        case 6: broken.revisions["revision"].familyId = "missing"; break;
        case 7: broken.revisions["revision"].canvasSize = {0, 1}; break;
        case 8: broken.revisions["revision"].colorPolicy = Artwork::ColorPolicy(99); break;
        case 9: broken.revisions["revision"].layers[1].id = "group"; break;
        case 10: broken.revisions["revision"].layers[1].sourceIndex = 0; break;
        case 11: broken.revisions["revision"].layers[1].parentIndex = 1; break;
        case 12: broken.revisions["revision"].layers[0].parentIndex = 1; break;
        case 13: broken.revisions["revision"].layers[1].bounds = {0, 0, -1, 1}; break;
        case 14: broken.revisions["revision"].layers[1].imageDigest = QByteArray(32, 'x'); break;
        case 15: broken.revisions["revision"].layers[0].imageDigest = broken.images.firstKey(); break;
        case 16: broken.revisions["revision"].layers[1].blendMode = "none-too-long"; break;
        case 17: broken.revisions["revision"].layers[1].diagnostics.append(QString(1, QChar::Null)); break;
        case 18: broken.materials["material"].layerId = "group"; break;
        case 19: broken.materials["material"].boundary = Artwork::Boundary(99); break;
        case 20: broken.materials["material"].missingInRevision = "revision"; break;
        case 21: broken.placements["placement"].materialId = "missing"; break;
        case 22: broken.placements["placement"].atlasId.clear(); break;
        case 23: broken.placements["placement"].filter = Artwork::Filter(99); break;
        case 24: broken.placements["placement"].gutter = 65; break;
        case 25: broken.placements["placement"].sourceDomain.setWidth(0); break;
        case 26: broken.placements["placement"].familyToAtlas = QTransform(1, 2, 2, 4, 0, 0); break;
        case 27: broken.bindings["mesh"].sampledMaterial.id = "other"; break;
        case 28: broken.bindings["mesh"].familyCoordinates.removeLast(); break;
        case 29: broken.bindings["mesh"].familyCoordinates[0].setY(std::numeric_limits<double>::quiet_NaN()); break;
        case 30: broken.bindings["mesh"].status = Artwork::BindingStatus(99); break;
        case 31: broken.bindings["mesh"].detachedReason = "unexpected"; break;
        case 32: broken.bindings["mesh"].status = Artwork::BindingStatus::Detached; break;
        case 33: broken.bindings["mesh"].topologyDigest.clear(); break;
        case 34: broken.bindings["mesh"].sampledMaterial.uvs[0] = std::numeric_limits<float>::infinity(); break;
        case 35: broken.bindings["mesh"].placementId = "missing"; break;
        case 36: broken.revisions["revision"].path = QString(32769, 'a'); break;
        case 37: broken.revisions["revision"].layers[1].kind = Artwork::LayerKind(99); break;
        case 38: broken.placements["placement"].sourceDomain = {1e308, 0, 1e308, 3}; break;
        }
        QString error; Artwork::Usage usage{123, 456, 789, 101}; const auto original = state(); auto output = original;
        QVERIFY(!Artwork::Validate(broken, &usage, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(usage.encodedBytes, 123); QCOMPARE(usage.decodedBytes, 456); QCOMPARE(usage.coordinateCount, 789); QCOMPARE(usage.metadataBytes, 101);
        QVERIFY(!Artwork::Freeze(broken, output, &error)); QVERIFY(output == original);
        QJsonObject object{{"sentinel", 1}}; QVERIFY(!Artwork::ToJson(broken, object, &error)); QCOMPARE(object, (QJsonObject{{"sentinel", 1}}));
        QByteArray hash = "sentinel"; QVERIFY(!Artwork::Fingerprint(broken, hash, &error)); QCOMPARE(hash, QByteArray("sentinel"));
    }
    void resourceAndParentChainLimits()
    {
        auto broken = state(); auto& revision = broken.revisions["revision"];
        revision.layers.clear();
        for (int i = 0; i < Artwork::MaximumDepth + 2; ++i) {
            Artwork::Layer layer; layer.id = QString::number(i); layer.kind = Artwork::LayerKind::Group; layer.bounds = {0, 0, 0, 0}; layer.parentIndex = i - 1;
            revision.layers.append(layer);
        }
        QVERIFY(!Artwork::Validate(broken)); broken = state();
        broken.revisions["revision"].layers[1].diagnostics = QStringList(33000, QString(2048, 'x'));
        QVERIFY(!Artwork::Validate(broken)); broken = state();
        for (int i = 0; i <= Artwork::MaximumRecords; ++i) broken.families.insert(QString::number(i), {});
        QVERIFY(!Artwork::Validate(broken));
    }
    void affineValidationAvoidsAbsoluteDeterminantCutoff()
    {
        QVERIFY(Artwork::ValidateTransform(QTransform())); QVERIFY(Artwork::ValidateTransform(QTransform(1e-100, 0, 0, 1e-100, 0, 0)));
        QVERIFY(Artwork::ValidateTransform(QTransform(-2, 0, 0, 3, 4, 5)));
        QVERIFY(!Artwork::ValidateTransform(QTransform(1e-14, 0, 0, 1, 0, 0)));
        QVERIFY(!Artwork::ValidateTransform(QTransform(1, 0, 0.01, 0, 1, 0, 0, 0, 1)));
        QVERIFY(!Artwork::ValidateTransform(QTransform(1, 0, 0, 1, std::numeric_limits<double>::infinity(), 0)));
        QVERIFY(!Artwork::ValidateTransform(QTransform(1e-300, 0, 0, 1e-300, 1e300, 0)));
    }
    void topologyDigestAcceptsFullUnsignedIndicesAndEmptyMeshes()
    {
        QByteArray empty, vertices, wide, changed;
        QVERIFY(Artwork::TopologyDigest(0, {}, empty)); QVERIFY(Artwork::TopologyDigest(17, {}, vertices)); QVERIFY(empty != vertices);
        QVERIFY(Artwork::TopologyDigest(65536, {0, 32768, 65535}, wide));
        QVERIFY(Artwork::TopologyDigest(65536, {0, 65535, 32768}, changed)); QVERIFY(wide != changed);
        QByteArray held = wide; QVERIFY(!Artwork::TopologyDigest(65535, {0, 32768, 65535}, held)); QCOMPARE(held, wide);
        QVERIFY(!Artwork::TopologyDigest(0, {0, 0, 0}, held)); QVERIFY(!Artwork::TopologyDigest(-1, {}, held));
        QVERIFY(!Artwork::TopologyDigest(3, {0, 1}, held));
        QVERIFY(!Artwork::TopologyDigest(CModelMaterialEdit::MaximumValues / 2 + 1, {}, held));
        QVERIFY(!Artwork::TopologyDigest(1, QVector<quint16>(Artwork::MaximumTopologyIndices + 3, 0), held)); QCOMPARE(held, wide);
        auto source = state(); auto& binding = source.bindings["mesh"];
        binding.familyCoordinates.clear(); binding.sampledMaterial.uvs.clear(); binding.sampledMaterial.textureIndex = -1; binding.topologyDigest = empty;
        QVERIFY(Artwork::Validate(source)); Artwork::State restored; QVERIFY(Artwork::FromJson(json(source), restored)); QVERIFY(restored == source);
    }
    void freezeOwnsBorrowedStorageAndDeduplicatesContent()
    {
        auto borrowed = state(); QByteArray pixels = borrowed.images.first().png, raw = borrowed.revisions["revision"].sourceBytes;
        QString familyId = "source", layerName = "borrowed layer"; char* pixelStorage = pixels.data(); char* sourceStorage = raw.data();
        QChar* familyStorage = familyId.data(); QChar* layerStorage = layerName.data();
        borrowed.images.first().png = QByteArray::fromRawData(pixelStorage, pixels.size());
        borrowed.families["source"].id = QString::fromRawData(familyStorage, familyId.size());
        borrowed.revisions["revision"].sourceBytes = QByteArray::fromRawData(sourceStorage, raw.size());
        borrowed.revisions["revision"].iccProfile = QByteArray::fromRawData(sourceStorage, raw.size());
        borrowed.revisions["revision"].layers[1].name = QString::fromRawData(layerStorage, layerName.size());
        auto second = borrowed.revisions["revision"]; second.id = "second"; borrowed.revisions.insert(second.id, second);
        Artwork::State frozen; QString error; QVERIFY2(Artwork::Freeze(borrowed, frozen, &error), qPrintable(error));
        Artwork::State expected; QVERIFY(Artwork::FromJson(json(frozen), expected));
        QVERIFY(frozen.images.first().png.constData() != pixelStorage); QVERIFY(frozen.revisions["revision"].sourceBytes.constData() != sourceStorage);
        QVERIFY(frozen.revisions["revision"].sourceBytes.constData() == frozen.revisions["second"].sourceBytes.constData());
        QVERIFY(frozen.revisions["revision"].sourceBytes.constData() == frozen.revisions["revision"].iccProfile.constData());
        QVERIFY(frozen.bindings["mesh"].familyCoordinates.constData() != borrowed.bindings["mesh"].familyCoordinates.constData());
        QVERIFY(frozen.bindings["mesh"].sampledMaterial.uvs.constData() != borrowed.bindings["mesh"].sampledMaterial.uvs.constData());
        pixelStorage[15] ^= 1; sourceStorage[2] ^= 1; familyStorage[0] = 'X'; layerStorage[0] = 'Y';
        QVERIFY(frozen == expected); QVERIFY(Artwork::Validate(frozen));
    }
    void blobTableDeduplicatesSourceImageAndProfile()
    {
        auto original = state(); auto& revision = original.revisions["revision"];
        revision.sourceBytes = original.images.first().png; revision.sourceDigest = original.images.firstKey(); revision.iccProfile = revision.sourceBytes;
        Artwork::Usage usage; QVERIFY(Artwork::Validate(original, &usage)); QCOMPARE(usage.encodedBytes, revision.sourceBytes.size());
        const auto encoded = json(original); QCOMPARE(encoded["blobs"].toObject().size(), 1);
        Artwork::State parsed; QVERIFY(Artwork::FromJson(encoded, parsed)); QVERIFY(parsed == original);
        QVERIFY(parsed.images.first().png.constData() == parsed.revisions.first().sourceBytes.constData());
    }
    void imageHeadersDoNotSubstituteForCompleteDecoding()
    {
        auto broken = state(); const auto png = broken.images.first().png; const auto idat = png.indexOf("IDAT"); QVERIFY(idat > 0);
        auto partial = png.left(idat + 4); QSize size;
        QVERIFY(CPngImage::Size(partial, size)); const auto key = digest(partial);
        broken.images.clear(); broken.images.insert(key, {size, partial}); broken.revisions["revision"].layers[1].imageDigest = key;
        QString error; QVERIFY(!Artwork::Validate(broken, nullptr, &error)); QVERIFY(!error.isEmpty());
    }
    void equalNullAndEmptyMetadataHaveIdenticalFingerprints()
    {
        auto first = state(), second = first;
        first.materials["material"].missingInRevision = QString(); second.materials["material"].missingInRevision = QStringLiteral("");
        first.bindings["mesh"].detachedReason = QString(); second.bindings["mesh"].detachedReason = QStringLiteral("");
        first.revisions["revision"].iccProfile = QByteArray(); second.revisions["revision"].iccProfile = QByteArray("");
        QVERIFY(first == second); QByteArray a, b; QVERIFY(Artwork::Fingerprint(first, a)); QVERIFY(Artwork::Fingerprint(second, b)); QCOMPARE(a, b);
    }
    void malformedJson_data()
    {
        QTest::addColumn<int>("fault"); for (int fault = 0; fault < 18; ++fault) QTest::newRow(qPrintable(QString::number(fault))) << fault;
    }
    void malformedJson()
    {
        QFETCH(int, fault); const auto original = state(); auto object = json(original);
        if (fault == 0) object["version"] = 2;
        if (fault == 1) object["extra"] = 1;
        if (fault == 2) object.remove("blobs");
        if (fault == 3) object["bindings"] = QJsonArray{};
        if (fault >= 4 && fault <= 7) {
            auto blobs = object["blobs"].toObject(); const auto key = blobs.keys().first();
            if (fault == 4) blobs[key] = "!!!!";
            if (fault == 5) blobs[key] = blobs[key].toString() + "\n";
            if (fault == 6) blobs[key] = QString::fromLatin1(QByteArray("wrong").toBase64());
            if (fault == 7) blobs[QString::fromLatin1(digest("unused").toHex())] = QString::fromLatin1(QByteArray("unused").toBase64());
            object["blobs"] = blobs;
        }
        if (fault >= 8 && fault <= 11) {
            auto rows = object["placements"].toObject(); auto row = rows["placement"].toObject();
            if (fault == 8) row["gutter"] = 1.5;
            if (fault == 9) row["familyToAtlas"] = "00";
            if (fault == 10) row["sourceDomain"] = QString(64, 'z');
            if (fault == 11) row["filter"] = "0";
            rows["placement"] = row; object["placements"] = rows;
        }
        if (fault >= 12 && fault <= 14) {
            auto rows = object["revisions"].toObject(); auto row = rows["revision"].toObject(); auto layers = row["layers"].toArray(); auto layer = layers[1].toObject();
            if (fault == 12) layer["externalId"] = 4294967296.0;
            if (fault == 13) layer["bounds"] = QJsonArray{2147483647, 0, 3, 2};
            if (fault == 14) layer["visible"] = 1;
            layers[1] = layer; row["layers"] = layers; rows["revision"] = row; object["revisions"] = rows;
        }
        if (fault >= 15) {
            auto rows = object["bindings"].toObject(); auto row = rows["mesh"].toObject();
            if (fault == 15) row["familyCoordinates"] = row["familyCoordinates"].toString().toUpper();
            if (fault == 16) row["familyCoordinates"] = "000000000000f87f0000000000000000";
            if (fault == 17) row["sampledMaterial"] = QJsonObject{};
            rows["mesh"] = row; object["bindings"] = rows;
        }
        auto output = original; QString error; QVERIFY(!Artwork::FromJson(object, output, &error)); QVERIFY(output == original); QVERIFY(!error.isEmpty());
    }
    void allAuthorDecisionsAffectFingerprint()
    {
        const auto original = state(); QByteArray before; QVERIFY(Artwork::Fingerprint(original, before));
        for (int field = 0; field < 12; ++field) {
            auto changed = original;
            if (field == 0) changed.families["source"].kind = Artwork::SourceKind::AtlasSnapshot;
            if (field == 1) changed.revisions["revision"].path = "/relocated/source.psd";
            if (field == 2) changed.revisions["revision"].iccProfile = "different ICC";
            if (field == 3) changed.revisions["revision"].colorPolicy = Artwork::ColorPolicy::ConvertToSRgb;
            if (field == 4) changed.revisions["revision"].layers[1].opacity = 17;
            if (field == 5) changed.materials["material"].applyLayerOpacity = false;
            if (field == 6) changed.placements["placement"].filter = Artwork::Filter::LinearLightPremultiplied;
            if (field == 7) changed.placements["placement"].familyToAtlas.translate(1, 2);
            if (field == 8) changed.bindings["mesh"].familyCoordinates[0].rx() += 0.25;
            if (field == 9) changed.bindings["mesh"].sampledMaterial.uvs[0] = 0.33f;
            if (field == 10) changed.bindings["mesh"].topologyDigest = digest("other triangles");
            if (field == 11) { changed.bindings["mesh"].status = Artwork::BindingStatus::Detached; changed.bindings["mesh"].detachedReason = "external PNG"; }
            QByteArray after; QVERIFY(Artwork::Fingerprint(changed, after)); QVERIFY2(before != after, qPrintable(QString::number(field)));
        }
    }
};
QTEST_GUILESS_MAIN(ModelArtworkTests)
#include "model_artwork_tests.moc"
