#ifndef CFILEOPTION_H
#define CFILEOPTION_H
#include <QString>
#include <QFileDialog>

class CFileOption
{
public:
    CFileOption();

    static QString GetDigModleFile(QWidget *parent = nullptr){
        QString caption = "Open File";

        // 设置文件选择模式，这里设置为选择单个文件
        QString filter = "Live2D 模型 (*.model3.json *.moc3);;model3.json (*.model3.json);;moc3 (*.moc3)";

        // 设置默认的文件名
        //QString fileName = QFileDialog::getOpenFileName(this, caption, m_CurFilePath, filter);
        QString fileName = QFileDialog::getOpenFileName(parent, caption, m_CurFilePath, filter);
        if(!fileName.isEmpty()){
            m_CurFilePath = GetFilePath(fileName) ;
        }

        return fileName;
    }
    static QString GetUrlPathName(const QUrl& url){
        QString suffixs = "model3.json";
        QFileInfo file(url.toLocalFile());	//toLocalFile可以获取文件路径，而非QUrl的file://开头的路径
        QString re ="";

        if( file.isFile() && file.fileName().contains(suffixs)) {
            re = file.filePath();
            m_CurFilePath = GetFilePath(re);
        }


        return re;
    }
    static QString GetSvaePsdFile(QWidget *parent = nullptr){
        QString fileName = QFileDialog::getSaveFileName(parent,"保存文件",    // 对话框标题
                                                        m_CurFilePath,         // 初始文件名
                                                        "psd文件 (*.psd)"); // 文件过滤器

        if(!fileName.isEmpty()){
            m_CurFilePath = GetFilePath(fileName);
        }
        return fileName;
    }

    static QString GetSelectDirectory(QWidget* parent = nullptr){
        QString dir = QFileDialog::getExistingDirectory(parent, "选择文件夹", m_CurFilePath,
                                                        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if(!dir.isEmpty()){
            m_CurFilePath = dir;
        }
        return dir;
    }

    static QString GetFileName(const QString& file){

        QFileInfo fileInfo(file);

        QString fileName = fileInfo.fileName(); // 获取文件名，包括扩展名

        return fileName;
    }
private:
    static QString GetFilePath(QString path){
        QFileInfo file(path);
        return file.absolutePath();
    }
    static QString m_CurFilePath;

};

#endif // CFILEOPTION_H
