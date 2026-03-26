#ifndef CTEXTUREPANEL_H
#define CTEXTUREPANEL_H

#include <QDockWidget>
#include <QComboBox>
#include <QLabel>
#include <QScrollArea>

class CTexturePanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CTexturePanel(QWidget *parent = nullptr);
    void LoadTextures(const QMap<int, QString>& texMap);

private:
    void showTexture(int index);

    QComboBox* m_selector;
    QLabel* m_imageLabel;
    QLabel* m_infoLabel;
    QScrollArea* m_scroll;

    QMap<int, QString> m_textures;
};

#endif // CTEXTUREPANEL_H
