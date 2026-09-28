#ifndef CMO_ARTWORK_SOURCE_FIXTURE_H
#define CMO_ARTWORK_SOURCE_FIXTURE_H

#include "cmo3/ccaffarchive.h"
#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

namespace cmo_artwork_source_fixture {
inline QByteArray pixels()
{
    QImage image(2, 1, QImage::Format_RGBA8888); image.setColorSpace(QColorSpace(QColorSpace::SRgb));
    image.setPixelColor(0, 0, QColor(17, 29, 43, 0)); image.setPixelColor(1, 0, QColor(83, 109, 151, 123));
    QByteArray result; QBuffer buffer(&result);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG")) return {};
    return result;
}
inline QByteArray archive(int documents = 2, const QString& version = "502020000")
{
    const auto png = pixels(); QString shared, wrappers;
    for (int index = 0; index < documents; ++index) {
        const QString key = QStringLiteral("d%1").arg(index);
        const auto entry = [&](const QString& id, const QString& name, bool group) {
            return QStringLiteral("<ACLayerEntry xs.n=\"super\"><s xs.n=\"name\">%1</s><s xs.n=\"memo\"/>"
                "<b xs.n=\"isVisible\">%2</b><b xs.n=\"isClipping\">false</b><CBlend_Normal xs.n=\"blend\"><ACBlend xs.n=\"super\"/></CBlend_Normal>"
                "<CLayerGuid xs.n=\"guid\" uuid=\"%3-guid\"/>%4<i xs.n=\"opacity255\">%5</i>"
                "<CLayeredImage xs.n=\"_layeredImage\" xs.ref=\"%6\"/></ACLayerEntry>")
                .arg(name, group ? "true" : "false", id, group ? QStringLiteral("<null xs.n=\"group\"/>") :
                    QStringLiteral("<CLayerGroup xs.n=\"group\" xs.ref=\"%1-root\"/>").arg(key), group ? "255" : "127", key);
        };
        shared += QStringLiteral("<CLayeredImage xs.id=\"%1\"><s xs.n=\"name\">同名文档</s><i xs.n=\"width\">20</i><i xs.n=\"height\">30</i>"
            "<CLayeredImageGuid xs.n=\"guid\" uuid=\"%1-guid\"/><CLayerGroup xs.n=\"_rootLayer\" xs.ref=\"%1-root\"/>"
            "<LayerSet xs.n=\"layerSet\"><CLayeredImage xs.n=\"_layeredImage\" xs.ref=\"%1\"/><carray_list xs.n=\"_layerEntryList\" count=\"3\">"
            "<CLayerGroup xs.ref=\"%1-root\"/><CLayer xs.ref=\"%1-pixels\"/><CLayer xs.ref=\"%1-empty\"/></carray_list></LayerSet></CLayeredImage>").arg(key);
        shared += QStringLiteral("<CLayerGroup xs.id=\"%1-root\"><ACLayerGroup xs.n=\"super\">%2"
            "<carray_list xs.n=\"_children\" count=\"2\"><CLayer xs.ref=\"%1-pixels\"/><CLayer xs.ref=\"%1-empty\"/></carray_list>"
            "</ACLayerGroup><null xs.n=\"layerIdentifier\"/></CLayerGroup>").arg(key, entry(key + "-root", "原始分组", true));
        for (bool empty : {false, true}) {
            const QString id = key + (empty ? "-empty" : "-pixels");
            const QString resource = empty ? QStringLiteral("<null xs.n=\"imageResource\"/>") :
                QStringLiteral("<CImageResource xs.n=\"imageResource\" width=\"2\" height=\"1\" type=\"INT_ARGB\" imageFileBuf_size=\"%1\">"
                    "<file xs.n=\"imageFileBuf\" path=\"original.png\"/></CImageResource>").arg(png.size());
            shared += QStringLiteral("<CLayer xs.id=\"%1\"><ACImageLayer xs.n=\"super\">%2</ACImageLayer><null xs.n=\"layerIdentifier\"/>"
                "<CRect xs.n=\"boundsOnImageDoc\"><i xs.n=\"x\">-3</i><i xs.n=\"y\">4</i><i xs.n=\"width\">%3</i><i xs.n=\"height\">%4</i></CRect>%5</CLayer>")
                .arg(id, entry(id, id, false), empty ? "0" : "2", empty ? "0" : "1", resource);
        }
        wrappers += QStringLiteral("<LayeredImageWrapper><CLayeredImage xs.n=\"image\" xs.ref=\"%1\"/></LayeredImageWrapper>").arg(key);
    }
    const auto xml = QStringLiteral("<root fileFormatVersion=\"%1\"><shared>%2</shared><main><CModelSource>"
        "<CTextureManager xs.n=\"textureManager\"><carray_list xs.n=\"_rawImages\" count=\"%3\">%4</carray_list>"
        "</CTextureManager></CModelSource></main></root>").arg(version, shared).arg(documents).arg(wrappers);
    cmo3::CCaffArchive archive; archive.AddEntry("main.xml", xml.toUtf8(), "main_xml", cmo3::CCaffArchive::CompressSmall);
    archive.AddEntry("original.png", png); QTemporaryDir directory; const auto path = directory.filePath("source.cmo3");
    if (!archive.Save(path)) return {};
    QFile file(path); return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}

#endif
