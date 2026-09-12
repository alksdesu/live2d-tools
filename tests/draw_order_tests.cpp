#include "live2dstruct/cdraworder.h"
#include <QtTest>

using namespace live2dstruct;

class DrawOrderTests : public QObject
{
    Q_OBJECT
    static CModelDrawable drawable(const QString& id, int parent)
    {
        CModelDrawable value;
        value.id = id;
        value.parentPartIndex = parent;
        return value;
    }

    static QStringList describe(const QVector<CLayerOrderItem>& items)
    {
        QStringList result;
        for (const auto& item : items) {
            const QString prefix = item.type == CLayerOrderItem::GroupStart ? "+"
                : item.type == CLayerOrderItem::GroupEnd ? "-" : "";
            result.append(prefix + item.id);
        }
        return result;
    }

private slots:
    void editsUseDrawValuesInsteadOfRanks()
    {
        const QVector<CDrawableOrder> entries = {{"edited", 400, 0}, {"middle", 450, 1}, {"front", 550, 2}};
        QCOMPARE(ResolveDrawableOrder(entries, {{"edited", 500}}), QVector<int>({1, 0, 2}));
    }

    void nativeOrderIsPreservedWithoutEdits()
    {
        const QVector<CDrawableOrder> entries = {{"local_high", 900, 0}, {"local_low", 100, 1}, {"global", 500, 2}};
        QCOMPARE(ResolveDrawableOrder(entries), QVector<int>({0, 1, 2}));
        QCOMPARE(ResolveDrawableOrder(entries, {{"missing", 999}}), QVector<int>({0, 1, 2}));
        QCOMPARE(ResolveDrawableOrder(entries, {{"local_high", 900}}), QVector<int>({0, 1, 2}));
    }

    void tiesFollowNativeOrder()
    {
        const QVector<CDrawableOrder> entries = {{"front", 500, 2}, {"back", 500, 0}, {"edited", 400, 1}};
        QCOMPARE(ResolveDrawableOrder(entries, {{"edited", 500}}), QVector<int>({1, 2, 0}));
    }

    void multipleEditsAndBoundaryValues()
    {
        const QVector<CDrawableOrder> entries = {{"a", 400, 0}, {"b", 500, 1}, {"c", 600, 2}};
        QCOMPARE(ResolveDrawableOrder(entries, {{"a", 1000}, {"c", 0}}), QVector<int>({2, 1, 0}));
        QCOMPARE(ResolveDrawableOrder(entries, {{"a", 2000}, {"c", -100}}), QVector<int>({2, 1, 0}));
    }

    void rankGapsCannotDropOrDuplicateMeshes()
    {
        const QVector<CDrawableOrder> entries = {{"a", 400, 80}, {"b", 400, -1}, {"c", 400, 80}};
        QCOMPARE(ResolveDrawableOrder(entries), QVector<int>({1, 0, 2}));
        QVERIFY(ResolveDrawableOrder({}).isEmpty());
    }

    void modelAnimationUpdatesUneditedValues()
    {
        QVector<CDrawableOrder> entries = {{"edited", 400, 0}, {"moving", 450, 1}, {"front", 550, 2}};
        const QHash<QString, int> overrides = {{"edited", 500}};
        QCOMPARE(ResolveDrawableOrder(entries, overrides), QVector<int>({1, 0, 2}));
        entries[1].drawOrder = 525;
        QCOMPARE(ResolveDrawableOrder(entries, overrides), QVector<int>({0, 1, 2}));
    }

    void groupedEditsStayInsideLocalGroup()
    {
        const QVector<CDrawableOrder> entries = {
            {"root", 400, 0}, {"group_front", 700, 2}, {"group_back", 700, 1}
        };
        const QHash<QString, int> groupIndices = {{"group_front", 0}, {"group_back", 0}};
        const QVector<CDrawableOrderGroup> groups = {{700, 700}};
        QCOMPARE(ResolveDrawableOrder(entries, {{"root", 800}, {"group_front", 500}, {"group_back", 900}},
                                       groupIndices, groups), QVector<int>({1, 2, 0}));
        QCOMPARE(ResolveDrawableOrder(entries, {}, groupIndices, groups), QVector<int>({0, 2, 1}));
    }

    void nestedGroupsPreserveHierarchyAndLayerOrder()
    {
        const QVector<CModelPart> parts = {{"body", -1}, {"face", 0}, {"empty", -1}};
        const QVector<CModelDrawable> entries = {
            drawable("background", -1), drawable("torso", 0), drawable("eyes", 1), drawable("mouth", 1)
        };
        QCOMPARE(describe(BuildGroupedLayerOrder(parts, entries, {0, 1, 3, 2})),
                 QStringList({"background", "+body", "torso", "+face", "mouth", "eyes", "-face", "-body"}));
    }

