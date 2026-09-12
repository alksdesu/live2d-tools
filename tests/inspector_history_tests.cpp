#include "ui/cinspectorpanel.h"
#include "live2dstruct/cmodeldocument.h"
#include <QtTest>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>

class InspectorHistoryTests : public QObject
{
    Q_OBJECT
private slots:
    void editsUndoAndRedo()
    {
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/Mark/Mark.moc3", &error), qPrintable(error));
        const QString id = document.Drawables().first().id;
        const auto& drawable = document.Drawables().first();
        const bool initialVisibility = drawable.visible;
        const float initialOpacity = document.DrawableOpacity(drawable);
        const int initialDrawOrder = qRound(document.DrawableDrawOrder(drawable));

        CInspectorPanel panel;
        panel.SetDocument(&document);
        panel.SetSelection(id, QStringLiteral("artmesh"));
        auto* visibility = panel.findChild<QCheckBox*>();
        auto* opacity = panel.findChild<QDoubleSpinBox*>();
        auto* drawOrder = panel.findChild<QSpinBox*>();
        QVERIFY(visibility && opacity && drawOrder);

        visibility->setChecked(!initialVisibility);
        QCOMPARE(document.Drawables().first().visible, !initialVisibility);
        panel.Undo();
        QCOMPARE(document.Drawables().first().visible, initialVisibility);
        panel.Redo();
        QCOMPARE(document.Drawables().first().visible, !initialVisibility);

        opacity->setValue(0.25);
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), 0.25f);
        panel.Undo();
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), initialOpacity);
        panel.Redo();
        QCOMPARE(document.DrawableOpacity(document.Drawables().first()), 0.25f);

        drawOrder->setValue(initialDrawOrder == 1000 ? 999 : initialDrawOrder + 1);
        const int changedDrawOrder = drawOrder->value();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), changedDrawOrder);
        panel.Undo();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), initialDrawOrder);
        panel.Redo();
        QCOMPARE(qRound(document.DrawableDrawOrder(document.Drawables().first())), changedDrawOrder);

        panel.SetSelections({id}, {QStringLiteral("artmesh")});
        panel.SetSelections({}, {});
        QVERIFY(!visibility->isEnabled());
    }
};

QTEST_MAIN(InspectorHistoryTests)
#include "inspector_history_tests.moc"
