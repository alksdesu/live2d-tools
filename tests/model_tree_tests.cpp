#include "ui/cmodeltreepanel.h"
#include "live2dstruct/cmodeldocument.h"
#include <QtTest>
#include <QSignalSpy>

class ModelTreeTests : public QObject
{
    Q_OBJECT
private slots:
    void mirrorsInitialVisibilityAndClearsSelection()
    {
        const QString mocPath = QString(TEST_RESOURCES) + "/Mark/Mark.moc3";
        live2dstruct::CModelDocument document;
        QString error;
        QVERIFY2(document.Load(mocPath, &error), qPrintable(error));

        CModelTreePanel panel;
        QSignalSpy selections(&panel, &CModelTreePanel::ObjectsSelected);
        panel.BuildTree(mocPath, nullptr);

        const auto expected = [&document](const QString& type, const QString& id, bool* found) {
            *found = true;
            if (type == QLatin1String("part")) {
                for (const auto& object : document.Parts()) if (object.id == id) return object.visible;
            } else if (type == QLatin1String("warp") || type == QLatin1String("rotation")) {
                for (const auto& object : document.Deformers()) if (object.id == id) return object.visible;
            } else if (type == QLatin1String("artmesh")) {
                for (const auto& object : document.Drawables()) if (object.id == id) return object.visible;
            }
            *found = false;
            return false;
        };

        const auto trees = panel.findChildren<QTreeWidget*>();
        QCOMPARE(trees.size(), 2);
        int itemCount = 0;
        for (QTreeWidget* tree : trees) {
            const auto visit = [&](QTreeWidgetItem* item, auto&& visitRef) -> void {
                const QString type = item->data(0, Qt::UserRole + 1).toString();
                const QString id = item->data(0, Qt::UserRole).toString();
                bool found = false;
                const bool visible = expected(type, id, &found);
                QVERIFY(found);
                QCOMPARE(item->checkState(0), visible ? Qt::Checked : Qt::Unchecked);
                ++itemCount;
                for (int i = 0; i < item->childCount(); ++i) visitRef(item->child(i), visitRef);
            };
            for (int i = 0; i < tree->topLevelItemCount(); ++i) visit(tree->topLevelItem(i), visit);
        }
        QVERIFY(itemCount > 0);

        QTreeWidget* partTree = trees.first();
        QVERIFY(partTree->topLevelItemCount() > 0);
        partTree->topLevelItem(0)->setSelected(true);
        QVERIFY(!selections.isEmpty());
        partTree->clearSelection();
        QVERIFY(!selections.isEmpty());
        const QList<QVariant> last = selections.last();
        QVERIFY(last.size() == 2);
        QVERIFY(last.at(0).toStringList().isEmpty());
        QVERIFY(last.at(1).toStringList().isEmpty());

        QSignalSpy visibilityChanges(&panel, &CModelTreePanel::VisibilityChanged);
        QTreeWidgetItem* deformer = nullptr;
        const auto findDeformer = [&](QTreeWidgetItem* item, auto&& findRef) -> void {
            const QString type = item->data(0, Qt::UserRole + 1).toString();
            if (!deformer && (type == QLatin1String("warp") || type == QLatin1String("rotation"))) {
                deformer = item;
                return;
            }
            for (int i = 0; i < item->childCount(); ++i) findRef(item->child(i), findRef);
        };
        for (QTreeWidget* tree : trees) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) findDeformer(tree->topLevelItem(i), findDeformer);
            if (deformer) break;
        }
        QVERIFY(deformer);
        deformer->setCheckState(0, Qt::Unchecked);
        bool sawDeformerEvent = false;
        for (const auto& event : visibilityChanges) {
            if (event.size() >= 2 && (event.at(1).toString() == QLatin1String("warp") ||
                                      event.at(1).toString() == QLatin1String("rotation"))) {
                sawDeformerEvent = true;
                break;
            }
        }
        QVERIFY(sawDeformerEvent);
    }
};

QTEST_MAIN(ModelTreeTests)
#include "model_tree_tests.moc"
