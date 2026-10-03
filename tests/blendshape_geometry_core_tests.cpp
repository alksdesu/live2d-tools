#include "live2dstruct/cmocblendshapegeometrybuilder.h"
#include "live2dstruct/cmocparameterstructurebuilder.h"
#include "live2dstruct/cblendshaperelations.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include "real_core_model_fixture.h"
#include <QCryptographicHash>
#include <QtEndian>
#include <limits>

using namespace live2dstruct;
using Builder = CMocBlendShapeGeometryBuilder;
using Columns = CMocParameterStructureBuilder;
class BlendShapeGeometryCoreTests : public QObject
{
    Q_OBJECT
    QByteArray m_bytes;
    Builder::Frame m_frame;
    int m_mesh = -1, m_parameter = -1;
    QVector<float> m_keys;
    int m_baseKey = -1;
    static int meshIndex(const CSectionTable& table, const QString& id)
    {
        for (int i = 0; i < table.art_meshes.size(); ++i) if (table.art_meshes[i]->ids == id) return i;
        return -1;
    }
    static QByteArray replaceWord(const QByteArray& bytes, const QByteArray& name, int row, int value)
    {
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        Columns::ColumnState state;
        for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
            if (name == span.name) {
                auto data = bytes.mid(span.offset, span.count * CSectionValidation::StorageWidth(span));
                qToLittleEndian(qint32(value), data.data() + row * 4); state.columns[span.slot] = data;
            }
        QByteArray result; QString error;
        if (state.columns.isEmpty() || !Columns::BuildColumnState(bytes, state, result, &error)) qFatal("%s", qPrintable(error));
        return result;
    }
    Builder::Request request() const
    {
        auto deltas = m_frame.deltas;
        for (int index = 0; index < deltas.size(); index += 2) deltas[index] += 10.f;
        return {m_frame, deltas};
    }
private slots:
    void initTestCase()
    {
        const auto path = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (path.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to official Mao MOC");
        m_bytes = read(path); QVERIFY(!m_bytes.isEmpty());
        auto reader = CReader::FromBytes(m_bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        QSet<int> glued;
        for (const auto* glue : table.glue) { glued.insert(glue->art_mesh_indices_a); glued.insert(glue->art_mesh_indices_b); }
        CBlendShapeRelations relations; QVERIFY(relations.Build(table));
        for (int shape = 0; shape < table.blend_shapes_art_meshes.size() && m_mesh < 0; ++shape) {
            const auto* source = table.blend_shapes_art_meshes[shape];
            if (glued.contains(source->target_indices)) continue;
            const auto* mesh = table.art_meshes[source->target_indices];
            for (int binding = 0; binding < source->blend_shape_keyform_binding_sources_counts && m_mesh < 0; ++binding) {
                const int row = source->blend_shape_keyform_binding_sources_begin_indices + binding;
                const auto& b = relations.Bindings()[row];
                if (!b.valid || b.constraints.count || b.keyforms.count < 2) continue;
                const auto& p = relations.ParameterBindings()[b.parameterBindingIndex];
                int editableOffset = -1;
                for (int offset = 0; offset < b.keyforms.count; ++offset)
                    if (offset != p.baseKeyIndex) { editableOffset = offset; break; }
                if (editableOffset < 0) continue;
                QString error;
                if (!Builder::Capture(m_bytes, {mesh->ids, shape, binding, editableOffset}, m_frame, &error)) continue;
                m_mesh = source->target_indices; m_parameter = b.parameterIndex; m_baseKey = p.baseKeyIndex;
                m_keys = relations.Keys().mid(p.keys.begin, p.keys.count);
            }
        }
        QVERIFY(m_mesh >= 0); QVERIFY(m_parameter >= 0); QVERIFY(!m_keys.isEmpty());
        qInfo() << "target" << m_frame.key.meshId << "global" << m_frame.sourceKeyformIndex
                << "keyformOffset" << m_frame.key.keyformOffset
                << "ordinary" << table.art_meshes[m_mesh]->keyform_sources_begin_indices
                << table.art_meshes[m_mesh]->keyform_sources_counts << "keys" << m_keys << "base" << m_baseKey;
        qInfo() << "parameter index" << m_parameter << "type" << table.parameters[m_parameter]->parameter_types;
    }
    void sourceRowsAndUnrelatedColumnsArePreserved()
    {
        const auto input = m_bytes; const auto edit = request(); Builder::Result result; QString error;
        QVERIFY2(Builder::Build(input, edit, result, &error), qPrintable(error)); QCOMPARE(input, m_bytes);
        QCOMPARE(result.frame.deltas, edit.deltas); QVERIFY(result.frame.sourceKeyformIndex != m_frame.sourceKeyformIndex);
        auto a = CReader::FromBytes(input), b = CReader::FromBytes(result.bytes); CSectionTable before(a), after(b);
        const auto first = CSectionValidation::ArraySpans(*before.sectionOffsetTable, *before.count_info);
        const auto second = CSectionValidation::ArraySpans(*after.sectionOffsetTable, *after.count_info);
        QCOMPARE(first.size(), second.size());
        for (int i = 0; i < first.size(); ++i) {
            const auto& old = first[i]; const auto& next = second[i];
            QCOMPARE(QByteArray(old.name), QByteArray(next.name));
            const auto oldData = old.offset ? input.mid(old.offset, old.count * CSectionValidation::StorageWidth(old)) : QByteArray();
            const auto newData = next.offset ? result.bytes.mid(next.offset, next.count * CSectionValidation::StorageWidth(next)) : QByteArray();
            if (QByteArray(old.name) == "blend_shapes_art_meshes.blend_shape_keyform_binding_sources_begin_indices") {
                auto expected = oldData;
                qToLittleEndian(qint32(before.blend_shape_keyform_bindings.size()), expected.data() + m_frame.key.shapeIndex * 4);
                QCOMPARE(newData, expected);
            } else {
                QVERIFY2(newData.startsWith(oldData), old.name);
                const QByteArray name(old.name);
                if (!name.startsWith("blend_shape_keyform_bindings.") && !name.startsWith("art_mesh_keyforms.") && name != "keyform_positions.xys")
                    QCOMPARE(newData, oldData);
            }
        }
        Builder::Frame reopened; QVERIFY(Builder::Capture(result.bytes, m_frame.key, reopened, &error));
        QCOMPARE(reopened.deltas, edit.deltas);
    }
    void coreNeutralAndSiblingEvaluationAreUnchanged()
    {
        const auto edit = request(); Builder::Result result; QString error;
        QVERIFY2(Builder::Build(m_bytes, edit, result, &error), qPrintable(error));
        CoreModel before, after; QVERIFY(before.load(m_bytes)); QVERIFY(after.load(result.bytes));
        auto defaults = QVector<float>(csmGetParameterDefaultValues(before.value), csmGetParameterDefaultValues(before.value) + csmGetParameterCount(before.value));
        before.pose(defaults); after.pose(defaults);
        QCOMPARE(frame(before.value).definition, frame(after.value).definition);
        QVERIFY2(changed(frame(before.value), frame(after.value)).isEmpty(), qPrintable(difference(frame(before.value), frame(after.value))));
        bool targetChanged = false;
        for (float value : m_keys) {
            auto pose = defaults; pose[m_parameter] = value; before.pose(pose); after.pose(pose);
            const auto a = frame(before.value), b = frame(after.value);
            QCOMPARE(a.definition, b.definition);
            for (int mesh = 0; mesh < a.values.size(); ++mesh) if (mesh != m_mesh) QCOMPARE(a.values[mesh], b.values[mesh]);
            targetChanged |= a.values[m_mesh] != b.values[m_mesh];
            qInfo() << "pose" << value << "target changed" << (a.values[m_mesh] != b.values[m_mesh]);
            for (int field = a.positionFields[m_mesh]; field < a.values[m_mesh].size(); ++field)
                QCOMPARE(a.values[m_mesh][field], b.values[m_mesh][field]);
            QVERIFY(before.intact()); QVERIFY(after.intact());
        }
        QVERIFY(targetChanged);
        Builder::Result restored; QVERIFY2(Builder::Build(result.bytes, {result.frame, m_frame.deltas}, restored, &error), qPrintable(error));
        CoreModel rollback; QVERIFY(rollback.load(restored.bytes));
        int checked = 0;
        for (const auto& pose : poses(before.value)) {
            before.pose(pose); rollback.pose(pose);
            QVERIFY2(difference(frame(before.value), frame(rollback.value)).isEmpty(), qPrintable(difference(frame(before.value), frame(rollback.value))));
            ++checked;
        }
        qInfo() << "restored Core poses" << checked;
    }
    void sharedLocalPositionsRemainIndependent()
    {
        auto reader = CReader::FromBytes(m_bytes); CSectionTable table(reader);
        const int siblingOffset = m_frame.key.keyformOffset == 0 ? 1 : 0;
        const int nextRow = m_frame.sourceKeyformIndex + siblingOffset - m_frame.key.keyformOffset;
        const int position = table.art_mesh_keyforms[m_frame.sourceKeyformIndex]->keyform_position_sources_begin_indices;
        const auto shared = replaceWord(m_bytes, "art_mesh_keyforms.keyform_position_sources_begin_indices", nextRow, position);
        Builder::Frame current, other;
        auto key = m_frame.key; key.keyformOffset = siblingOffset;
        QVERIFY(Builder::Capture(shared, m_frame.key, current)); QVERIFY(Builder::Capture(shared, key, other));
        QCOMPARE(current.deltas, other.deltas);
        auto changedDeltas = current.deltas; changedDeltas[0] += 2.f;
        Builder::Result result; QString error; QVERIFY2(Builder::Build(shared, {current, changedDeltas}, result, &error), qPrintable(error));
        Builder::Frame untouched; QVERIFY(Builder::Capture(result.bytes, key, untouched)); QCOMPARE(untouched.deltas, other.deltas);
    }
    void gluedTargetsAreRejected()
    {
        auto reader = CReader::FromBytes(m_bytes); CSectionTable table(reader);
        QSet<int> glued;
        for (const auto* glue : table.glue) {
            if (!glue) continue;
            glued.insert(glue->art_mesh_indices_a); glued.insert(glue->art_mesh_indices_b);
        }
        for (int shape = 0; shape < table.blend_shapes_art_meshes.size(); ++shape) {
            const auto* source = table.blend_shapes_art_meshes[shape];
            if (!glued.contains(source->target_indices)) continue;
            const auto* mesh = table.art_meshes[source->target_indices];
            Builder::Frame output = m_frame; QString error;
            QVERIFY(!Builder::Capture(m_bytes, {mesh->ids, shape, 0, 0}, output, &error));
            QVERIFY(error.contains(QStringLiteral("Glue")));
            QCOMPARE(output.deltas, m_frame.deltas);
            return;
        }
        QSKIP("Official Mao fixture has no BlendShape target participating in Glue");
    }
    void invalidRequestsKeepOutput_data()
    {
        QTest::addColumn<int>("mode");
        for (int i = 0; i < 10; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidRequestsKeepOutput()
    {
        QFETCH(int, mode); auto edit = request();
        if (mode == 0) edit.before.key.meshId = "not-the-mesh";
        if (mode == 1) edit.before.key.shapeIndex = -1;
        if (mode == 2) edit.before.key.bindingOffset = std::numeric_limits<int>::max();
        if (mode == 3) edit.before.key.keyformOffset = -1;
        if (mode == 4) edit.before.sourceDigest.fill('x');
        if (mode == 5) edit.before.deltas[0] += 1;
        if (mode == 6) edit.deltas.removeLast();
        if (mode == 7) edit.deltas[0] = std::numeric_limits<float>::quiet_NaN();
        if (mode == 8) edit.deltas[0] = std::numeric_limits<float>::infinity();
        if (mode == 9) edit.deltas = edit.before.deltas;
        Builder::Result output; output.bytes = "sentinel"; output.frame = m_frame;
        QString error; QVERIFY(!Builder::Build(m_bytes, edit, output, &error)); QVERIFY(!error.isEmpty());
        QCOMPARE(output.bytes, QByteArray("sentinel")); QCOMPARE(output.frame.deltas, m_frame.deltas);
    }
};
QTEST_APPLESS_MAIN(BlendShapeGeometryCoreTests)
#include "blendshape_geometry_core_tests.moc"
