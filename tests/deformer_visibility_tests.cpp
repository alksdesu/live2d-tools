#include "live2dstruct/cmodeldocument.h"

#include <QFile>
#include <QtEndian>
#include <QtTest>
#include <cstring>

using namespace live2dstruct;

namespace {
QByteArray ModelBytes(const QString& name)
{
    QFile file(QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

int Word(const QByteArray& bytes, int offset)
{
    return qFromLittleEndian<qint32>(bytes.constData() + offset);
}

void Put(QByteArray& bytes, int offset, int value)
{
    qToLittleEndian(qint32(value), bytes.data() + offset);
}

QString NodeId(int index) { return QStringLiteral("Deformer%1").arg(index); }
QString MeshId(int index) { return QStringLiteral("Mesh%1").arg(index); }

int AddColumn(QByteArray& bytes, int slot, int count, int width = 4)
{
    const int oldSize = bytes.size();
    const int begin = (oldSize + 63) & ~63;
    bytes.resize(begin + count * width);
    std::memset(bytes.data() + oldSize, 0, bytes.size() - oldSize);
    Put(bytes, slot, begin);
    return begin;
}

QByteArray Hierarchy(const QVector<int>& parents, const QVector<int>& meshParents)
{
    QByteArray bytes = ModelBytes("Mark");
    if (bytes.isEmpty()) return {};
    const int counts = Word(bytes, 0x40);
    for (int index = 0; index < 32; ++index) Put(bytes, counts + index * 4, 0);
    Put(bytes, counts + 4, parents.size());
    Put(bytes, counts + 16, meshParents.size());
    for (int slot = 0x68; slot <= 0x88; slot += 4) {
        const int width = slot == 0x6c ? 64 : 4;
        const int begin = AddColumn(bytes, slot, parents.size(), width);
        for (int index = 0; index < parents.size(); ++index) {
            if (slot == 0x6c) {
                const auto id = NodeId(index).toUtf8();
                std::memcpy(bytes.data() + begin + index * 64, id.constData(), id.size());
            } else {
                int value = 0;
                if (slot == 0x70 || slot == 0x7c || slot == 0x88) value = -1;
                if (slot == 0x74 || slot == 0x78) value = 1;
                if (slot == 0x80) value = parents[index];
                Put(bytes, begin + index * 4, value);
            }
        }
    }
    for (int slot = 0xb4; slot <= 0x100; slot += 4) {
        const int width = slot == 0xc4 ? 64 : slot == 0xe8 ? 1 : 4;
        const int begin = AddColumn(bytes, slot, meshParents.size(), width);
        for (int index = 0; index < meshParents.size(); ++index) {
            if (slot == 0xc4) {
                const auto id = MeshId(index).toUtf8();
                std::memcpy(bytes.data() + begin + index * 64, id.constData(), id.size());
            } else if (width == 4) {
                int value = 0;
                if (slot == 0xc8 || slot == 0xdc) value = -1;
                if (slot == 0xd4 || slot == 0xd8) value = 1;
                if (slot == 0xe0) value = meshParents[index];
                Put(bytes, begin + index * 4, value);
            }
        }
    }
    return bytes;
}

QSet<QString> Expected(const CModelDocument& document)
{
    QSet<QString> result;
    const auto& deformers = document.Deformers();
    for (const auto& mesh : document.Drawables()) {
        QSet<int> visited;
        int parent = mesh.parentDeformerIndex;
        while (parent >= 0 && parent < deformers.size() && !visited.contains(parent)) {
            visited.insert(parent);
            if (!deformers[parent].visible) {
                result.insert(mesh.id);
                break;
            }
            parent = document.DeformerParentIndex(deformers[parent].id);
        }
    }
    return result;
}

int IndexOf(const CModelDocument& document, const QString& id)
{
    for (int index = 0; index < document.Deformers().size(); ++index)
        if (document.Deformers()[index].id == id) return index;
    return -1;
}
}

class DeformerVisibilityTests : public QObject
{
    Q_OBJECT
private slots:
    void realHierarchyKeepsAncestorVisibility_data()
    {
        QTest::addColumn<QString>("model");
        QTest::addColumn<QString>("parent");
        QTest::addColumn<QString>("child");
        QTest::addColumn<QString>("mesh");
        QTest::newRow("Mark") << QString("Mark") << QString("Rotation") << QString("Warp6") << QString("Mouth1");
        QTest::newRow("Wanko") << QString("Wanko") << QString("B_FACE_03") << QString("B_FACE_01") << QString("D_FACE_01");
    }

    void realHierarchyKeepsAncestorVisibility()
    {
        QFETCH(QString, model);
        QFETCH(QString, parent);
        QFETCH(QString, child);
        QFETCH(QString, mesh);
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(ModelBytes(model), model + ".moc3", &error), qPrintable(error));
        QVERIFY(IndexOf(document, parent) >= 0);
        QCOMPARE(document.DeformerParentIndex(child), IndexOf(document, parent));
        for (const auto& deformer : document.Deformers()) QVERIFY(document.SetDeformerVisible(deformer.id, true));
        QVERIFY(document.HiddenDeformerDrawables().isEmpty());

        QVERIFY(document.SetDeformerVisible(child, false));
        const auto childHidden = document.HiddenDeformerDrawables();
        QVERIFY(childHidden.contains(mesh));
        QCOMPARE(childHidden, Expected(document));
        QVERIFY(document.SetDeformerVisible(parent, false));
        const auto bothHidden = document.HiddenDeformerDrawables();
        QVERIFY(bothHidden.contains(mesh));
        for (const auto& id : childHidden) QVERIFY(bothHidden.contains(id));
        QCOMPARE(bothHidden, Expected(document));

        QVERIFY(document.SetDeformerVisible(child, true));
        QCOMPARE(document.HiddenDeformerDrawables(), bothHidden);
        QVERIFY(document.SetDeformerVisible(parent, true));
        QVERIFY(document.HiddenDeformerDrawables().isEmpty());
        QVERIFY(document.SetDeformerVisible(child, false));
        QCOMPARE(document.HiddenDeformerDrawables(), childHidden);
        QVERIFY(document.SetDeformerVisible(child, true));
        QVERIFY(document.SetDrawableVisible(mesh, false));
        QVERIFY(document.HiddenDeformerDrawables().isEmpty());
    }

    void effectiveParentOverridesMoveHiddenBranches()
    {
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(Hierarchy({-1, -1, 0, 2}, {0, 1, 2, 3, -1}), "parents.moc3", &error), qPrintable(error));
        QVERIFY(document.SetDeformerVisible(NodeId(0), false));
        const QSet<QString> original{MeshId(0), MeshId(2), MeshId(3)};
        QCOMPARE(document.HiddenDeformerDrawables(), original);
        QVERIFY(document.SetDeformerParentIndex(NodeId(2), 1));
        QCOMPARE(document.HiddenDeformerDrawables(), QSet<QString>{MeshId(0)});
        QCOMPARE(document.HiddenDeformerDrawables(), Expected(document));
        QVERIFY(document.SetDeformerVisible(NodeId(1), false));
        const QSet<QString> allBranches{MeshId(0), MeshId(1), MeshId(2), MeshId(3)};
        QCOMPARE(document.HiddenDeformerDrawables(), allBranches);
        QVERIFY(document.SetDeformerParentIndex(NodeId(2), -1));
        const QSet<QString> roots{MeshId(0), MeshId(1)};
        QCOMPARE(document.HiddenDeformerDrawables(), roots);
        QVERIFY(document.SetDeformerParentIndices({{NodeId(2), 0}, {NodeId(3), 1}}));
        QCOMPARE(document.HiddenDeformerDrawables(), allBranches);
        QVERIFY(!document.SetDeformerParentIndex(NodeId(0), 2));
        QCOMPARE(document.HiddenDeformerDrawables(), allBranches);
        QVERIFY(document.SetDeformerVisible(NodeId(1), true));
        const QSet<QString> firstBranch{MeshId(0), MeshId(2)};
        QCOMPARE(document.HiddenDeformerDrawables(), firstBranch);
    }

