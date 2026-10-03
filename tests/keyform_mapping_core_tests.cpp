#include "core/Live2DCubismCore.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/ckeyformmappingcontext.h"
#include "live2dstruct/cmocdeformerorder.h"
#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/tree/c2dtree.h"
#include "deformer_snapshot_fixture.h"
#include <QtEndian>
#include <cmath>
#include <QtTest>
#include <QFile>
#include <new>
#include <cstring>
#include <limits>

using namespace live2dstruct;

namespace {
class CoreModel
{
public:
    explicit CoreModel(const QByteArray& bytes)
    {
        m_moc = ::operator new(bytes.size(), std::align_val_t(csmAlignofMoc));
        std::memcpy(m_moc, bytes.constData(), bytes.size());
        if (!csmHasMocConsistency(m_moc, bytes.size())) return;
        auto* moc = csmReviveMocInPlace(m_moc, bytes.size());
        if (!moc) return;
        const auto size = csmGetSizeofModel(moc);
        m_storage = ::operator new(size, std::align_val_t(csmAlignofModel));
        model = csmInitializeModelInPlace(moc, m_storage, size);
    }
    CoreModel(const CoreModel&) = delete;
    CoreModel& operator=(const CoreModel&) = delete;
    ~CoreModel()
    {
        ::operator delete(m_storage, std::align_val_t(csmAlignofModel));
        ::operator delete(m_moc, std::align_val_t(csmAlignofMoc));
    }
    void SetPose(const QMap<QString, float>& pose)
    {
        const auto* ids = csmGetParameterIds(model);
        auto* values = csmGetParameterValues(model);
        for (int index = 0; index < csmGetParameterCount(model); ++index) values[index] = pose.value(QString::fromUtf8(ids[index]));
        csmUpdateModel(model);
    }
    QVector<float> Positions(const QString& id) const
    {
        const auto* ids = csmGetDrawableIds(model);
        for (int index = 0; index < csmGetDrawableCount(model); ++index) if (QString::fromUtf8(ids[index]) == id) {
            const int count = csmGetDrawableVertexCounts(model)[index];
            const auto* values = csmGetDrawableVertexPositions(model)[index];
            QVector<float> result;
            for (int vertex = 0; vertex < count; ++vertex) { result.append(values[vertex].X); result.append(values[vertex].Y); }
            return result;
        }
        return {};
    }
    csmModel* model = nullptr;
private:
    void* m_moc = nullptr;
    void* m_storage = nullptr;
};
QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
QByteArray rotationFixture(const QString& variant)
{
    auto bytes = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
    auto reader = CReader::FromBytes(bytes);
    CSectionTable table(reader);
    if (!table.IsValid()) return {};
    const auto* offsets = table.sectionOffsetTable;
    const auto putFloat = [&](qint64 offset, float value) {
        quint32 bits;
        std::memcpy(&bits, &value, sizeof(bits));
        qToLittleEndian(bits, bytes.data() + offset);
    };
    const auto putInt = [&](qint64 offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); };
    int meshIndex = -1;
    for (int index = 0; index < table.art_meshes.size(); ++index)
        if (table.art_meshes[index]->ids == "LegL") meshIndex = index;
    if (meshIndex < 0) return {};
    const auto* mesh = table.art_meshes[meshIndex];
    const bool editor = variant.startsWith("editor-"), folded = variant == "folded";
    const int rotationSource = editor ? 2 : 1, rotationNode = editor ? 5 : 1;
    const auto* rotation = table.rotation_deformers[rotationSource];
    const auto* warp = table.warp_deformers[0];
    putInt(offsets->art_meshes->parent_deformer_indices + meshIndex * 4, folded ? 2 : rotationNode);
    putInt(offsets->deformers->parent_deformer_indices + rotationNode * 4, variant.startsWith("parent-") ? 0 : 2);
    putFloat(offsets->rotation_deformers->base_angles + rotationSource * 4, variant.contains("base30") ? 30 : 0);
    for (int key = 0; key < rotation->keyform_sources_counts; ++key) {
        const int row = rotation->keyform_sources_begin_indices + key;
        putFloat(offsets->rotation_deformer_keyforms->origin_x + row * 4, 0.25f);
        putFloat(offsets->rotation_deformer_keyforms->origin_y + row * 4,
                 variant.startsWith("nonlinear-") ? variant.mid(10).toFloat() : editor ? 0.25f : 0.4f);
        putFloat(offsets->rotation_deformer_keyforms->angles + row * 4, variant.contains("angle30") ? 30 : 0);
        putFloat(offsets->rotation_deformer_keyforms->scales + row * 4, editor ? 0.5f : variant == "own-negative" ? -2 : variant == "own-zero" ? 0 : variant == "pixel-local" ? 1.0f / 2400 : 2);
        putInt(offsets->rotation_deformer_keyforms->is_reflect_x + row * 4, variant == "own-reflect-x");
        putInt(offsets->rotation_deformer_keyforms->is_reflect_y + row * 4, variant == "own-reflect-y");
    }
    if (variant.startsWith("parent-") || editor) {
        const auto* parent = table.rotation_deformers[0];
        putFloat(offsets->rotation_deformers->base_angles, 0);
        for (int key = 0; key < parent->keyform_sources_counts; ++key) {
            const int row = parent->keyform_sources_begin_indices + key;
            putFloat(offsets->rotation_deformer_keyforms->origin_x + row * 4, 10);
            putFloat(offsets->rotation_deformer_keyforms->origin_y + row * 4, editor ? -4 : 20);
            putFloat(offsets->rotation_deformer_keyforms->angles + row * 4, editor ? 90 : 20);
            putFloat(offsets->rotation_deformer_keyforms->scales + row * 4, editor ? 1 : variant == "parent-negative" ? -3 : 3);
            putInt(offsets->rotation_deformer_keyforms->is_reflect_x + row * 4, variant == "parent-reflect-x");
            putInt(offsets->rotation_deformer_keyforms->is_reflect_y + row * 4, variant == "parent-reflect-y");
        }
    }
    const int rows = editor || folded ? 1 : warp->rows, columns = editor || folded ? 1 : warp->columns;
    if (editor || folded) {
        putInt(offsets->deformers->parent_deformer_indices + 2 * 4, folded ? -1 : 0);
        putInt(offsets->warp_deformers->rows, rows);
        putInt(offsets->warp_deformers->columns, columns);
        putInt(offsets->warp_deformers->vertex_counts, 4);
    }
    putInt(offsets->warp_deformers->is_quad_source, variant == "quad" || variant == "editor-quad");
    for (int key = 0; key < warp->keyform_sources_counts; ++key) {
        const int begin = table.warp_deformer_keyforms[warp->keyform_sources_begin_indices + key]->keyform_position_sources_begin_indices;
        for (int y = 0; y <= rows; ++y) for (int x = 0; x <= columns; ++x) {
            const float u = float(x) / columns, v = float(y) / rows;
            float a = 2 * u + 10, b = 3 * v + 20;
            if (variant == "rotated") { a = -3 * v + 10; b = 2 * u + 20; }
            if (variant == "reflected-x") a = -2 * u + 10;
            if (variant == "reflected-y") b = -3 * v + 20;
            if (variant.contains("shear")) a += v;
            if (variant == "quad" || variant == "nested-warp" || variant.startsWith("nonlinear-")) { a += u * v; b += u * v; }
            if (editor) { a = 2 * u + u * v; b = 2 * v + u * v; }
            if (folded) { a = 2 * u + 2 * v - 4 * u * v; b = 2 * v; }
            if (variant == "collapsed") { a = 10; b = 20; }
            if (variant == "opposite-axis") b = 20 + std::max(0.0f, v - 0.4f);
            const int position = begin + 2 * (y * (columns + 1) + x);
            putFloat(offsets->keyform_positions->xys + position * 4, a);
            putFloat(offsets->keyform_positions->xys + (position + 1) * 4, b);
        }
    }
    for (int key = 0; key < mesh->keyform_sources_counts; ++key) {
        const int begin = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + key]->keyform_position_sources_begin_indices;
        for (int vertex = 0; vertex < mesh->vertex_counts; ++vertex) {
            putFloat(offsets->keyform_positions->xys + (begin + vertex * 2) * 4, folded ? 0.25f : variant == "pixel-local" ? 3.31860352f + (vertex % 3 == 1 ? 1 : 0) : vertex % 3 == 1 ? 1 : 0);
            putFloat(offsets->keyform_positions->xys + (begin + vertex * 2 + 1) * 4, folded ? 0.5f : vertex % 3 == 2 ? 1 : 0);
        }
    }
    if (variant == "nested-warp") {
        putInt(offsets->art_meshes->parent_deformer_indices + meshIndex * 4, 3);
        const auto* inner = table.warp_deformers[1];
        for (int key = 0; key < inner->keyform_sources_counts; ++key) {
            const int begin = table.warp_deformer_keyforms[inner->keyform_sources_begin_indices + key]->keyform_position_sources_begin_indices;
            for (int y = 0; y <= inner->rows; ++y) for (int x = 0; x <= inner->columns; ++x) {
                const int position = begin + 2 * (y * (inner->columns + 1) + x);
                putFloat(offsets->keyform_positions->xys + position * 4, 0.17f + 0.71f * float(x) / inner->columns);
                putFloat(offsets->keyform_positions->xys + (position + 1) * 4, 0.13f + 0.81f * float(y) / inner->rows);
            }
        }
        for (int key = 0; key < mesh->keyform_sources_counts; ++key) {
            const int begin = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + key]->keyform_position_sources_begin_indices;
            for (int vertex = 0; vertex < mesh->vertex_counts; ++vertex) {
                putFloat(offsets->keyform_positions->xys + (begin + vertex * 2) * 4, 0.33f);
                putFloat(offsets->keyform_positions->xys + (begin + vertex * 2 + 1) * 4, 0.43f);
            }
        }
    }
    return bytes;
}
QMap<QString, float> poseFor(const CModelDocument& document, const QString& id, int keyform)
{
    QMap<QString, float> result, corner;
    for (const auto& parameter : document.Parameters()) result.insert(parameter.id, parameter.defaultValue);
    auto reader = CReader::FromBytes(document.SourceBytes());
    CSectionTable table(reader);
    CKeyformBindingGrid bindings;
    if (!bindings.Build(table, nullptr, true) || !bindings.Pose(CKeyformBindingGrid::ArtMesh, id, keyform, corner)) return {};
    for (auto value = corner.cbegin(); value != corner.cend(); ++value) result[value.key()] = value.value();
    return result;
}

