#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/util/cfilereader.h"

#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
#include <limits>

using namespace live2dstruct;

namespace {
QByteArray read(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

QString sample(const QString& name)
{
    return QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3";
}
}

class ModelSnapshotTests : public QObject
{
    Q_OBJECT
private slots:
    void fileAndMemoryLoadsKeepTheSameSource_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void fileAndMemoryLoadsKeepTheSameSource()
    {
        QFETCH(QString, name);
        const QString path = sample(name);
        const QByteArray bytes = read(path);
        QVERIFY(!bytes.isEmpty());
        CModelDocument file, memory;
        QString error = "old error";
        QVERIFY2(file.Load(path, &error), qPrintable(error));
        QVERIFY(error.isEmpty());
        QVERIFY2(memory.LoadBytes(bytes, path, &error), qPrintable(error));
        QCOMPARE(file.SourcePath(), path);
        QCOMPARE(file.SourceBytes(), bytes);
        QCOMPARE(memory.SourceBytes(), file.SourceBytes());
        QCOMPARE(memory.SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        QCOMPARE(memory.SourceDigest(), CModelSession::ComputeSourceDigest(path));
        QCOMPARE(memory.Drawables().size(), file.Drawables().size());
        QCOMPARE(memory.Deformers().size(), file.Deformers().size());
        for (int index = 0; index < file.Drawables().size(); ++index) {
            const auto& original = file.Drawables()[index];
            const auto& restored = memory.Drawables()[index];
            QCOMPARE(restored.id, original.id);
            QCOMPARE(restored.keyforms.size(), original.keyforms.size());
            for (int keyform = 0; keyform < original.keyforms.size(); ++keyform)
                QCOMPARE(restored.keyforms[keyform].positions, original.keyforms[keyform].positions);
        }
        QVERIFY2(file.SourceMatchesDisk(&error), qPrintable(error));
        QVERIFY2(memory.SourceMatchesDisk(&error), qPrintable(error));
        QByteArray preview;
        QVERIFY2(memory.BuildPreviewMoc(preview, &error), qPrintable(error));
        QCOMPARE(preview, bytes);
    }

    void borrowedAndMutableInputCannotChangeLoadedSnapshots()
    {
        QByteArray owner = read(sample("Mark"));
        const QByteArray original(owner.constData(), owner.size());
        char* external = owner.data();
        const QByteArray borrowed = QByteArray::fromRawData(external, owner.size());
        auto reader = CReader::FromBytes(borrowed);
        CModelDocument document;
        QVERIFY(document.LoadBytes(borrowed, QStringLiteral("memory")));
        external[0] = 'X';
        QCOMPARE(reader.m_byte, original);
        QCOMPARE(document.SourceBytes(), original);
        QVERIFY(reader.IsMoc3());
        QCOMPARE(reader.readInt(), int(0x33434f4d));
        auto mutableReader = CReader::FromBytes(owner);
        external[1] = 'Y';
        QCOMPARE(mutableReader.m_byte[1], char('O'));
        const char byte = 0;
        const auto oversized = QByteArray::fromRawData(&byte, qsizetype(std::numeric_limits<int>::max()) + 1);
        const auto rejected = CReader::FromBytes(oversized);
        QVERIFY(!rejected.IsValid());
        QVERIFY(rejected.m_byte.isEmpty());
    }

    void diskReplacementDoesNotRetargetEditsAndUndoReturnsOriginalBytes()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QByteArray original = read(sample("Mark"));
        const QString path = directory.filePath("model.moc3");
        QVERIFY(write(path, original));
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        const auto& mesh = document.Drawables().first();
        const QString id = mesh.id;
        const QVector<float> before = document.DrawableKeyformPositions(id, 0);
        QVERIFY(!before.isEmpty());
        QVector<float> changed = before;
        changed[0] += 0.125f;
        QVERIFY(document.ExecuteEdit({id, "artmesh", CModelEdit::KeyformPositions,
                                     QVariant::fromValue(before), QVariant::fromValue(changed), 0, false}));
        QVERIFY(write(path, read(sample("Wanko"))));
        QVERIFY(!document.SourceMatchesDisk(&error));
        QVERIFY(error.contains(QStringLiteral("外部更改")));
        QCOMPARE(document.SourceBytes(), original);
        const QByteArray digest = document.SourceDigest();
        QByteArray preview;
        QVERIFY2(document.BuildPreviewMoc(preview, &error), qPrintable(error));
        CModelDocument restored;
        QVERIFY2(restored.LoadBytes(preview, "preview", &error), qPrintable(error));
        QCOMPARE(restored.DrawableKeyformPositions(id, 0), changed);
        QCOMPARE(document.SourceDigest(), digest);
        QVERIFY(document.UndoEdit());
        QVERIFY2(document.BuildPreviewMoc(preview, &error), qPrintable(error));
        QCOMPARE(preview, original);
        QVERIFY(document.RedoEdit());
        QVERIFY(QFile::remove(path));
        QVERIFY(!document.SourceMatchesDisk(&error));
        QVERIFY2(document.BuildPreviewMoc(preview, &error), qPrintable(error));
        QVERIFY2(restored.LoadBytes(preview, "preview", &error), qPrintable(error));
        QCOMPARE(restored.DrawableKeyformPositions(id, 0), changed);
    }

