#include "live2dstruct/cparameterorganization.h"
#include "live2dstruct/cmodeldocument.h"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <limits>
#include <cmath>

using namespace live2dstruct;
using Organization = CParameterOrganization;
namespace {
Organization::State organized(const QStringList& ids)
{
    Organization::State result; result.configured = true;
    result.nodes.append({Organization::Node::Folder, "face", {}, QStringLiteral("脸部"), "#aAbB01"});
    for (int index = 0; index < ids.size(); ++index)
        result.nodes.append({Organization::Node::Parameter, ids[index], index < 2 ? "face" : QString(), {}, {}});
    if (ids.size() >= 2) result.pairs.append({ids[0], ids[1], false, true});
    return result;
}
QStringList ids(const CModelDocument& document)
{
    QStringList result; for (const auto& parameter : document.Parameters()) result.append(parameter.id); return result;
}
}
class ParameterOrganizationTests : public QObject
{
    Q_OBJECT
private slots:
    void colorsOrderAndTypedIdsRoundTrip()
    {
        auto input = organized({"face", "B", "C"}); input.nodes.insert(2, {Organization::Node::Folder, "other", {}, "Other", "#112233"});
        Organization::State normalized; QString error;
        QVERIFY2(Organization::Normalize(input, normalized, &error), qPrintable(error));
        QCOMPARE(normalized.nodes[0].color, QString("#AABB01")); QCOMPARE(input.nodes[0].color, QString("#aAbB01"));
        QVERIFY(Organization::Validate(normalized, {"face", "B", "C"}, &error));
        QJsonObject object; QVERIFY(Organization::ToJson(normalized, object, &error));
        Organization::State result; QVERIFY(Organization::FromJson(object, result, &error)); QCOMPARE(result, normalized);
        QCOMPARE(result.nodes[1].kind, Organization::Node::Parameter); QCOMPARE(result.nodes[1].id, QString("face"));
        QCOMPARE(result.nodes[2].kind, Organization::Node::Folder);
        QVERIFY(Organization::Normalize({}, result)); QVERIFY(!result.configured); QVERIFY(result.nodes.isEmpty());
    }

