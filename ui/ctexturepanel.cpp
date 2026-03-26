#include "ctexturepanel.h"
#include <QVBoxLayout>
#include <QFileInfo>
#include <QDebug>

CTexturePanel::CTexturePanel(QWidget *parent)
    : QDockWidget("纹理", parent)
{
    QWidget* wrapper = new QWidget(this);
    QVBoxLayout* mainLayout = new QVBoxLayout(wrapper);

    m_selector = new QComboBox(wrapper);
    mainLayout->addWidget(m_selector);

    m_scroll = new QScrollArea(wrapper);
    m_scroll->setWidgetResizable(true);
    m_scroll->setAlignment(Qt::AlignCenter);

    m_imageLabel = new QLabel(m_scroll);
    m_imageLabel->setAlignment(Qt::AlignCenter);
    m_imageLabel->setScaledContents(false);
    m_scroll->setWidget(m_imageLabel);
    mainLayout->addWidget(m_scroll, 1);

    m_infoLabel = new QLabel("未加载纹理", wrapper);
    m_infoLabel->setStyleSheet("color: #666; padding: 4px;");
    mainLayout->addWidget(m_infoLabel);

    setWidget(wrapper);

    connect(m_selector, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CTexturePanel::showTexture);
}

void CTexturePanel::LoadTextures(const QMap<int, QString>& texMap)
{
    m_textures = texMap;
    m_selector->clear();
    m_imageLabel->clear();
    m_infoLabel->setText("未加载纹理");

    if(texMap.isEmpty()) return;

    for(auto it = texMap.constBegin(); it != texMap.constEnd(); ++it){
        QFileInfo fi(it.value());
        m_selector->addItem(QString("texture_%1 (%2)").arg(it.key()).arg(fi.fileName()), it.key());
    }

    if(m_selector->count() > 0){
        showTexture(0);
    }
}

void CTexturePanel::showTexture(int index)
{
    if(index < 0 || index >= m_selector->count()) return;

    int texKey = m_selector->itemData(index).toInt();
    if(!m_textures.contains(texKey)) return;

    QString path = m_textures[texKey];
    QImage img(path);

    if(img.isNull()){
        m_imageLabel->setText("无法加载: " + path);
        m_infoLabel->setText("加载失败");
        return;
    }

    // 缩放到面板宽度，保持比例
    int availWidth = m_scroll->viewport()->width() - 10;
    if(availWidth < 100) availWidth = 300;
    QPixmap pix = QPixmap::fromImage(img);
    if(pix.width() > availWidth){
        pix = pix.scaledToWidth(availWidth, Qt::SmoothTransformation);
    }
    m_imageLabel->setPixmap(pix);

    QFileInfo fi(path);
    m_infoLabel->setText(QString("%1 | %2x%3 | %4")
        .arg(fi.fileName())
        .arg(img.width()).arg(img.height())
        .arg(fi.suffix().toUpper()));
}
