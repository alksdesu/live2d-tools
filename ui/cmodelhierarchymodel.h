#ifndef CMODELHIERARCHYMODEL_H
#define CMODELHIERARCHYMODEL_H

#include <QAbstractItemModel>
#include <QMap>
#include <QStringList>
#include <memory>

class CCidJson;
namespace live2dstruct { class CSectionTable; }

class CModelHierarchyModel : public QAbstractItemModel
{
    Q_OBJECT
public:
    enum class Projection { Parts, Deformers };
    struct Data;
    using Snapshot = std::shared_ptr<Data>;
    struct ParameterInfo {
        QStringList ids;
        QList<float> minimums, maximums, defaults;
    };

    explicit CModelHierarchyModel(Projection projection, QObject* parent = nullptr);
    static bool Prepare(const live2dstruct::CSectionTable& section, CCidJson* cid,
                        Snapshot& result, QString* error = nullptr);
    void SetSnapshot(const Snapshot& snapshot);
    QModelIndex index(int row, int column, const QModelIndex& parent = {}) const override;
    QModelIndex parent(const QModelIndex& child) const override;
    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role) override;

    QModelIndex IndexForObject(const QString& id, const QString& type) const;
    QModelIndex IndexForKey(const QString& key) const;
    QModelIndexList Indices() const;
    QStringList SubtreeKeys(const QModelIndex& root) const;
    QStringList MeshesForSelection(const QModelIndex& root) const;
    ParameterInfo ParametersForSelection(const QModelIndex& root) const;
    QMap<QString, bool> VisibilityState() const;
    QMap<QString, bool> LockedState() const;
    QMap<QString, bool> SoloVisibility(const QStringList& selectedKeys) const;
    QMap<QString, bool> UpdateVisibility(const QMap<QString, bool>& values);
    QStringList UpdateLocked(const QMap<QString, bool>& values);
    void NotifyObjects(const QStringList& keys);
    QStringList UpdateDeformerDisplayNames(const QMap<QString, QString>& names);
    void NotifyDisplayNames(const QStringList& keys);
    static QString Key(const QModelIndex& index);

signals:
    void CheckRequested(const QModelIndex& index, bool visible);

private:
    int ObjectIndex(const QModelIndex& index) const;
    QModelIndex ObjectIndex(int object) const;
    Snapshot m_data;
    Projection m_projection;
};

#endif
