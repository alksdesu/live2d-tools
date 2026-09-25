#include "live2dstruct/cdrawableflags.h"
#include "live2dstruct/cmodeldocument.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectiontable.h"
#include "opengl/ctextureextractor.h"

#include <QtTest>
#include <QFile>
#include <QMap>
#include <QSet>
#include <QTemporaryDir>
#include <QtEndian>
#include <algorithm>
#include <limits>

using namespace live2dstruct;

namespace {
QString ModelPath(const QString& model)
{
    return QString(TEST_RESOURCES) + '/' + model + '/' + model + ".moc3";
}

struct RawDrawables
{
    QByteArray bytes;
    QVector<quint8> flags;
    QStringList ids;
    QVector<int> textures;
    QVector<int> maskCounts;

    bool Load(const QString& path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        bytes = file.readAll();
        if (bytes.left(4) != "MOC3" || bytes.size() < 0x104) return false;
        const auto integer = [&](quint64 position, quint32& value) {
            if (position > static_cast<quint64>(bytes.size() - 4)) return false;
            value = qFromLittleEndian<quint32>(bytes.constData() + position);
            return true;
        };
        quint32 countsOffset, count, idsOffset, textureOffset, flagOffset, maskCountOffset;
        if (!integer(0x40, countsOffset) || !integer(quint64(countsOffset) + 16, count) ||
            !integer(0xc4, idsOffset) || !integer(0xe4, textureOffset) ||
            !integer(0xe8, flagOffset) || !integer(0x100, maskCountOffset) || count > 1000000 ||
            quint64(idsOffset) + quint64(count) * 64 > quint64(bytes.size()) ||
            quint64(flagOffset) + count > quint64(bytes.size())) return false;
        for (quint32 index = 0; index < count; ++index) {
            quint32 texture, masks;
            if (!integer(quint64(textureOffset) + index * 4, texture) ||
                !integer(quint64(maskCountOffset) + index * 4, masks) ||
                texture > quint32(std::numeric_limits<int>::max()) || masks > 1000000) return false;
            flags.append(static_cast<quint8>(bytes[flagOffset + index]));
            const QByteArray name = bytes.mid(idsOffset + index * 64, 64);
            const qsizetype end = name.indexOf('\0');
            ids.append(QString::fromUtf8(end < 0 ? name : name.left(end)));
            textures.append(static_cast<int>(texture));
            maskCounts.append(static_cast<int>(masks));
        }
        return !flags.isEmpty();
    }
};

QMap<int, int> ExpectedHistogram(const QString& model)
{
    if (model == "Mark") return {{0x04, 30}};
    if (model == "Natori") return {{0x04, 175}, {0x06, 1}};
    if (model == "Rice") return {{0x00, 7}, {0x04, 143}, {0x05, 21}, {0x08, 2}, {0x0c, 5}};
    return {{0x00, 33}, {0x04, 1}};
}

int ExpectedBlend(int flags)
{
    constexpr int modes[]{0, 1, 2, 1};
    return modes[flags % 4];
}

quint32 ExpectedPsdKey(int flags)
{
    constexpr quint32 keys[]{0x6e6f726d, 0x6c646467, 0x6d756c20};
    return keys[ExpectedBlend(flags)];
}

void AddModels()
{
    QTest::addColumn<QString>("model");
    for (const char* model : {"Mark", "Natori", "Rice", "Wanko"})
        QTest::newRow(model) << QString::fromLatin1(model);
}
}

class DrawableFlagsTests : public QObject
{
    Q_OBJECT
private slots:
    void everyByteUsesCoreBlendPriority()
    {
        for (int flags = 0; flags <= 255; ++flags) {
            QCOMPARE(CDrawableFlags::BlendMode(flags), ExpectedBlend(flags));
            QCOMPARE(CDrawableFlags::Culling(flags), (flags / 4) % 2 == 0);
            QCOMPARE(CDrawableFlags::MaskInverted(flags), (flags / 8) % 2 != 0);
            QCOMPARE(CTextureExtractor::BlendModeToPsd(flags), ExpectedPsdKey(flags));
        }
    }

