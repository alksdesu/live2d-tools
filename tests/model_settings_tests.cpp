#include "std/cmodeljson.h"

#include <QtTest>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <limits>

namespace {
QJsonObject settings()
{
    return {{"Version", 3}, {"FileReferences", QJsonObject{{"Moc", "model.moc3"}}}};
}

QByteArray json(const QJsonObject& root)
{
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
}

class ModelSettingsTests : public QObject
{
    Q_OBJECT
private slots:
    void parseFingerprintUsesExactBytesAndClearsOnEveryFailure()
    {
        QTemporaryDir temporary; QVERIFY(temporary.isValid());
        const auto path = temporary.filePath("model.model3.json");
        const QByteArray raw = "{\r\n\"Version\":3,\"FileReferences\":{\"Moc\":\"model.moc3\"}}\r\n";
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(raw), raw.size()); file.close();
        CModelJson model; QVERIFY(model.SourceDigest().isEmpty()); model.Parse(path); QVERIFY(model.IsOpen());
        QCOMPARE(model.SourceDigest(), QCryptographicHash::hash(raw, QCryptographicHash::Sha256));
        auto textBytes = raw; textBytes.replace("\r\n", "\n");
        QVERIFY(model.SourceDigest() != QCryptographicHash::hash(textBytes, QCryptographicHash::Sha256));
        for (const auto& bad : {QByteArray("{"), QByteArray("[]"), QByteArray("{}"), QByteArray(R"({"FileReferences":{"Moc":""}})")}) {
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(bad), bad.size()); file.close();
            model.Parse(path); QVERIFY(!model.IsOpen()); QVERIFY(model.SourceDigest().isEmpty());
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate)); QCOMPARE(file.write(raw), raw.size()); file.close();
            model.Parse(path); QVERIFY(model.IsOpen()); QCOMPARE(model.SourceDigest().size(), 32);
        }
        model.Parse(temporary.filePath("missing")); QVERIFY(!model.IsOpen()); QVERIFY(model.SourceDigest().isEmpty());
        model.Parse(path); QVERIFY(model.IsOpen());
        model.Parse(temporary.path()); QVERIFY(!model.IsOpen()); QVERIFY(model.SourceDigest().isEmpty());
    }

    void userDataRegistrationOnlyAcceptsTheOneWayAddition()
    {
        auto before = settings();
        before["Layout"] = QJsonObject{{"width", 2}, {"custom", QJsonArray{1, "two"}}};
        before["Groups"] = QJsonArray{QJsonObject{{"Target", "Parameter"}, {"Name", "EyeBlink"}, {"Ids", QJsonArray{"P"}}}};
        before["Custom"] = QJsonObject{{"nested", true}};
        auto files = before["FileReferences"].toObject();
        files["Vendor"] = QJsonObject{{"opaque", "preserved"}}; before["FileReferences"] = files;
        for (const QString& path : {QString("labels.userdata3.json"), QString("../shared/标签.json"),
             QString("/absolute/labels.json"), QString("C:\\assets\\labels.json"), QString("https://example.invalid/labels.json")}) {
            for (bool empty : {false, true}) {
                auto original = before;
                auto previous = files;
                if (empty) previous["UserData"] = "";
                original["FileReferences"] = previous;
                auto updated = original; auto registered = previous; registered["UserData"] = path; updated["FileReferences"] = registered;
                QString reference("unchanged");
                QVERIFY(CModelJson::IsUserDataRegistration(json(original), QJsonDocument(updated).toJson(), &reference));
                QCOMPARE(reference, path);
                QVERIFY(!CModelJson::IsUserDataRegistration(json(updated), json(original), &reference)); QCOMPARE(reference, path);
                QVERIFY(!CModelJson::IsUserDataRegistration(json(updated), json(updated), &reference)); QCOMPARE(reference, path);
            }
        }
        QString output("unchanged");
        QVERIFY(!CModelJson::IsUserDataRegistration(json(before), json(before) + " \r\n", &output)); QCOMPARE(output, QString("unchanged"));
        for (const QJsonValue& invalid : {QJsonValue(), QJsonValue(3), QJsonValue(false), QJsonValue(QJsonArray{}),
             QJsonValue(""), QJsonValue(QString("a") + QChar::Null)}) {
            auto after = before; auto changed = files; changed["UserData"] = invalid; after["FileReferences"] = changed;
            QVERIFY(!CModelJson::IsUserDataRegistration(json(before), json(after), &output)); QCOMPARE(output, QString("unchanged"));
        }
        auto registered = before; files["UserData"] = "labels.json"; registered["FileReferences"] = files;
        for (const QJsonValue& invalid : {QJsonValue(), QJsonValue(false), QJsonValue(3), QJsonValue("already.json")}) {
            auto original = before; auto previous = files; previous["UserData"] = invalid; original["FileReferences"] = previous;
            QVERIFY(!CModelJson::IsUserDataRegistration(json(original), json(registered), &output)); QCOMPARE(output, QString("unchanged"));
        }
        QVERIFY(!CModelJson::IsUserDataRegistration("{", json(registered), &output));
        QVERIFY(!CModelJson::IsUserDataRegistration(json(before), "[]", &output)); QCOMPARE(output, QString("unchanged"));
    }

    void userDataRegistrationRetainsAllOtherSettings_data()
    {
        QTest::addColumn<QString>("field");
        for (const char* field : {"Moc", "Textures", "Physics", "Pose", "DisplayInfo", "Motions", "Expressions", "Layout", "Groups", "Version", "UnknownRoot", "UnknownFileReference"})
            QTest::newRow(field) << QString::fromLatin1(field);
    }
    void userDataRegistrationRetainsAllOtherSettings()
    {
        QFETCH(QString, field);
        auto before = settings();
        before["Layout"] = QJsonObject{{"width", 2}};
        before["Groups"] = QJsonArray{QJsonObject{{"Name", "EyeBlink"}, {"Ids", QJsonArray{"P"}}}};
        before["UnknownRoot"] = QJsonObject{{"value", 1}};
        auto files = before["FileReferences"].toObject();
        files["Textures"] = QJsonArray{"texture.png"}; files["Physics"] = "rig.json"; files["Pose"] = "pose.json"; files["DisplayInfo"] = "names.json";
        files["Motions"] = QJsonObject{{"Idle", QJsonArray{QJsonObject{{"File", "move.json"}, {"Sound", "voice.wav"}}}}};
        files["Expressions"] = QJsonArray{QJsonObject{{"File", "face.json"}, {"Name", "Face"}}};
        files["UnknownFileReference"] = QJsonObject{{"value", 1}}; before["FileReferences"] = files;
        auto after = before; files["UserData"] = "labels.json"; after["FileReferences"] = files;
        if (field == "Layout") after[field] = QJsonObject{{"width", 3}};
        else if (field == "Groups") after[field] = QJsonArray{QJsonObject{{"Name", "EyeBlink"}, {"Ids", QJsonArray{"Q"}}}};
        else if (field == "Version") after[field] = 4;
        else if (field == "UnknownRoot") after[field] = QJsonObject{{"value", 2}};
        else {
            if (field == "Textures") files[field] = QJsonArray{"other.png"};
            else if (field == "Motions") files[field] = QJsonObject{{"Idle", QJsonArray{QJsonObject{{"File", "move.json"}, {"Sound", "changed.wav"}}}}};
            else if (field == "Expressions") files[field] = QJsonArray{QJsonObject{{"File", "face.json"}, {"Name", "Other"}}};
            else if (field == "UnknownFileReference") files[field] = QJsonObject{{"value", 2}};
            else files[field] = "other-file";
            after["FileReferences"] = files;
        }
        QVERIFY(CModelJson::ValidateRuntimeSettings(json(before))); QVERIFY(CModelJson::ValidateRuntimeSettings(json(after)));
        QString reference("unchanged");
        QVERIFY(!CModelJson::IsUserDataRegistration(json(before), json(after), &reference)); QCOMPARE(reference, QString("unchanged"));
    }

    void userDataRegistrationChecksBothInputSizeBoundaries()
    {
        const auto before = json(settings()); auto root = settings(); auto files = root["FileReferences"].toObject();
        files["UserData"] = "labels.json"; root["FileReferences"] = files; const auto after = json(root);
        QByteArray maximumBefore = before + QByteArray(16 * 1024 * 1024 - before.size(), ' ');
        QByteArray maximumAfter = after + QByteArray(16 * 1024 * 1024 - after.size(), ' ');
        QString reference("unchanged");
        QVERIFY(CModelJson::IsUserDataRegistration(maximumBefore, maximumAfter, &reference)); QCOMPARE(reference, QString("labels.json"));
        maximumBefore.append(' '); QVERIFY(!CModelJson::IsUserDataRegistration(maximumBefore, after, &reference));
        QCOMPARE(reference, QString("labels.json"));
        maximumAfter.append(' '); QVERIFY(!CModelJson::IsUserDataRegistration(before, maximumAfter, &reference));
        QCOMPARE(reference, QString("labels.json"));
    }

    void acceptsOriginalModelSettings_data()
    {
        QTest::addColumn<QString>("model");
        for (const char* name : {"Mark", "Rice", "Natori", "Wanko"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void acceptsOriginalModelSettings()
    {
        QFETCH(QString, model);
        QFile source(QString(TEST_RESOURCES) + '/' + model + '/' + model + ".model3.json");
        QVERIFY(source.open(QIODevice::ReadOnly));
        QString error;
        QVERIFY2(CModelJson::ValidateRuntimeSettings(source.readAll(), &error), qPrintable(error));
        QVERIFY(error.isEmpty());
    }

    void acceptsMissingTexturesAndUnusedSlots()
    {
        QJsonObject root = settings();
        QString error = "previous error";
        QVERIFY(CModelJson::ValidateRuntimeSettings(json(root), &error));
        QVERIFY(error.isEmpty());
        const QList<QJsonArray> arrays{
            QJsonArray(), QJsonArray{""}, QJsonArray{"", ""},
            QJsonArray{"", "textures/not-yet-loaded.png", "", "textures/图集.png"}
        };
        for (const QJsonArray& array : arrays) {
            QJsonObject references = root.value("FileReferences").toObject();
            references.insert("Textures", array);
            root.insert("FileReferences", references);
            QVERIFY2(CModelJson::ValidateRuntimeSettings(json(root), &error), qPrintable(error));
        }
    }

    void ignoresUnknownLayoutAndGroupExtensions()
    {
        QJsonObject root = settings();
        root.insert("Layout", QJsonObject{
            {"vendor_string", "auto"}, {"vendor_object", QJsonObject{{"mode", 1}}},
            {"vendor_array", QJsonArray{1, "two"}}, {"vendor_boolean", true},
            {"vendor_null", QJsonValue()}, {"vendor_large", 1.0e300},
            {"width", -2}, {"center_x", 0.5}
        });
        root.insert("Groups", QJsonArray{
            QJsonObject{{"Name", "EyeBlink"}, {"Ids", QJsonArray{"ParamEyeLOpen", "ParamEyeROpen"}}},
            QJsonObject{{"Name", "CustomGroup"}, {"Ids", QJsonArray{"", "CustomId"}},
                        {"Target", QJsonObject{{"vendor", true}}}, {"Type", "CustomType"}}
        });
        QString error;
        QVERIFY2(CModelJson::ValidateRuntimeSettings(json(root), &error), qPrintable(error));
    }

    void finiteLayoutValuesRemainForMatrixValidation_data()
    {
        QTest::addColumn<QString>("key");
        for (const char* key : {"width", "height", "x", "y", "left", "right", "top", "bottom", "center_x", "center_y"})
            QTest::newRow(key) << QString::fromLatin1(key);
    }

    void finiteLayoutValuesRemainForMatrixValidation()
    {
        QFETCH(QString, key);
        QJsonObject root = settings();
        for (double value : {0.0, -2.0, 1.5, double(std::numeric_limits<float>::max()),
                             -double(std::numeric_limits<float>::max())}) {
            root.insert("Layout", QJsonObject{{key, value}});
            QString error;
            QVERIFY2(CModelJson::ValidateRuntimeSettings(json(root), &error), qPrintable(error));
        }
    }

    void rejectsKnownLayoutInvalidValues_data()
    {
        finiteLayoutValuesRemainForMatrixValidation_data();
    }

    void rejectsKnownLayoutInvalidValues()
    {
        QFETCH(QString, key);
        const QList<QJsonValue> invalid{
            QJsonValue(), "2", "NaN", "Infinity", true, QJsonArray{1},
            QJsonObject{{"number", 2}}, 1.0e300, -1.0e300
        };
        for (const QJsonValue& value : invalid) {
            QJsonObject root = settings();
            root.insert("Layout", QJsonObject{{key, value}});
            QString error;
            QVERIFY(!CModelJson::ValidateRuntimeSettings(json(root), &error));
            QVERIFY2(error.contains("Layout." + key), qPrintable(error));
        }
    }

    void rejectsMalformedStructureAndNul_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<QString>("field");
        const auto row = [](const char* name, const char* data, const char* field) {
            QTest::newRow(name) << QByteArray(data) << QString::fromLatin1(field);
        };
        row("empty", "", "model3.json");
        row("invalid-json", "{broken", "model3.json");
        row("root-array", "[]", "model3.json");
        row("missing-references", "{}", "FileReferences");
        row("bad-references", R"({"FileReferences":[]})", "FileReferences");
        row("missing-moc", R"({"FileReferences":{}})", "Moc");
        row("empty-moc", R"({"FileReferences":{"Moc":""}})", "Moc");
        row("numeric-moc", R"({"FileReferences":{"Moc":42}})", "Moc");
        row("nul-moc", R"({"FileReferences":{"Moc":"model\u0000.moc3"}})", "Moc");
        row("textures-null", R"({"FileReferences":{"Moc":"model.moc3","Textures":null}})", "Textures");
        row("textures-object", R"({"FileReferences":{"Moc":"model.moc3","Textures":{}}})", "Textures");
        row("textures-string", R"({"FileReferences":{"Moc":"model.moc3","Textures":""}})", "Textures");
        row("texture-number", R"({"FileReferences":{"Moc":"model.moc3","Textures":[1]}})", "Textures[0]");
        row("texture-null", R"({"FileReferences":{"Moc":"model.moc3","Textures":[null]}})", "Textures[0]");
        row("texture-nul", R"({"FileReferences":{"Moc":"model.moc3","Textures":["tex\u0000.png"]}})", "Textures[0]");
        row("layout-array", R"({"FileReferences":{"Moc":"model.moc3"},"Layout":[]})", "Layout");
        row("layout-null", R"({"FileReferences":{"Moc":"model.moc3"},"Layout":null})", "Layout");
        row("groups-object", R"({"FileReferences":{"Moc":"model.moc3"},"Groups":{}})", "Groups");
        row("group-null", R"({"FileReferences":{"Moc":"model.moc3"},"Groups":[null]})", "Groups[0]");
        row("group-name-number", R"({"FileReferences":{"Moc":"model.moc3"},"Groups":[{"Name":1,"Ids":[]}]})", "Name");
        row("group-name-nul", R"({"FileReferences":{"Moc":"model.moc3"},"Groups":[{"Name":"Eye\u0000Blink","Ids":[]}]})", "Name");
        row("missing-group-ids", R"({"FileReferences":{"Moc":"model.moc3"},"Groups":[{"Name":"Custom"}]})", "Ids");
        row("group-ids-string", R"({"FileReferences":{"Moc":"model.moc3"},"Groups":[{"Name":"Custom","Ids":"Param"}]})", "Ids");
        row("group-id-number", R"({"FileReferences":{"Moc":"model.moc3"},"Groups":[{"Name":"Custom","Ids":[1]}]})", "Ids");
        row("group-id-nul", R"({"FileReferences":{"Moc":"model.moc3"},"Groups":[{"Name":"Custom","Ids":["Param\u0000X"]}]})", "Ids");
    }

    void rejectsMalformedStructureAndNul()
    {
        QFETCH(QByteArray, data);
        QFETCH(QString, field);
        QString error;
        QVERIFY(!CModelJson::ValidateRuntimeSettings(data, &error));
        QVERIFY2(error.contains(field), qPrintable(error));
        QVERIFY(!CModelJson::ValidateRuntimeSettings(data));
    }

    void legacyParseDoesNotAdoptRuntimeGate()
    {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QString path = temporary.filePath("model.model3.json");
        QJsonObject root = settings();
        root.insert("Layout", QJsonObject{{"width", "invalid at runtime"}});
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray bytes = json(root);
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        QVERIFY(!CModelJson::ValidateRuntimeSettings(bytes));
        CModelJson parsed;
        parsed.Parse(path);
        QVERIFY(parsed.IsOpen());
        QCOMPARE(parsed.m_moc, QStringLiteral("model.moc3"));
        QVERIFY(parsed.m_Textures.isEmpty());
    }
};

QTEST_APPLESS_MAIN(ModelSettingsTests)
#include "model_settings_tests.moc"