QByteArray patchRotationForm(const QByteArray& baseline, const QString& id, int keyform,
                            const CModelRotationKeyform& form)
{
    auto reader = CReader::FromBytes(baseline);
    CSectionTable table(reader);
    if (!table.IsValid()) return {};
    for (const auto* deformer : table.deformers) {
        if (deformer->ids != id || deformer->types != 1) continue;
        const auto* source = table.rotation_deformers.value(deformer->specific_sources_indices);
        if (!source || keyform < 0 || keyform >= source->keyform_sources_counts) return {};
        const int row = source->keyform_sources_begin_indices + keyform;
        const auto* offsets = table.sectionOffsetTable->rotation_deformer_keyforms;
        QByteArray result = baseline;
        for (const auto& field : {qMakePair(offsets->opacities, form.opacity), qMakePair(offsets->angles, form.angle),
                                 qMakePair(offsets->origin_x, form.originX), qMakePair(offsets->origin_y, form.originY),
                                 qMakePair(offsets->scales, form.scale)}) {
            quint32 bits;
            std::memcpy(&bits, &field.second, sizeof(bits));
            qToLittleEndian(bits, result.data() + field.first + row * 4);
        }
        qToLittleEndian(qint32(form.reflectX), result.data() + offsets->is_reflect_x + row * 4);
        qToLittleEndian(qint32(form.reflectY), result.data() + offsets->is_reflect_y + row * 4);
        return result;
    }
    return {};
}

