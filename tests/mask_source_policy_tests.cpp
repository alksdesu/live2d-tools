#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/cmaskeditpolicy.h"
#include "deformer_replay_fixture.h"
#include <QtTest>
#include <QJsonDocument>
#include <QJsonArray>

using namespace live2dstruct;
using Columns = DeformerReplayFixture::RawColumns;
namespace {
QByteArray withMasks(QByteArray source, const QVector<int>& values, int owner = 0)
{
    Columns raw(source);
    const auto span = raw.spans.value("drawable_masks.art_mesh_sources_indices");
    auto pool = raw.column(span.name);
    const int begin = pool.size() / 4;
    pool.resize(pool.size() + values.size() * 4);
    for (int index = 0; index < values.size(); ++index)
        qToLittleEndian(qint32(values[index]), pool.data() + (begin + index) * 4);
    raw.state.columns[span.slot] = pool;
    raw.state.counts[span.countIndex] = pool.size() / 4;
    raw.set("art_meshes.drawable_mask_sources_begin_indices", owner, begin);
    raw.set("art_meshes.drawable_mask_sources_counts", owner, values.size());
    return raw.encode();
}

QByteArray fixture(const QVector<int>& masks = { 1, 1, 2 })
{
    return withMasks(DeformerReplayFixture::read("Mark"), masks);
}

bool load(CModelDocument& document, const QByteArray& bytes, QString* error = nullptr)
{
    return document.LoadBytes(bytes, QStringLiteral("source-mask-fixture.moc3"), error);
}

QVector<CModelDiagnostic> masks(const CModelDocument& document)
{
    QVector<CModelDiagnostic> result;
    for (const auto& diagnostic : document.Validate())
        if (diagnostic.code.startsWith("mask"))
            result.append(diagnostic);
    return result;
}

CModelSession capture(const CModelDocument& document)
{
    CModelSession result;
    result.version = 13;
    result.sourceMoc = document.SourcePath();
    result.sourceDigest = document.SourceDigest();
    result.deformerDisplayNames = document.DeformerDisplayNames();
    result.structure = document.ParameterStructure();
    result.meshStructure = document.HasMeshStructure();
    result.materialStructure = document.HasMaterialStructure();
    result.drawableMaterials = document.DrawableMaterialOverrides();
    result.deformerStructure = document.HasDeformerStructure();
    if (result.deformerStructure)
        result.sourceDeformerIds = document.SourceDeformerIds();
    result.drawableMaskIndices = document.DrawableMaskIndexOverrides();
    result.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
    for (auto it = document.ParameterValues().cbegin(); it != document.ParameterValues().cend(); ++it)
        result.parameters.insert(it.key(), it.value());
    result.parameterMetadata = document.ParameterMetadataOverrides();
    return result;
}

bool createParameter(CModelDocument& document, CModelEdit& command, QString* error)
{
    CMocParameterStructureBuilder::CreateRequest request;
    request.parameter.id = "ParamMaskSource";
    return document.MakeCreateParameterEdit(request, "Source mask", command, error) && document.ExecuteEdit(command);
}

bool setMasks(CModelDocument& document, const QVector<int>& values)
{
    const auto id = document.Drawables().front().id;
    CModelEdit command { id, "artmesh", CModelEdit::MaskIndices, QVariant::fromValue(document.DrawableMaskIndices(id)),
        QVariant::fromValue(values), -1, false };
    return document.ExecuteEdit(command);
}

QVector<int> previewMasks(const CModelDocument& document)
{
    QByteArray bytes;
    if (!document.BuildPreviewMoc(bytes))
        return { };
    auto reader = CReader::FromBytes(bytes);
    CSectionTable table(reader);
    if (!table.IsValid())
        return { };
    const auto* mesh = table.art_meshes[0];
    QVector<int> result;
    for (int at = 0; at < mesh->drawable_mask_sources_counts; ++at)
        result.append(table.drawable_masks[mesh->drawable_mask_sources_begin_indices + at]->art_mesh_sources_indices);
    return result;
}
}