    void realReparentingUsesTheEffectiveHierarchy()
    {
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(ModelBytes("Wanko"), "Wanko.moc3", &error), qPrintable(error));
        for (const auto& deformer : document.Deformers()) QVERIFY(document.SetDeformerVisible(deformer.id, true));
        const int face = IndexOf(document, "B_FACE_01");
        QVERIFY(face >= 0);
        QVERIFY(document.SetDeformerVisible("B_FACE_01", false));
        QVERIFY(!document.HiddenDeformerDrawables().contains("D_FACE_00"));
        QVERIFY(document.SetDeformerParentIndex("B_FACE_02", face));
        QVERIFY(document.HiddenDeformerDrawables().contains("D_FACE_00"));
        QVERIFY(document.HiddenDeformerDrawables().contains("D_FACE_01"));
        QCOMPARE(document.HiddenDeformerDrawables(), Expected(document));
        QVERIFY(document.SetDeformerParentIndex("B_FACE_02", -1));
        QVERIFY(!document.HiddenDeformerDrawables().contains("D_FACE_00"));
    }

    void invalidParentsAndCyclesRetainKnownHiddenAncestors()
    {
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(Hierarchy({1, 2, 0, 2, -1, 99, -7, 6}, {0, 1, 2, 3, 4, 5, 6, 7, -1, 999}),
                                   "cycles.moc3", &error), qPrintable(error));
        QVERIFY(document.HiddenDeformerDrawables().isEmpty());
        QVERIFY(document.SetDeformerVisible(NodeId(3), false));
        QCOMPARE(document.HiddenDeformerDrawables(), QSet<QString>{MeshId(3)});
        QVERIFY(document.SetDeformerVisible(NodeId(0), false));
        const QSet<QString> cycle{MeshId(0), MeshId(1), MeshId(2), MeshId(3)};
        QCOMPARE(document.HiddenDeformerDrawables(), cycle);
        QVERIFY(document.SetDeformerVisible(NodeId(5), false));
        QVERIFY(document.SetDeformerVisible(NodeId(6), false));
        const QSet<QString> expected{MeshId(0), MeshId(1), MeshId(2), MeshId(3), MeshId(5), MeshId(6), MeshId(7)};
        QCOMPARE(document.HiddenDeformerDrawables(), expected);
        QCOMPARE(document.HiddenDeformerDrawables(), Expected(document));
        bool cycleDiagnostic = false, parentDiagnostic = false;
        for (const auto& diagnostic : document.Validate()) {
            cycleDiagnostic = cycleDiagnostic || diagnostic.code == "deformer_cycle";
            parentDiagnostic = parentDiagnostic || diagnostic.code == "deformer_parent";
        }
        QVERIFY(cycleDiagnostic);
        QVERIFY(parentDiagnostic);
        QVERIFY(document.SetDeformerVisible(NodeId(0), true));
        QVERIFY(!document.HiddenDeformerDrawables().contains(MeshId(1)));
        QVERIFY(document.HiddenDeformerDrawables().contains(MeshId(3)));
    }

    void selfCycleIsBounded()
    {
        CModelDocument document;
        QVERIFY(document.LoadBytes(Hierarchy({0, 0}, {0, 1}), "self-cycle.moc3"));
        QVERIFY(document.HiddenDeformerDrawables().isEmpty());
        QVERIFY(document.SetDeformerVisible(NodeId(0), false));
        const QSet<QString> both{MeshId(0), MeshId(1)};
        QCOMPARE(document.HiddenDeformerDrawables(), both);
        QVERIFY(document.SetDeformerVisible(NodeId(0), true));
        QVERIFY(document.HiddenDeformerDrawables().isEmpty());
    }

    void deepSharedAncestorsUseBoundedIteration()
    {
        constexpr int depth = 32768;
        QVector<int> parents(depth), meshParents(depth);
        for (int index = 0; index < depth; ++index) {
            parents[index] = index + 1 == depth ? -1 : index + 1;
            meshParents[index] = index;
        }
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(Hierarchy(parents, meshParents), "deep.moc3", &error), qPrintable(error));
        QVERIFY(document.HiddenDeformerDrawables().isEmpty());
        QVERIFY(document.SetDeformerVisible(NodeId(depth - 1), false));
        for (int repeat = 0; repeat < 3; ++repeat) {
            const auto hidden = document.HiddenDeformerDrawables();
            QCOMPARE(hidden.size(), depth);
            QVERIFY(hidden.contains(MeshId(0)));
            QVERIFY(hidden.contains(MeshId(depth - 1)));
        }
        QVERIFY(document.SetDeformerVisible(NodeId(depth / 2), false));
        QVERIFY(document.SetDeformerVisible(NodeId(depth - 1), true));
        const auto hidden = document.HiddenDeformerDrawables();
        QCOMPARE(hidden.size(), depth / 2 + 1);
        QVERIFY(hidden.contains(MeshId(0)));
        QVERIFY(hidden.contains(MeshId(depth / 2)));
        QVERIFY(!hidden.contains(MeshId(depth / 2 + 1)));
        document.Clear();
        QVERIFY(document.HiddenDeformerDrawables().isEmpty());
    }
};

QTEST_GUILESS_MAIN(DeformerVisibilityTests)
#include "deformer_visibility_tests.moc"
