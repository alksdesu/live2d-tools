#include <QtTest>
#include <QJsonArray>
#include <limits>
#include "std/cpsdexportcoverage.h"

namespace {
using Coverage = CPsdExportCoverage;
Coverage::Context context(int count = 4)
{
    Coverage::Context result;
    result.sourceMocDigest = QByteArray(32, 's');
    result.runtimeMocDigest = QByteArray(32, 'r');
    result.canvas = QSize(64, 48);
    result.runtimeCount = count;
    return result;
}
QVector<Coverage::Record> records()
{
    return {{0, "Mesh0", "眼睛", "Part", 1, Coverage::State::Raster, Coverage::Reason::None, QRect(2, 3, 5, 7)},
        {1, "Mesh1", "眼睛", "Part", 2, Coverage::State::Empty, Coverage::Reason::NoTriangles, {}},
        {2, "Mesh2", "隐藏", "", 3, Coverage::State::Excluded, Coverage::Reason::EditorHidden, {}},
        {3, "Mesh3", "未完成", "", 4, Coverage::State::Pending, Coverage::Reason::None, {}}};
}
}

class PsdExportCoverageTests : public QObject
{
    Q_OBJECT
private slots:
    void defaultsDistinguishUnknownFromEmptyModel()
    {
        Coverage unknown;
        QVERIFY(unknown.IsEmpty()); QVERIFY(unknown.IsValid()); QVERIFY(!unknown.IsCommitted());
        QVERIFY(unknown.ToJson().isEmpty()); QVERIFY(unknown.Text().isEmpty());
        Coverage known;
        QVERIFY(Coverage::Capture(context(0), {}, true, known));
        QVERIFY(!known.IsEmpty()); QVERIFY(known.IsCommitted()); QVERIFY(unknown != known);
        QCOMPARE(known.Statistics().source, 0);
        QCOMPARE(known.ToJson()["records"].toArray().size(), 0);
    }
    void partialReportConservesEveryIdentityAndDoesNotClaimOutput()
    {
        Coverage result;
        QVERIFY(Coverage::Capture(context(), records(), false, result));
        const auto stats = result.Statistics();
        QCOMPARE(stats.source, 4); QCOMPARE(stats.requested, 3);
        QCOMPARE(stats.raster, 1); QCOMPARE(stats.empty, 1); QCOMPARE(stats.excluded, 1); QCOMPARE(stats.pending, 1);
        const auto rows = result.ToJson()["records"].toArray();
        QCOMPARE(rows.size(), 4);
        QCOMPARE(rows[1].toObject()["id"].toString(), QString("Mesh1"));
        QCOMPARE(rows[1].toObject()["psdLayerId"].toInt(), 2);
        QCOMPARE(rows[1].toObject()["reason"].toString(), QString("no_triangles"));
        QVERIFY(result.Text().contains(QStringLiteral("尚未提交")));
        QVERIFY(result.Text().contains(QStringLiteral("不代表原始素材已恢复")));
        QVERIFY(result.Text(1).contains(QStringLiteral("另有 2 项")));
        QCOMPARE(result.ToJson()["records"].toArray().size(), 4);
    }
    void committedReportHasNoPendingAndRetainsDuplicateDisplayNames()
    {
        auto rows = records(); rows[3].state = Coverage::State::Empty; rows[3].reason = Coverage::Reason::OutsideCanvas;
        Coverage result;
        QVERIFY(Coverage::Capture(context(), rows, true, result));
        QVERIFY(result.IsCommitted()); QCOMPARE(result.Statistics().pending, 0);
        QVERIFY(result.Text().contains(QStringLiteral("已写入目标 PSD")));
        QCOMPARE(result.Records()[0].displayName, result.Records()[1].displayName);
        QVERIFY(result.Records()[0].psdLayerId != result.Records()[1].psdLayerId);
    }
    void methodExplicitlyDescribesCanvasClipping()
    {
        auto input = context(); Coverage gpu, cpu;
        QVERIFY(Coverage::Capture(input, records(), false, gpu));
        input.method = Coverage::Method::Cpu;
        QVERIFY(Coverage::Capture(input, records(), false, cpu));
        QVERIFY(gpu.ToJson()["clipsToCanvas"].toBool()); QVERIFY(!cpu.ToJson()["clipsToCanvas"].toBool());
        QVERIFY(gpu.Text().contains(QStringLiteral("按画布裁剪")));
        QVERIFY(cpu.Text().contains(QStringLiteral("画布外的像素")));
        QVERIFY(gpu != cpu);
    }
    void ownsRawAliasesAndDetachedVectorStorage()
    {
        auto input = context(); auto rows = records();
        char digest[32]; std::fill(std::begin(digest), std::end(digest), 'a');
        QChar id[]{'I', 'D'}, label[]{'<', 'b', '>'}, part[]{'P'};
        input.sourceMocDigest = QByteArray::fromRawData(digest, 32);
        rows[0].id = QString::fromRawData(id, 2);
        rows[0].displayName = QString::fromRawData(label, 3);
        rows[0].partId = QString::fromRawData(part, 1);
        auto* alias = rows.data();
        Coverage result; QVERIFY(Coverage::Capture(input, rows, false, result));
        const auto json = result.ToJson();
        digest[0] = 'z'; id[0] = 'X'; label[0] = 'Y'; part[0] = 'Z'; alias[0].bounds = QRect(9, 9, 1, 1);
        QCOMPARE(result.ToJson(), json);
        Coverage copy = result; result = {}; QCOMPARE(copy.ToJson(), json);
    }
    void invalidRecordsAreAtomic_data()
    {
        QTest::addColumn<int>("mutation");
        for (int index = 0; index < 18; ++index) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void invalidRecordsAreAtomic()
    {
        QFETCH(int, mutation);
        auto input = context(); auto rows = records(); bool committed = false;
        Coverage result; QVERIFY(Coverage::Capture(input, rows, false, result)); const Coverage before = result;
        switch (mutation) {
        case 0: input.sourceMocDigest.clear(); break;
        case 1: input.runtimeCount = 3; break;
        case 2: input.method = static_cast<Coverage::Method>(5); break;
        case 3: input.canvas = QSize(30001, 40); break;
        case 4: rows[1].id = rows[0].id; break;
        case 5: rows[0].id.clear(); break;
        case 6: rows[0].runtimeIndex = 1; break;
        case 7: rows[1].psdLayerId = 1; break;
        case 8: rows[0].bounds = {}; break;
        case 9: rows[1].reason = Coverage::Reason::None; break;
        case 10: rows[2].reason = Coverage::Reason::OutsideCanvas; break;
        case 11: input.includeHidden = true; break;
        case 12: rows[3].bounds = QRect(0, 0, 1, 1); break;
        case 13: rows[3].reason = Coverage::Reason::NoTriangles; break;
        case 14: rows[0].displayName = QString(QChar(0xd800)); break;
        case 15: rows[0].partId.append(QChar::Null); break;
        case 16: committed = true; break;
        case 17: rows[0].state = static_cast<Coverage::State>(8); break;
        }
        QString error;
        QVERIFY(!Coverage::Capture(input, rows, committed, result, &error));
        QVERIFY(!error.isEmpty()); QVERIFY(result == before);
    }
    void budgetAndUnknownEnumCannotForgeCompleteness()
    {
        auto rows = records(); auto input = context(); Coverage result;
        rows[0].displayName = QString(32769, 'a');
        QVERIFY(!Coverage::Capture(input, rows, false, result));
        rows = records(); rows[1].reason = static_cast<Coverage::Reason>(99);
        QVERIFY(!Coverage::Capture(input, rows, false, result));
        input.runtimeCount = Coverage::MaximumRecords + 1;
        QVERIFY(!Coverage::Capture(input, {}, false, result));
        QVERIFY(result.IsEmpty());
    }
    void aggregateTextBudgetIsCheckedBeforeOwningEveryName()
    {
        QVector<Coverage::Record> rows;
        const QString sharedName(32768, 'x');
        for (int index = 0; index < 513; ++index)
            rows.append({index, QString::number(index), sharedName, {}, quint32(index + 1)});
        Coverage result, before;
        QVERIFY(Coverage::Capture(context(), records(), false, result)); before = result;
        QString error;
        QVERIFY(!Coverage::Capture(context(rows.size()), rows, false, result, &error));
        QVERIFY(error.contains(QStringLiteral("预算"))); QVERIFY(result == before);
    }
    void extremeRectangleDoesNotOverflowDuringValidation()
    {
        auto rows = records();
        rows[0].bounds.setCoords(std::numeric_limits<int>::min(), 0, std::numeric_limits<int>::max() - 1, 20);
        Coverage result;
        QVERIFY(!Coverage::Capture(context(), rows, false, result));
        QVERIFY(result.IsEmpty());
    }
    void fullReportDoesNotTruncateTheVisualDetailLimit()
    {
        QVector<Coverage::Record> rows;
        for (int index = 0; index < 180; ++index)
            rows.append({index, QString::number(index), QStringLiteral("<b>显示名称</b>"), {}, quint32(index + 1),
                Coverage::State::Empty, Coverage::Reason::DegenerateBounds, {}});
        Coverage result; QVERIFY(Coverage::Capture(context(rows.size()), rows, true, result));
        QCOMPARE(result.ToJson()["records"].toArray().size(), 180);
        QVERIFY(result.Text().contains(QStringLiteral("另有 80 项")));
        QVERIFY(result.Text().contains(QStringLiteral("<b>显示名称</b>")));
    }
};
QTEST_GUILESS_MAIN(PsdExportCoverageTests)
#include "psd_export_coverage_tests.moc"