void compareCoreAtPose(const QByteArray& candidateBytes, const QByteArray& expectedBytes, const QMap<QString, float>& pose)
{
    CoreModel candidate(candidateBytes), expected(expectedBytes);
    QVERIFY(candidate.model);
    QVERIFY(expected.model);
    candidate.SetPose(pose);
    expected.SetPose(pose);
    QCOMPARE(csmGetDrawableCount(candidate.model), csmGetDrawableCount(expected.model));
    for (int mesh = 0; mesh < csmGetDrawableCount(candidate.model); ++mesh) {
        const QString id = QString::fromUtf8(csmGetDrawableIds(expected.model)[mesh]);
        QCOMPARE(QString::fromUtf8(csmGetDrawableIds(candidate.model)[mesh]), id);
        const auto actualPositions = candidate.Positions(id), expectedPositions = expected.Positions(id);
        QCOMPARE(actualPositions.size(), expectedPositions.size());
        for (int index = 0; index < actualPositions.size(); ++index) {
            QVERIFY(std::isfinite(actualPositions[index]));
            QCOMPARE(actualPositions[index], expectedPositions[index]);
        }
        QCOMPARE(csmGetDrawableOpacities(candidate.model)[mesh], csmGetDrawableOpacities(expected.model)[mesh]);
        QCOMPARE(csmGetDrawableDrawOrders(candidate.model)[mesh], csmGetDrawableDrawOrders(expected.model)[mesh]);
        QCOMPARE(csmGetDrawableRenderOrders(candidate.model)[mesh], csmGetDrawableRenderOrders(expected.model)[mesh]);
        QCOMPARE(csmGetDrawableConstantFlags(candidate.model)[mesh], csmGetDrawableConstantFlags(expected.model)[mesh]);
        QCOMPARE(csmGetDrawableTextureIndices(candidate.model)[mesh], csmGetDrawableTextureIndices(expected.model)[mesh]);
        QCOMPARE(csmGetDrawableMaskCounts(candidate.model)[mesh], csmGetDrawableMaskCounts(expected.model)[mesh]);
        for (int mask = 0; mask < csmGetDrawableMaskCounts(candidate.model)[mesh]; ++mask)
            QCOMPARE(csmGetDrawableMasks(candidate.model)[mesh][mask], csmGetDrawableMasks(expected.model)[mesh][mask]);
        for (int vertex = 0; vertex < actualPositions.size() / 2; ++vertex) {
            const auto actual = csmGetDrawableVertexUvs(candidate.model)[mesh][vertex];
            const auto reference = csmGetDrawableVertexUvs(expected.model)[mesh][vertex];
            QCOMPARE(actual.X, reference.X);
            QCOMPARE(actual.Y, reference.Y);
        }
        QCOMPARE(csmGetDrawableIndexCounts(candidate.model)[mesh], csmGetDrawableIndexCounts(expected.model)[mesh]);
        for (int index = 0; index < csmGetDrawableIndexCounts(candidate.model)[mesh]; ++index)
            QCOMPARE(csmGetDrawableIndices(candidate.model)[mesh][index], csmGetDrawableIndices(expected.model)[mesh][index]);
        for (bool multiply : {false, true}) {
            const auto actual = multiply ? csmGetDrawableMultiplyColors(candidate.model)[mesh] : csmGetDrawableScreenColors(candidate.model)[mesh];
            const auto reference = multiply ? csmGetDrawableMultiplyColors(expected.model)[mesh] : csmGetDrawableScreenColors(expected.model)[mesh];
            QCOMPARE(actual.X, reference.X); QCOMPARE(actual.Y, reference.Y);
            QCOMPARE(actual.Z, reference.Z); QCOMPARE(actual.W, reference.W);
        }
    }
}

}

class KeyformMappingCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void signedRotationEditsSurviveHistoryAndSession_data()
    {
        QTest::addColumn<QString>("variant");
        QTest::addColumn<QString>("field");
        for (const auto* field : {"angle", "origin", "reflectX", "reflectY"})
            QTest::newRow(qPrintable(QStringLiteral("negative-source-") + field)) << QString("own-negative") << QString(field);
        QTest::newRow("new-negative-scale") << QString("scale") << QString("negative");
        QTest::newRow("new-zero-scale") << QString("scale") << QString("zero");
        QTest::newRow("zero-source-origin") << QString("own-zero") << QString("origin");
    }

    void signedRotationEditsSurviveHistoryAndSession()
    {
        QFETCH(QString, variant);
        QFETCH(QString, field);
        QByteArray baseline;
        QString error;
        QVERIFY2(CMocDeformerOrder::Normalize(rotationFixture(variant), baseline, &error), qPrintable(error));
        CModelDocument document;
        QVERIFY2(document.LoadBytes(baseline, "signed-rotation.moc3", &error), qPrintable(error));
        const auto sourceDigest = document.SourceDigest();
        const QString target = QStringLiteral("Rotation8");
        const auto before = document.DeformerRotationKeyforms(target);
        QVERIFY(before.size() > 1);
        const int keyform = before.size() - 1;
        QMap<QString, float> pose, corner;
        for (const auto& parameter : document.Parameters()) pose.insert(parameter.id, parameter.defaultValue);
        QVERIFY2(document.KeyformParameterValues(target, "rotation", keyform, corner, &error), qPrintable(error));
        bool nonDefault = false;
        for (auto value = corner.cbegin(); value != corner.cend(); ++value) {
            nonDefault |= pose.value(value.key()) != value.value();
            pose[value.key()] = value.value();
        }
        QVERIFY(nonDefault);
        for (auto value = pose.cbegin(); value != pose.cend(); ++value) QVERIFY(document.SetParameterValue(value.key(), value.value()));
        document.ClearEditHistory();
        const CModelDocument loaded = document;
        for (const auto& diagnostic : document.Validate()) QVERIFY(diagnostic.code != "rotation_keyform");
        auto after = before;
        if (field == "angle") after[keyform].angle = 37;
        if (field == "origin") { after[keyform].originX += .17f; after[keyform].originY -= .23f; }
        if (field == "reflectX") after[keyform].reflectX = !after[keyform].reflectX;
        if (field == "reflectY") after[keyform].reflectY = !after[keyform].reflectY;
        if (field == "negative") after[keyform].scale = -1.5f;
        if (field == "zero") after[keyform].scale = 0;
        const auto expected = patchRotationForm(baseline, target, keyform, after[keyform]);
        QVERIFY(!expected.isEmpty());
        CoreModel sourceCore(baseline), editedCore(expected);
        QVERIFY(sourceCore.model && editedCore.model);
        sourceCore.SetPose(pose); editedCore.SetPose(pose);
        const auto sourcePositions = sourceCore.Positions("LegL"), changedPositions = editedCore.Positions("LegL");
        QCOMPARE(sourcePositions.size(), changedPositions.size());
        double maximumChange = 0;
        for (int index = 0; index < sourcePositions.size(); ++index)
            maximumChange = std::max(maximumChange, std::abs(double(sourcePositions[index]) - changedPositions[index]));
        QVERIFY2(maximumChange > .001, "The chosen non-default corner must actually use the edited form");
        QVERIFY(document.ExecuteEdit({target, "rotation", CModelEdit::RotationKeyforms,
            QVariant::fromValue(before), QVariant::fromValue(after), keyform, false}));
        QCOMPARE(document.DeformerRotationKeyforms(target), after);
        QByteArray runtime;
        QVERIFY2(document.BuildRuntimeMoc(runtime, &error), qPrintable(error));
        compareCoreAtPose(runtime, expected, pose);

        QMap<QString, float> uneditedPose = pose, uneditedCorner;
        QVERIFY(document.KeyformParameterValues(target, "rotation", 0, uneditedCorner, &error));
        for (auto value = uneditedCorner.cbegin(); value != uneditedCorner.cend(); ++value) uneditedPose[value.key()] = value.value();
        compareCoreAtPose(runtime, baseline, uneditedPose);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.DeformerRotationKeyforms(target), before);
        QVERIFY(!document.CanUndoEdit());
        QVERIFY(document.DeformerRotationKeyformOverrides().isEmpty());
        QByteArray undone;
        QVERIFY2(document.BuildRuntimeMoc(undone, &error), qPrintable(error));
        compareCoreAtPose(undone, baseline, pose);
        QVERIFY(document.RedoEdit());
        QCOMPARE(document.DeformerRotationKeyforms(target), after);
        QByteArray redone;
        QVERIFY2(document.BuildRuntimeMoc(redone, &error), qPrintable(error));
        QCOMPARE(redone, runtime);

        CModelSession captured;
        captured.sourceMoc = document.SourcePath();
        captured.sourceDigest = document.SourceDigest();
        captured.deformerRotationKeyforms = document.DeformerRotationKeyformOverrides();
        for (const auto& parameter : document.Parameters()) captured.parameters.insert(parameter.id, document.ParameterValue(parameter.id));
        CModelSession parsed;
        QVERIFY2(CModelSession::FromJson(captured.ToJson(), &parsed, &error), qPrintable(error));
        CModelDocument restored;
        QVERIFY2(parsed.RestoreDocument(loaded, restored, &error), qPrintable(error));
        QCOMPARE(restored.DeformerRotationKeyforms(target), after);
        QCOMPARE(restored.SourceDigest(), sourceDigest);
        for (auto value = pose.cbegin(); value != pose.cend(); ++value) QCOMPARE(restored.ParameterValue(value.key()), value.value());
        QByteArray restoredBytes;
        QVERIFY2(restored.BuildRuntimeMoc(restoredBytes, &error), qPrintable(error));
        compareCoreAtPose(restoredBytes, expected, pose);
        QCOMPARE(document.SourceBytes(), baseline);
        QCOMPARE(document.SourceDigest(), sourceDigest);
    }

    void nonFiniteRotationScalePreservesOutputs_data()
    {
        QTest::addColumn<float>("scale");
        QTest::newRow("nan") << std::numeric_limits<float>::quiet_NaN();
        QTest::newRow("positive-infinity") << std::numeric_limits<float>::infinity();
        QTest::newRow("negative-infinity") << -std::numeric_limits<float>::infinity();
    }

    void nonFiniteRotationScalePreservesOutputs()
    {
        QFETCH(float, scale);
        QByteArray baseline;
        QString error;
        QVERIFY(CMocDeformerOrder::Normalize(rotationFixture("own-negative"), baseline, &error));
        CModelDocument document;
        QVERIFY(document.LoadBytes(baseline, "signed-rotation.moc3", &error));
        const auto before = document.DeformerRotationKeyforms("Rotation8");
        const auto revision = document.Revision();
        auto invalid = before;
        invalid[0].scale = scale;
        QByteArray runtime;
        QVERIFY(document.BuildRuntimeMoc(runtime, &error));
        QVERIFY(!document.SetDeformerRotationKeyforms("Rotation8", invalid));
        QVERIFY(!document.ExecuteEdit({"Rotation8", "rotation", CModelEdit::RotationKeyforms,
            QVariant::fromValue(before), QVariant::fromValue(invalid), 0, false}));
        QCOMPARE(document.Revision(), revision);
        QCOMPARE(document.DeformerRotationKeyforms("Rotation8"), before);
        QVERIFY(!document.CanUndoEdit() && !document.CanRedoEdit());
        QByteArray afterFailure;
        QVERIFY(document.BuildRuntimeMoc(afterFailure, &error));
        QCOMPARE(afterFailure, runtime);
        CMocPreviewBuilder builder;
        QVERIFY(builder.Initialize(baseline, &error));
        CMocPreviewBuilder::Overrides overrides;
        overrides.rotationKeyforms.insert("Rotation8", invalid);
        QByteArray result("unchanged output");
        QVERIFY(!builder.BuildOverrides(overrides, result, &error));
        QCOMPARE(result, QByteArray("unchanged output"));
        QVERIFY(!error.isEmpty());
        CModelSession session;
        session.sourceMoc = document.SourcePath(); session.sourceDigest = document.SourceDigest();
        session.deformerRotationKeyforms.insert("Rotation8", invalid);
        CModelSession parsed; parsed.sourceMoc = "unchanged";
        QVERIFY(!CModelSession::FromJson(session.ToJson(), &parsed, &error));
        QCOMPARE(parsed.sourceMoc, QString("unchanged"));
        CModelDocument restored = document;
        QVERIFY(!session.RestoreDocument(document, restored, &error));
        QCOMPARE(restored.Revision(), revision);
        QCOMPARE(restored.DeformerRotationKeyforms("Rotation8"), before);
        compareCoreAtPose(runtime, baseline, poseFor(document, "LegL", 0));
    }

    void rotationBasisMatchesRealCore_data()
    {
        QTest::addColumn<QString>("variant");
        for (const auto* name : {"scale", "rotated", "reflected-x", "reflected-y", "shear", "shear-angle30", "shear-base30",
                                 "own-reflect-x", "own-reflect-y", "own-negative", "parent-rotation", "parent-reflect-x", "parent-reflect-y", "parent-negative",
                                 "quad", "opposite-axis", "editor-quad", "editor-tri", "nested-warp", "nonlinear-0.34", "nonlinear-0.35", "nonlinear-0.37", "nonlinear-0.4", "nonlinear-0.42"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }
    void rotationBasisMatchesRealCore()
    {
        QFETCH(QString, variant);
        const auto bytes = rotationFixture(variant);
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(bytes, "fixture.moc3", &error), qPrintable(error));
        CoreModel core(bytes);
        QVERIFY(core.model);
        const auto pose = poseFor(document, "LegL", 0);
        core.SetPose(pose);
        const auto actual = core.Positions("LegL");
        CKeyformMappingContext context;
        QVERIFY2(context.Build(document, "LegL", 0, pose, &error), qPrintable(error));
        QVERIFY2(context.MatchesRuntime(actual, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        C2DTree tree(table);
        for (auto value = pose.cbegin(); value != pose.cend(); ++value) tree.SetParaValue(value.key(), value.value());
        if (variant != "nested-warp") for (auto* node : tree.GetWarpKeys()) {
            auto* rotation = dynamic_cast<CRotationNode*>(node);
            if (!rotation || rotation->getName() != (variant.startsWith("editor-") ? "Rotation" : "Rotation8")) continue;
            QVERIFY(rotation->GetCanvasBasis());
            const auto& basis = *rotation->GetCanvasBasis();
            const QPointF x(actual[2] - actual[0], -actual[3] + actual[1]);
            const QPointF y(actual[4] - actual[0], -actual[5] + actual[1]);
            const double lx = std::hypot(x.x(), x.y()), ly = std::hypot(y.x(), y.y());
            QVERIFY(std::hypot(basis.xAxis.x() - x.x() / lx, basis.xAxis.y() - x.y() / lx) < 0.00002);
            QVERIFY(std::hypot(basis.yAxis.x() - y.x() / ly, basis.yAxis.y() - y.y() / ly) < 0.00002);
        }
        if (variant.startsWith("editor-")) {
            const bool quad = variant == "editor-quad";
            const double length = std::sqrt(82.0);
            const QPointF origin = quad ? QPointF(9.4375, -3.4375) : QPointF(9.5, -3.5);
            const QPointF xAxis = quad ? QPointF(1 / length, 9 / length) : QPointF(0, 1);
            const QPointF yAxis = quad ? QPointF(-9 / length, 1 / length) : QPointF(-1, 0);
            for (int vertex = 0; vertex < 3; ++vertex) {
                const auto point = origin + (vertex == 1 ? xAxis * 0.5 : vertex == 2 ? yAxis * 0.5 : QPointF{});
                QVERIFY(std::hypot(point.x() - actual[vertex * 2], point.y() + actual[vertex * 2 + 1]) < 0.00002);
            }
        }
        if (variant == "nested-warp") {
            QVERIFY(std::abs(actual[0] - 10.99636014) < 0.000003);
            QVERIFY(std::abs(actual[1] + 21.62266076) < 0.000003);
        }
        QVector<float> restored;
        QVERIFY2(context.FromModel(actual, restored, &error), qPrintable(error));
        const auto local = document.DrawableKeyformPositions("LegL", 0);
        QCOMPARE(restored.size(), local.size());
        for (int index = 0; index < local.size(); ++index) QVERIFY2(std::abs(restored[index] - local[index]) < 0.00002, qPrintable(variant));
        if (variant == "scale" || variant == "collapsed") {
            const QVector<float> expected = variant == "scale" ? QVector<float>{10.5f, -21.2f, 12.5f, -21.2f, 10.5f, -23.2f}
                                                                 : QVector<float>{10, -20, 10, -18, 12, -20};
            for (int index = 0; index < 6; ++index) QVERIFY(std::abs(actual[index] - expected[index]) < 0.000005);
        }
    }
    void supportedSampleCornersMatchRealCore_data()
    {
        QTest::addColumn<QString>("path");
        for (const auto* name : {"Mark", "Natori", "Rice", "Wanko"})
            QTest::newRow(name) << QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3";
        const QString mao = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (!mao.isEmpty()) QTest::newRow("Mao") << mao;
    }
    void supportedSampleCornersMatchRealCore()
    {
        QFETCH(QString, path);
        const auto bytes = read(path);
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(bytes, path, &error), qPrintable(error));
        CoreModel core(bytes);
        QVERIFY(core.model);
        int checked = 0, unsupported = 0;
        QMap<QString, int> unsupportedReasons;
        for (const auto& mesh : document.Drawables()) for (int form = 0; form < mesh.keyforms.size(); ++form) {
            const auto pose = poseFor(document, mesh.id, form);
            CKeyformMappingContext context;
            if (!context.Build(document, mesh.id, form, pose, &error)) {
                QString reason;
                if (error.contains("Glue")) reason = "Glue";
                else if (error.contains("BlendShape")) reason = "BlendShape";
                else if (error.contains(QStringLiteral("尚不支持关键形姿态反解"))) reason = "parameter-type";
                else if (error.contains(QStringLiteral("无法唯一反解"))) reason = "inverse-not-unique-or-degenerate";
                QVERIFY2(!reason.isEmpty(), qPrintable(mesh.id + ':' + QString::number(form) + ": " + error));
                ++unsupported;
                ++unsupportedReasons[reason];
                continue;
            }
            core.SetPose(pose);
            QVERIFY2(context.MatchesRuntime(core.Positions(mesh.id), &error), qPrintable(mesh.id + ':' + QString::number(form) + ": " + error));
            ++checked;
        }
        QVERIFY(checked > 0);
        qInfo() << "real Core corners checked" << checked << "unsupported before Core comparison" << unsupported << unsupportedReasons;
    }
    void fullyCollapsedParentDoesNotGuessRotationDirection()
    {
        const auto bytes = rotationFixture("collapsed");
        CModelDocument document;
        QString error;
        QVERIFY(document.LoadBytes(bytes, "collapsed.moc3", &error));
        CoreModel core(bytes);
        QVERIFY(core.model);
        core.SetPose(poseFor(document, "LegL", 0));
        const auto actual = core.Positions("LegL");
        QVERIFY(!actual.isEmpty());
        CKeyformMappingContext mapping;
        QVERIFY(!mapping.Build(document, "LegL", 0, poseFor(document, "LegL", 0), &error));
        QVERIFY2(error.contains(QStringLiteral("父链无法求值")), qPrintable(error));
        std::shared_ptr<const CCoordinateFrame> parent, result;
        QVERIFY(CCoordinateFrame::CreateWarp(1, 1, {10, 20, 10, 20, 10, 20, 10, 20}, false, {}, parent));
        result = parent;
        CCoordinateFrame::RotationBasis basis{{7, 8}, {1, 2}, {3, 4}};
        CRotationNodeKeyform key(1, 0, 0.25f, 0.4f, 2, false, false);
        QVERIFY(!CCoordinateFrame::CreateRotation(0, key, parent, result, basis));
        QCOMPARE(result.get(), parent.get());
        QCOMPARE(basis.origin, QPointF(7, 8));
        QCOMPARE(basis.xAxis, QPointF(1, 2));
        QCOMPARE(basis.yAxis, QPointF(3, 4));
    }

    void foldedWarpRemainsNonInvertibleWithRealCore()
    {
        const auto bytes = rotationFixture("folded");
        CModelDocument document;
        QString error;
        QVERIFY(document.LoadBytes(bytes, "folded.moc3", &error));
        CoreModel core(bytes);
        QVERIFY(core.model);
        const auto pose = poseFor(document, "LegL", 0);
        core.SetPose(pose);
        const auto positions = core.Positions("LegL");
        QVERIFY(std::abs(positions[0] - 1.5f) < 0.00001f);
        QVERIFY(std::abs(positions[1] + 1.0f) < 0.00001f);
        CKeyformMappingContext context;
        QVERIFY(!context.Build(document, "LegL", 0, pose, &error));
        QVERIFY2(error.contains(QStringLiteral("无法唯一反解")), qPrintable(error));
    }

    void roundTripUsesModelUnitsForPixelLocalCoordinates()
    {
        const auto bytes = rotationFixture("pixel-local");
        CModelDocument document;
        QString error;
        QVERIFY(document.LoadBytes(bytes, "pixels.moc3", &error));
        const auto pose = poseFor(document, "LegL", 0);
        CoreModel core(bytes);
        QVERIFY(core.model);
        core.SetPose(pose);
        CKeyformMappingContext context;
        QVERIFY2(context.Build(document, "LegL", 0, pose, &error), qPrintable(error));
        const auto local = document.DrawableKeyformPositions("LegL", 0);
        QVector<float> model, restored, reprojected;
        QVERIFY(context.ToModel(local, model, &error));
        QVERIFY(context.FromModel(model, restored, &error));
        QVERIFY(std::abs(restored[0] - local[0]) > CKeyformMappingContext::AbsoluteTolerance);
        QVERIFY(context.ToModel(restored, reprojected, &error));
        QVERIFY2(context.MatchesRuntime(reprojected, &error), qPrintable(error));
        QVERIFY2(context.MatchesRuntime(core.Positions("LegL"), &error), qPrintable(error));
    }

    void collapsedOwnScaleRetainsFiniteDisplayBasis()
    {
        const auto bytes = rotationFixture("own-zero");
        CModelDocument document;
        QString error;
        QVERIFY(document.LoadBytes(bytes, "zero.moc3", &error));
        const auto pose = poseFor(document, "LegL", 0);
        CoreModel core(bytes);
        QVERIFY(core.model);
        core.SetPose(pose);
        const auto positions = core.Positions("LegL");
        auto reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        C2DTree tree(table);
        for (auto value = pose.cbegin(); value != pose.cend(); ++value) tree.SetParaValue(value.key(), value.value());
        bool found = false;
        for (auto* node : tree.GetWarpKeys()) if (auto* rotation = dynamic_cast<CRotationNode*>(node); rotation && rotation->getName() == "Rotation8") {
            found = true;
            QVERIFY(rotation->GetCanvasBasis());
            const auto& basis = *rotation->GetCanvasBasis();
            QVERIFY(std::abs(std::hypot(basis.xAxis.x(), basis.xAxis.y()) - 1) < 0.000001);
            QVERIFY(std::abs(std::hypot(basis.yAxis.x(), basis.yAxis.y()) - 1) < 0.000001);
            for (int vertex = 0; vertex < positions.size() / 2; ++vertex)
                QVERIFY(std::hypot(basis.origin.x() - positions[vertex * 2], basis.origin.y() + positions[vertex * 2 + 1]) < 0.00002);
        }
        QVERIFY(found);
        CKeyformMappingContext mapping;
        QVERIFY(!mapping.Build(document, "LegL", 0, pose, &error));
    }
    void legacyEditorFixturesNeedCanonicalLayout()
    {
        for (bool quad : {false, true}) {
            DeformerSnapshotData::Fixture fixture(quad);
            CoreModel raw(fixture.bytes);
            qInfo() << "raw legacy fixture Core accepted" << quad << bool(raw.model);
            if (raw.model) {
                raw.SetPose({{"P", 0}});
                const auto actual = raw.Positions("Mesh");
                QVERIFY(!actual.isEmpty());
            }
        }
    }
    void rootAndWarpPositionsMatchCore()
    {
        const auto bytes = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(bytes, "Mark.moc3", &error), qPrintable(error));
        CoreModel core(bytes);
        QVERIFY(core.model);
        QMap<QString, float> pose;
        for (const auto& parameter : document.Parameters()) pose.insert(parameter.id, parameter.defaultValue);
        QMap<QString, float> corner;
        QVERIFY(document.KeyformParameterValues("Body", "artmesh", 0, corner, &error));
        for (auto value = corner.cbegin(); value != corner.cend(); ++value) pose[value.key()] = value.value();
        core.SetPose(pose);
        CKeyformMappingContext context;
        QVERIFY2(context.Build(document, "Body", 0, pose, &error), qPrintable(error));
        QVERIFY2(context.MatchesRuntime(core.Positions("Body"), &error), qPrintable(error));
    }
};
QTEST_GUILESS_MAIN(KeyformMappingCoreTests)
#include "keyform_mapping_core_tests.moc"