class MaskSourcePolicyTests : public QObject
{
    Q_OBJECT
private slots:
    void diagnosticAggregateAndEqualityRemainCompatible()
    {
        CModelDiagnostic diagnostic { CModelDiagnostic::Error, "code", "message", 3 };
        QVERIFY(diagnostic.objectType.isEmpty());
        QVERIFY(diagnostic.objectId.isEmpty());
        const auto baseline = diagnostic;
        QCOMPARE(diagnostic, baseline);
        diagnostic.objectType = "artmesh";
        QVERIFY(diagnostic != baseline);
        auto second = diagnostic;
        second.objectId = "different";
        QVERIFY(second != diagnostic);
    }

    void retainedCountsRespectOriginalOwnerAndRequestedOrder_data()
    {
        QTest::addColumn<QVector<int>>("requested");
        QTest::addColumn<bool>("accepted");
        QTest::addColumn<bool>("warning");
        QTest::newRow("delete-B") << QVector<int> { 1, 1 } << true << true;
        QTest::newRow("keep-request-order") << QVector<int> { 2, 1, 1 } << true << true;
        QTest::newRow("append-new-unique") << QVector<int> { 1, 3, 1 } << true << true;
        QTest::newRow("explicit-dedupe") << QVector<int> { 1, 2 } << true << false;
        QTest::newRow("clear") << QVector<int> { } << true << false;
        QTest::newRow("add-third-A") << QVector<int> { 1, 1, 1 } << false << false;
        QTest::newRow("repeat-single-source-B") << QVector<int> { 2, 2 } << false << false;
        QTest::newRow("new-repeated-C") << QVector<int> { 3, 3 } << false << false;
        QTest::newRow("self") << QVector<int> { 0 } << false << false;
        QTest::newRow("negative") << QVector<int> { -1 } << false << false;
        QTest::newRow("range") << QVector<int> { 999999 } << false << false;
    }
    void retainedCountsRespectOriginalOwnerAndRequestedOrder()
    {
        QFETCH(QVector<int>, requested);
        QFETCH(bool, accepted);
        QFETCH(bool, warning);
        CModelDocument document;
        QString error;
        QVERIFY2(load(document, fixture(), &error), qPrintable(error));
        const auto id = document.Drawables().front().id;
        const auto before = document.AuthoringStructureState();
        const auto revision = document.Revision();
        QCOMPARE(setMasks(document, requested), accepted);
        if (!accepted) {
            QCOMPARE(document.AuthoringStructureState(), before);
            QCOMPARE(document.Revision(), revision);
            return;
        }
        QCOMPARE(document.DrawableMaskIndices(id), requested);
        QCOMPARE(previewMasks(document), requested);
        const auto diagnostics = masks(document);
        QCOMPARE(diagnostics.size(), warning ? 1 : 0);
        if (warning) {
            QCOMPARE(diagnostics.front().severity, CModelDiagnostic::Warning);
            QCOMPARE(diagnostics.front().code, QString("mask_source_duplicate"));
            QCOMPARE(diagnostics.front().objectType, QString("artmesh"));
            QCOMPARE(diagnostics.front().objectId, id);
            QCOMPARE(diagnostics.front().index, 0);
        }
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DrawableMaskIndices(id), (QVector<int> { 1, 1, 2 }));
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DrawableMaskIndices(id), requested);
    }

    void ordinaryStructureBakeRetainsOriginalSourceAndHistory()
    {
        const auto bytes = fixture();
        CModelDocument document;
        QString error;
        QVERIFY(load(document, bytes, &error));
        const auto original = document;
        const auto id = document.Drawables().front().id;
        QVERIFY(setMasks(document, { 1, 1 }));
        const auto partial = document.AuthoringStructureState();
        CModelEdit structure;
        QVERIFY2(createParameter(document, structure, &error), qPrintable(error));
        QVERIFY(document.DrawableMaskIndexOverrides().isEmpty());
        QCOMPARE(document.SourceBytes(), bytes);
        QCOMPARE(previewMasks(document), (QVector<int> { 1, 1 }));
        QCOMPARE(masks(document).front().code, QString("mask_source_duplicate"));
        CModelSession parsed;
        QVERIFY(CModelSession::FromJson(capture(document).ToJson(), &parsed, &error));
        QCOMPARE(parsed.version, 13);
        CModelDocument restored;
        QVERIFY2(parsed.RestoreDocument(original, restored, &error), qPrintable(error));
        QCOMPARE(restored.AuthoringStructureState(), document.AuthoringStructureState());
        QCOMPARE(masks(restored), masks(document));
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.AuthoringStructureState(), partial);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DrawableMaskIndices(id), (QVector<int> { 1, 1, 2 }));
        QVERIFY(document.RedoEdit());
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DrawableMaskIndices(id), (QVector<int> { 1, 1 }));
    }

    void bakedReductionNeedsStructuralUndoToRestoreMultiplicity()
    {
        CModelDocument document;
        QString error;
        QVERIFY(load(document, fixture(), &error));
        QVERIFY(setMasks(document, { 1 }));
        CModelEdit command;
        QVERIFY(createParameter(document, command, &error));
        const auto state = document.AuthoringStructureState();
        QVERIFY(!setMasks(document, { 1, 1 }));
        QCOMPARE(document.AuthoringStructureState(), state);
        QVERIFY(document.UndoEdit());
        QVERIFY(document.UndoEdit());
        QCOMPARE(previewMasks(document), (QVector<int> { 1, 1, 2 }));
        QVERIFY(document.RedoEdit());
        QVERIFY(document.RedoEdit());
        QCOMPARE(previewMasks(document), (QVector<int> { 1 }));
    }

    void explicitRetainedOverrideUsesVersionFourteen()
    {
        CModelDocument document;
        QString error;
        QVERIFY(load(document, fixture(), &error));
        const auto original = document;
        QVERIFY(setMasks(document, { 1, 1 }));
        const auto session = capture(document);
        QVERIFY(session.HasMaskMultiplicityOverrides());
        const auto bytes = session.ToJson();
        QVERIFY(!bytes.isEmpty());
        QCOMPARE(QJsonDocument::fromJson(bytes).object()["version"].toInt(), 14);
        CModelSession parsed;
        QVERIFY2(CModelSession::FromJson(bytes, &parsed, &error), qPrintable(error));
        QCOMPARE(parsed.version, 14);
        QCOMPARE(parsed.drawableMaskIndices, session.drawableMaskIndices);
        CModelDocument restored;
        QVERIFY2(parsed.RestoreDocument(original, restored, &error), qPrintable(error));
        QCOMPARE(restored.DrawableMaskIndices(restored.Drawables().front().id), (QVector<int> { 1, 1 }));
        QCOMPARE(previewMasks(restored), (QVector<int> { 1, 1 }));
        auto json = QJsonDocument::fromJson(bytes).object();
        json["version"] = 13;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(), &parsed, &error));
        auto older = session;
        older.version = 13;
        const auto before = restored.AuthoringStructureState();
        QVERIFY(!older.RestoreDocument(original, restored, &error));
        QCOMPARE(restored.AuthoringStructureState(), before);
        json["version"] = 14;
        json.remove("deformerDisplayNames");
        QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(), &parsed, &error));
    }

    void versionFourteenCannotCreateOrTransferAllowance_data()
    {
        QTest::addColumn<int>("fault");
        QTest::newRow("extra-occurrence") << 0;
        QTest::newRow("other-owner") << 1;
        QTest::newRow("other-source") << 2;
        QTest::newRow("negative") << 3;
    }
    void versionFourteenCannotCreateOrTransferAllowance()
    {
        QFETCH(int, fault);
        CModelDocument original;
        QString error;
        QVERIFY(load(original, fixture(), &error));
        auto session = capture(original);
        session.version = 14;
        const auto id = original.Drawables().front().id;
        session.drawableMaskIndices.insert(fault == 1 ? original.Drawables()[3].id : id,
            fault == 0       ? QVector<int> { 1, 1, 1 }
                : fault == 3 ? QVector<int> { -1, -1 }
                             : QVector<int> { 1, 1 });
        auto bytes = session.ToJson();
        CModelSession decoded;
        if (fault == 3) {
            QVERIFY(!CModelSession::FromJson(bytes, &decoded, &error));
            return;
        }
        QVERIFY(CModelSession::FromJson(bytes, &decoded, &error));
        CModelDocument loaded = original;
        if (fault == 2) {
            QVERIFY(load(loaded, fixture({ 1, 2 }), &error));
            decoded.sourceDigest = loaded.SourceDigest();
        }
        auto output = original;
        const auto state = output.AuthoringStructureState();
        QVERIFY(!decoded.RestoreDocument(loaded, output, &error));
        QCOMPARE(output.AuthoringStructureState(), state);
    }

    void oldVersionJsonStillRejectsDuplicateOverrides_data()
    {
        QTest::addColumn<int>("version");
        for (int version = 1; version <= 13; ++version)
            QTest::newRow(qPrintable(QString::number(version))) << version;
    }
    void oldVersionJsonStillRejectsDuplicateOverrides()
    {
        QFETCH(int, version);
        CModelDocument document;
        QVERIFY(load(document, fixture()));
        auto session = capture(document);
        auto json = QJsonDocument::fromJson(session.ToJson()).object();
        json["version"] = version;
        if (version < 13)
            json.remove("deformerDisplayNames");
        if (version < 12)
            json.remove("artwork");
        if (version < 11)
            json.remove("textureWorkspace");
        if (version < 10) {
            json.remove("materialStructure");
            json.remove("drawableMaterials");
        }
        if (version < 9)
            json.remove("geometryObjectPairs");
        if (version < 8)
            json.remove("parameterOrganization");
        if (version < 7)
            json.remove("textures");
        if (version < 6) {
            json.remove("deformerStructure");
            json.remove("sourceDeformerIds");
        }
        if (version < 5)
            json.remove("meshStructure");
        if (version < 3) {
            json.remove("parameterResources");
            json.remove("physicsDraft");
        }
        json["drawableMaskIndices"] = QJsonObject { { document.Drawables().front().id, QJsonArray { 1, 1 } } };
        CModelSession decoded;
        QString error;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(), &decoded, &error));
        QVERIFY(error.contains(QStringLiteral("蒙版")));
    }

    void forgedStructureCannotAcquireSourceAllowance()
    {
        const auto source = DeformerReplayFixture::read("Mark");
        CModelDocument document;
        QString error;
        QVERIFY(load(document, source, &error));
        const auto forged = withMasks(source, { 1, 1 });
        CMocParameterStructureBuilder::ColumnState columns;
        QVERIFY(CMocParameterStructureBuilder::CaptureColumnState(source, forged, columns, &error));
        const auto state = document.AuthoringStructureState();
        for (int entry = 0; entry < 5; ++entry) {
            auto candidate = document;
            bool accepted = false;
            if (entry == 0)
                accepted = candidate.SetParameterStructure(columns, &error);
            else if (entry == 1)
                accepted = candidate.SetAuthoringStructure(columns, &error);
            else if (entry == 2)
                accepted = candidate.SetMaterialStructure(columns, false, false, { }, &error);
            else if (entry == 3)
                accepted = candidate.SetDeformerStructure(columns, false, document.SourceDeformerIds(), &error);
            else {
                auto session = capture(document);
                session.version = 14;
                session.structure = columns;
                CModelSession parsed;
                QVERIFY(CModelSession::FromJson(session.ToJson(), &parsed, &error));
                accepted = parsed.RestoreDocument(document, candidate, &error);
            }
            QVERIFY(!accepted);
            QCOMPARE(candidate.AuthoringStructureState(), state);
        }
    }

    void sourceSnapshotOwnsBorrowedBytesAndReloadReplacesIt()
    {
        auto bytes = fixture();
        auto* borrowed = bytes.data();
        const QByteArray snapshot(bytes.constData(), bytes.size());
        CModelDocument document;
        QString error;
        QVERIFY(load(document, QByteArray::fromRawData(borrowed, bytes.size()), &error));
        std::fill(borrowed, borrowed + bytes.size(), '\0');
        QCOMPARE(document.SourceBytes(), snapshot);
        QVERIFY(setMasks(document, { 1, 1 }));
        QCOMPARE(masks(document).front().severity, CModelDiagnostic::Warning);
        QVERIFY(load(document, fixture({ 1, 2 }), &error));
        QVERIFY(!setMasks(document, { 1, 1 }));
        QVERIFY(masks(document).isEmpty());
        document.Clear();
        QVERIFY(load(document, fixture(), &error));
        QVERIFY(setMasks(document, { 1, 1 }));
    }

    void invalidOriginalReferencesStayErrors_data()
    {
        QTest::addColumn<QVector<int>>("values");
        QTest::newRow("self") << QVector<int> { 0, 0 };
        QTest::newRow("range") << QVector<int> { 999, 999 };
        QTest::newRow("negative") << QVector<int> { -1, -1 };
    }
    void invalidOriginalReferencesStayErrors()
    {
        QFETCH(QVector<int>, values);
        CModelDocument document;
        QString error;
        QVERIFY(load(document, fixture(values), &error));
        QVERIFY(!masks(document).isEmpty());
        for (const auto& diagnostic : masks(document))
            QCOMPARE(diagnostic.severity, CModelDiagnostic::Error);
        QVERIFY(setMasks(document, { }));
        QVERIFY(masks(document).isEmpty());
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DrawableMaskIndices(document.Drawables().front().id), values);
        for (const auto& diagnostic : masks(document))
            QCOMPARE(diagnostic.severity, CModelDiagnostic::Error);
    }

    void ambiguousSourceIdsCannotProveRepeatedReferences_data()
    {
        QTest::addColumn<int>("duplicate");
        QTest::newRow("owner") << 0;
        QTest::newRow("target") << 1;
    }
    void ambiguousSourceIdsCannotProveRepeatedReferences()
    {
        QFETCH(int, duplicate);
        auto source = fixture();
        Columns raw(source);
        const auto span = raw.spans.value("art_meshes.ids");
        auto ids = raw.column(span.name);
        ids.replace(span.width * 2, span.width, ids.mid(span.width * duplicate, span.width));
        raw.state.columns[span.slot] = ids;
        source = raw.encode();
        CModelDocument document;
        QString error;
        QVERIFY(load(document, source, &error));
        bool errorFound = false;
        for (const auto& diagnostic : masks(document)) {
            QVERIFY(diagnostic.code != QString("mask_source_duplicate"));
            errorFound
                |= diagnostic.code == QString("mask_duplicate") && diagnostic.severity == CModelDiagnostic::Error;
        }
        QVERIFY(errorFound);
        QVERIFY(!setMasks(document, { 1, 1 }));
    }

    void rawPreviewPreservesOnlyItsBaselineMultiplicity()
    {
        const auto source = fixture();
        CModelDocument document;
        QVERIFY(load(document, source));
        const auto id = document.Drawables().front().id;
        CMocPreviewBuilder builder;
        QString error;
        QVERIFY(builder.Initialize(source, { }, &error));
        CMocPreviewBuilder::Overrides edits;
        edits.drawableMasks[id] = { 2, 1, 1 };
        QByteArray result;
        QVERIFY2(builder.BuildOverrides(edits, result, &error), qPrintable(error));
        CModelDocument changed;
        QVERIFY(load(changed, result));
        QCOMPARE(changed.DrawableMaskIndices(id), (QVector<int> { 2, 1, 1 }));
        edits.drawableMasks[id] = { 1, 1, 1 };
        const auto sentinel = result;
        QVERIFY(!builder.BuildOverrides(edits, result, &error));
        QCOMPARE(result, sentinel);
    }

    void preparedMeshTransactionKeepsOriginalSourceAllowance()
    {
        CModelDocument document;
        QString error;
        const auto source = fixture();
        QVERIFY(load(document, source, &error));
        QVERIFY(setMasks(document, { 1, 1 }));
        const auto before = document.AuthoringStructureState();
        const auto& drawable = document.Drawables().front();
        CMocMeshStructureBuilder::MeshEdit mesh;
        mesh.artMeshId = drawable.id;
        mesh.indices = document.DrawableIndices(drawable.id);
        for (int index = 0; index < drawable.vertexCount; ++index) {
            mesh.vertices.append({ { index }, { 1 } });
            mesh.oldToNew.append(index);
        }
        mesh.vertices.append({ { 0, 1 }, { .5, .5 } });
        CMocMeshStructureBuilder::Request request;
        request.meshes.append(mesh);
        CModelEdit command;
        QVERIFY2(document.MakeMeshStructureEdit(request, command, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(command));
        QCOMPARE(previewMasks(document), (QVector<int> { 1, 1 }));
        QCOMPARE(masks(document).front().code, QString("mask_source_duplicate"));
        QCOMPARE(document.SourceBytes(), source);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(document.RedoEdit());
        QCOMPARE(masks(document).front().code, QString("mask_source_duplicate"));
    }

    void versionFourteenWriterEnforcesReadableArrayLimits()
    {
        CModelDocument document;
        QVERIFY(load(document, fixture()));
        auto session = capture(document);
        const auto id = document.Drawables().front().id;
        session.drawableMaskIndices[id] = QVector<int>(10001, 1);
        QVERIFY(session.ToJson().isEmpty());
        session.drawableMaskIndices[id] = { -1, -1 };
        QVERIFY(session.ToJson().isEmpty());
        session.drawableMaskIndices[id] = { 1, 1 };
        session.deformerDisplayNames.reset();
        QVERIFY(session.ToJson().isEmpty());
    }

    void repeatedValidationUsesImmutableSourceIdentity()
    {
        CModelDocument document;
        QVERIFY(load(document, fixture()));
        const auto expected = document.Validate();
        QElapsedTimer elapsed;
        elapsed.start();
        for (int iteration = 0; iteration < 500; ++iteration)
            QCOMPARE(document.Validate(), expected);
        qInfo() << "500 mask provenance validations, ms:" << elapsed.elapsed();
    }

    void invalidRawSlicesCannotBecomeSilentEmptyMasks()
    {
        auto bytes = fixture();
        Columns raw(bytes);
        raw.set("art_meshes.drawable_mask_sources_begin_indices", 0, 999999);
        CModelDocument document;
        QString error;
        QVERIFY(load(document, raw.encode(), &error));
        bool found = false;
        for (const auto& diagnostic : masks(document))
            if (diagnostic.code == "mask_slice") {
                found = true;
                QCOMPARE(diagnostic.severity, CModelDiagnostic::Error);
                QCOMPARE(diagnostic.objectId, document.Drawables().front().id);
            }
        QVERIFY(found);
        raw.set("art_meshes.drawable_mask_sources_counts", 0, 0);
        QVERIFY(load(document, raw.encode(), &error));
        QVERIFY(masks(document).isEmpty());
        QVERIFY(load(document, bytes, &error));
        QCOMPARE(masks(document).front().code, QString("mask_source_duplicate"));
    }

    void workingSliceRepairAndHistoryDoNotReuseSourceErrors()
    {
        Columns damaged(fixture());
        damaged.set("art_meshes.drawable_mask_sources_begin_indices", 0, 999999);
        const auto source = damaged.encode();
        CModelDocument document;
        QString error;
        QVERIFY(load(document, source, &error));
        const auto sourceDiagnostics = masks(document);
        QVERIFY(std::any_of(sourceDiagnostics.cbegin(), sourceDiagnostics.cend(),
            [](const auto& item) { return item.code == "mask_slice"; }));
        Columns repaired(source);
        repaired.set("art_meshes.drawable_mask_sources_begin_indices", 0, 0);
        repaired.set("art_meshes.drawable_mask_sources_counts", 0, 0);
        QVERIFY2(document.SetParameterStructure(repaired.state, &error), qPrintable(error));
        QVERIFY(masks(document).isEmpty());
        QCOMPARE(document.SourceBytes(), source);
        const auto baseline = document.AuthoringStructureState();
        CModelEdit command;
        QVERIFY2(createParameter(document, command, &error), qPrintable(error));
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.AuthoringStructureState(), baseline);
        QVERIFY(masks(document).isEmpty());
        QVERIFY(document.RedoEdit());
        QVERIFY(masks(document).isEmpty());
        CModelSession parsed;
        QVERIFY(CModelSession::FromJson(capture(document).ToJson(), &parsed, &error));
        CModelDocument loaded, restored;
        QVERIFY(load(loaded, source, &error));
        QVERIFY2(parsed.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QVERIFY(masks(restored).isEmpty());
        QVERIFY(load(document, source, &error));
        const auto diagnostics = masks(document);
        QVERIFY(std::any_of(
            diagnostics.cbegin(), diagnostics.cend(), [](const auto& item) { return item.code == "mask_slice"; }));
    }

    void sharedPolicyRejectsInvalidTargetsAndExcessReferences()
    {
        QString error;
        QVERIFY(CMaskEditPolicy::Validate({ 1, 1, 2 }, { 2, 1, 3, 1 }, 0, 4, &error));
        QVERIFY(!CMaskEditPolicy::Validate({ 1, 1, 2 }, { 1, 1, 1 }, 0, 4, &error));
        QVERIFY(!CMaskEditPolicy::Validate({ 0, 0 }, { 0, 0 }, 0, 4, &error));
        QVERIFY(!CMaskEditPolicy::Validate({ 8 }, { 8 }, 0, 4, &error));
        QVERIFY(!CMaskEditPolicy::Validate({ }, { }, -1, 4, &error));
        QVERIFY(!CMaskEditPolicy::Validate({ }, QVector<int>(CMaskEditPolicy::MaximumReferences + 1, 1), 0, 4, &error));
    }

    void editedMultiplicityCannotExceedPersistedOverrideLimit()
    {
        const QVector<int> original(10002, 1), tooLarge(10001, 1), allowed(10000, 1);
        CModelDocument document; QString error;
        QVERIFY(load(document, fixture(original), &error));
        const auto loaded = document;
        const auto id = document.Drawables().front().id;
        QVERIFY(document.SetDrawableMaskIndices(id, original));
        QVERIFY(document.DrawableMaskIndexOverrides().isEmpty());
        QVERIFY(!capture(document).ToJson().isEmpty());
        const auto state = document.AuthoringStructureState();
        const auto revision = document.Revision();
        QVERIFY(!document.SetDrawableMaskIndices(id, tooLarge));
        QCOMPARE(document.AuthoringStructureState(), state);
        QCOMPARE(document.Revision(), revision);
        QVERIFY(!setMasks(document, tooLarge));
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(setMasks(document, allowed));
        QCOMPARE(previewMasks(document), allowed);
        auto session = capture(document);
        QVERIFY(!session.ToJson().isEmpty());
        CModelSession decoded;
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
        QCOMPARE(decoded.version, 14);
        CModelDocument restored;
        QVERIFY2(decoded.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QCOMPARE(restored.DrawableMaskIndices(id), allowed);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DrawableMaskIndices(id), original);
        QVERIFY(document.DrawableMaskIndexOverrides().isEmpty());
        QVERIFY(!capture(document).ToJson().isEmpty());
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DrawableMaskIndices(id), allowed);
        auto json = QJsonDocument::fromJson(session.ToJson()).object();
        QJsonArray oversized;
        for (int index : tooLarge) oversized.append(index);
        json["drawableMaskIndices"] = QJsonObject{{id, oversized}};
        QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(), &decoded, &error));
        session.version = 14;
        session.drawableMaskIndices[id] = tooLarge;
        const auto before = restored.AuthoringStructureState();
        QVERIFY(!session.RestoreDocument(loaded, restored, &error));
        QCOMPARE(restored.AuthoringStructureState(), before);
    }

    void officialMaoSourceBecomesOneStructuredWarning()
    {
        const auto path = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (path.isEmpty())
            return;
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        const auto diagnostics = masks(document);
        QCOMPARE(diagnostics.size(), 1);
        QCOMPARE(diagnostics[0].code, QString("mask_source_duplicate"));
        QCOMPARE(diagnostics[0].severity, CModelDiagnostic::Warning);
        QCOMPARE(diagnostics[0].objectId, QString("ArtMesh202"));
        const auto originals = document.DrawableMaskIndices("ArtMesh202");
        const auto source = document.SourceBytes();
        CModelEdit command;
        QVERIFY(createParameter(document, command, &error));
        QCOMPARE(document.SourceBytes(), source);
        QCOMPARE(document.DrawableMaskIndices("ArtMesh202"), originals);
        QCOMPARE(masks(document), diagnostics);
    }
};
QTEST_GUILESS_MAIN(MaskSourcePolicyTests)
#include "mask_source_policy_tests.moc"
