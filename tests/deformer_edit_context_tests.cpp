#include "live2dstruct/cdeformereditcontext.h"
#include "deformer_snapshot_fixture.h"
#include "live2dstruct/csectionvalidation.h"
#include <QtTest>
#include <QTemporaryDir>
#include <limits>

using namespace live2dstruct;
using namespace DeformerSnapshotData;

namespace {
QByteArray rotationBlendFixture(const QByteArray& bytes, int specific)
{
    auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
    if (!table.IsValid()) return {};
    QByteArray result(0x3c0, '\0');
    result.replace(0, 0x40, bytes.left(0x40)); result[4] = 5;
    put(result, 0x40, 0x2c0); put(result, 0x44, 0x380);
    result.replace(0x2c0, 128, bytes.mid(table.sectionOffsetTable->count_info->count_info_offset, 128));
    result.replace(0x380, 20, bytes.mid(integer(bytes, 0x44), 20));
    for (auto span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
        if (!span.offset || !span.count) continue;
        const QByteArray name(span.name);
        if (name == "parameters.runtime_space_0" || name == "parameters.extension_runtime_space_0") span.width = 8;
        put(result, span.slot, result.size()); result.append(bytes.mid(span.offset, span.count * span.width));
        result.append(QByteArray((64 - result.size() % 64) % 64, '\0'));
    }
    put(result, 0x2c0 + 33 * 4, 1);
    for (int slot : {0x288, 0x28c, 0x290}) {
        put(result, slot, result.size());
        const int offset = result.size(); result.append(QByteArray(64, '\0'));
        put(result, offset, slot == 0x288 ? specific : 0);
    }
    return result;
}
}