    void invalidStatesAreRejectedAtomically_data()
    {
        QTest::addColumn<int>("kind"); const char* rows[]{"unconfigured-nodes", "unconfigured-pairs", "nested-folder", "unknown-folder", "folder-duplicate",
            "parameter-duplicate", "parameter-name", "empty-folder-name", "invalid-kind", "color-alpha", "color-text", "color-length", "self-pair", "missing-pair",
            "duplicate-pair", "overlapping-disabled-pair", "null-id", "oversized-name", "node-budget"};
        for (int kind = 0; kind < 19; ++kind) QTest::newRow(rows[kind]) << kind;
    }
    void invalidStatesAreRejectedAtomically()
    {
        QFETCH(int, kind); auto input = organized({"A", "B", "C"});
        if (kind == 0) input.configured = false;
        if (kind == 1) { input.configured = false; input.nodes.clear(); }
        if (kind == 2) input.nodes[0].parentFolder = "face";
        if (kind == 3) input.nodes[1].parentFolder = "missing";
        if (kind == 4) input.nodes.append(input.nodes[0]);
        if (kind == 5) input.nodes.append(input.nodes[1]);
        if (kind == 6) input.nodes[1].name = "not a parameter label";
        if (kind == 7) input.nodes[0].name = "  ";
        if (kind == 8) input.nodes[0].kind = static_cast<Organization::Node::Kind>(9);
        if (kind == 9) input.nodes[0].color = "#11223344";
        if (kind == 10) input.nodes[0].color = "red";
        if (kind == 11) input.nodes[0].color = "#ABC";
        if (kind == 12) input.pairs[0].second = "A";
        if (kind == 13) input.pairs[0].second = "missing";
        if (kind == 14) input.pairs.append(input.pairs[0]);
        if (kind == 15) input.pairs.append({"B", "C", true, false});
        if (kind == 16) input.nodes[1].id.append(QChar::Null);
        if (kind == 17) input.nodes[0].name = QString(Organization::MaximumTextLength + 1, 'x');
        if (kind == 18) input.nodes.resize(Organization::MaximumNodes + 1);
        Organization::State output = organized({"keep", "other"}); const auto before = output; QString error;
        QVERIFY(!Organization::Normalize(input, output, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(output, before);
        QJsonObject json{{"keep", true}}; QVERIFY(!Organization::ToJson(input, json, &error)); QCOMPARE(json, QJsonObject({{"keep", true}}));
    }

    void directoryReconciliationRemovesPairsAndDoesNotReseedRecreatedIds()
    {
        auto state = organized({"A", "B", "C"}); Organization::State removed; QString error;
        QVERIFY(Organization::Reconcile(state, {"A", "B", "C"}, {"B", "C"}, removed, &error));
        QVERIFY(removed.configured); QVERIFY(removed.pairs.isEmpty()); QCOMPARE(removed.nodes.size(), 3);
        Organization::State created; QVERIFY(Organization::Reconcile(removed, {"B", "C"}, {"B", "C", "A"}, created, &error));
        QCOMPARE(created.nodes.last().id, QString("A")); QVERIFY(created.nodes.last().parentFolder.isEmpty());
        QVERIFY(created.nodes.last().color.isEmpty()); QVERIFY(created.pairs.isEmpty());
        Organization::State empty; QVERIFY(Organization::Reconcile(created, {"B", "C", "A"}, {}, empty, &error));
        QVERIFY(empty.configured); QCOMPARE(empty.nodes.size(), 1); QCOMPARE(empty.nodes[0].kind, Organization::Node::Folder);
        Organization::State original; QVERIFY(Organization::Reconcile({}, {"A"}, {"A", "B"}, original)); QVERIFY(!original.configured);
        const auto held = created; QVERIFY(!Organization::Reconcile(state, {"A", "B"}, {"B"}, created, &error)); QCOMPARE(created, held);
        QVERIFY(!Organization::Validate(state, {"A", "B"}, &error));
    }

    void malformedJsonPreservesResult_data()
    {
        QTest::addColumn<int>("kind"); for (int kind = 0; kind < 9; ++kind) QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void malformedJsonPreservesResult()
    {
        QFETCH(int, kind); QJsonObject object; QVERIFY(Organization::ToJson(organized({"A", "B"}), object));
        if (kind == 0) object.remove("configured"); if (kind == 1) object["configured"] = 1;
        if (kind == 2) object["nodes"] = QJsonObject{}; if (kind == 3) object["unknown"] = true;
        if (kind >= 4 && kind <= 6) {
            auto nodes = object["nodes"].toArray(); auto row = nodes[0].toObject();
            if (kind == 4) row["kind"] = 1; if (kind == 5) row.remove("color"); if (kind == 6) row["extra"] = true;
            nodes[0] = row; object["nodes"] = nodes;
        }
        if (kind >= 7) {
            auto pairs = object["pairs"].toArray(); auto row = pairs[0].toObject();
            if (kind == 7) row["enabled"] = "true"; else row["reversed"] = 0;
            pairs[0] = row; object["pairs"] = pairs;
        }
        auto output = organized({"keep", "other"}); const auto before = output; QString error;
        QVERIFY(!Organization::FromJson(object, output, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(output, before);
    }

    void pairMappingUsesCompleteRangeAndExplicitFirstAsAuthority()
    {
        CModelDocument document; QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto parameters = ids(document); const auto first = parameters[0], second = parameters[1];
        auto left = document.ParameterMetadata(first), right = document.ParameterMetadata(second);
        left.minimum = -10; left.maximum = 30; left.defaultValue = 0;
        right.minimum = 2; right.maximum = 10; right.defaultValue = 3;
        QVERIFY(document.SetParameterMetadata(first, left)); QVERIFY(document.SetParameterMetadata(second, right));
        auto organization = organized(parameters); QVERIFY(document.SetParameterOrganization(organization));
        const auto revision = document.Revision(); QMap<QString, float> result; QString error;
        QVERIFY(Organization::ExpandUserValues(document, {{first, 0}}, result, &error)); QCOMPARE(result.value(second), 4.f);
        QVERIFY(Organization::ExpandUserValues(document, {{second, 4}}, result, &error)); QCOMPARE(result.value(first), 0.f);
        organization.pairs[0].reversed = true; QVERIFY(document.SetParameterOrganization(organization));
        QVERIFY(Organization::ExpandUserValues(document, {{first, -10}}, result, &error)); QCOMPARE(result.value(second), 10.f);
        QVERIFY(Organization::ExpandUserValues(document, {{first, 30}}, result, &error)); QCOMPARE(result.value(second), 2.f);
        QVERIFY(Organization::ExpandUserValues(document, {{first, 0}, {second, 8}}, result, &error)); QCOMPARE(result.size(), 2);
        const auto held = result; QVERIFY(!Organization::ExpandUserValues(document, {{first, 0}, {second, 4}}, result, &error)); QCOMPARE(result, held);
        right.minimum = 1; right.maximum = std::nextafter(1.f, 2.f); right.defaultValue = 1;
        QVERIFY(document.SetParameterMetadata(second, right));
        QVERIFY(Organization::ExpandUserValues(document, {{first, 7}}, result, &error)); const auto quantized = result;
        QVERIFY(Organization::ExpandUserValues(document, quantized, result, &error)); QCOMPARE(result, quantized);
        QCOMPARE(document.Revision(), revision + 2);
    }

    void disabledAndZeroWidthPairsHaveExplicitBehavior()
    {
        CModelDocument document; QVERIFY(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3"));
        const auto parameters = ids(document); const auto first = parameters[0], second = parameters[1];
        auto metadata = document.ParameterMetadata(first); metadata.minimum = metadata.maximum = metadata.defaultValue = .5f;
        QVERIFY(document.SetParameterMetadata(first, metadata)); auto organization = organized(parameters); QVERIFY(document.SetParameterOrganization(organization));
        QMap<QString, float> result{{"keep", 9}}; const auto held = result; QString error;
        QVERIFY(!Organization::ExpandUserValues(document, {{first, .5f}}, result, &error)); QCOMPARE(result, held);
        organization.pairs[0].enabled = false; QVERIFY(document.SetParameterOrganization(organization));
        QVERIFY(Organization::ExpandUserValues(document, {{first, .5f}}, result, &error)); QCOMPARE(result, (QMap<QString, float>{{first, .5f}}));
        for (const auto& input : QVector<QMap<QString, float>>{{}, {{"missing", 0}}, {{first, .6f}}, {{second, std::numeric_limits<float>::infinity()}}}) {
            result = held; QVERIFY(!Organization::ExpandUserValues(document, input, result, &error)); QCOMPARE(result, held);
        }
    }
};
QTEST_GUILESS_MAIN(ParameterOrganizationTests)
#include "parameter_organization_tests.moc"
