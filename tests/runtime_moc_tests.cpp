#include "live2dstruct/cmocruntimebuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include <QtTest>
#include <QFile>
#include <QtEndian>
#include <cmath>
#include <limits>

using namespace live2dstruct;
using RuntimeBuilder = CMocRuntimeBuilder;

namespace {
QByteArray source(const QString& name)
{
    QFile file(QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QVector<quint16> indices(const CSectionTable& table, int mesh)
{
    const auto* item = table.art_meshes[mesh];
    QVector<quint16> result;
    for (int index = 0; index < item->position_index_sources_counts; ++index)
        result.append(quint16(table.position_indices->indices[item->position_index_sources_begin_indices + index]));
    return result;
}
}

class RuntimeMocTests : public QObject
{
    Q_OBJECT
private slots:
    void noOpPreservesBytes_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void noOpPreservesBytes()
    {
        QFETCH(QString, name);
        const auto bytes = source(name);
        QVERIFY(!bytes.isEmpty());
        QByteArray result;
        QString error;
        QVERIFY2(RuntimeBuilder::Build(bytes, {}, result, &error), qPrintable(error));
        QCOMPARE(result, bytes);
    }

    void authoringFieldsDetachCompleteRowsAndIndices_data() { noOpPreservesBytes_data(); }
    void authoringFieldsDetachCompleteRowsAndIndices()
    {
        QFETCH(QString, name);
        const auto bytes = source(name);
        auto reader = CReader::FromBytes(bytes); CSectionTable before(reader);
        QVERIFY(before.IsValid());
        const auto* mesh = before.art_meshes.first();
        auto topology = indices(before, 0);
        QVERIFY(topology.size() > 3);
        topology.resize(3);
        RuntimeBuilder::Overrides overrides;
        overrides.drawableOpacity.insert(mesh->ids, .4f);
        overrides.drawableDrawOrder.insert(mesh->ids, 0);
        overrides.drawableIndices.insert(mesh->ids, topology);
        QByteArray result;
        QString error;
        QVERIFY2(RuntimeBuilder::Build(bytes, overrides, result, &error), qPrintable(error));
        auto changedReader = CReader::FromBytes(result); CSectionTable after(changedReader);
        QVERIFY(after.IsValid());
        const auto* changed = after.art_meshes.first();
        QCOMPARE(changed->keyform_sources_counts, mesh->keyform_sources_counts);
        QVERIFY(changed->keyform_sources_begin_indices >= before.art_mesh_keyforms.size());
        QCOMPARE(indices(after, 0), topology);
        for (int index = 0; index < changed->keyform_sources_counts; ++index) {
            const auto* form = after.art_mesh_keyforms[changed->keyform_sources_begin_indices + index];
            QCOMPARE(form->opacities, .4f);
            QCOMPARE(form->draw_orders, 0.f);
            QCOMPARE(form->keyform_position_sources_begin_indices,
                before.art_mesh_keyforms[mesh->keyform_sources_begin_indices + index]->keyform_position_sources_begin_indices);
        }
        const auto oldSpans = CSectionValidation::ArraySpans(*before.sectionOffsetTable, *before.count_info);
        const auto newSpans = CSectionValidation::ArraySpans(*after.sectionOffsetTable, *after.count_info);
        for (int index = 0; index < oldSpans.size(); ++index) {
            const auto& old = oldSpans[index]; const auto& next = newSpans[index];
            if (!old.offset || (!QByteArray(old.name).startsWith("art_mesh_keyforms.") && QByteArray(old.name) != "position_indices.indices")) continue;
            QCOMPARE(result.mid(next.offset, old.count * old.width), bytes.mid(old.offset, old.count * old.width));
        }
        for (int index = 1; index < before.art_meshes.size(); ++index) {
            QCOMPARE(after.art_meshes[index]->keyform_sources_begin_indices, before.art_meshes[index]->keyform_sources_begin_indices);
            QCOMPARE(indices(after, index), indices(before, index));
        }
        QCOMPARE(source(name), bytes);
    }

    void sharedRowsRespectIndexIsolation_data()
    {
        QTest::addColumn<bool>("sharedIndices");
        QTest::addColumn<bool>("changeIndices");
        QTest::newRow("shared-forms-independent-indices") << false << true;
        QTest::newRow("shared-forms-shared-indices-no-op") << true << false;
        QTest::newRow("shared-forms-shared-indices-rejected") << true << true;
    }

    void sharedRowsRespectIndexIsolation()
    {
        QFETCH(bool, sharedIndices);
        QFETCH(bool, changeIndices);
        auto bytes = source("Mark");
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int first = -1, second = -1;
        for (int a = 0; a < table.art_meshes.size() && first < 0; ++a)
            for (int b = a + 1; b < table.art_meshes.size(); ++b)
                if (table.art_meshes[a]->vertex_counts == table.art_meshes[b]->vertex_counts &&
                    table.art_meshes[a]->keyform_sources_counts == table.art_meshes[b]->keyform_sources_counts &&
                    table.art_meshes[a]->position_index_sources_counts > 3) { first = a; second = b; break; }
        QVERIFY(first >= 0); QVERIFY(second >= 0);
        const auto* mesh = table.art_meshes[first];
        const auto* offsets = table.sectionOffsetTable->art_meshes;
        qToLittleEndian(qint32(mesh->keyform_sources_begin_indices), bytes.data() + offsets->keyform_sources_begin_indices + second * 4);
        if (sharedIndices) {
            qToLittleEndian(qint32(mesh->position_index_sources_begin_indices), bytes.data() + offsets->position_index_sources_begin_indices + second * 4);
            qToLittleEndian(qint32(mesh->position_index_sources_counts), bytes.data() + offsets->position_index_sources_counts + second * 4);
        }
        const auto immutable = bytes;
        RuntimeBuilder::Overrides overrides;
        overrides.drawableOpacity.insert(mesh->ids, .25f);
        auto topology = indices(table, first);
        if (changeIndices) topology.resize(3);
        overrides.drawableIndices.insert(mesh->ids, topology);
        QByteArray result("unchanged output");
        QString error;
        if (sharedIndices && changeIndices) {
            QVERIFY(!RuntimeBuilder::Build(bytes, overrides, result, &error));
            QCOMPARE(result, QByteArray("unchanged output"));
            QCOMPARE(bytes, immutable);
            QVERIFY2(error.contains(mesh->ids) && error.contains(table.art_meshes[second]->ids), qPrintable(error));
            return;
        }
        QVERIFY2(RuntimeBuilder::Build(bytes, overrides, result, &error), qPrintable(error));
        QCOMPARE(bytes, immutable);
        auto changedReader = CReader::FromBytes(result); CSectionTable after(changedReader);
        QVERIFY(after.IsValid());
        QCOMPARE(after.art_meshes[second]->keyform_sources_begin_indices, mesh->keyform_sources_begin_indices);
        QCOMPARE(indices(after, second), indices(table, sharedIndices ? first : second));
        QCOMPARE(indices(after, first), topology);
        QVERIFY(after.art_meshes[first]->keyform_sources_begin_indices != after.art_meshes[second]->keyform_sources_begin_indices);
        for (int local = 0; local < mesh->keyform_sources_counts; ++local) {
            const auto* original = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + local];
            const auto* changed = after.art_mesh_keyforms[after.art_meshes[first]->keyform_sources_begin_indices + local];
            const auto* sibling = after.art_mesh_keyforms[after.art_meshes[second]->keyform_sources_begin_indices + local];
            QCOMPARE(changed->opacities, .25f);
            QCOMPARE(sibling->opacities, original->opacities);
            QCOMPARE(sibling->draw_orders, original->draw_orders);
            QCOMPARE(sibling->keyform_position_sources_begin_indices, original->keyform_position_sources_begin_indices);
            QCOMPARE(changed->keyform_position_sources_begin_indices, original->keyform_position_sources_begin_indices);
        }
    }

    void hiddenAncestorsPreserveGeometry_data()
    {
        QTest::addColumn<QString>("type");
        for (const char* type : {"artmesh", "part", "warp", "rotation"}) QTest::newRow(type) << QString::fromLatin1(type);
    }
    void hiddenAncestorsPreserveGeometry()
    {
        QFETCH(QString, type);
        const auto bytes = source("Mark");
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int selected = 0;
        if (type == "warp" || type == "rotation") {
            selected = -1;
            for (int index = 0; index < table.deformers.size(); ++index)
                if (table.deformers[index]->types == (type == "warp" ? 0 : 1)) { selected = index; break; }
        }
        QVERIFY(selected >= 0);
        const QString id = type == "artmesh" ? table.art_meshes[selected]->ids : type == "part"
            ? table.parts[selected]->ids : table.deformers[selected]->ids;
        RuntimeBuilder::Overrides overrides;
        overrides.visibility.insert(type + '|' + id, false);
        QByteArray result;
        QString error;
        QVERIFY2(RuntimeBuilder::Build(bytes, overrides, result, &error), qPrintable(error));
        auto changedReader = CReader::FromBytes(result); CSectionTable after(changedReader);
        QVERIFY(after.IsValid());
        const auto inPart = [&](int part) {
            for (int step = 0; part >= 0 && step < table.parts.size(); ++step) {
                if (part == selected) return true;
                part = table.parts[part]->parent_part_indices;
            }
            return false;
        };
        int hidden = 0;
        for (int index = 0; index < table.art_meshes.size(); ++index) {
            const auto* mesh = table.art_meshes[index];
            bool affected = type == "artmesh" && index == selected;
            if (type == "part") affected = inPart(mesh->parent_part_indices);
            int parent = mesh->parent_deformer_indices;
            for (int step = 0; parent >= 0 && step < table.deformers.size(); ++step) {
                if (type != "part" && type != "artmesh") affected = affected || parent == selected;
                parent = table.deformers[parent]->parent_deformer_indices;
            }
            if (affected) ++hidden;
            const auto* changed = after.art_meshes[index];
            QCOMPARE(indices(after, index), indices(table, index));
            for (int local = 0; local < mesh->keyform_sources_counts; ++local) {
                const auto* old = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + local];
                const auto* next = after.art_mesh_keyforms[changed->keyform_sources_begin_indices + local];
                QCOMPARE(next->opacities, affected ? 0.f : old->opacities);
                QCOMPARE(next->draw_orders, old->draw_orders);
                QCOMPARE(next->keyform_position_sources_begin_indices, old->keyform_position_sources_begin_indices);
            }
        }
        QVERIFY(hidden > 0);
    }

