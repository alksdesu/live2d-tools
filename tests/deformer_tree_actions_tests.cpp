#include "ui/cmodeltreepanel.h"
#include "ui/cmodelhierarchymodel.h"
#include "ui/cmodelhierarchyview.h"
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <QItemSelectionModel>
#include <QMenu>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTimer>

namespace {
QByteArray sharedIds()
{
    live2dstruct::CReader reader(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
    live2dstruct::CSectionTable table(reader);
    if (!table.IsValid() || table.parts.isEmpty() || table.art_meshes.isEmpty()) return {};
    int warp = -1, rotation = -1;
    for (int index = 0; index < table.deformers.size(); ++index) {
        if (warp < 0 && table.deformers[index]->types == 0) warp = index;
        if (rotation < 0 && table.deformers[index]->types == 1) rotation = index;
    }
    if (warp < 0 || rotation < 0) return {};
    auto bytes = reader.m_byte;
    const auto& offsets = *table.sectionOffsetTable;
    for (int offset : {offsets.parts->ids, offsets.art_meshes->ids,
                       offsets.deformers->ids + warp * 64, offsets.deformers->ids + rotation * 64})
        bytes.replace(offset, 64, QByteArray("SharedId").leftJustified(64, '\0'));
    return bytes;
}

QSet<QString> selection(CModelHierarchyView* view)
{
    QSet<QString> result;
    for (const auto& index : view->selectionModel()->selectedRows()) result.insert(CModelHierarchyModel::Key(index));
    return result;
}

struct MenuResult
{
    bool opened = false, found = false, enabled = false;
    QStringList actions;
};

MenuResult menuAction(CModelHierarchyView* view, const QModelIndex& index, const QString& text, bool choose)
{
    view->scrollTo(index);
    QCoreApplication::processEvents();
    MenuResult result;
    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, view, [] {
        if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) menu->close();
    });
    watchdog.start(3000);
    QTimer selector;
    selector.setSingleShot(true);
    QObject::connect(&selector, &QTimer::timeout, view, [&] {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) return;
        result.opened = true;
        for (auto* action : menu->actions()) {
            result.actions.append(action->text());
            if (action->text() != text) continue;
            result.found = true;
            result.enabled = action->isEnabled();
            if (choose && result.enabled) {
                menu->setActiveAction(action);
                QTest::keyClick(menu, Qt::Key_Return);
                return;
            }
        }
        menu->close();
    });
    selector.start(0);
    QMetaObject::invokeMethod(view, "customContextMenuRequested", Q_ARG(QPoint, view->visualRect(index).center()));
    return result;
}
}

class DeformerTreeActionsTests : public QObject
{
    Q_OBJECT
private slots:
    void buttonsHonorExplicitAvailability_data()
    {
        QTest::addColumn<bool>("create"); QTest::addColumn<bool>("remove");
        for (bool create : {false, true}) for (bool remove : {false, true})
            QTest::newRow(qPrintable(QString("create%1-remove%2").arg(create).arg(remove))) << create << remove;
    }
    void buttonsHonorExplicitAvailability()
    {
        QFETCH(bool, create); QFETCH(bool, remove);
        CModelTreePanel panel;
        auto* createButton = panel.findChild<QPushButton*>("createDeformerButton");
        auto* removeButton = panel.findChild<QPushButton*>("removeDeformerButton");
        QVERIFY(createButton && removeButton);
        QVERIFY(!createButton->isEnabled() && !removeButton->isEnabled());
        QVERIFY(panel.BuildTreeBytes(sharedIds(), nullptr));
        panel.HighlightObjects({"SharedId"}, {"warp"});
        const auto visibility = panel.VisibilityState(), locked = panel.LockedState();
        QSignalSpy created(&panel, &CModelTreePanel::CreateDeformerRequested);
        QSignalSpy removed(&panel, &CModelTreePanel::RemoveDeformerRequested);
        QSignalSpy selected(&panel, &CModelTreePanel::ObjectsSelected);
        panel.SetDeformerActionsEnabled(create, remove);
        panel.resize(460, 500); panel.show();
        QTest::qWait(1);
        QVERIFY(createButton->visibleRegion().contains(createButton->rect().center()));
        QVERIFY(removeButton->visibleRegion().contains(removeButton->rect().center()));
        QTest::mouseClick(createButton, Qt::LeftButton);
        QTest::mouseClick(removeButton, Qt::LeftButton);
        QCOMPARE(created.size(), int(create)); QCOMPARE(removed.size(), int(remove));
        QCOMPARE(selected.size(), 0); QCOMPARE(panel.VisibilityState(), visibility); QCOMPARE(panel.LockedState(), locked);
        panel.SetDeformerActionsEnabled(false, false);
        QTest::mouseClick(createButton, Qt::LeftButton); QTest::mouseClick(removeButton, Qt::LeftButton);
        QCOMPARE(created.size(), int(create)); QCOMPARE(removed.size(), int(remove));
    }

