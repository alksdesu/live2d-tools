#include "ui/cxyparameterpad.h"
#include <QtTest>
#include <QSignalSpy>

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
};

QTEST_MAIN(XyParameterPadTests)
#include "xy_parameter_pad_tests.moc"
