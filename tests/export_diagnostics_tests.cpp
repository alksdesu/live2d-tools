#include "std/cexportdiagnostics.h"
#include <QtTest>
#include <QJsonArray>

using Diagnostic = live2dstruct::CModelDiagnostic;

class ExportDiagnosticsTests : public QObject
{
    Q_OBJECT
private slots:
    void ownsBorrowedContextAndMessages()
    {
        QString owner = QStringLiteral("Eye <L>"), message = QStringLiteral("保留来源的重复引用"), path = QStringLiteral("/tmp/model.moc3");
        QByteArray digest(32, 's');
        const auto review = CExportDiagnostics::Capture({{Diagnostic::Warning, "mask_source_duplicate",
            QString::fromRawData(message.constData(), message.size()), 42, "artmesh", QString::fromRawData(owner.constData(), owner.size())}},
            QString::fromRawData(path.constData(), path.size()), QByteArray::fromRawData(digest.constData(), digest.size()), quint64(1) << 63);
        owner[0] = QLatin1Char('X'); message[0] = QLatin1Char('X'); path[0] = QLatin1Char('X'); digest[0] = 'x';
        QCOMPARE(review.WarningCount(), 1);
        QCOMPARE(review.ErrorCount(), 0);
        QCOMPARE(review.Entries()[0].objectId, QStringLiteral("Eye <L>"));
        QVERIFY(review.Text().contains(QStringLiteral("保留来源的重复引用")));
        const auto json = review.ToJson();
        QCOMPARE(json["sourceMoc"].toString(), QStringLiteral("/tmp/model.moc3"));
        QCOMPARE(json["sourceDigest"].toString(), QString::fromLatin1(QByteArray(32, 's').toHex()));
        QCOMPARE(json["documentRevision"].toString(), QString::number(quint64(1) << 63));
        QCOMPARE(json["entries"].toArray()[0].toObject()["objectId"].toString(), QStringLiteral("Eye <L>"));
        QVERIFY(!review.IsTruncated());
    }

    void preservesCountsAndErrorsWhenDisplayIsBounded()
    {
        QVector<Diagnostic> values(CExportDiagnostics::MaximumEntries + 4,
            {Diagnostic::Warning, "mask_source_duplicate", "warning", 1, "artmesh", "Eye"});
        values.append({Diagnostic::Error, "mask_range", "invalid index", 3, "artmesh", "Face"});
        const auto review = CExportDiagnostics::Capture(values);
        QCOMPARE(review.WarningCount(), CExportDiagnostics::MaximumEntries + 4);
        QCOMPARE(review.ErrorCount(), 1);
        QCOMPARE(review.Entries().size(), CExportDiagnostics::MaximumEntries);
        QCOMPARE(review.Entries().first().severity, Diagnostic::Error);
        QCOMPARE(review.FirstError(), QStringLiteral("Face：invalid index"));
        QVERIFY(review.IsTruncated());
        QVERIFY(review.ToJson()["truncated"].toBool());
        QVERIFY(review.Text().contains(QStringLiteral("完整报告")));
    }

    void boundsTextWithoutLosingFailureStatus()
    {
        const auto review = CExportDiagnostics::Capture({
            {Diagnostic::Error, "oversized", QString(CExportDiagnostics::MaximumCharacters + 1, QLatin1Char('x')), -1},
            {Diagnostic::Warning, "small", "retained", 0}});
        QCOMPARE(review.ErrorCount(), 1);
        QCOMPARE(review.WarningCount(), 1);
        QCOMPARE(review.Entries().size(), 1);
        QCOMPARE(review.Entries()[0].message, QStringLiteral("retained"));
        QVERIFY(!review.FirstError().isEmpty());
        QVERIFY(review.IsTruncated());
        QVERIFY(review.Text().size() < CExportDiagnostics::MaximumCharacters);
    }

    void equalityTracksSourceAndStructuredDetails()
    {
        const QVector<Diagnostic> values{{Diagnostic::Warning, "source", "retained", 1, "artmesh", "Eye"}};
        const auto a = CExportDiagnostics::Capture(values, "a", QByteArray(32, 'a'), 1);
        QCOMPARE(a, CExportDiagnostics::Capture(values, "a", QByteArray(32, 'a'), 1));
        QVERIFY(a != CExportDiagnostics::Capture(values, "a", QByteArray(32, 'a'), 2));
        auto changed = values; changed[0].objectId = "Face";
        QVERIFY(a != CExportDiagnostics::Capture(changed, "a", QByteArray(32, 'a'), 1));
    }
};

QTEST_GUILESS_MAIN(ExportDiagnosticsTests)
#include "export_diagnostics_tests.moc"