    void unsignedByteReadsKeepHighBitsAndRejectEof()
    {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray data = QByteArray::fromHex("00010203040880ff");
        const QString path = temp.filePath("flags.bin");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(data), data.size());
        file.close();
        CReader reader(path);
        QVERIFY(reader.IsValid());
        for (int index = 0; index < data.size(); ++index) {
            QCOMPARE(reader.readUByte(), static_cast<uint8_t>(data[index]));
            QCOMPARE(reader.m_pos, index + 1);
            QVERIFY(reader.IsValid());
        }
        QCOMPARE(reader.readUByte(), uint8_t(0));
        QVERIFY(!reader.IsValid());
        QVERIFY(!reader.Error().isEmpty());
        QCOMPARE(reader.m_pos, int(data.size()));
        QCOMPARE(reader.readUByte(), uint8_t(0));
        QCOMPARE(reader.m_pos, int(data.size()));
    }

    void packedFlagsUseByteStrideWithoutAdjacentData()
    {
        const QString path = ModelPath("Mark");
        RawDrawables raw;
        QVERIFY(raw.Load(path));
        CReader reader(path);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        auto offsets = *table.sectionOffsetTable->art_meshes;
        const QByteArray flags = QByteArray::fromHex("00010203040880ff");
        offsets.drawable_flags = reader.m_byte.size();
        reader.m_byte.append(flags);
        reader.m_byte.append(QByteArray::fromHex("123456789abcdef00123456789abcdef0123456789abcdef00123456789abcdef0"));
        for (int index = 0; index < flags.size(); ++index) {
            CArt_meshes mesh(reader, offsets, index);
            QVERIFY(reader.IsValid());
            QCOMPARE(mesh.drawable_flags, int(static_cast<quint8>(flags[index])));
            QCOMPARE(mesh.ids, raw.ids[index]);
            QCOMPARE(mesh.texture_nos, raw.textures[index]);
        }
    }

    void sourceBytesMatchParserAndDocument_data() { AddModels(); }

    void sourceBytesMatchParserAndDocument()
    {
        QFETCH(QString, model);
        const QString path = ModelPath(model);
        RawDrawables raw;
        QVERIFY(raw.Load(path));
        CReader reader(path);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        QCOMPARE(table.art_meshes.size(), raw.flags.size());
        QCOMPARE(document.Drawables().size(), raw.flags.size());
        QMap<int, int> histogram;
        for (int index = 0; index < raw.flags.size(); ++index) {
            const int flags = raw.flags[index];
            ++histogram[flags];
            const auto& drawable = document.Drawables()[index];
            QCOMPARE(table.art_meshes[index]->drawable_flags, flags);
            QCOMPARE(drawable.id, raw.ids[index]);
            QCOMPARE(drawable.flags, flags);
            QCOMPARE(drawable.blendMode, ExpectedBlend(flags));
            QCOMPARE(drawable.culling, (flags / 4) % 2 == 0);
            QCOMPARE(document.DrawableMaskInverted(drawable.id), (flags / 8) % 2 != 0);
        }
        QCOMPARE(histogram, ExpectedHistogram(model));
        QVERIFY(document.DrawableMaskInvertedOverrides().isEmpty());
        QVERIFY(!document.DrawableMaskInverted("missing-drawable"));
    }

    void sourceInversionSupportsExplicitFalseHistoryAndSession()
    {
        const QString path = ModelPath("Rice");
        RawDrawables raw;
        QVERIFY(raw.Load(path));
        CModelDocument document;
        QString error;
        QVERIFY2(document.Load(path, &error), qPrintable(error));
        QStringList effective, unused;
        for (int index = 0; index < raw.flags.size(); ++index) {
            if ((raw.flags[index] / 8) % 2 == 0) continue;
            const QString& id = raw.ids[index];
            QVERIFY(document.DrawableMaskInverted(id));
            QCOMPARE(document.Drawables()[index].flags, int(raw.flags[index]));
            if (raw.maskCounts[index] == 0) {
                unused.append(id);
                QVERIFY(document.DrawableMaskIndices(id).isEmpty());
            } else {
                effective.append(id);
                QCOMPARE(document.DrawableMaskIndices(id).size(), raw.maskCounts[index]);
            }
        }
        effective.sort();
        unused.sort();
        QStringList expected{"ArtMesh50", "ArtMesh18", "ArtMesh19", "ArtMesh9", "ArtMesh165"};
        expected.sort();
        QCOMPARE(effective, expected);
        QCOMPARE(unused, (QStringList{"ArtMesh168", "ArtMesh169"}));
        for (const QString& id : effective) {
            CModelEdit edit{id, "artmesh", CModelEdit::MaskInverted, true, false};
            edit.coalesce = false;
            QVERIFY(document.ExecuteEdit(edit));
            QVERIFY(!document.DrawableMaskInverted(id));
            QVERIFY(document.DrawableMaskInvertedOverrides().contains(id));
            QVERIFY(!document.DrawableMaskInvertedOverrides().value(id));
            QVERIFY(document.UndoEdit());
            QVERIFY(document.DrawableMaskInverted(id));
            QVERIFY(document.RedoEdit());
            QVERIFY(!document.DrawableMaskInverted(id));
        }
        CModelSession session;
        session.sourceMoc = path;
        session.drawableMaskInverted = document.DrawableMaskInvertedOverrides();
        CModelSession restored;
        QVERIFY2(CModelSession::FromJson(session.ToJson(), &restored, &error), qPrintable(error));
        CModelDocument reloaded;
        QVERIFY2(reloaded.Load(path, &error), qPrintable(error));
        for (const QString& id : effective) {
            QVERIFY(reloaded.DrawableMaskInverted(id));
            QVERIFY(restored.drawableMaskInverted.contains(id));
            QVERIFY(!restored.drawableMaskInverted.value(id));
            QVERIFY(reloaded.SetDrawableMaskInverted(id, restored.drawableMaskInverted.value(id)));
            QVERIFY(!reloaded.DrawableMaskInverted(id));
        }
        for (const QString& id : unused) QVERIFY(reloaded.DrawableMaskInverted(id));
    }

    void textureExtractionRetainsSourceFlags_data() { AddModels(); }

    void textureExtractionRetainsSourceFlags()
    {
        QFETCH(QString, model);
        const QString path = ModelPath(model);
        RawDrawables raw;
        QVERIFY(raw.Load(path));
        CReader reader(path);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QImage atlas(16, 16, QImage::Format_ARGB32);
        atlas.fill(qRgba(120, 80, 40, 255));
        const QString atlasPath = temp.filePath("atlas.png");
        QVERIFY(atlas.save(atlasPath, "PNG"));
        const int maximumTexture = *std::max_element(raw.textures.cbegin(), raw.textures.cend());
        QVERIFY(maximumTexture >= 0 && maximumTexture < 100);
        CTextureExtractor extractor;
        QVERIFY(extractor.LoadAtlases(QVector<QString>(maximumTexture + 1, atlasPath)));
        const auto layers = extractor.ExtractAll(&table, true);
        QCOMPARE(layers.size(), raw.flags.size());
        QSet<int> seen;
        QMap<int, int> histogram;
        for (const auto& layer : layers) {
            QVERIFY(layer.meshIndex >= 0 && layer.meshIndex < raw.flags.size());
            QVERIFY(!seen.contains(layer.meshIndex));
            seen.insert(layer.meshIndex);
            const int flags = raw.flags[layer.meshIndex];
            QCOMPARE(layer.drawableFlags, flags);
            QCOMPARE(layer.name, raw.ids[layer.meshIndex]);
            QCOMPARE(layer.textureNo, raw.textures[layer.meshIndex]);
            QCOMPARE(CTextureExtractor::BlendModeToPsd(layer.drawableFlags), ExpectedPsdKey(flags));
            QVERIFY(!layer.image.isNull());
            ++histogram[layer.drawableFlags];
        }
        QCOMPARE(histogram, ExpectedHistogram(model));
    }
};

QTEST_GUILESS_MAIN(DrawableFlagsTests)
#include "drawable_flags_tests.moc"
