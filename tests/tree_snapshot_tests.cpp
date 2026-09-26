#include "live2dstruct/ctreemanage.h"
#include "live2dstruct/ckeyformmappingcontext.h"
#include "deformer_snapshot_fixture.h"

#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <limits>
#include <type_traits>

using namespace live2dstruct;
using namespace DeformerSnapshotData;

class TreeSnapshotTests : public QObject
{
    Q_OBJECT
private:
    static QVector<float> meshPositions(C2DTree& tree)
    {
        for (auto* node : tree.getArtTree()) {
            if (node->getName() != "Mesh") continue;
            const auto& positions = node->GetCanvasPositions();
            return QVector<float>(positions.cbegin(), positions.cend());
        }
        return {};
    }

private slots:
    void cleanup()
    {
        CTreeManage::ReleaseInstance();
        QVERIFY(!CTreeManage::GetInstance());
        QVERIFY(CTreeManage::UnsupportedDeformers().isEmpty());
    }

    void prepareAndSwapKeepBothTreesIndependent()
    {
        static_assert(!std::is_copy_constructible_v<CTreeManage::Snapshot>);
        static_assert(noexcept(CTreeManage::SwapSnapshot(std::declval<CTreeManage::Snapshot&>())));
        Fixture fixture;
        QVERIFY(fixture.table.IsValid());
        CTreeManage::Snapshot first;
        QString error;
        QVERIFY2(CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 0}}, first, &error), qPrintable(error));
        QVERIFY(!CTreeManage::GetInstance());
        const auto firstPoints = rigidMeshCanvasPositions();
        QVERIFY(near(meshPositions(*first.tree), firstPoints));
        auto* original = first.tree.get();
        CTreeManage::SwapSnapshot(first);
        QCOMPARE(CTreeManage::GetInstance(), original);
        QVERIFY(!first.tree);
        CTreeManage::Snapshot second;
        auto bytes = fixture.bytes;
        real(bytes, fixture.table.sectionOffsetTable->rotation_deformer_keyforms->origin_x, 20);
        QVERIFY2(CTreeManage::PrepareSnapshot(bytes, {{"P", 0}}, second, &error), qPrintable(error));
        auto secondPoints = firstPoints;
        for (int index = 0; index < secondPoints.size(); index += 2) secondPoints[index] += 10;
        QVERIFY(near(meshPositions(*second.tree), secondPoints));
        QVERIFY(near(meshPositions(*CTreeManage::GetInstance()), firstPoints));
        auto* replacement = second.tree.get();
        CTreeManage::SwapSnapshot(second);
        QCOMPARE(CTreeManage::GetInstance(), replacement);
        QCOMPARE(second.tree.get(), original);
        bytes.fill('\0');
        fixture.bytes.clear();
        QVERIFY(near(meshPositions(*CTreeManage::GetInstance()), secondPoints));
        CTreeManage::SwapSnapshot(second);
        QCOMPARE(CTreeManage::GetInstance(), original);
        QVERIFY(near(meshPositions(*CTreeManage::GetInstance()), firstPoints));
        QCOMPARE(second.tree.get(), replacement);
    }

    void invalidCandidatePreservesTheOutputAndGlobalTree_data()
    {
        QTest::addColumn<QString>("invalid");
        for (const char* value : {"bytes", "missing-pose", "extra-pose", "nan-pose", "infinite-pose", "parent", "cycle", "duplicate"})
            QTest::newRow(value) << QString::fromLatin1(value);
    }

    void invalidCandidatePreservesTheOutputAndGlobalTree()
    {
        QFETCH(QString, invalid);
        Fixture fixture;
        CTreeManage::Snapshot old, output;
        QVERIFY(CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 0}}, old));
        QVERIFY(CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 1}}, output));
        CTreeManage::SwapSnapshot(old);
        auto* global = CTreeManage::GetInstance();
        auto* out = output.tree.get();
        const auto globalPoints = meshPositions(*global), outPoints = meshPositions(*out);
        auto values = QMap<QString, float>{{"P", 0}};
        const auto* offsets = fixture.table.sectionOffsetTable;
        if (invalid == "bytes") fixture.bytes = "invalid";
        if (invalid == "missing-pose") values.clear();
        if (invalid == "extra-pose") values.insert("unknown", 0);
        if (invalid == "nan-pose") values["P"] = std::numeric_limits<float>::quiet_NaN();
        if (invalid == "infinite-pose") values["P"] = std::numeric_limits<float>::infinity();
        if (invalid == "parent") fixture.column(offsets->deformers->parent_deformer_indices, 0, 9);
        if (invalid == "cycle") fixture.column(offsets->deformers->parent_deformer_indices, 0, 2);
        if (invalid == "duplicate") fixture.id(offsets->deformers->ids, 1, "Root");
        QString error;
        QVERIFY(!CTreeManage::PrepareSnapshot(fixture.bytes, values, output, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(CTreeManage::GetInstance(), global);
        QCOMPARE(output.tree.get(), out);
        QCOMPARE(meshPositions(*global), globalPoints);
        QCOMPARE(meshPositions(*out), outPoints);
    }

    void unsupportedAncestorsPropagateOnlyToTheirDescendants_data()
    {
        QTest::addColumn<bool>("blendShape");
        QTest::newRow("blend-shape") << true;
        QTest::newRow("type1") << false;
    }

    void unsupportedAncestorsPropagateOnlyToTheirDescendants()
    {
        QFETCH(bool, blendShape);
        Fixture fixture;
        if (blendShape) {
            fixture.count(27, 1);
            fixture.appendColumn(0x234, {0});
            fixture.appendColumn(0x238, {0});
            fixture.appendColumn(0x23c, {0});
        } else {
            put(fixture.bytes, integer(fixture.bytes, 0x208), 1);
            fixture.column(fixture.table.sectionOffsetTable->deformers->keyform_binding_sources_indices, 1, 0);
            fixture.column(fixture.table.sectionOffsetTable->warp_deformers->keyform_sources_counts, 0, 2);
            fixture.count(7, 2);
            const auto* offsets = fixture.table.sectionOffsetTable->warp_deformer_keyforms;
            fixture.column(offsets->keyform_position_sources_begin_indices, 1, 12);
            real(fixture.bytes, offsets->opacities + 4, 1);
        }
        CTreeManage::Snapshot candidate;
        QString error;
        QVERIFY2(CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 0}}, candidate, &error), qPrintable(error));
        QCOMPARE(candidate.unsupportedDeformers, (QSet<QString>{"Warp", "Child"}));
        CTreeManage::SwapSnapshot(candidate);
        QCOMPARE(CTreeManage::UnsupportedDeformers(), (QSet<QString>{"Warp", "Child"}));
        QVERIFY(CTreeManage::GetInstance());
    }

    void legacyLoadIsTransactionalAndReleaseDoesNotReadDiskAgain()
    {
        Fixture fixture;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath("model.moc3");
        QVERIFY(write(path, fixture.bytes));
        QString error;
        QVERIFY2(CTreeManage::InitOpenLive2d(path, &error), qPrintable(error));
        auto* loaded = CTreeManage::GetInstance();
        const auto positions = meshPositions(*loaded);
        QVERIFY(write(path, "changed on disk"));
        QCOMPARE(CTreeManage::GetInstance(), loaded);
        QVERIFY(!CTreeManage::InitOpenLive2d(path, &error));
        QCOMPARE(CTreeManage::GetInstance(), loaded);
        QCOMPARE(meshPositions(*loaded), positions);
        QVERIFY(write(path, fixture.bytes));
        CTreeManage::ReleaseInstance();
        QVERIFY(!CTreeManage::GetInstance());
        QVERIFY(!CTreeManage::GetInstance());
    }

    void dynamicUnsupportedChainDoesNotDisableAnUnrelatedEditedMesh()
    {
        Fixture fixture;
        fixture.BindRoot(true);
        fixture.column(fixture.table.sectionOffsetTable->deformers->parent_deformer_indices, 2, -1);
        CModelDocument document;
        QVERIFY(document.LoadBytes(fixture.bytes, "mixed-cpu-support"));
        auto local = document.DrawableKeyformPositions("Mesh", 0);
        local[0] = 0.5f;
        QVERIFY(document.SetDrawableKeyformPositions("Mesh", 0, local));
        QByteArray bytes;
        QString error;
        QVERIFY2(document.BuildPreviewMoc(bytes, &error), qPrintable(error));
        CTreeManage::Snapshot snapshot;
        QVERIFY2(CTreeManage::PrepareSnapshot(bytes, {{"P", 0.5f}}, snapshot, &error), qPrintable(error));
        QCOMPARE(snapshot.unsupportedDeformers, (QSet<QString>{"Root", "Warp"}));
        QCOMPARE(snapshot.Deformers().size(), size_t(3));
        CKeyformMappingContext mapping;
        QVERIFY2(mapping.Build(document, "Mesh", 0, {{"P", 0.5f}}, &error), qPrintable(error));
        const QVector<float> expected{0.5f, 0.5f, 0.75f, 0.25f, 0.25f, 0.25f};
        QVERIFY(mapping.MatchesRuntime(coreCoordinates(expected)));
        QVERIFY(near(meshPositions(*snapshot.tree), expected));
        CTreeManage::SwapSnapshot(snapshot);
        CTreeManage::GetInstance()->SetParaValue("P", 0);
        QVERIFY(CTreeManage::RefreshCurrentSnapshot(&error));
        QVERIFY(CTreeManage::UnsupportedDeformers().isEmpty());
        CTreeManage::GetInstance()->SetParaValue("P", 0.5f);
        QVERIFY(CTreeManage::RefreshCurrentSnapshot(&error));
        QCOMPARE(CTreeManage::UnsupportedDeformers(), (QSet<QString>{"Root", "Warp"}));
        QVERIFY(document.SetDeformerParentIndex("Child", 1));
        QVERIFY(!mapping.Build(document, "Mesh", 0, {{"P", 0.5f}}, &error));
        QVERIFY(mapping.MatchesRuntime(coreCoordinates(expected)));
    }

    void uncoveredAxesInvalidateOnlyTheirChain_data()
    {
        QTest::addColumn<QString>("owner");
        QTest::addColumn<bool>("singleton");
        QTest::addColumn<bool>("detached");
        for (const QString& owner : {QString("Root"), QString("Warp"), QString("Mesh")})
            for (bool singleton : {false, true})
                for (bool detached : {false, true})
                    QTest::newRow(qPrintable(QString("%1-singleton%2-detached%3").arg(owner).arg(singleton).arg(detached)))
                        << owner << singleton << detached;
    }

    void uncoveredAxesInvalidateOnlyTheirChain()
    {
        QFETCH(QString, owner);
        QFETCH(bool, singleton);
        QFETCH(bool, detached);
        Fixture fixture;
        const auto& offsets = *fixture.table.sectionOffsetTable;
        if (owner == "Root") fixture.BindRoot(false);
        else if (owner == "Warp") {
            fixture.column(offsets.art_meshes->keyform_binding_sources_indices, 0, -1);
            fixture.column(offsets.art_meshes->keyform_sources_counts, 0, 1);
            fixture.column(offsets.deformers->keyform_binding_sources_indices, 1, 0);
            fixture.column(offsets.warp_deformers->keyform_binding_sources_indices, 0, 0);
            fixture.column(offsets.warp_deformers->keyform_sources_counts, 0, 2);
            fixture.count(7, 2);
            fixture.column(offsets.warp_deformer_keyforms->keyform_position_sources_begin_indices, 1, 12);
            real(fixture.bytes, offsets.warp_deformer_keyforms->opacities + 4, 1);
        }
        if (singleton) {
            fixture.column(offsets.parameter_bindings->keys_sources_counts, 0, 1);
            fixture.column(owner == "Root" ? offsets.rotation_deformers->keyform_sources_counts
                : owner == "Warp" ? offsets.warp_deformers->keyform_sources_counts
                : offsets.art_meshes->keyform_sources_counts, 0, 1);
        }
        if (detached) fixture.column(offsets.deformers->parent_deformer_indices, 2, -1);
        real(fixture.bytes, offsets.parameters->min_values, -2);
        real(fixture.bytes, offsets.parameters->max_values, 2);
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(fixture.bytes, "uncovered-axis.moc3", &error), qPrintable(error));
        CKeyformMappingContext mapping;
        QVERIFY2(mapping.Build(document, "Mesh", 0, {{"P", 0}}, &error), qPrintable(error));
        QVector<float> oldWorld;
        QVERIFY(mapping.ToModel(document.DrawableKeyformPositions("Mesh", 0), oldWorld));
        CTreeManage::Snapshot snapshot;
        QVERIFY(CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 0}}, snapshot, &error));
        const auto validPositions = meshPositions(*snapshot.tree);
        QSet<QString> unsupported;
        if (owner == "Root") unsupported = {"Root", "Warp"};
        if (owner == "Warp") unsupported = {"Warp"};
        if (owner != "Mesh" && !detached) unsupported.insert("Child");
        const bool affectedMesh = owner == "Mesh" || !detached;
        for (float outside : {-0.5f, singleton ? 0.5f : 1.5f}) {
            bool changed = false;
            QVERIFY2(CTreeManage::SyncPose(snapshot, {{"P", outside}}, changed, &error), qPrintable(error));
            QVERIFY(changed);
            QVERIFY(snapshot.HasValidEvaluation());
            QCOMPARE(snapshot.unsupportedDeformers, unsupported);
            QCOMPARE(meshPositions(*snapshot.tree).isEmpty(), affectedMesh);
            if (!affectedMesh) QCOMPARE(meshPositions(*snapshot.tree), validPositions);
            for (auto* node : snapshot.Deformers())
                QCOMPARE(node->GetCanvasPositions().empty(), unsupported.contains(node->getName()));
            const auto revision = snapshot.PoseRevision();
            QVERIFY(CTreeManage::SyncPose(snapshot, {{"P", outside}}, changed, &error));
            QVERIFY(!changed);
            QCOMPARE(snapshot.PoseRevision(), revision);
            QCOMPARE(mapping.Build(document, "Mesh", 0, {{"P", outside}}, &error), !affectedMesh);
            QVERIFY(mapping.MatchesRuntime(oldWorld));
            CTreeManage::Snapshot directlyLoaded;
            QVERIFY2(CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", outside}}, directlyLoaded, &error), qPrintable(error));
            QCOMPARE(directlyLoaded.unsupportedDeformers, unsupported);
            QCOMPARE(meshPositions(*directlyLoaded.tree).isEmpty(), affectedMesh);
            QVERIFY(CTreeManage::SyncPose(snapshot, {{"P", 0}}, changed, &error));
            QVERIFY(changed);
            QVERIFY(snapshot.unsupportedDeformers.isEmpty());
            QCOMPARE(meshPositions(*snapshot.tree), validPositions);
            QVERIFY(mapping.Build(document, "Mesh", 0, {{"P", 0}}, &error));
        }
    }

    void batchPoseSyncEvaluatesOnceAndKeepsTheMocDigest()
    {
        Fixture fixture;
        fixture.BindRoot(false);
        real(fixture.bytes, fixture.table.sectionOffsetTable->rotation_deformer_keyforms->origin_x + 4, 14);
        CTreeManage::Snapshot snapshot;
        QVERIFY(CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 0}}, snapshot));
        const auto digest = QCryptographicHash::hash(fixture.bytes, QCryptographicHash::Sha256);
        QCOMPARE(snapshot.MocDigest(), digest);
        const auto revision = snapshot.PoseRevision();
        QVERIFY(snapshot.HasValidEvaluation());
        const auto first = meshPositions(*snapshot.tree);
        bool changed = true;
        QVERIFY(CTreeManage::SyncPose(snapshot, {{"P", 0}}, changed));
        QVERIFY(!changed);
        QCOMPARE(snapshot.PoseRevision(), revision);
        QVERIFY(CTreeManage::SyncPose(snapshot, {{"P", 0.25f}}, changed));
        QVERIFY(changed);
        QCOMPARE(snapshot.PoseRevision(), revision + 1);
        QCOMPARE(snapshot.Pose(), (QMap<QString, float>{{"P", 0.25f}}));
        auto expected = first;
        for (int index = 0; index < expected.size(); index += 2) expected[index] += 1;
        QVERIFY(near(meshPositions(*snapshot.tree), expected));
        QCOMPARE(snapshot.MocDigest(), digest);
        QVERIFY(CTreeManage::SyncPose(snapshot, {{"P", 0.25f}}, changed));
        QVERIFY(!changed);
        QCOMPARE(snapshot.PoseRevision(), revision + 1);
        snapshot.tree->SetParaValue("P", 0.75f);
        QVERIFY(CTreeManage::SyncPose(snapshot, {{"P", 0.25f}}, changed));
        QVERIFY(changed);
        QCOMPARE(snapshot.PoseRevision(), revision + 2);
        QVERIFY(near(meshPositions(*snapshot.tree), expected));
        CTreeManage::SwapSnapshot(snapshot);
        QVERIFY(CTreeManage::SyncCurrentPose({{"P", 0}}, changed));
        QVERIFY(changed);
        QCOMPARE(CTreeManage::CurrentSnapshot().MocDigest(), digest);
        QVERIFY(near(meshPositions(*CTreeManage::GetInstance()), first));
        const auto currentRevision = CTreeManage::CurrentSnapshot().PoseRevision();
        QVERIFY(CTreeManage::RefreshCurrentSnapshot());
        QCOMPARE(CTreeManage::CurrentSnapshot().PoseRevision(), currentRevision);
    }

    void invalidBatchPoseNeverWritesAnyNode_data()
    {
        QTest::addColumn<QMap<QString, float>>("invalid");
        QTest::newRow("missing") << QMap<QString, float>{};
        QTest::newRow("extra") << QMap<QString, float>{{"P", 1}, {"unknown", 1}};
        QTest::newRow("wrong-id") << QMap<QString, float>{{"unknown", 1}};
        QTest::newRow("nan") << QMap<QString, float>{{"P", std::numeric_limits<float>::quiet_NaN()}};
        QTest::newRow("infinity") << QMap<QString, float>{{"P", std::numeric_limits<float>::infinity()}};
    }

    void invalidBatchPoseNeverWritesAnyNode()
    {
        using Values = QMap<QString, float>;
        QFETCH(Values, invalid);
        Fixture fixture;
        CTreeManage::Snapshot snapshot;
        QVERIFY(CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 0.25f}}, snapshot));
        const auto revision = snapshot.PoseRevision();
        const auto pose = snapshot.Pose();
        const auto points = meshPositions(*snapshot.tree);
        const auto exclusions = snapshot.unsupportedDeformers;
        bool changed = true;
        QString error;
        QVERIFY(!CTreeManage::SyncPose(snapshot, invalid, changed, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!changed);
        QCOMPARE(snapshot.PoseRevision(), revision);
        QCOMPARE(snapshot.Pose(), pose);
        QCOMPARE(meshPositions(*snapshot.tree), points);
        QCOMPARE(snapshot.unsupportedDeformers, exclusions);
        QVERIFY(snapshot.HasValidEvaluation());
        for (auto* node : snapshot.tree->getParametersTree())
            QCOMPARE(dynamic_cast<CParameterNode*>(node)->GetCurValue(), 0.25f);
    }

    void allParameterInputsAreValidatedBeforeTheFirstWrite()
    {
        CModelDocument document;
        QVERIFY(document.Load(sample("Mark")));
        const auto baseline = pose(document);
        QVERIFY(baseline.size() > 1);
        CTreeManage::Snapshot snapshot;
        QVERIFY(CTreeManage::PrepareSnapshot(document.SourceBytes(), baseline, snapshot));
        const auto revision = snapshot.PoseRevision();
        auto invalid = baseline;
        invalid.first() += 0.1f;
        invalid.last() = std::numeric_limits<float>::quiet_NaN();
        bool changed = true;
        QVERIFY(!CTreeManage::SyncPose(snapshot, invalid, changed));
        QVERIFY(!changed);
        QCOMPARE(snapshot.PoseRevision(), revision);
        QCOMPARE(snapshot.Pose(), baseline);
        for (auto* node : snapshot.tree->getParametersTree()) {
            const auto* parameter = dynamic_cast<CParameterNode*>(node);
            QVERIFY(parameter);
            QCOMPARE(parameter->GetCurValue(), baseline.value(parameter->getName()));
        }
        auto values = baseline;
        values.first() += 0.1f;
        values.last() += 0.1f;
        QVERIFY(CTreeManage::SyncPose(snapshot, values, changed));
        QVERIFY(changed);
        QCOMPARE(snapshot.PoseRevision(), revision + 1);
        for (auto* node : snapshot.tree->getParametersTree()) {
            const auto* parameter = dynamic_cast<CParameterNode*>(node);
            QVERIFY(parameter);
            QCOMPARE(parameter->GetCurValue(), values.value(parameter->getName()));
        }
    }

    void invalidTransformDefinitionsHideTheirDescendants_data()
    {
        QTest::addColumn<bool>("rotation");
        QTest::newRow("rotation-base-angle") << true;
        QTest::newRow("warp-grid") << false;
    }

    void invalidTransformDefinitionsHideTheirDescendants()
    {
        QFETCH(bool, rotation);
        Fixture fixture;
        const auto* offsets = fixture.table.sectionOffsetTable;
        if (rotation) real(fixture.bytes, offsets->rotation_deformers->base_angles, std::numeric_limits<float>::quiet_NaN());
        else fixture.column(offsets->warp_deformers->columns, 0, 0);
        CTreeManage::Snapshot snapshot;
        QString error;
        QVERIFY2(CTreeManage::PrepareSnapshot(fixture.bytes, {{"P", 0}}, snapshot, &error), qPrintable(error));
        QCOMPARE(snapshot.unsupportedDeformers,
                 rotation ? (QSet<QString>{"Root", "Warp", "Child"}) : (QSet<QString>{"Warp", "Child"}));
    }

    void originalModelsPrepareWithoutPublishing_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* value : {"Mark", "Rice", "Natori", "Wanko"}) QTest::newRow(value) << QString::fromLatin1(value);
    }

    void originalModelsPrepareWithoutPublishing()
    {
        QFETCH(QString, name);
        CModelDocument document;
        QVERIFY(document.Load(sample(name)));
        CTreeManage::Snapshot candidate;
        QString error;
        QVERIFY2(CTreeManage::PrepareSnapshot(document.SourceBytes(), pose(document), candidate, &error), qPrintable(error));
        QVERIFY(candidate.tree);
        QVERIFY(!CTreeManage::GetInstance());
        for (auto* node : candidate.tree->getParametersTree()) {
            const auto* parameter = dynamic_cast<CParameterNode*>(node);
            QVERIFY(parameter);
            QCOMPARE(parameter->GetCurValue(), document.ParameterValue(parameter->getName()));
        }
    }
};

QTEST_GUILESS_MAIN(TreeSnapshotTests)
#include "tree_snapshot_tests.moc"
