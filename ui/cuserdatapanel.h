#ifndef CUSERDATAPANEL_H
#define CUSERDATAPANEL_H

#include <QDockWidget>
#include <QSet>
#include "live2dstruct/cmodeluserdata.h"

class QLabel;
class QTableWidget;
class QPushButton;
class CTableEditDelegate;

class CUserDataPanel : public QDockWidget
{
    Q_OBJECT
public:
    explicit CUserDataPanel(QWidget* parent = nullptr);

    bool LoadModel(const QString& modelJsonPath, QString* error = nullptr);
    bool LoadUserData(const QString& path, QString* error = nullptr);
    bool SaveUserData(QString* error = nullptr);
    bool CaptureUserDataBytes(QByteArray& bytes, QString* error = nullptr) const;
    void SetValidArtMeshIds(const QSet<QString>& ids);
    bool HasUnsavedChanges() const;
    const QString& FilePath() const { return m_path; }
    const QString& SourcePath() const { return m_path; }
    const QByteArray& SourceDigest() const { return m_sourceDigest; }

signals:
    void UserDataSaved(const QString& path);
    void DirtyChanged(bool dirty);
    void ArtMeshSelected(const QString& id);

private:
    void AddRow();
    void RemoveRows();
    void RebuildTable();
    void SetDirty(bool dirty);
    void RefreshState();
    bool ReadTable(live2dstruct::CModelUserData* data, QString* error) const;

    QLabel* m_status = nullptr;
    QTableWidget* m_table = nullptr;
    CTableEditDelegate* m_valueDelegate = nullptr;
    QPushButton* m_addButton = nullptr;
    QPushButton* m_removeButton = nullptr;
    QPushButton* m_saveButton = nullptr;
    live2dstruct::CModelUserData m_data;
    QString m_path;
    QByteArray m_sourceDigest;
    QString m_modelJsonPath;
    QByteArray m_modelSourceDigest;
    QSet<QString> m_validArtMeshIds;
    bool m_hasModelIds = false;
    bool m_dirty = false;
    bool m_updating = false;
};

#endif // CUSERDATAPANEL_H
