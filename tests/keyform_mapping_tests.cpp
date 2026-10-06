#include "live2dstruct/ckeyformmappingcontext.h"
#include "deformer_snapshot_fixture.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/csectiontable.h"

#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

using namespace live2dstruct;

using namespace DeformerSnapshotData;

class KeyformMappingTests : public QObject
{
    Q_OBJECT
private slots:
    void sourceModelsHaveSupportedMeshes_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* name : {"Mark", "Rice", "Natori", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }

    void sourceModelsHaveSupportedMeshes()
    {
        QFETCH(QString, name);
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(sample(name), &error), qPrintable(error));
        bool found = false;
        for (const auto& mesh : document.Drawables()) {
            QMap<QString, float> corner;
            if (!document.KeyformParameterValues(mesh.id, "artmesh", 0, corner, &error)) continue;
            auto values = pose(document);
            for (auto it = corner.cbegin(); it != corner.cend(); ++it) values.insert(it.key(), it.value());
            CKeyformMappingContext context;
            if (!context.Build(document, mesh.id, 0, values, &error)) continue;
            const auto local = document.DrawableKeyformPositions(mesh.id, 0);
            QVector<float> world, restored;
            QVERIFY(context.ToModel(local, world));
            QVERIFY(context.MatchesRuntime(world));
            QVERIFY(context.FromModel(world, restored));
            QVERIFY(near(local, restored));
            found = true;
            break;
        }
        QVERIFY2(found, qPrintable(name + ": " + error));
    }

    void nestedRotationWarpRotationUsesIndependentExpectedPoints_data()
    {
        QTest::addColumn<bool>("quad");
        QTest::newRow("bilinear") << true;
        QTest::newRow("triangles") << false;
    }

    void nestedRotationWarpRotationUsesIndependentExpectedPoints()
    {
        QFETCH(bool, quad);
        Fixture fixture(quad);
        QVERIFY(fixture.table.IsValid());
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(fixture.bytes, "nested.moc3", &error), qPrintable(error));
        CKeyformMappingContext context;
        QVERIFY2(context.Build(document, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
        const auto expected = coreCoordinates(rigidMeshCanvasPositions(quad));
        const auto local = document.DrawableKeyformPositions("Mesh", 0);
        for (int repeat = 0; repeat < 10; ++repeat) {
            QVector<float> world;
            QVERIFY(context.ToModel(local, world));
            QVERIFY(near(world, expected));
            QVERIFY(context.MatchesRuntime(expected));
            QVERIFY(context.FromModel(world, world));
            QVERIFY(near(world, local));
        }
        CKeyformMappingContext copied = context;
        document.Clear();
        QVERIFY(copied.MatchesRuntime(expected));
        QVector<float> localSubset;
        QVERIFY(copied.FromModel(expected.mid(0, 2), localSubset));
        QVERIFY(near(localSubset, local.mid(0, 2)));
    }

    void overridesAndDiskChangesDoNotRetargetAnExistingContext()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("model.moc3");
        QVERIFY(write(path, fixture.bytes));
        CModelDocument document;
        QVERIFY(document.Load(path));
        auto local = document.DrawableKeyformPositions("Mesh", 1);
        local[0] = 0.25f;
        QVERIFY(document.SetDrawableKeyformPositions("Mesh", 1, local));
        CKeyformMappingContext context;
        QString error;
        QVERIFY(write(path, "replaced on disk"));
        QVERIFY2(context.Build(document, "Mesh", 1, {{"P", 1}}, &error), qPrintable(error));
        QVector<float> world;
        QVERIFY(context.ToModel(local, world));
        QVERIFY(context.MatchesRuntime(world));
        local[0] += 0.1f;
        QVERIFY(document.SetDrawableKeyformPositions("Mesh", 1, local));
        QVERIFY(QFile::remove(path));
        QVERIFY(context.MatchesRuntime(world));
        CKeyformMappingContext changed;
        QVERIFY2(changed.Build(document, "Mesh", 1, {{"P", 1}}, &error), qPrintable(error));
        QVERIFY(!changed.MatchesRuntime(world));
        document.Clear();
        QVERIFY(context.MatchesRuntime(world));
    }

    void unsupportedChainsAndInvalidBuildsPreservePreviousContext_data()
    {
        QTest::addColumn<QString>("invalid");
        for (const char* value : {"wrong-corner", "missing-pose", "extra-pose", "nan-pose", "out-of-range", "missing-mesh",
                                  "bad-keyform", "cycle", "missing-parent", "rotation-collapse", "folded-warp", "type1-target",
                                  "type1-parent", "blend-mesh", "blend-parent", "glue", "edited-collapse", "edited-folded"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }

    void unsupportedChainsAndInvalidBuildsPreservePreviousContext()
    {
        QFETCH(QString, invalid);
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        CModelDocument baseline;
        QVERIFY(baseline.LoadBytes(fixture.bytes, "baseline"));
        CKeyformMappingContext context;
        QString error;
        QVERIFY2(context.Build(baseline, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
        QVector<float> expected;
        QVERIFY(context.ToModel(baseline.DrawableKeyformPositions("Mesh", 0), expected));
        const auto& o = *fixture.table.sectionOffsetTable;
        auto values = pose(baseline);
        QString id = "Mesh";
        int keyform = 0;
        if (invalid == "wrong-corner") values["P"] = 0.5f;
        if (invalid == "missing-pose") values.clear();
        if (invalid == "extra-pose") values.insert("unknown", 0);
        if (invalid == "nan-pose") values["P"] = std::numeric_limits<float>::quiet_NaN();
        if (invalid == "out-of-range") values["P"] = 2;
        if (invalid == "missing-mesh") id = "missing";
        if (invalid == "bad-keyform") keyform = 2;
        if (invalid == "cycle") fixture.column(o.deformers->parent_deformer_indices, 0, 2);
        if (invalid == "missing-parent") fixture.column(o.deformers->parent_deformer_indices, 0, 3);
        if (invalid == "rotation-collapse") real(fixture.bytes, o.rotation_deformer_keyforms->scales, 0);
        if (invalid == "folded-warp" || invalid == "edited-folded")
            fixture.column(o.art_meshes->parent_deformer_indices, 0, 1);
        if (invalid == "folded-warp") {
            const QVector<float> folded{0, 0, 2, 2, 0, 2, 2, 0};
            for (int i = 0; i < folded.size(); ++i) real(fixture.bytes, o.keyform_positions->xys + (12 + i) * 4, folded[i]);
            real(fixture.bytes, o.keyform_positions->xys, 0.5f);
            real(fixture.bytes, o.keyform_positions->xys + 4, 0);
        }
        if (invalid == "type1-target" || invalid == "type1-parent") {
            put(fixture.bytes, integer(fixture.bytes, 0x208), 1);
            if (invalid == "type1-parent") {
                fixture.column(o.art_meshes->keyform_binding_sources_indices, 0, -1);
                fixture.column(o.art_meshes->keyform_sources_counts, 0, 1);
                fixture.column(o.deformers->keyform_binding_sources_indices, 0, 0);
                fixture.column(o.rotation_deformers->keyform_sources_counts, 0, 2);
            }
        }
        if (invalid == "blend-mesh") {
            const bool mesh = invalid == "blend-mesh";
            fixture.count(mesh ? 28 : 27, 1);
            fixture.appendColumn(mesh ? 0x240 : 0x234, {0});
            fixture.appendColumn(mesh ? 0x244 : 0x238, {0});
            fixture.appendColumn(mesh ? 0x248 : 0x23c, {0});
        }
        if (invalid == "glue") {
            fixture.count(20, 1);
            for (int slot : {0x1a4, 0x1ac, 0x1b0, 0x1b4, 0x1b8, 0x1bc, 0x1c0, 0x1c4})
                fixture.appendColumn(slot, {slot == 0x1ac ? -1 : 0});
            const int offset = fixture.bytes.size();
            fixture.bytes.append(QByteArray("Glue").leftJustified(64, '\0'));
            put(fixture.bytes, 0x1a8, offset);
        }
        CModelDocument invalidDocument;
        QVERIFY2(invalidDocument.LoadBytes(fixture.bytes, "invalid-candidate", &error), qPrintable(error));
        if (invalid == "edited-folded") {
            auto forms = invalidDocument.DeformerWarpKeyforms("Warp");
            QVERIFY(!forms.isEmpty());
            forms[0].positions = {0, 0, 2, 2, 0, 2, 2, 0};
            QVERIFY(invalidDocument.SetDeformerWarpKeyforms("Warp", forms));
            auto local = invalidDocument.DrawableKeyformPositions("Mesh", 0);
            local[0] = 0.5f;
            local[1] = 0;
            QVERIFY(invalidDocument.SetDrawableKeyformPositions("Mesh", 0, local));
        }
        if (invalid == "edited-collapse") {
            auto forms = invalidDocument.DeformerRotationKeyforms("Root");
            QVERIFY(!forms.isEmpty());
            forms[0].scale = 0;
            QVERIFY(invalidDocument.SetDeformerRotationKeyforms("Root", forms));
        }
        if (invalid == "blend-mesh" || invalid == "blend-parent") {
            QVERIFY2(context.Build(invalidDocument, id, keyform, values, &error), qPrintable(invalid));
            QVERIFY(context.IsValid());
            QVERIFY(context.MatchesRuntime(expected));
        } else {
            QVERIFY2(!context.Build(invalidDocument, id, keyform, values, &error), qPrintable(invalid));
            QVERIFY(!error.isEmpty());
            if (invalid == "glue") {
                const auto& diagnostics = context.LastDiagnostics();
                QCOMPARE(diagnostics.targetArtMeshId, QStringLiteral("Mesh"));
                QVERIFY(diagnostics.glueAffected);
                QCOMPARE(diagnostics.glueIds, QStringList{QStringLiteral("Glue")});
                QCOMPARE(diagnostics.gluePairs, QStringList{QStringLiteral("Mesh ↔ Mesh")});
                QVERIFY(diagnostics.reason.contains(QStringLiteral("Glue")));
            }
            QVERIFY(context.IsValid());
            QVERIFY(context.MatchesRuntime(expected));
            QVector<float> local;
            QVERIFY(context.FromModel(expected, local));
            QVERIFY(near(local, baseline.DrawableKeyformPositions("Mesh", 0)));
        }
    }

    void noOpParentAndKeyformOverridesRemainSupported()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "unchanged-overrides"));
        QVERIFY(document.SetDeformerParentIndex("Child", 1));
        QVERIFY(document.SetDeformerRotationKeyforms("Root", document.DeformerRotationKeyforms("Root")));
        QVERIFY(document.SetDeformerWarpKeyforms("Warp", document.DeformerWarpKeyforms("Warp")));
        CKeyformMappingContext context;
        QString error;
        QVERIFY2(context.Build(document, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
    }

    void patchedParentsAndKeyformsUseTheSameCoordinates_data()
    {
        QTest::addColumn<QString>("edit");
        QTest::addColumn<QVector<float>>("expected");
        QTest::newRow("detach-parent") << QString("detach") << QVector<float>{0.25f, -0.5f, 0.75f, -0.25f, 0.25f, -0.25f};
        QTest::newRow("skip-warp-parent") << QString("parent") << QVector<float>{9.5f, 3.75f, 9.75f, 3.25f, 9.75f, 3.75f};
        auto translated = coreCoordinates(rigidMeshCanvasPositions());
        for (int index = 0; index < translated.size(); index += 2) { translated[index] += 2; translated[index + 1] -= 3; }
        QTest::newRow("warp") << QString("warp") << translated;
        const float unit = 1.0f / std::sqrt(82.0f);
        QTest::newRow("rotation") << QString("rotation")
            << QVector<float>{-0.875f + 0.5f * unit, 0.125f + 4.5f * unit,
                              -0.875f - 9 * unit, 0.125f + unit, -0.875f, 0.125f};
        QTest::newRow("combined-with-local") << QString("combined") << QVector<float>{-1.75f, 0, -2.5f, -0.5f, -1.5f, -0.5f};
    }

    void patchedParentsAndKeyformsUseTheSameCoordinates()
    {
        QFETCH(QString, edit);
        QFETCH(QVector<float>, expected);
        Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "modified-chain"));
        CKeyformMappingContext old, changed;
        QString error;
        QVERIFY(old.Build(document, "Mesh", 0, {{"P", 0}}, &error));
        const auto oldLocal = document.DrawableKeyformPositions("Mesh", 0);
        QVector<float> oldWorld;
        QVERIFY(old.ToModel(oldLocal, oldWorld));
        if (edit == "detach" || edit == "parent" || edit == "combined")
            QVERIFY(document.SetDeformerParentIndex("Child", edit == "detach" ? -1 : 0));
        if (edit == "warp" || edit == "combined") {
            auto forms = document.DeformerWarpKeyforms("Warp");
            forms[0].opacity = 0.4f;
            for (int index = 0; index < forms[0].positions.size(); index += 2) {
                forms[0].positions[index] += 3;
                forms[0].positions[index + 1] -= 2;
            }
            QVERIFY(document.SetDeformerWarpKeyforms("Warp", forms));
        }
        if (edit == "rotation" || edit == "combined") {
            auto forms = document.DeformerRotationKeyforms("Root");
            forms[0] = {0.6f, 180, -2, 1, 2, true, false};
            QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        }
        if (edit == "combined") {
            auto local = oldLocal;
            local[0] = 0.25f;
            QVERIFY(document.SetDrawableKeyformPositions("Mesh", 0, local));
        }
        const auto local = document.DrawableKeyformPositions("Mesh", 0);
        QVERIFY2(changed.Build(document, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
        QVector<float> world, restored;
        QVERIFY(changed.ToModel(local, world));
        QVERIFY(near(world, expected));
        QVERIFY(changed.MatchesRuntime(expected));
        QVERIFY(!changed.MatchesRuntime(oldWorld));
        QVERIFY(changed.FromModel(expected, restored));
        QVERIFY(near(restored, local));
        QVERIFY(old.MatchesRuntime(oldWorld));
        QCOMPARE(document.SourceBytes(), fixture.bytes);
        document.Clear();
        QVERIFY(changed.MatchesRuntime(expected));
    }

    void patchedParentKeyformsInterpolateAtTheFullPose()
    {
        Fixture fixture;
        fixture.BindRoot(false);
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "parent-interpolation"));
        auto forms = document.DeformerRotationKeyforms("Root");
        QCOMPARE(forms.size(), 2);
        forms[1].originX += 8;
        QVERIFY(document.SetDeformerRotationKeyforms("Root", forms));
        CKeyformMappingContext context;
        QString error;
        QVERIFY2(context.Build(document, "Mesh", 0, {{"P", 0.25f}}, &error), qPrintable(error));
        auto expected = coreCoordinates(rigidMeshCanvasPositions());
        for (int index = 0; index < expected.size(); index += 2) expected[index] += 2;
        QVERIFY(context.MatchesRuntime(expected));
        QVector<float> restored;
        QVERIFY(context.FromModel(expected, restored));
        QVERIFY(near(restored, document.DrawableKeyformPositions("Mesh", 0)));
    }

    void editedParentBlendShapeChainMapsThroughPoseOverrides()
    {
        Fixture fixture;
        fixture.column(fixture.table.sectionOffsetTable->deformers->parent_deformer_indices, 2, -1);
        fixture.count(27, 1);
        fixture.appendColumn(0x234, {0});
        fixture.appendColumn(0x238, {0});
        fixture.appendColumn(0x23c, {0});
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "reparent-to-blend"));
        CKeyformMappingContext context;
        QString error;
        QVERIFY2(context.Build(document, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
        QVector<float> oldWorld;
        QVERIFY(context.ToModel(document.DrawableKeyformPositions("Mesh", 0), oldWorld));
        QVERIFY(document.SetDeformerParentIndex("Child", 1));
        QVERIFY2(context.Build(document, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
        QVector<float> updatedWorld;
        QVERIFY(context.ToModel(document.DrawableKeyformPositions("Mesh", 0), updatedWorld));
        QVERIFY(context.MatchesRuntime(updatedWorld));
        QVERIFY(updatedWorld != oldWorld);
    }

    void unrelatedBlendShapeDoesNotDisableTheTargetChain()
    {
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        const auto& o = *fixture.table.sectionOffsetTable;
        fixture.count(4, 2);
        fixture.id(o.art_meshes->ids, 1, "Other");
        for (const auto& entry : {std::pair{o.art_meshes->keyform_binding_sources_indices, -1},
                                  {o.art_meshes->keyform_sources_begin_indices, 0},
                                  {o.art_meshes->keyform_sources_counts, 1},
                                  {o.art_meshes->parent_part_indices, -1},
                                  {o.art_meshes->parent_deformer_indices, -1},
                                  {o.art_meshes->vertex_counts, 3},
                                  {o.art_meshes->uv_sources_begin_indices, 0},
                                  {o.art_meshes->position_index_sources_begin_indices, 0},
                                  {o.art_meshes->position_index_sources_counts, 3},
                                  {o.art_meshes->drawable_mask_sources_begin_indices, 0},
                                  {o.art_meshes->drawable_mask_sources_counts, 0}})
            fixture.column(entry.first, 1, entry.second);
        fixture.count(28, 1);
        fixture.appendColumn(0x240, {1});
        fixture.appendColumn(0x244, {0});
        fixture.appendColumn(0x248, {0});
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(fixture.bytes, "unrelated-blend", &error), qPrintable(error));
        CKeyformMappingContext context;
        QVERIFY2(context.Build(document, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
        QVERIFY2(context.Build(document, "Other", 0, {{"P", 0}}, &error), qPrintable(error));
        QVERIFY(context.IsValid());
    }

    void comparisonToleranceAndInvalidMappingsDoNotChangeOutputs()
    {
        CKeyformMappingContext context;
        QVector<float> output{7, 9};
        QVERIFY(!context.ToModel({0, 0}, output));
        QCOMPARE(output, (QVector<float>{7, 9}));
        QVERIFY(!context.FromModel({0, 0}, output));
        QCOMPARE(output, (QVector<float>{7, 9}));
        Fixture fixture;
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "tolerance"));
        QVERIFY(context.Build(document, "Mesh", 0, {{"P", 0}}));
        QVector<float> expected;
        QVERIFY(context.ToModel(document.DrawableKeyformPositions("Mesh", 0), expected));
        auto close = expected;
        close[0] += float(CKeyformMappingContext::AbsoluteTolerance * 0.25);
        QVERIFY(context.MatchesRuntime(close));
        auto shifted = expected;
        shifted[0] += 0.001f;
        QVERIFY(!context.MatchesRuntime(shifted));
        auto relative = expected;
        relative[0] += float((CKeyformMappingContext::AbsoluteTolerance +
                              CKeyformMappingContext::RelativeTolerance * std::abs(expected[0])) * 2);
        QVERIFY(!context.MatchesRuntime(relative));
        QVERIFY(!context.MatchesRuntime(expected.mid(0, 2)));
        auto nonFinite = expected;
        nonFinite[0] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!context.MatchesRuntime(nonFinite));
        for (const QVector<float>& input : {QVector<float>{0}, QVector<float>{std::numeric_limits<float>::quiet_NaN(), 0},
                                           QVector<float>{0, std::numeric_limits<float>::infinity()}}) {
            output = {7, 9};
            QVERIFY(!context.ToModel(input, output));
            QCOMPARE(output, (QVector<float>{7, 9}));
            QVERIFY(!context.FromModel(input, output));
            QCOMPARE(output, (QVector<float>{7, 9}));
        }
    }
};

QTEST_GUILESS_MAIN(KeyformMappingTests)
#include "keyform_mapping_tests.moc"
