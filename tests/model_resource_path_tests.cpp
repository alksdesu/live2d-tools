#include "std/cmodelresourcepath.h"
#include "std/cmodeljson.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QScopeGuard>

namespace {
QJsonObject settings()
{
    return {{"Version", 3}, {"FileReferences", QJsonObject{
        {"Moc", "模型.moc3"}, {"Textures", QJsonArray{"纹理.png"}},
        {"Physics", "物理.physics3.json"}, {"Pose", "姿势.pose3.json"},
        {"UserData", "标签.userdata3.json"}, {"DisplayInfo", "名称.cdi3.json"},
        {"Expressions", QJsonArray{QJsonObject{{"Name", "笑容"}, {"File", "表情.exp3.json"}}}},
        {"Motions", QJsonObject{{"Idle", QJsonArray{QJsonObject{{"File", "动作.motion3.json"}, {"Sound", "声音.wav"}}}}}}
    }}};
}
QByteArray json(const QJsonObject& root) { return QJsonDocument(root).toJson(QJsonDocument::Compact); }
bool write(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
void replacePath(QJsonObject& root, const QString& field, const QJsonValue& value)
{
    auto files = root["FileReferences"].toObject();
    if (field == "Texture") files["Textures"] = QJsonArray{value};
    else if (field.startsWith("Expression")) {
        auto expression = files["Expressions"].toArray().first().toObject();
        expression[field == "ExpressionFile" ? "File" : "Name"] = value;
        files["Expressions"] = QJsonArray{expression};
    } else if (field == "MotionFile" || field == "Sound") {
        auto motion = files["Motions"].toObject()["Idle"].toArray().first().toObject();
        motion[field == "MotionFile" ? "File" : "Sound"] = value;
        files["Motions"] = QJsonObject{{"Idle", QJsonArray{motion}}};
    } else files[field] = value;
    root["FileReferences"] = files;
}
}

class ModelResourcePathTests : public QObject
{
    Q_OBJECT
private slots:
    void resolvesFilesIndependentlyOfWorkingDirectory()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const auto directory = temporary.filePath(QStringLiteral("模型 空格 🧪"));
        const auto modelDirectory = directory + "/model";
        QVERIFY(QDir().mkpath(modelDirectory));
        const auto path = directory + QStringLiteral("/动作 文件.motion3.json");
        const QByteArray content("original bytes"); QVERIFY(write(path, content));
        const QString previous = QDir::currentPath();
        const auto restore = qScopeGuard([&] { QDir::setCurrent(previous); });
        QVERIFY(QDir::setCurrent(temporary.path()));
        for (const QString& reference : {path, QStringLiteral("../动作 文件.motion3.json"), QStringLiteral(".././动作 文件.motion3.json")}) {
            const auto resolved = CModelResourcePath::Resolve(modelDirectory, reference);
            QCOMPARE(resolved, path);
            QFile file(resolved); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), content);
            QCOMPARE(CModelResourcePath::Identity(modelDirectory, reference), QStringLiteral("../动作 文件.motion3.json"));
        }
        QCOMPARE(CModelResourcePath::Resolve(QStringLiteral("模型 空格 🧪/model"), QStringLiteral("../动作 文件.motion3.json")), path);
    }

    void emptyAndEmbeddedNulNeverResolveToDirectory()
    {
        const auto directory = QDir::tempPath();
        QVERIFY(CModelResourcePath::Resolve(directory, {}).isEmpty());
        QVERIFY(CModelResourcePath::Identity(directory, {}).isEmpty());
        const QString invalid = QStringLiteral("valid") + QChar::Null + QStringLiteral("ignored");
        QVERIFY(CModelResourcePath::Resolve(directory, invalid).isEmpty());
        QVERIFY(CModelResourcePath::Identity(directory, invalid).isEmpty());
        QVERIFY(CModelResourcePath::Resolve(invalid, QStringLiteral("model.moc3")).isEmpty());
    }

    void keepsNativePathAndSymlinkIdentity()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const auto source = temporary.filePath(QStringLiteral("实际.json"));
        const auto alias = temporary.filePath(QStringLiteral("别名.json"));
        QVERIFY(write(source, "bytes")); QVERIFY(QFile::link(source, alias));
        QCOMPARE(CModelResourcePath::Resolve(temporary.path(), QStringLiteral("别名.json")), alias);
        QCOMPARE(CModelResourcePath::Identity(temporary.path(), alias), QStringLiteral("别名.json"));
        QCOMPARE(CModelResourcePath::Resolve(temporary.path(), QStringLiteral("literal%20name.json")), temporary.filePath(QStringLiteral("literal%20name.json")));
#ifndef Q_OS_WIN
        const QString windows = QStringLiteral("C:\\models\\character.moc3");
        QCOMPARE(CModelResourcePath::Resolve(temporary.path(), windows), temporary.filePath(windows));
        QCOMPARE(CModelResourcePath::Identity(temporary.path(), windows), windows);
