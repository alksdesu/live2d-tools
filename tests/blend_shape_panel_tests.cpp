#include "ui/cblendshapepanel.h"
#include "ui/para/ccidjson.h"

#include <QtTest>
#include <QFile>
#include <QLabel>
#include <QSlider>
#include <QTemporaryDir>
#include <QTreeWidgetItemIterator>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

namespace {
bool save(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

struct Fixture
{
    QTemporaryDir directory;
    CCidJson cid;
    QString id, path;

    bool create(float minimum, float maximum, float defaultValue)
    {
        QFile file(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        if (!directory.isValid() || !file.open(QIODevice::ReadOnly)) return false;
        QByteArray bytes = file.readAll();
        const auto word = [&](int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); };
        id = QString::fromUtf8(bytes.mid(word(0x108), 64).split('\0').first());
        for (const auto& field : {std::pair{0x110, minimum}, {0x10c, maximum}, {0x114, defaultValue}}) {
            quint32 bits;
            std::memcpy(&bits, &field.second, sizeof(bits));
            qToLittleEndian(bits, bytes.data() + word(field.first));
        }
        path = directory.filePath("range.moc3");
        const QString cidPath = directory.filePath("range.cdi3.json");
        const QJsonDocument json(QJsonObject{{"CombinedParameters", QJsonArray{QJsonArray{id}}}});
        return save(path, bytes) && save(cidPath, json.toJson()) && cid.Parse(cidPath);
    }
};

QWidget* row(CBlendShapePanel& panel, const QString& id)
{
    auto* tree = panel.findChild<QTreeWidget*>();
    if (!tree) return nullptr;
    for (QTreeWidgetItemIterator item(tree); *item; ++item)
        if ((*item)->data(0, Qt::UserRole).toString() == id) return tree->itemWidget(*item, 1);
    return nullptr;
}
}

class BlendShapePanelTests : public QObject
{
    Q_OBJECT

    static void labelMatches(QLabel* label, float value)
    {
        QVERIFY(label);
        QCOMPARE(label->text(), QString::number(value, 'g', 9));
        bool converted = false;
        const double number = label->text().toDouble(&converted);
        QVERIFY(converted && std::isfinite(number));
        const double tolerance = std::max(std::abs(double(value)) * 5e-9,
                                          double(std::numeric_limits<float>::denorm_min()) * 0.5);
        QVERIFY(std::abs(number - double(value)) <= tolerance);
        QVERIFY(label->maximumWidth() >= label->fontMetrics().horizontalAdvance(label->text()));
    }

private slots:
    void finiteRangesSupportDefaultSliderAndSilentSync_data()
    {
        QTest::addColumn<float>("minimum");
        QTest::addColumn<float>("maximum");
        QTest::addColumn<float>("defaultValue");
        QTest::addColumn<int>("defaultPosition");
        const float maximum = std::numeric_limits<float>::max();
        const float h = std::ldexp(1.0f, 127);
        const float tiny = std::numeric_limits<float>::denorm_min();
        QTest::newRow("full-float-range") << -maximum << maximum << 0.0f << 500;
        QTest::newRow("positive-large") << h << 1.5f * h << 1.25f * h << 500;
        QTest::newRow("negative-large") << -1.5f * h << -h << -1.25f * h << 500;
        QTest::newRow("positive-subnormal") << 0.0f << 16 * tiny << 4 * tiny << 250;
        QTest::newRow("signed-subnormal") << -16 * tiny << 16 * tiny << 0.0f << 500;
        QTest::newRow("fixed-large") << h << h << h << 0;
        QTest::newRow("fixed-subnormal") << tiny << tiny << tiny << 0;
    }

    void finiteRangesSupportDefaultSliderAndSilentSync()
    {
        QFETCH(float, minimum);
        QFETCH(float, maximum);
        QFETCH(float, defaultValue);
        QFETCH(int, defaultPosition);
        Fixture fixture;
        QVERIFY(fixture.create(minimum, maximum, defaultValue));
        CBlendShapePanel panel;
        QSignalSpy changes(&panel, &CBlendShapePanel::ParamChanged);
        panel.BuildBlendShapes(fixture.path, &fixture.cid);
        QVERIFY(panel.getBlendShapeParamIds().contains(fixture.id));
        auto* widget = row(panel, fixture.id);
        QVERIFY(widget);
        auto* slider = widget->findChild<QSlider*>();
        auto* label = widget->findChild<QLabel*>();
        QVERIFY(slider && label);
        QCOMPARE(slider->value(), defaultPosition);
        QCOMPARE(slider->isEnabled(), minimum != maximum);
        labelMatches(label, defaultValue);
        QCOMPARE(changes.size(), 0);

        for (int position : {0, 250, 500, 750, 1000}) {
            const float expected = float(static_cast<long double>(minimum) +
                (static_cast<long double>(maximum) - minimum) * position / 1000);
            if (minimum != maximum) {
                const int before = slider->value();
                slider->setValue(position);
                QCOMPARE(changes.size(), before == position ? 0 : 1);
                if (!changes.isEmpty()) {
                    const auto event = changes.takeFirst();
                    QCOMPARE(event[0].toString(), fixture.id);
                    QCOMPARE(event[1].toFloat(), expected);
                    QVERIFY(std::isfinite(event[1].toFloat()));
                    QVERIFY(!event[2].toBool());
                }
                labelMatches(label, expected);
            }
            panel.SyncParameterValue(fixture.id, expected);
            QCOMPARE(changes.size(), 0);
            labelMatches(label, expected);
            QVERIFY(slider->value() >= 0 && slider->value() <= 1000);
        }
        panel.SyncParameterValue(fixture.id, std::numeric_limits<float>::max());
        labelMatches(label, maximum);
        QCOMPARE(slider->value(), minimum == maximum ? 0 : 1000);
        panel.SyncParameterValue(fixture.id, std::numeric_limits<float>::lowest());
        labelMatches(label, minimum);
        QCOMPARE(slider->value(), 0);
        panel.SyncParameterValue(fixture.id, defaultValue);
        QCOMPARE(slider->value(), defaultPosition);
        labelMatches(label, defaultValue);
        const auto text = label->text();
        const int previous = slider->value();
        for (float invalid : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                               -std::numeric_limits<float>::infinity()}) {
            panel.SyncParameterValue(fixture.id, invalid);
            QCOMPARE(slider->value(), previous);
            QCOMPARE(label->text(), text);
        }
        panel.SyncParameterValue("not-a-parameter", 1.0f);
        QCOMPARE(slider->value(), previous);
        QCOMPARE(label->text(), text);
        QCOMPARE(changes.size(), 0);
    }
};

QTEST_MAIN(BlendShapePanelTests)
#include "blend_shape_panel_tests.moc"