    void invalidRequestsNeverPublishPartialOutput_data()
    {
        QTest::addColumn<int>("kind");
        for (int index = 0; index < 10; ++index) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void invalidRequestsNeverPublishPartialOutput()
    {
        QFETCH(int, kind);
        auto bytes = source("Mark");
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        const auto id = table.art_meshes.first()->ids;
        RuntimeBuilder::Overrides overrides;
        overrides.drawableOpacity.insert(id, .4f);
        if (kind == 0) overrides.drawableOpacity.insert("missing", .5f);
        if (kind == 1) overrides.drawableOpacity[id] = std::numeric_limits<float>::quiet_NaN();
        if (kind == 2) overrides.drawableOpacity[id] = 1.1f;
        if (kind == 3) overrides.drawableDrawOrder[id] = -1;
        if (kind == 4) overrides.drawableIndices[id] = {65535, 0, 1};
        if (kind == 5) overrides.drawableIndices[id] = {0, 1};
        if (kind == 6) overrides.visibility.insert("rotation|" + id, false);
        if (kind == 7) {
            overrides.visibility.insert("artmesh|" + id, false);
            qToLittleEndian(qint32(0), bytes.data() + table.sectionOffsetTable->parts->parent_part_indices);
        }
        if (kind == 8) {
            overrides.drawableDrawOrder[id] = 0;
            const auto* group = table.sectionOffsetTable->draw_order_group_objects;
            qToLittleEndian(qint32(1), bytes.data() + group->types);
            qToLittleEndian(qint32(0), bytes.data() + group->indices);
            qToLittleEndian(qint32(0), bytes.data() + group->self_indices);
        }
        if (kind == 9) {
            overrides.drawableIndices[id] = {0, 1, 2};
            qToLittleEndian(qint32(table.position_indices->indices.size() + 1),
                           bytes.data() + table.sectionOffsetTable->art_meshes->position_index_sources_begin_indices);
        }
        QByteArray result = "retained";
        QString error;
        QVERIFY(!RuntimeBuilder::Build(bytes, overrides, result, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(result, QByteArray("retained"));
    }
};

QTEST_GUILESS_MAIN(RuntimeMocTests)
#include "runtime_moc_tests.moc"