    void typedMultiHighlightDoesNotEmitUserSelectionAndClearsBothViews()
    {
        CModelTreePanel panel; QVERIFY(panel.BuildTreeBytes(sharedIds(), nullptr));
        QSignalSpy selected(&panel, &CModelTreePanel::ObjectsSelected);
        QSignalSpy single(&panel, &CModelTreePanel::ObjectSelected);
        const QStringList types{"rotation", "part", "artmesh", "warp"};
        panel.HighlightObjects({"SharedId", "SharedId", "SharedId", "SharedId"}, types);
        for (auto* view : panel.findChildren<CModelHierarchyView*>()) {
            auto* model = qobject_cast<CModelHierarchyModel*>(view->model()); QVERIFY(model);
            QSet<QString> expected;
            for (const auto& type : types) if (model->IndexForObject("SharedId", type).isValid()) expected.insert(type + "|SharedId");
            QCOMPARE(selection(view), expected);
            QCOMPARE(CModelHierarchyModel::Key(view->currentIndex()), QString("rotation|SharedId"));
        }
        QCOMPARE(selected.size(), 0); QCOMPARE(single.size(), 0);
        panel.HighlightObjects({"SharedId", "missing"}, {"warp", "rotation"});
        for (auto* view : panel.findChildren<CModelHierarchyView*>())
            QCOMPARE(selection(view), (QSet<QString>{"warp|SharedId"}));
        panel.HighlightObjects({}, {});
        for (auto* view : panel.findChildren<CModelHierarchyView*>()) {
            QVERIFY(selection(view).isEmpty()); QVERIFY(!view->currentIndex().isValid());
        }
        QCOMPARE(selected.size(), 0); QCOMPARE(single.size(), 0);
    }

    void rebuildingPreservesTypedMultiSelectionAndFailureKeepsIt()
    {
        CModelTreePanel panel; const auto bytes = sharedIds(); QVERIFY(panel.BuildTreeBytes(bytes, nullptr));
        panel.HighlightObjects({"SharedId", "SharedId"}, {"rotation", "warp"});
        QSignalSpy selected(&panel, &CModelTreePanel::ObjectsSelected);
        QVERIFY(panel.BuildTreeBytes(bytes, nullptr));
        QString error; QVERIFY(!panel.BuildTreeBytes("bad", nullptr, &error)); QVERIFY(!error.isEmpty());
        for (auto* view : panel.findChildren<CModelHierarchyView*>()) {
            QCOMPARE(selection(view), (QSet<QString>{"rotation|SharedId", "warp|SharedId"}));
            QCOMPARE(CModelHierarchyModel::Key(view->currentIndex()), QString("rotation|SharedId"));
        }
        QCOMPARE(selected.size(), 0);
    }