class DeformerEditContextTests : public QObject
{
    Q_OBJECT
private slots:
    void rootAndNestedPoints_data()
    {
        QTest::addColumn<QString>("id"); QTest::addColumn<QString>("type"); QTest::addColumn<QVector<float>>("expected");
        QTest::newRow("root-rotation") << QString("Root") << QString("rotation") << QVector<float>{10, 4};
        QTest::newRow("warp-under-rotation") << QString("Warp") << QString("warp") << QVector<float>{10, 4, 10, 2, 8, 4, 7, 1};
        QTest::newRow("rotation-under-warp") << QString("Child") << QString("rotation") << QVector<float>{9.4375f, 3.4375f};
    }
    void rootAndNestedPoints()
    {
        QFETCH(QString, id); QFETCH(QString, type); QFETCH(QVector<float>, expected);
        Fixture fixture; CModelDocument document; QString error;
        QVERIFY(document.LoadBytes(fixture.bytes, "fixture"));
        CDeformerEditContext context;
        QVERIFY2(context.Build(document, id, type, 0, {{"P", 0}}, &error), qPrintable(error));
        QVERIFY(near(context.ModelPoints(), expected));
        QVector<float> local, restored;
        QVERIFY(context.FromModel(expected, local, &error));
        QVERIFY(context.ToModel(local, restored, &error));
        QVERIFY(near(restored, expected));
        if (type == "rotation") {
            const QPointF direction = context.RotationDirection();
            QVERIFY(std::abs(std::hypot(direction.x(), direction.y()) - 1) < 0.000001);
            const QPointF expectedDirection = id == "Root" ? QPointF(1, 0) : QPointF(9 / std::sqrt(82.0), 1 / std::sqrt(82.0));
            QVERIFY(std::hypot(direction.x() - expectedDirection.x(), direction.y() - expectedDirection.y()) < 0.000001);
        } else QCOMPARE(context.RotationDirection(), QPointF());
    }
    void rootWarpAndFoldedOwnGeometryStayEditable_data()
    {
        QTest::addColumn<bool>("folded");
        QTest::newRow("root") << false;
        QTest::newRow("folded-own") << true;
    }
    void rootWarpAndFoldedOwnGeometryStayEditable()
    {
        QFETCH(bool, folded);
        Fixture fixture; const auto& o = *fixture.table.sectionOffsetTable;
        fixture.column(o.deformers->parent_deformer_indices, 1, -1);
        if (folded) for (int index = 0; index < 8; ++index)
            real(fixture.bytes, o.keyform_positions->xys + (12 + index) * 4, QVector<float>{0, 0, 2, 2, 0, 2, 2, 0}[index]);
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "folded"));
        CDeformerEditContext context; QString error;
        QVERIFY2(context.Build(document, "Warp", "warp", 0, {{"P", 0}}, &error), qPrintable(error));
        const auto local = document.DeformerWarpKeyforms("Warp")[0].positions;
        QVERIFY(near(context.ModelPoints(), coreCoordinates(local)));
        QVector<float> selected;
        QVERIFY(context.FromModel(context.ModelPoints().mid(2, 2), selected));
        QCOMPARE(selected, local.mid(2, 2));
    }
    void ownZeroAndSignedScaleDoNotBlockParentMapping_data()
    {
        QTest::addColumn<float>("scale"); QTest::addColumn<bool>("reflect");
        for (float scale : {-2.0f, 0.0f, 2.0f}) for (bool reflect : {false, true})
            QTest::newRow(qPrintable(QString("%1-%2").arg(scale).arg(reflect))) << scale << reflect;
    }
    void ownZeroAndSignedScaleDoNotBlockParentMapping()
    {
        QFETCH(float, scale); QFETCH(bool, reflect);
        Fixture fixture; const auto* o = fixture.table.sectionOffsetTable->rotation_deformer_keyforms;
        real(fixture.bytes, o->scales + 4, scale); fixture.column(o->is_reflect_y, 1, reflect);
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "signed"));
        CDeformerEditContext context; QString error;
        QVERIFY2(context.Build(document, "Child", "rotation", 0, {{"P", 0}}, &error), qPrintable(error));
        QVector<float> local; QVERIFY(context.FromModel(context.ModelPoints(), local));
        QVERIFY(near(local, {0.25f, 0.25f}));
        const double sign = (scale < 0 ? -1 : 1) * (reflect ? -1 : 1);
        QVERIFY(std::abs(context.RotationDirection().x() - sign * 9 / std::sqrt(82.0)) < 0.000001);
    }
    void immutableLifetimeAndEffectiveOverrides()
    {
        Fixture fixture; QTemporaryDir directory; QVERIFY(directory.isValid());
        const QString path = directory.filePath("fixture.moc3"); QVERIFY(write(path, fixture.bytes));
        CModelDocument document; QVERIFY(document.Load(path));
        auto forms = document.DeformerRotationKeyforms("Child"); forms[0].originX = 0.4f;
        QVERIFY(document.SetDeformerRotationKeyforms("Child", forms));
        CDeformerEditContext context; QString error;
        QVERIFY2(context.Build(document, "Child", "rotation", 0, {{"P", 0}}, &error), qPrintable(error));
        const auto expected = context.ModelPoints(); const auto direction = context.RotationDirection();
        CDeformerEditContext copy = context;
        forms[0].originX = 0.7f; QVERIFY(document.SetDeformerRotationKeyforms("Child", forms));
        QVERIFY(write(path, "changed on disk")); document.Clear();
        QCOMPARE(copy.ModelPoints(), expected); QCOMPARE(copy.RotationDirection(), direction);
        QVector<float> local; QVERIFY(copy.FromModel(expected, local)); QVERIFY(near(local, {0.4f, 0.25f}));
        QVERIFY(!context.Build(document, "Child", "rotation", 0, {}, &error));
        QCOMPARE(context.ModelPoints(), expected);
    }
    void invalidBuildKeepsPriorContext_data()
    {
        QTest::addColumn<QString>("kind");
        for (const char* kind : {"type", "id", "keyform", "missing-pose", "extra-pose", "nan", "range", "corner",
                                 "ancestor-range", "singleton-range", "axis-type", "parent-zero", "cycle", "duplicate-id"})
            QTest::newRow(kind) << QString::fromLatin1(kind);
    }
    void invalidBuildKeepsPriorContext()
    {
        QFETCH(QString, kind);
        Fixture fixture; CModelDocument baseline; QVERIFY(baseline.LoadBytes(fixture.bytes, "base"));
        CDeformerEditContext context; QString error;
        QVERIFY(context.Build(baseline, "Child", "rotation", 0, {{"P", 0}}, &error));
        const auto expected = context.ModelPoints();
        const auto& o = *fixture.table.sectionOffsetTable;
        QString id = "Child", type = "rotation"; int key = 0; QMap<QString, float> values{{"P", 0}};
        if (kind == "type") type = "artmesh";
        if (kind == "id") id = "missing";
        if (kind == "keyform") key = 9;
        if (kind == "missing-pose") values.clear();
        if (kind == "extra-pose") values["other"] = 0;
        if (kind == "nan") values["P"] = std::numeric_limits<float>::quiet_NaN();
        if (kind == "range") values["P"] = 2;
        if (kind == "corner" || kind == "axis-type" || kind == "ancestor-range" || kind == "singleton-range") {
            fixture.BindRoot(false);
            if (kind == "corner") { id = "Root"; values["P"] = 0.5f; }
            if (kind == "axis-type") put(fixture.bytes, integer(fixture.bytes, 0x208), 1);
            if (kind == "ancestor-range") { real(fixture.bytes, o.keys->values, 0.25f); real(fixture.bytes, o.keys->values + 4, 0.75f); }
            if (kind == "singleton-range") {
                fixture.column(o.rotation_deformers->keyform_sources_counts, 0, 1);
                fixture.column(o.parameter_bindings->keys_sources_counts, 0, 1);
                real(fixture.bytes, o.keys->values, 0.5f);
            }
        }
        if (kind == "parent-zero") real(fixture.bytes, o.rotation_deformer_keyforms->scales, 0);
        if (kind == "cycle") fixture.column(o.deformers->parent_deformer_indices, 0, 2);
        if (kind == "duplicate-id") fixture.id(o.deformers->ids, 0, "Child");
        CModelDocument document; QVERIFY2(document.LoadBytes(fixture.bytes, "invalid", &error), qPrintable(error));
        QVERIFY2(!context.Build(document, id, type, key, values, &error), qPrintable(kind));
        QVERIFY(!error.isEmpty()); QCOMPARE(context.ModelPoints(), expected);
    }
    void invalidMappingIsAtomic_data()
    {
        QTest::addColumn<QVector<float>>("points");
        QTest::newRow("odd") << QVector<float>{1};
        QTest::newRow("nan") << QVector<float>{0, std::numeric_limits<float>::quiet_NaN()};
        QTest::newRow("inf") << QVector<float>{std::numeric_limits<float>::infinity(), 0};
    }
    void invalidMappingIsAtomic()
    {
        QFETCH(QVector<float>, points);
        Fixture fixture; CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "fixture"));
        CDeformerEditContext context; QVERIFY(context.Build(document, "Child", "rotation", 0, {{"P", 0}}));
        QVector<float> output{7, 9}; QString error;
        QVERIFY(!context.ToModel(points, output, &error)); QCOMPARE(output, QVector<float>({7, 9}));
        QVERIFY(!context.FromModel(points, output, &error)); QCOMPARE(output, QVector<float>({7, 9}));
        auto inPlace = points; QVERIFY(!context.FromModel(inPlace, inPlace));
        QCOMPARE(inPlace.size(), points.size());
    }
    void ambiguousParentInverseRejectsOnlyAtTheRequestedPoint()
    {
        Fixture fixture; const auto& o = *fixture.table.sectionOffsetTable;
        fixture.column(o.deformers->parent_deformer_indices, 1, -1);
        const QVector<float> folded{0, 0, 2, 0, 2, 2, 0, 2};
        for (int i = 0; i < 8; ++i) real(fixture.bytes, o.keyform_positions->xys + (12 + i) * 4, folded[i]);
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "folded-parent"));
        CDeformerEditContext context; QString error;
        QVERIFY2(context.Build(document, "Child", "rotation", 0, {{"P", 0}}, &error), qPrintable(error));
        QVector<float> local{7, 9}; QVERIFY(!context.FromModel({1, -1}, local, &error)); QCOMPARE(local, QVector<float>({7, 9}));
        QVERIFY(error.contains(QStringLiteral("唯一")));
    }
    void unrelatedBlendShapeAndDownstreamGlueDoNotBlockRoot()
    {
        Fixture fixture; fixture.count(27, 1);
        fixture.appendColumn(0x234, {0}); fixture.appendColumn(0x238, {0}); fixture.appendColumn(0x23c, {0});
        fixture.count(20, 1);
        for (int slot : {0x1a4, 0x1ac, 0x1b0, 0x1b4, 0x1b8, 0x1bc, 0x1c0, 0x1c4})
            fixture.appendColumn(slot, {slot == 0x1ac ? -1 : 0});
        const int offset = fixture.bytes.size(); fixture.bytes.append(QByteArray("Glue").leftJustified(64, '\0'));
        put(fixture.bytes, 0x1a8, offset);
        CModelDocument document; QString error; QVERIFY2(document.LoadBytes(fixture.bytes, "bs", &error), qPrintable(error));
        CDeformerEditContext context; QVERIFY2(context.Build(document, "Root", "rotation", 0, {{"P", 0}}, &error), qPrintable(error));
        const auto expected = context.ModelPoints();
        QVERIFY(!context.Build(document, "Warp", "warp", 0, {{"P", 0}}, &error));
        QVERIFY(error.contains("BlendShape")); QCOMPARE(context.ModelPoints(), expected);
        QVERIFY(!context.Build(document, "Child", "rotation", 0, {{"P", 0}}, &error));
        QVERIFY(error.contains("BlendShape"));
    }
    void rotationBlendShapeTargetsAndAncestorsAreExplicitlyBlocked()
    {
        Fixture fixture; QString error;
        CModelDocument childBlend;
        QVERIFY2(childBlend.LoadBytes(rotationBlendFixture(fixture.bytes, 1), "rotation-bs", &error), qPrintable(error));
        CDeformerEditContext context;
        QVERIFY2(context.Build(childBlend, "Root", "rotation", 0, {{"P", 0}}, &error), qPrintable(error));
        QVERIFY2(context.Build(childBlend, "Warp", "warp", 0, {{"P", 0}}, &error), qPrintable(error));
        const auto expected = context.ModelPoints();
        QVERIFY(!context.Build(childBlend, "Child", "rotation", 0, {{"P", 0}}, &error));
        QVERIFY(error.contains("BlendShape")); QCOMPARE(context.ModelPoints(), expected);
        CModelDocument rootBlend;
        QVERIFY2(rootBlend.LoadBytes(rotationBlendFixture(fixture.bytes, 0), "ancestor-bs", &error), qPrintable(error));
        QVERIFY(!context.Build(rootBlend, "Warp", "warp", 0, {{"P", 0}}, &error));
        QVERIFY(error.contains("BlendShape")); QCOMPARE(context.ModelPoints(), expected);
        QVERIFY(!context.Build(rootBlend, "Child", "rotation", 0, {{"P", 0}}, &error));
        QVERIFY(error.contains("BlendShape"));
    }
    void sharedSpecificSourceKeepsDistinctGenericOwners()
    {
        Fixture fixture; const auto& o = *fixture.table.sectionOffsetTable;
        fixture.count(1, 4); fixture.id(o.deformers->ids, 3, "Sibling");
        fixture.column(o.deformers->types, 3, 0); fixture.column(o.deformers->specific_sources_indices, 3, 0);
        fixture.column(o.deformers->keyform_binding_sources_indices, 3, -1);
        fixture.column(o.deformers->parent_part_indices, 3, -1);
        fixture.column(o.deformers->parent_deformer_indices, 3, -1);
        fixture.column(o.deformers->is_visible, 3, 1); fixture.column(o.deformers->is_enabled, 3, 1);
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(fixture.bytes, "shared", &error), qPrintable(error));
        CDeformerEditContext parented, root;
        QVERIFY2(parented.Build(document, "Warp", "warp", 0, {{"P", 0}}, &error), qPrintable(error));
        QVERIFY2(root.Build(document, "Sibling", "warp", 0, {{"P", 0}}, &error), qPrintable(error));
        QVERIFY(root.ModelPoints() != parented.ModelPoints());
        QCOMPARE(root.ModelPoints(), coreCoordinates(document.DeformerWarpKeyforms("Sibling")[0].positions));
    }
    void unbuiltContextIsSafe()
    {
        CDeformerEditContext context; QVERIFY(!context.IsValid()); QVERIFY(context.ModelPoints().isEmpty());
        QCOMPARE(context.RotationDirection(), QPointF()); QVector<float> output{3, 5};
        QVERIFY(!context.ToModel({}, output)); QVERIFY(!context.FromModel({}, output)); QCOMPARE(output, QVector<float>({3, 5}));
    }
};
QTEST_GUILESS_MAIN(DeformerEditContextTests)
#include "deformer_edit_context_tests.moc"