    void failedLoadsKeepBaselinePoseBindingsAndHistory()
    {
        CModelDocument document;
        QVERIFY(document.Load(sample("Wanko")));
        document.SetLocked("artmesh", "HitArea", true);
        const float originalValue = document.ParameterValue("PARAM_MOUTH_OPEN_Y");
        QVERIFY(document.ExecuteEdit({"PARAM_MOUTH_OPEN_Y", "parameter", CModelEdit::ParameterValue,
                                     originalValue, 0.25f}));
        const QByteArray original = document.SourceBytes(), digest = document.SourceDigest();
        QMap<QString, float> pose;
        QString error;
        QVERIFY2(document.KeyformParameterValues("D_FACE_01", "artmesh", 1, pose, &error), qPrintable(error));
        const auto expected = pose;
        QVERIFY(!document.LoadBytes(QByteArrayLiteral("MOC3"), "other", &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(document.SourceBytes(), original);
        QCOMPARE(document.SourceDigest(), digest);
        QCOMPARE(document.SourcePath(), sample("Wanko"));
        QVERIFY(document.IsLocked("artmesh", "HitArea"));
        QVERIFY2(document.KeyformParameterValues("D_FACE_01", "artmesh", 1, pose, &error), qPrintable(error));
        QCOMPARE(pose, expected);
        QVERIFY(!document.Load(QStringLiteral("/nonexistent/snapshot.moc3"), &error));
        QCOMPARE(document.SourceBytes(), original);
        QCOMPARE(document.ParameterValue("PARAM_MOUTH_OPEN_Y"), 0.25f);
        QVERIFY(document.UndoEdit());
        QCOMPARE(document.ParameterValue("PARAM_MOUTH_OPEN_Y"), originalValue);
        document.Clear();
        QVERIFY(document.SourceBytes().isEmpty());
        QVERIFY(document.SourceDigest().isEmpty());
        QByteArray output("unchanged");
        QVERIFY(!document.BuildPreviewMoc(output, &error));
        QCOMPARE(output, QByteArray("unchanged"));
        QVERIFY(!document.SourceMatchesDisk(&error));
    }

    void documentCopiesKeepIndependentEdits()
    {
        CModelDocument original;
        QVERIFY(original.Load(sample("Mark")));
        CModelDocument copy = original;
        const QString id = original.Drawables().first().id;
        auto positions = copy.DrawableKeyformPositions(id, 0);
        positions[0] += 0.25f;
        QVERIFY(copy.SetDrawableKeyformPositions(id, 0, positions));
        QByteArray a, b;
        QVERIFY(original.BuildPreviewMoc(a));
        QVERIFY(copy.BuildPreviewMoc(b));
        QCOMPARE(a, original.SourceBytes());
        QVERIFY(a != b);
        original.Clear();
        QVERIFY(copy.IsLoaded());
        QCOMPARE(copy.DrawableKeyformPositions(id, 0), positions);
        QVERIFY(copy.BuildPreviewMoc(b));
    }

    void selectedKeyformUsesEffectiveParameterRanges()
    {
        CModelDocument document;
        QVERIFY(document.Load(sample("Wanko")));
        QMap<QString, float> values;
        QString error;
        QVERIFY2(document.KeyformParameterValues("D_FACE_01", "artmesh", 1, values, &error), qPrintable(error));
        QCOMPARE(values.value("PARAM_MOUTH_OPEN_Y"), 1.0f);
        auto metadata = document.ParameterMetadata("PARAM_MOUTH_OPEN_Y");
        metadata.maximum = 0.5f;
        metadata.defaultValue = 0.5f;
        QVERIFY(document.SetParameterMetadata("PARAM_MOUTH_OPEN_Y", metadata));
        const QMap<QString, float> previous{{"sentinel", 2}};
        values = previous;
        QVERIFY(!document.KeyformParameterValues("D_FACE_01", "artmesh", 1, values, &error));
        QCOMPARE(values, previous);
        QVERIFY(error.contains(QStringLiteral("当前范围")));
        QVERIFY(!document.KeyformParameterValues("D_FACE_01", "invalid", 1, values, &error));
        QCOMPARE(values, previous);
        QVERIFY2(document.KeyformParameterValues("D_FACE_01", "artmesh", 0, values, &error), qPrintable(error));
        QCOMPARE(values.value("PARAM_MOUTH_OPEN_Y"), 0.0f);
    }
};

QTEST_GUILESS_MAIN(ModelSnapshotTests)
#include "model_snapshot_tests.moc"
