#include "live2dstruct/cmodeluserdata.h"
#include "std/cmodeljson.h"
#include "std/cmodeljsonwriter.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>

using live2dstruct::CModelUserData;

class ModelUserDataTests : public QObject
{
    Q_OBJECT
private slots:
    void registrationRejectsChangedManifestBeforeCreatingFiles_data()
    {
        QTest::addColumn<QString>("change");
        for (const char* change : {"moc", "textures", "groups", "layout", "extension", "userdata", "whitespace", "newlines", "malformed", "short-digest"})
            QTest::newRow(change) << QString::fromLatin1(change);
    }

    void registrationRejectsChangedManifestBeforeCreatingFiles()
    {
        QFETCH(QString, change);
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto path = temp.filePath("model.model3.json");
        const QByteArray original = "{\r\n\"Version\":3,\"FileReferences\":{\"Moc\":\"model.moc3\"},\r\n\"Custom\":42}\r\n";
        QByteArray updated;
        auto root = QJsonDocument::fromJson(original).object();
        auto files = root.value("FileReferences").toObject();
        if (change == "moc") files["Moc"] = "different.moc3";
        else if (change == "textures") files["Textures"] = QJsonArray{"different.png"};
        else if (change == "userdata") files["UserData"] = "registered.json";
        else if (change == "groups") root["Groups"] = QJsonArray{QJsonObject{{"Name", "EyeBlink"}, {"Ids", QJsonArray{"ParamA"}}}};
        else if (change == "layout") root["Layout"] = QJsonObject{{"width", 3}};
        else if (change == "extension") root["Custom"] = 43;
        root["FileReferences"] = files;
        if (change == "whitespace") updated = original + ' ';
        else if (change == "newlines") { updated = original; updated.replace("\r\n", "\n"); }
        else if (change == "malformed") updated = "{";
        else if (change == "short-digest") updated = original;
        else updated = QJsonDocument(root).toJson();
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(updated), updated.size()); file.close();
        QFile unrelated(temp.filePath("keep.userdata3.json")); QVERIFY(unrelated.open(QIODevice::WriteOnly));
        QCOMPARE(unrelated.write("keep"), 4); unrelated.close();
        const auto entries = QDir(temp.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        CModelUserData data; data.SetEntries({{"ArtMesh", "Body", "tag", {}}});
        const auto digest = change == "short-digest" ? QByteArray("short") : QCryptographicHash::hash(original, QCryptographicHash::Sha256);
        QString result("unchanged"), error;
        QVERIFY(!CModelJsonWriter::CreateUserData(path, data, &result, &error, digest));
        QVERIFY(!error.isEmpty()); QCOMPARE(result, QString("unchanged"));
        QCOMPARE(QDir(temp.path()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot), entries);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), updated); file.close();
        QVERIFY(unrelated.open(QIODevice::ReadOnly)); QCOMPARE(unrelated.readAll(), QByteArray("keep"));
    }

    void registrationAcceptsExactRawManifestFingerprint()
    {
        QTemporaryDir temp; QVERIFY(temp.isValid());
        const auto path = temp.filePath("model.model3.json");
        const QByteArray original = "{\r\n\"Version\":3,\"FileReferences\":{\"Moc\":\"model.moc3\",\"UserData\":\"\"},\r\n\"Custom\":42}\r\n";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(original), original.size()); file.close();
        CModelJson loaded; loaded.Parse(path); QVERIFY(loaded.IsOpen());
        QCOMPARE(loaded.SourceDigest(), QCryptographicHash::hash(original, QCryptographicHash::Sha256));
        CModelUserData data; data.SetEntries({{"ArtMesh", "Body", "tag", {}}});
        QString result("unchanged"), error;
        QVERIFY2(CModelJsonWriter::CreateUserData(path, data, &result, &error, loaded.SourceDigest()), qPrintable(error));
        QVERIFY(result != "unchanged"); QVERIFY(QFileInfo::exists(result));
        CModelUserData saved; QVERIFY(saved.Load(result)); QCOMPARE(saved.ToJson(), data.ToJson());
        QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll();
        const auto root = QJsonDocument::fromJson(bytes).object();
        QCOMPARE(root.value("Custom").toInt(), 42);
        QCOMPARE(QDir(temp.path()).filePath(root.value("FileReferences").toObject().value("UserData").toString()), result);
        CModelJson after; after.Parse(path); QVERIFY(after.IsOpen());
        QCOMPARE(after.SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        QVERIFY(after.SourceDigest() != loaded.SourceDigest());
    }

    void loadAndSaveReportDigestsOfExactBinaryBytes()
    {
        QTemporaryDir temp; const auto path = temp.filePath("raw.userdata3.json");
        const QByteArray raw = "{\r\n\"Version\":3,\"Meta\":{\"UserDataCount\":99},\r\n\"UserData\":[{\"Target\":\"ArtMesh\",\"Id\":\"A\",\"Value\":\"old\"}]}\r\n";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(raw), raw.size()); file.close();
        CModelUserData data; QByteArray digest("unchanged");
        QVERIFY(data.Load(path, nullptr, &digest));
        QCOMPARE(digest, QCryptographicHash::hash(raw, QCryptographicHash::Sha256));
        QVERIFY(digest != QCryptographicHash::hash(data.ToJson(), QCryptographicHash::Sha256));
        const auto loadedDigest = digest; const auto original = data.ToJson(); QString error;
        QVERIFY(!data.Load(temp.filePath("missing"), &error, &digest)); QCOMPARE(digest, loadedDigest); QCOMPARE(data.ToJson(), original);
        const auto invalid = temp.filePath("invalid"); QFile broken(invalid); QVERIFY(broken.open(QIODevice::WriteOnly));
        QCOMPARE(broken.write("{"), 1); broken.close();
        QVERIFY(!data.Load(invalid, &error, &digest)); QCOMPARE(digest, loadedDigest); QCOMPARE(data.ToJson(), original);
        auto entries = data.Entries(); entries[0].value = QStringLiteral("新内容🌸"); data.SetEntries(entries);
        const auto output = temp.filePath("saved.userdata3.json"); QVERIFY(data.Save(output, &error, &digest));
        QFile saved(output); QVERIFY(saved.open(QIODevice::ReadOnly)); const auto bytes = saved.readAll();
        QCOMPARE(bytes, data.ToJson()); QCOMPARE(digest, QCryptographicHash::hash(bytes, QCryptographicHash::Sha256));
        const auto savedDigest = digest;
        QVERIFY(!data.Save(temp.path(), &error, &digest)); QCOMPARE(digest, savedDigest);
        entries[0].id.clear(); data.SetEntries(entries);
        QVERIFY(!data.Save(output, &error, &digest)); QCOMPARE(digest, savedDigest);
    }

    void loadsAndSavesCubismData()
    {
        CModelUserData data;
        QString error;
        QVERIFY2(data.Load(QString(TEST_RESOURCES) + "/Mark/Mark.userdata3.json", &error), qPrintable(error));
        QCOMPARE(data.Entries().size(), 5);
        QCOMPARE(data.Entries().first().target, QStringLiteral("ArtMesh"));
        QCOMPARE(data.Entries().first().value, QStringLiteral("hair"));
        QTemporaryDir temp;
        const QString path = temp.filePath(QStringLiteral("edited.userdata3.json"));
        QVERIFY2(data.Save(path, &error), qPrintable(error));
        CModelUserData decoded;
        QVERIFY2(decoded.Load(path, &error), qPrintable(error));
        QCOMPARE(decoded.ToJson(), data.ToJson());
    }

    void preservesExtensionsAndRecomputesCounts()
    {
        CModelUserData data;
        QVERIFY(data.FromJson(R"({"Version":3,"Extra":{"nested":true},
            "Meta":{"UserDataCount":99999,"TotalUserDataSize":-1,"Editor":"keep"},
            "UserData":[{"Target":"ArtMesh","Id":"Body","Value":"before","Color":7}]})"));
        auto entries = data.Entries();
        const QString unicode = QString::fromUtf8("头发🌸\n标签");
        entries[0].value = unicode;
        entries.append({"ArtMesh", "Eye", "eye", {}});
        data.SetEntries(entries);
        const auto root = QJsonDocument::fromJson(data.ToJson()).object();
        QCOMPARE(root.value("Meta").toObject().value("UserDataCount").toInt(), 2);
        QCOMPARE(root.value("Meta").toObject().value("TotalUserDataSize").toInt(), unicode.toUtf8().size() + 3);
        QCOMPARE(root.value("Meta").toObject().value("Editor").toString(), QStringLiteral("keep"));
        QVERIFY(root.value("Extra").toObject().value("nested").toBool());
        QCOMPARE(root.value("UserData").toArray().first().toObject().value("Color").toInt(), 7);
        CModelUserData decoded;
        QVERIFY(decoded.FromJson(data.ToJson()));
        QCOMPARE(decoded.Entries().first().value, unicode);
    }

    void rejectsInvalidInputWithoutLosingData_data()
    {
        QTest::addColumn<QByteArray>("input");
        QTest::newRow("json") << QByteArray("{");
        QTest::newRow("array") << QByteArray("[]");
        QTest::newRow("fractional-version") << QByteArray(R"({"Version":3.1,"UserData":[]})");
        QTest::newRow("string-version") << QByteArray(R"({"Version":"3","UserData":[]})");
        QTest::newRow("meta") << QByteArray(R"({"Version":3,"Meta":[],"UserData":[]})");
        QTest::newRow("entries") << QByteArray(R"({"Version":3,"UserData":{}})");
        QTest::newRow("not-object") << QByteArray(R"({"Version":3,"UserData":[1]})");
        QTest::newRow("value-type") << QByteArray(R"({"Version":3,"UserData":[{"Target":"ArtMesh","Id":"A","Value":7}]})");
        QTest::newRow("target") << QByteArray(R"({"Version":3,"UserData":[{"Target":"Part","Id":"A","Value":"a"}]})");
        QTest::newRow("empty-id") << QByteArray(R"({"Version":3,"UserData":[{"Target":"ArtMesh","Id":"  ","Value":"a"}]})");
        QTest::newRow("nul-id") << QByteArray(R"({"Version":3,"UserData":[{"Target":"ArtMesh","Id":"A\u0000B","Value":"a"}]})");
        QTest::newRow("nul-value") << QByteArray(R"({"Version":3,"UserData":[{"Target":"ArtMesh","Id":"A","Value":"a\u0000b"}]})");
        QTest::newRow("duplicate") << QByteArray(R"({"Version":3,"UserData":[
            {"Target":"ArtMesh","Id":"A","Value":"a"},{"Target":"ArtMesh","Id":"A","Value":"b"}]})");
        QTest::newRow("size-limit") << QByteArray(16 * 1024 * 1024 + 1, ' ');
    }

    void rejectsInvalidInputWithoutLosingData()
    {
        QFETCH(QByteArray, input);
        CModelUserData data;
        data.SetEntries({{"ArtMesh", "Original", "keep", {}}});
        const QByteArray before = data.ToJson();
        QString error;
        QVERIFY(!data.FromJson(input, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(data.ToJson(), before);
        QVERIFY(!data.Load("/missing/labels.userdata3.json", &error));
        QCOMPARE(data.ToJson(), before);
    }

    void failedSavePreservesFile()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("labels.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("old"), 3);
        file.close();
        CModelUserData data;
        data.SetEntries({{"ArtMesh", "Body", "first", {}}, {"ArtMesh", "Body", "duplicate", {}}});
        QString error;
        QVERIFY(!data.Save(path, &error));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("old"));
        data.SetEntries({{"ArtMesh", "Body", "valid", {}}});
        QVERIFY(!data.Save(temp.path(), &error));
        QVERIFY(!data.Save(QString(), &error));
        data.SetEntries({});
        QVERIFY(data.Save(path, &error));
        CModelUserData loaded;
        QVERIFY(loaded.Load(path, &error));
        QVERIFY(loaded.Entries().isEmpty());
    }

    void registersNewResourceWithoutOverwritingFiles()
    {
        QTemporaryDir temp;
        const QString path = temp.filePath("model.model3.json");
        QFile modelFile(path);
        const QByteArray original = R"({"Version":3,"FileReferences":{"Moc":"model.moc3", "Textures":["texture.png"]},
            "Groups":[{"Target":"Parameter","Name":"EyeBlink","Ids":["Eye"]}],"Custom":42})";
        QVERIFY(modelFile.open(QIODevice::WriteOnly));
        QCOMPARE(modelFile.write(original), original.size());
        modelFile.close();
        QFile existing(temp.filePath("model.userdata3.json"));
        QVERIFY(existing.open(QIODevice::WriteOnly));
        QCOMPARE(existing.write("unrelated"), 9);
        existing.close();
        CModelUserData data;
        data.SetEntries({{"ArtMesh", "Body", "skin", {}}});
        QString result, error;
        QVERIFY2(CModelJsonWriter::CreateUserData(path, data, &result, &error), qPrintable(error));
        QVERIFY(QFileInfo::exists(result));
        QVERIFY(existing.open(QIODevice::ReadOnly));
        QCOMPARE(existing.readAll(), QByteArray("unrelated"));
        QVERIFY(modelFile.open(QIODevice::ReadOnly));
        const QByteArray after = modelFile.readAll();
        const auto root = QJsonDocument::fromJson(after).object();
        modelFile.close();
        QCOMPARE(root.value("Custom").toInt(), 42);
        QCOMPARE(root.value("Groups"), QJsonDocument::fromJson(original).object().value("Groups"));
        const QString reference = root.value("FileReferences").toObject().value("UserData").toString();
        QVERIFY(QDir::isRelativePath(reference));
        QCOMPARE(QDir(temp.path()).filePath(reference), result);
        CModelUserData reloaded;
        QVERIFY(reloaded.Load(result));
        QCOMPARE(reloaded.Entries().first().value, QStringLiteral("skin"));
        CModelJson manifest;
        manifest.Parse(path);
        QVERIFY(manifest.getAllZipFiles().contains(result));
        const QStringList files = QDir(temp.path()).entryList(QDir::Files);
        QVERIFY(!CModelJsonWriter::CreateUserData(path, data, &result, &error));
        QCOMPARE(QDir(temp.path()).entryList(QDir::Files), files);
        QVERIFY(modelFile.open(QIODevice::ReadOnly));
        QCOMPARE(modelFile.readAll(), after);
    }

    void failedRegistrationRemovesUnreferencedResource()
    {
        QTemporaryDir temp;
        QFile modelFile(temp.filePath("model.model3.json"));
        const QByteArray bytes = R"({"Version":3,"FileReferences":{"Moc":"model.moc3"}})";
        QVERIFY(modelFile.open(QIODevice::WriteOnly));
        QCOMPARE(modelFile.write(bytes), bytes.size());
        modelFile.close();
        QVERIFY(modelFile.setPermissions(QFileDevice::ReadOwner));
        if (QFileInfo(modelFile).isWritable()) {
            modelFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            QSKIP("当前账户仍可写入只读文件");
        }
        CModelUserData data;
        data.SetEntries({{"ArtMesh", "Body", "tag", {}}});
        QString result, error;
        const bool saved = CModelJsonWriter::CreateUserData(modelFile.fileName(), data, &result, &error);
        modelFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        QVERIFY(!saved);
        QVERIFY(!error.isEmpty());
        QVERIFY(result.isEmpty());
        QCOMPARE(QDir(temp.path()).entryList(QDir::Files), QStringList{"model.model3.json"});
        QVERIFY(modelFile.open(QIODevice::ReadOnly));
        QCOMPARE(modelFile.readAll(), bytes);
    }
};

QTEST_GUILESS_MAIN(ModelUserDataTests)
#include "model_userdata_tests.moc"