    void contextMenuMatchesButtonsAndUsesActualSelection_data()
    {
        QTest::addColumn<bool>("deformerTab"); QTest::addColumn<bool>("enabled"); QTest::addColumn<bool>("remove");
        for (bool tab : {false, true}) for (bool enabled : {false, true}) for (bool remove : {false, true})
            QTest::newRow(qPrintable(QString("tab%1-enabled%2-remove%3").arg(tab).arg(enabled).arg(remove))) << tab << enabled << remove;
    }
    void contextMenuMatchesButtonsAndUsesActualSelection()
    {
        QFETCH(bool, deformerTab); QFETCH(bool, enabled); QFETCH(bool, remove);
        CModelTreePanel panel; QVERIFY(panel.BuildTreeBytes(sharedIds(), nullptr));
        panel.SetDeformerActionsEnabled(enabled, enabled);
        auto* tabs = panel.findChild<QTabWidget*>(); QVERIFY(tabs); tabs->setCurrentIndex(deformerTab ? 1 : 0);
        auto* view = panel.findChild<CModelHierarchyView*>(deformerTab ? "deformerHierarchy" : "partHierarchy");
        auto* model = qobject_cast<CModelHierarchyModel*>(view->model()); QVERIFY(model);
        const auto index = model->IndexForObject("SharedId", "rotation"); QVERIFY(index.isValid());
        panel.resize(460, 500); panel.show(); QTest::qWait(1);
        QSignalSpy selected(&panel, &CModelTreePanel::ObjectsSelected);
        QSignalSpy created(&panel, &CModelTreePanel::CreateDeformerRequested);
        QSignalSpy removed(&panel, &CModelTreePanel::RemoveDeformerRequested);
        const auto result = menuAction(view, index, remove ? QStringLiteral("删除变形器") : QStringLiteral("新建变形器"), true);
        QVERIFY(result.opened && result.found); QCOMPARE(result.enabled, enabled);
        QCOMPARE(selected.size(), 1); QCOMPARE(selected.first()[0].toStringList(), (QStringList{"SharedId"}));
        QCOMPARE(selected.first()[1].toStringList(), (QStringList{"rotation"}));
        QCOMPARE(created.size(), int(enabled && !remove)); QCOMPARE(removed.size(), int(enabled && remove));
    }

    void lockedRowsKeepAvailabilityAndMenuUnlockUsesOneBatch()
    {
        CModelTreePanel panel; QVERIFY(panel.BuildTreeBytes(sharedIds(), nullptr));
        panel.HighlightObjects({"SharedId", "SharedId"}, {"warp", "rotation"});
        panel.ApplyLockedState({{"warp|SharedId", true}, {"rotation|SharedId", true}});
        panel.SetDeformerActionsEnabled(true, false);
        panel.resize(460, 500); panel.show(); QTest::qWait(1);
        auto* view = panel.findChild<CModelHierarchyView*>("partHierarchy");
        auto* model = qobject_cast<CModelHierarchyModel*>(view->model()); QVERIFY(model);
        const auto index = model->IndexForObject("SharedId", "warp");
        QSignalSpy removed(&panel, &CModelTreePanel::RemoveDeformerRequested);
        const auto disabled = menuAction(view, index, QStringLiteral("删除变形器"), true);
        QVERIFY(disabled.opened && disabled.found && !disabled.enabled); QCOMPARE(removed.size(), 0);
        QVERIFY(panel.LockedState().value("warp|SharedId") && panel.LockedState().value("rotation|SharedId"));
        QSignalSpy batches(&panel, &CModelTreePanel::LocksBatchChanged);
        QSignalSpy singles(&panel, &CModelTreePanel::ObjectLockChanged);
        const auto unlocked = menuAction(view, index, QStringLiteral("解锁选中对象"), true);
        QVERIFY(unlocked.opened && unlocked.found && unlocked.enabled);
        QCOMPARE(batches.size(), 1); QCOMPARE(singles.size(), 2);
        QCOMPARE((batches.first()[0].value<QMap<QString,bool>>()), (QMap<QString,bool>{{"warp|SharedId", false}, {"rotation|SharedId", false}}));
        QVERIFY(!panel.LockedState().value("warp|SharedId") && !panel.LockedState().value("rotation|SharedId"));
        QCOMPARE(selection(view), (QSet<QString>{"rotation|SharedId", "warp|SharedId"}));
    }
};

QTEST_MAIN(DeformerTreeActionsTests)
#include "deformer_tree_actions_tests.moc"