#endif
    }

    void knownPathsRejectInvalidValues_data()
    {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value");
        for (const auto& field : {"Moc", "Texture", "Physics", "Pose", "UserData", "DisplayInfo", "ExpressionFile", "ExpressionName", "MotionFile", "Sound"}) {
            QTest::newRow(qPrintable(QString::fromLatin1(field) + "-nul")) << QString::fromLatin1(field) << QJsonValue(QStringLiteral("a") + QChar::Null + QStringLiteral("b"));
            QTest::newRow(qPrintable(QString::fromLatin1(field) + "-number")) << QString::fromLatin1(field) << QJsonValue(12);
            QTest::newRow(qPrintable(QString::fromLatin1(field) + "-null")) << QString::fromLatin1(field) << QJsonValue(QJsonValue::Null);
        }
    }

    void knownPathsRejectInvalidValues()
    {
        QFETCH(QString, field); QFETCH(QJsonValue, value);
        auto root = settings(); replacePath(root, field, value);
        QString error; QVERIFY(!CModelJson::ValidateRuntimeSettings(json(root), &error)); QVERIFY(!error.isEmpty());
        QTemporaryDir temporary; const auto path = temporary.filePath("invalid.model3.json"); QVERIFY(write(path, json(root)));
        CModelJson parsed; parsed.Parse(path); QVERIFY(!parsed.IsOpen()); QVERIFY(parsed.getAllZipFiles().isEmpty());
    }

    void malformedContainersFailBeforeSdkAccess_data()
    {
        QTest::addColumn<int>("caseIndex");
        for (int index = 0; index < 7; ++index) QTest::newRow(qPrintable(QString::number(index))) << index;
    }

    void malformedContainersFailBeforeSdkAccess()
    {
        QFETCH(int, caseIndex); auto root = settings(); auto files = root["FileReferences"].toObject();
        switch (caseIndex) {
        case 0: files["Expressions"] = QJsonObject{}; break;
        case 1: files["Expressions"] = QJsonArray{"file"}; break;
        case 2: files["Motions"] = QJsonArray{}; break;
        case 3: files["Motions"] = QJsonObject{{"Idle", "file"}}; break;
        case 4: files["Motions"] = QJsonObject{{"Idle", QJsonArray{1}}}; break;
        case 5: files["Motions"] = QJsonObject{{QStringLiteral("Idle") + QChar::Null, QJsonArray{}}}; break;
        case 6: files["Textures"] = QJsonObject{}; break;
        }
        root["FileReferences"] = files;
        QString error; QVERIFY(!CModelJson::ValidateRuntimeSettings(json(root), &error)); QVERIFY(!error.isEmpty());
    }

    void optionalEmptyAndUnknownExtensionsStayCompatible()
    {
        auto root = settings();
        for (const auto& field : {"Texture", "Physics", "Pose", "UserData", "DisplayInfo", "ExpressionFile", "MotionFile", "Sound"})
            replacePath(root, QString::fromLatin1(field), QString());
        auto files = root["FileReferences"].toObject();
        files["FutureResources"] = QJsonObject{{"NotAPath", QJsonArray{1, true, QJsonValue::Null}}};
        root["FileReferences"] = files;
        QString error; QVERIFY2(CModelJson::ValidateRuntimeSettings(json(root), &error), qPrintable(error));
        QTemporaryDir temporary; const auto path = temporary.filePath("model.model3.json"); QVERIFY(write(path, json(root)));
        CModelJson parsed; parsed.Parse(path); QVERIFY(parsed.IsOpen());
        QCOMPARE(parsed.getAllZipFiles(), QStringList({path, temporary.filePath(QStringLiteral("模型.moc3"))}));
    }

    void resourceListUsesResolvedPathsAndIncludesMotionSound()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid()); auto root = settings();
        const auto external = temporary.filePath(QStringLiteral("absolute/模型.moc3")); replacePath(root, "Moc", external);
        replacePath(root, "Physics", QStringLiteral("../外部/物理.physics3.json"));
        const auto path = temporary.filePath("model.model3.json"); QVERIFY(write(path, json(root)));
        CModelJson parsed; parsed.Parse(path); QVERIFY(parsed.IsOpen()); QCOMPARE(parsed.getMocFileName(), external);
        const auto resources = parsed.getAllZipFiles();
        QCOMPARE(resources.size(), 10);
        QVERIFY(resources.contains(external));
        QVERIFY(resources.contains(QDir::cleanPath(temporary.path() + QStringLiteral("/../外部/物理.physics3.json"))));
        QVERIFY(resources.contains(temporary.filePath(QStringLiteral("声音.wav"))));
        QVERIFY(!resources.contains(temporary.path() + "/" + external));
        parsed.Parse(temporary.filePath("missing.model3.json"));
        QVERIFY(!parsed.IsOpen()); QVERIFY(parsed.getAllZipFiles().isEmpty()); QVERIFY(parsed.m_Sounds.isEmpty());
    }

    void originalModelsRemainValid_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString::fromLatin1(name);
    }
    void originalModelsRemainValid()
    {
        QFETCH(QString, name);
        const auto directory = QString(TEST_RESOURCES) + "/" + name;
        const auto names = QDir(directory).entryList({"*.model3.json"}, QDir::Files); QCOMPARE(names.size(), 1);
        CModelJson parsed; parsed.Parse(QDir(directory).filePath(names.first())); QVERIFY(parsed.IsOpen());
        QVERIFY(QFileInfo::exists(parsed.getMocFileName()));
        for (const auto& path : parsed.getAllZipFiles()) QVERIFY2(QFileInfo::exists(path), qPrintable(path));
    }
};
QTEST_GUILESS_MAIN(ModelResourcePathTests)
#include "model_resource_path_tests.moc"
