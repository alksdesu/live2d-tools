#include "ui/cxyparameterpad.h"
#include <QtTest>
#include <QSignalSpy>
#include <limits>

class XyParameterPadTests : public QObject
{
    Q_OBJECT
private slots:
    void clampsAndEmits()
    {
        CXYParameterPad pad;
        pad.SetRange(-1.0f, 1.0f, -2.0f, 2.0f);
        pad.SetValues(4.0f, -4.0f);
        QCOMPARE(pad.XValue(), 1.0f);
        QCOMPARE(pad.YValue(), -2.0f);
        QSignalSpy changes(&pad, &CXYParameterPad::ValuesChanged);
        pad.SetValues(0.0f, 0.0f);
        QCOMPARE(changes.count(), 1);
        QCOMPARE(changes.at(0).at(0).toFloat(), 0.0f);
        QCOMPARE(changes.at(0).at(1).toFloat(), 0.0f);
    }

    void pointerAndKeyboardControl()
    {
        CXYParameterPad pad;
        pad.SetRange(-1.0f, 1.0f, -1.0f, 1.0f);
        pad.resize(200, 160);
        pad.show();
        QTest::qWait(20);
        QTest::mouseClick(&pad, Qt::LeftButton, Qt::NoModifier, QPoint(100, 78));
        QVERIFY(qAbs(pad.XValue()) < 0.05f);
        QVERIFY(qAbs(pad.YValue()) < 0.05f);
        const float before = pad.XValue();
        QTest::keyClick(&pad, Qt::Key_Right);
        QVERIFY(pad.XValue() > before);
    }

    void extremeRangesStayFiniteAcrossPointerAndKeyboard()
    {
        CXYParameterPad pad;
        const float maximum = std::numeric_limits<float>::max();
        pad.SetRange(-maximum, maximum, -maximum, maximum);
        pad.resize(200, 160);
        pad.show();
        QTest::mouseClick(&pad, Qt::LeftButton, Qt::NoModifier, QPoint(100, 78));
        QCOMPARE(pad.XValue(), 0.0f);
        QCOMPARE(pad.YValue(), 0.0f);
        QTest::keyClick(&pad, Qt::Key_Right);
        QCOMPARE(pad.XValue(), float(double(maximum) / 50.0));
        for (int index = 0; index < 30; ++index) {
            QTest::keyClick(&pad, Qt::Key_Up, Qt::ShiftModifier);
            QTest::keyClick(&pad, Qt::Key_Right, Qt::ShiftModifier);
        }
        QCOMPARE(pad.XValue(), maximum);
        QCOMPARE(pad.YValue(), maximum);
        QTest::mouseClick(&pad, Qt::LeftButton, Qt::NoModifier, QPoint(14, 146));
        QCOMPARE(pad.XValue(), -maximum);
        QCOMPARE(pad.YValue(), -maximum);
        QVERIFY(!pad.grab().isNull());
    }

    void smallAndFixedRangesKeepRepresentableChanges()
    {
        CXYParameterPad pad;
        const float unit = std::numeric_limits<float>::denorm_min();
        pad.SetRange(0, unit * 2, 4, 4);
        QSignalSpy changes(&pad, &CXYParameterPad::ValuesChanged);
        pad.SetValues(unit, 10);
        QCOMPARE(pad.XValue(), unit);
        QCOMPARE(pad.YValue(), 4.0f);
        QCOMPARE(changes.size(), 1);
        pad.SetValues(unit * 2, -10);
        QCOMPARE(pad.XValue(), unit * 2);
        QCOMPARE(changes.size(), 2);
        pad.resize(200, 160);
        pad.show();
        QTest::mouseClick(&pad, Qt::LeftButton, Qt::NoModifier, QPoint(100, 78));
        QCOMPARE(pad.XValue(), unit);
        QCOMPARE(pad.YValue(), 4.0f);
        QTest::keyClick(&pad, Qt::Key_Up);
        QCOMPARE(pad.YValue(), 4.0f);
        QVERIFY(!pad.grab().isNull());
    }

    void invalidValuesAndRangesAreAtomic()
    {
        CXYParameterPad pad;
        pad.SetValues(0.5f, -0.5f);
        QSignalSpy changes(&pad, &CXYParameterPad::ValuesChanged);
        pad.SetValues(std::numeric_limits<float>::quiet_NaN(), 1.0f);
        pad.SetValues(1.0f, std::numeric_limits<float>::infinity());
        pad.SetRange(2, 1, 4, 5);
        pad.SetRange(0, 1, 0, std::numeric_limits<float>::infinity());
        QCOMPARE(pad.XValue(), 0.5f);
        QCOMPARE(pad.YValue(), -0.5f);
        QVERIFY(changes.isEmpty());
        pad.SetValues(-2, 2);
        QCOMPARE(pad.XValue(), -1.0f);
        QCOMPARE(pad.YValue(), 1.0f);
    }
};

QTEST_MAIN(XyParameterPadTests)
#include "xy_parameter_pad_tests.moc"
