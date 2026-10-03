#include "ui/cblendshapegeometrydialog.h"
#include <QtTest>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableView>
#include <stdexcept>

namespace {
live2dstruct::CModelBlendShapeGeometryEntry entry()
{
    live2dstruct::CModelBlendShapeGeometryEntry result;
    result.meshId = QStringLiteral("Eye_L"); result.shapeIndex = 2; result.bindingOffset = 17;
    result.keyformOffset = 29; result.sourceKeyformIndex = 3; result.sourceDigest = QByteArray(32, '\x42');
    result.deltas = {0.0f, -0.0f, 1.25f, -2.5f};
    return result;
}

QAbstractItemModel* model(CBlendShapeGeometryDialog& dialog)
{
    auto* table = dialog.findChild<QTableView*>(QStringLiteral("blendShapeGeometryTable"));
    return table ? table->model() : nullptr;
}

QPushButton* button(CBlendShapeGeometryDialog& dialog, QDialogButtonBox::StandardButton which)
{
    auto* box = dialog.findChild<QDialogButtonBox*>(QStringLiteral("blendShapeGeometryButtons"));
    return box ? box->button(which) : nullptr;
}
}

class BlendShapeGeometryDialogTests : public QObject
{
    Q_OBJECT
private slots:
    void rendersTargetAndLocalDisplacements()
    {
        CBlendShapeGeometryDialog dialog(entry());
        QCOMPARE(dialog.findChild<QLabel*>(QStringLiteral("blendShapeGeometryTarget"))->text(),
                 QStringLiteral("Eye_L\n融合形 2 · 绑定 17 · 关键形 29 · 来源行 3 · 2 个顶点"));
        auto* table = dialog.findChild<QTableView*>(QStringLiteral("blendShapeGeometryTable")); QVERIFY(table);
        QCOMPARE(table->model()->rowCount(), 2); QCOMPARE(table->model()->columnCount(), 3);
        QCOMPARE(table->model()->data(table->model()->index(0, 1), Qt::DisplayRole).toString(), QStringLiteral("0"));
        QCOMPARE(table->model()->data(table->model()->index(0, 2), Qt::DisplayRole).toString(), QStringLiteral("-0"));
        QCOMPARE(table->model()->data(table->model()->index(1, 1), Qt::DisplayRole).toString(), QStringLiteral("1.25"));
        QVERIFY(!(table->model()->flags(table->model()->index(0, 0)) & Qt::ItemIsEditable));
    }

    void savesChangedCoordinatesAndCancelDoesNotSubmit()
    {
        CBlendShapeGeometryDialog dialog(entry());
        int calls = 0; QVector<float> received;
        dialog.SetSubmissionHandler([&](const QVector<float>& values, QString*) { ++calls; received = values; return true; });
        QVERIFY(!button(dialog, QDialogButtonBox::Save)->isEnabled());
        auto* data = model(dialog); QVERIFY(data);
        QVERIFY(data->setData(data->index(1, 1), QStringLiteral("3.5")));
        QVERIFY(button(dialog, QDialogButtonBox::Save)->isEnabled());
        dialog.reject(); QCOMPARE(calls, 0); QVERIFY(received.isEmpty());

        CBlendShapeGeometryDialog saved(entry());
        saved.SetSubmissionHandler([&](const QVector<float>& values, QString*) { ++calls; received = values; return true; });
        QVERIFY(model(saved)->setData(model(saved)->index(1, 1), QStringLiteral("3.5")));
        saved.accept(); QCOMPARE(calls, 1); QCOMPARE(received.at(2), 3.5f); QCOMPARE(saved.result(), int(QDialog::Accepted));
    }

