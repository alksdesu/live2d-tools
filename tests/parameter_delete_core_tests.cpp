#include "real_core_model_fixture.h"
#include "live2dstruct/cmocparameterstructurebuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include "parameter_delete_fixture.h"
#include <QFileInfo>
#include <QRandomGenerator>
#include <QtEndian>

using namespace live2dstruct;
using Builder = CMocParameterStructureBuilder;

class ParameterDeleteCoreTests : public QObject
{
    Q_OBJECT
    void compareFrozen(const QByteArray& original, const QByteArray& candidate, const QString& parameter, float freeze)
    {
        CoreModel before, after;
        QVERIFY(before.load(original)); QVERIFY(after.load(candidate));
        const int count = csmGetParameterCount(before.value);
        QCOMPARE(csmGetParameterCount(after.value), count - 1);
        int removed = -1, next = 0;
        for (int index = 0; index < count; ++index) {
            const QString id = QString::fromUtf8(csmGetParameterIds(before.value)[index]);
            if (id == parameter) { removed = index; continue; }
            QCOMPARE(QString::fromUtf8(csmGetParameterIds(after.value)[next]), id);
            QCOMPARE(csmGetParameterMinimumValues(after.value)[next], csmGetParameterMinimumValues(before.value)[index]);
            QCOMPARE(csmGetParameterMaximumValues(after.value)[next], csmGetParameterMaximumValues(before.value)[index]);
            QCOMPARE(csmGetParameterDefaultValues(after.value)[next], csmGetParameterDefaultValues(before.value)[index]);
            QCOMPARE(csmGetParameterTypes(after.value)[next], csmGetParameterTypes(before.value)[index]);
            const int keys = csmGetParameterKeyCounts(before.value)[index];
            QCOMPARE(csmGetParameterKeyCounts(after.value)[next], keys);
            QCOMPARE(raw(csmGetParameterKeyValues(after.value)[next], keys), raw(csmGetParameterKeyValues(before.value)[index], keys));
            ++next;
        }
        QVERIFY(removed >= 0);
        auto samples = poses(before.value);
        const auto defaults = samples.first();
        for (int index = 0; index < count; ++index) {
            auto pose = defaults;
            pose[index] = static_cast<float>((double(csmGetParameterMinimumValues(before.value)[index]) + csmGetParameterMaximumValues(before.value)[index]) * .5);
            samples.append(pose);
        }
        QRandomGenerator random(20260925);
        for (int sample = 0; sample < 32; ++sample) {
            auto pose = defaults;
            for (int index = 0; index < count; ++index) {
                const double low = csmGetParameterMinimumValues(before.value)[index], high = csmGetParameterMaximumValues(before.value)[index];
                pose[index] = static_cast<float>(low + random.generateDouble() * (high - low));
            }
            samples.append(pose);
        }
        QSet<int> drawables;
        for (int index = 0; index < csmGetDrawableCount(before.value); ++index) drawables.insert(index);
        for (int sample = 0; sample < samples.size(); ++sample) {
            auto oldPose = samples[sample]; oldPose[removed] = freeze;
            auto newPose = oldPose; newPose.removeAt(removed);
            before.pose(oldPose); after.pose(newPose);
            const auto expected = frame(before.value), actual = frame(after.value);
            QVERIFY2(changed(expected, actual, drawables).isEmpty(), qPrintable(QStringLiteral("%1 at pose %2: %3").arg(parameter).arg(sample).arg(difference(expected, actual, drawables))));
        }
        QVERIFY(before.intact() && after.intact());
    }

private slots:
    void frozenBindingsMatchEveryOtherParameterPose_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<QString>("parameter"); QTest::addColumn<float>("freeze");
        for (const auto& model : models()) {
            auto reader = CReader::FromBytes(read(model.second)); CSectionTable table(reader);
            QVERIFY(table.IsValid());
            QStringList ids = QFileInfo(model.second).baseName() == QLatin1String("Wanko")
                ? QStringList{QStringLiteral("PARAM_ANGLE_X"), QStringLiteral("PARAM_BODY_ANGLE_Y")}
                : QStringList{QStringLiteral("ParamAngleX"), QStringLiteral("ParamAngleZ")};
            if (QFileInfo(model.second).baseName() == QLatin1String("Natori")) ids += {QStringLiteral("ParamArmAL04"), QStringLiteral("ParamArmAR04"), QStringLiteral("ParamArmCR03")};
            for (const auto& id : ids) {
                const CParameters* parameter = nullptr;
                for (const auto* value : table.parameters) if (value->ids == id) parameter = value;
                QVERIFY(parameter);
                for (float value : {parameter->default_values, static_cast<float>(parameter->min_values + .625 * (double(parameter->max_values) - parameter->min_values))})
                    QTest::newRow(qPrintable(model.first + '-' + id + '-' + QString::number(value, 'g', 9))) << model.second << id << value;
            }
        }
    }

    void frozenBindingsMatchEveryOtherParameterPose()
    {
        QFETCH(QString, path); QFETCH(QString, parameter); QFETCH(float, freeze);
        const auto original = read(path);
        Builder::DeleteImpact impact; Builder::Result result; QString error;
        QVERIFY2(Builder::AnalyzeDelete(original, {parameter, freeze}, impact, &error), qPrintable(error));
        QVERIFY2(impact.blockingReasons.isEmpty(), qPrintable(impact.blockingReasons.join('\n')));
        QVERIFY2(Builder::Delete(original, {parameter, freeze}, result, &error), qPrintable(error));
        compareFrozen(original, result.bytes, parameter, freeze);
    }

    void constantLimiterKeepsMinimumCombination_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<float>("freeze");
        for (const auto& model : models()) for (float freeze : {0.0f, .37f, 1.0f})
            QTest::newRow(qPrintable(model.first + '-' + QString::number(freeze))) << model.second << freeze;
    }

    void constantLimiterKeepsMinimumCombination()
    {
        QFETCH(QString, path); QFETCH(float, freeze);
        const auto bytes = read(path);
        Builder::CreateRequest create; create.parameter.id = QStringLiteral("ParamDeleteLimiter");
        create.parameter.minimum = 0; create.parameter.maximum = 1; create.parameter.defaultValue = .25f;
        Builder::Result created, removed;
        QString error;
        QVERIFY(Builder::Create(bytes, create, created, &error));
        auto reader = CReader::FromBytes(created.bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QByteArray fixture = created.bytes;
        if (!table.blend_shape_constraints.empty()) {
            Builder::ColumnState state;
            for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
                if (QByteArray(span.name) == "blend_shape_constraints.parameter_indices") {
                    auto values = fixture.mid(span.offset, span.count * span.width);
                    qToLittleEndian(qint32(table.parameters.size() - 1), values.data());
                    state.columns.insert(span.slot, values);
                }
            QVERIFY(!state.columns.isEmpty());
            QVERIFY(Builder::BuildColumnState(created.bytes, state, fixture, &error));
        }
        QVERIFY2(Builder::Delete(fixture, {create.parameter.id, freeze}, removed, &error), qPrintable(error));
        compareFrozen(fixture, removed.bytes, create.parameter.id, freeze);
    }

    void reflectionProjectionMatchesCore_data()
    {
        QTest::addColumn<int>("pattern"); QTest::addColumn<bool>("fast"); QTest::addColumn<float>("freeze");
        for (int pattern : {0, 1}) for (bool fast : {false, true})
            for (float value : fast ? QVector<float>{-5, -2.5f, 0, 3.7f, 10} : QVector<float>{-1, -.63f, 0, .25f, .9f, 1})
                QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3").arg(pattern).arg(fast).arg(value))) << pattern << fast << value;
    }

    void reflectionProjectionMatchesCore()
    {
        QFETCH(int, pattern); QFETCH(bool, fast); QFETCH(float, freeze);
        ParameterDeleteFixture::Reflection fixture;
        QString error;
        QVERIFY(ParameterDeleteFixture::MakeReflection(read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"), pattern, fixture, &error));
        const QString id = fast ? fixture.fastParameter : fixture.parameter;
        Builder::Result result;
        QVERIFY2(Builder::Delete(fixture.bytes, {id, freeze}, result, &error), qPrintable(error));
        compareFrozen(fixture.bytes, result.bytes, id, freeze);
    }

    void singletonKeepsExactKeyAndRejectsUnprovenOffKey_data()
    {
        QTest::addColumn<QString>("model"); QTest::addColumn<QString>("parameter"); QTest::addColumn<float>("offKey");
        QTest::newRow("artmesh-warp") << QStringLiteral("Mark") << QStringLiteral("ParamAngleX") << 7.5f;
        QTest::newRow("rotation") << QStringLiteral("Mark") << QStringLiteral("ParamAngleZ") << 7.5f;
        QTest::newRow("part") << QStringLiteral("Natori") << QStringLiteral("ParamArmAL04") << -.375f;
        QTest::newRow("glue") << QStringLiteral("Natori") << QStringLiteral("ParamArmCR03") << 7.5f;
    }

    void singletonKeepsExactKeyAndRejectsUnprovenOffKey()
    {
        QFETCH(QString, model); QFETCH(QString, parameter); QFETCH(float, offKey);
        const auto original = read(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3");
        QByteArray fixture; QString error;
        QVERIFY2(ParameterDeleteFixture::MakeSingleton(original, parameter, 0, fixture, &error), qPrintable(error));
        CoreModel accepted; QVERIFY(accepted.load(fixture));
        Builder::DeleteImpact impact; Builder::Result result; result.bytes = "sentinel";
        QVERIFY(Builder::AnalyzeDelete(fixture, {parameter, offKey}, impact, &error));
        QVERIFY(!impact.blockingReasons.isEmpty());
        QVERIFY(!Builder::Delete(fixture, {parameter, offKey}, result, &error));
        QCOMPARE(result.bytes, QByteArray("sentinel"));
        QVERIFY2(Builder::Delete(fixture, {parameter, 0}, result, &error), qPrintable(error));
        compareFrozen(fixture, result.bytes, parameter, 0);
    }

    void noncanonicalReflectionIsRejectedBeforePublication()
    {
        ParameterDeleteFixture::Reflection fixture; QString error;
        QVERIFY(ParameterDeleteFixture::MakeReflection(read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"), 2, fixture, &error));
        CoreModel core;
        QVERIFY(!core.load(fixture.bytes));
        Builder::DeleteImpact impact; Builder::Result result; result.bytes = "sentinel";
        QVERIFY(Builder::AnalyzeDelete(fixture.bytes, {fixture.parameter, .25f}, impact, &error));
        QVERIFY(!impact.blockingReasons.isEmpty());
        QVERIFY(!Builder::Delete(fixture.bytes, {fixture.parameter, .25f}, result, &error));
        QCOMPARE(result.bytes, QByteArray("sentinel"));
    }

    void sharedSpecificOwnersFreezeTogether_data()
    {
        QTest::addColumn<float>("freeze");
        QTest::newRow("exact-key") << 0.0f;
        QTest::newRow("interpolated") << 7.5f;
    }

    void sharedSpecificOwnersFreezeTogether()
    {
        QFETCH(float, freeze);
        auto bytes = read(QString(TEST_RESOURCES) + "/Natori/Natori.moc3");
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int first = -1, second = -1;
        for (int index = 0; index < table.deformers.size(); ++index) {
            if (table.deformers[index]->ids == QLatin1String("Rotation3")) first = index;
            if (table.deformers[index]->ids == QLatin1String("Rotation17")) second = index;
        }
        QVERIFY(first >= 0 && second >= 0);
        const auto& columns = *table.sectionOffsetTable->deformers;
        qToLittleEndian(qint32(table.deformers[first]->specific_sources_indices), bytes.data() + columns.specific_sources_indices + second * 4);
        qToLittleEndian(qint32(table.deformers[first]->keyform_binding_sources_indices), bytes.data() + columns.keyform_binding_sources_indices + second * 4);
        CKeyformBindingGrid grid;
        auto sharedReader = CReader::FromBytes(bytes); CSectionTable shared(sharedReader);
        QVERIFY(shared.IsValid()); QVERIFY(grid.Build(shared));
        const auto axes = grid.Axes(CKeyformBindingGrid::Rotation, QStringLiteral("Rotation3"));
        QCOMPARE(axes.size(), 1);
        Builder::Result result; QString error;
        QVERIFY2(Builder::Delete(bytes, {axes.first().parameterId, freeze}, result, &error), qPrintable(error));
        auto resultReader = CReader::FromBytes(result.bytes); CSectionTable after(resultReader);
        QVERIFY(after.IsValid());
        QCOMPARE(after.deformers[first]->specific_sources_indices, shared.deformers[first]->specific_sources_indices);
        QCOMPARE(after.deformers[second]->specific_sources_indices, shared.deformers[second]->specific_sources_indices);
        compareFrozen(bytes, result.bytes, axes.first().parameterId, freeze);
    }
};

QTEST_APPLESS_MAIN(ParameterDeleteCoreTests)
#include "parameter_delete_core_tests.moc"