    void interleavedPartsSplitWithoutChangingStacking()
    {
        const QVector<CModelPart> parts = {{"body", -1}, {"arm", 0}, {"prop", -1}};
        const QVector<CModelDrawable> entries = {
            drawable("back_arm", 1), drawable("object", 2), drawable("front_arm", 1), drawable("torso", 0)
        };
        QCOMPARE(describe(BuildGroupedLayerOrder(parts, entries, {0, 1, 2, 3})),
                 QStringList({"+body", "+arm", "back_arm", "-arm", "-body", "+prop", "object", "-prop",
                              "+body", "+arm", "front_arm", "-arm", "torso", "-body"}));
    }

    void filteredIndicesKeepNamesAlignedAndOmitEmptyGroups()
    {
        const QVector<CModelPart> parts = {{"included", -1}, {"omitted", -1}};
        const QVector<CModelDrawable> entries = {
            drawable("hidden", 1), drawable("visible", 0), drawable("outside", 99)
        };
        const auto items = BuildGroupedLayerOrder(parts, entries, {-1, 1, 1, 90, 2});
        QCOMPARE(describe(items), QStringList({"+included", "visible", "-included", "outside"}));
        QCOMPARE(items[1].drawableIndex, 1);
        QCOMPARE(items[3].drawableIndex, 2);
        QVERIFY(BuildGroupedLayerOrder(parts, entries, {}).isEmpty());
    }

    void cyclicAndInvalidParentsRemainFinite()
    {
        const QVector<CModelPart> parts = {
            {"a", 1}, {"b", 0}, {"child", 0}, {"self", 3}, {"invalid", 100}, {"negative", -4}
        };
        const QVector<CModelDrawable> entries = {
            drawable("in_child", 2), drawable("in_b", 1), drawable("in_self", 3),
            drawable("in_invalid", 4), drawable("in_negative", 5)
        };
        QCOMPARE(describe(BuildGroupedLayerOrder(parts, entries, {0, 1, 2, 3, 4})),
                 QStringList({"+a", "+child", "in_child", "-child", "-a", "+b", "in_b", "-b",
                              "+self", "in_self", "-self", "+invalid", "in_invalid", "-invalid",
                              "+negative", "in_negative", "-negative"}));
    }

    void deepHierarchyDoesNotRequireCallStackRecursion()
    {
        const int depth = 10000;
        QVector<CModelPart> parts;
        for (int i = 0; i < depth; ++i) parts.append({QString::number(i), i - 1});
        const auto items = BuildGroupedLayerOrder(parts, {drawable("leaf", depth - 1)}, {0});
        QCOMPARE(items.size(), depth * 2 + 1);
        QCOMPARE(items[depth].id, QString("leaf"));
        for (int i = 0; i < depth; ++i) {
            QCOMPARE(items[i].type, CLayerOrderItem::GroupStart);
            QCOMPARE(items[i].id, QString::number(i));
            QCOMPARE(items[items.size() - i - 1].type, CLayerOrderItem::GroupEnd);
            QCOMPARE(items[items.size() - i - 1].id, QString::number(i));
        }
    }

    void sampleGroups_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto* name : {"Mark", "Natori", "Rice", "Wanko"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void sampleGroups()
    {
        QFETCH(QString, name);
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(QString(TEST_RESOURCES) + "/" + name + "/" + name + ".moc3", &error), qPrintable(error));
        QVector<int> order;
        for (int i = document.Drawables().size() - 1; i >= 0; --i) order.append(i);
        const auto items = BuildGroupedLayerOrder(document.Parts(), document.Drawables(), order);
        QVector<int> actualOrder;
        QStringList openGroups;
        for (const auto& item : items) {
            if (item.type == CLayerOrderItem::GroupStart) openGroups.append(item.id);
            else if (item.type == CLayerOrderItem::GroupEnd) {
                QVERIFY(!openGroups.isEmpty());
                QCOMPARE(openGroups.takeLast(), item.id);
            } else {
                QVERIFY(item.drawableIndex >= 0 && item.drawableIndex < document.Drawables().size());
                const auto& mesh = document.Drawables()[item.drawableIndex];
                QCOMPARE(item.id, mesh.id);
                if (mesh.parentPartIndex < 0) QVERIFY(openGroups.isEmpty());
                else {
                    QVERIFY(!openGroups.isEmpty());
                    QCOMPARE(openGroups.last(), document.Parts()[mesh.parentPartIndex].id);
                }
                actualOrder.append(item.drawableIndex);
            }
        }
        QCOMPARE(actualOrder, order);
        QVERIFY(openGroups.isEmpty());
    }
};

QTEST_GUILESS_MAIN(DrawOrderTests)
#include "draw_order_tests.moc"