    void resetRestoresSignedZeroAndSaveIsDisabled()
    {
        CBlendShapeGeometryDialog dialog(entry());
        dialog.SetSubmissionHandler([](const QVector<float>&, QString*) { return true; });
        auto* data = model(dialog); QVERIFY(data);
        QVERIFY(data->setData(data->index(0, 2), QStringLiteral("4")));
        QVERIFY(button(dialog, QDialogButtonBox::Reset)->isEnabled());
        button(dialog, QDialogButtonBox::Reset)->click();
        QVERIFY(!button(dialog, QDialogButtonBox::Save)->isEnabled());
        QCOMPARE(data->data(data->index(0, 2)).toString(), QStringLiteral("-0"));
    }

    void invalidValuesStayVisibleAndDoNotSubmit_data()
    {
        QTest::addColumn<QString>("value");
        QTest::newRow("empty") << QString(); QTest::newRow("nan") << QStringLiteral("nan");
        QTest::newRow("infinity") << QStringLiteral("inf"); QTest::newRow("overflow") << QStringLiteral("1e100");
        QTest::newRow("underflow") << QStringLiteral("1e-100");
    }

    void invalidValuesStayVisibleAndDoNotSubmit()
    {
        QFETCH(QString, value);
        CBlendShapeGeometryDialog dialog(entry()); int calls = 0;
        dialog.SetSubmissionHandler([&](const QVector<float>&, QString*) { ++calls; return true; });
        auto* data = model(dialog); QVERIFY(data); QVERIFY(!data->setData(data->index(0, 1), value));
        QCOMPARE(data->data(data->index(0, 1)).toString(), value);
        QVERIFY(!button(dialog, QDialogButtonBox::Save)->isEnabled());
        dialog.accept(); QCOMPARE(calls, 0); QVERIFY(dialog.isVisible() || dialog.result() == 0);
        QVERIFY(!dialog.Error().isEmpty());
    }

    void failedCallbackRetainsDraftAndCanRetry()
    {
        CBlendShapeGeometryDialog dialog(entry()); int calls = 0;
        dialog.SetSubmissionHandler([&](const QVector<float>&, QString* error) {
            ++calls; *error = QStringLiteral("模型拒绝了该端点"); return calls > 1;
        });
        auto* data = model(dialog); QVERIFY(data); QVERIFY(data->setData(data->index(1, 2), QStringLiteral("9")));
        dialog.show(); dialog.accept(); QVERIFY(dialog.isVisible()); QCOMPARE(calls, 1);
        QCOMPARE(data->data(data->index(1, 2)).toString(), QStringLiteral("9"));
        QCOMPARE(dialog.Error(), QStringLiteral("模型拒绝了该端点")); QVERIFY(!dialog.IsSubmitting());
        dialog.accept(); QCOMPARE(calls, 2); QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }

    void callbackExceptionAndReentryAreSafe()
    {
        CBlendShapeGeometryDialog dialog(entry()); int calls = 0;
        dialog.SetSubmissionHandler([&](const QVector<float>&, QString*) -> bool {
            ++calls; const bool submitting = dialog.IsSubmitting(); dialog.reject(); dialog.accept();
            if (!submitting) throw std::runtime_error("not submitting");
            throw std::runtime_error("boom");
        });
        auto* data = model(dialog); QVERIFY(data); QVERIFY(data->setData(data->index(0, 1), QStringLiteral("7")));
        dialog.show(); dialog.accept(); QCOMPARE(calls, 1); QVERIFY(dialog.isVisible()); QVERIFY(!dialog.Error().isEmpty());
        QVERIFY(!dialog.IsSubmitting());
    }

    void emptySnapshotIsRejected()
    {
        auto invalid = entry(); invalid.deltas.clear();
        CBlendShapeGeometryDialog dialog(invalid); int calls = 0;
        dialog.SetSubmissionHandler([&](const QVector<float>&, QString*) { ++calls; return true; });
        QVERIFY(!button(dialog, QDialogButtonBox::Save)->isEnabled()); dialog.accept(); QCOMPARE(calls, 0);
        QVERIFY(!dialog.Error().isEmpty());
    }
};

QTEST_MAIN(BlendShapeGeometryDialogTests)
#include "blend_shape_geometry_dialog_tests.moc"
