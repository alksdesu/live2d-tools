#ifndef CDEFORMERCREATEDIALOG_H
#define CDEFORMERCREATEDIALOG_H

#include "live2dstruct/cmocdeformerstructurebuilder.h"
#include <QDialog>
#include <QRectF>
#include <QSet>
#include <functional>
#include <memory>
#include <optional>

class CDeformerCreateDialog : public QDialog
{
    Q_OBJECT
public:
    using Builder = live2dstruct::CMocDeformerStructureBuilder;
    struct Context {
        QString suggestedId;
        QSet<QString> existingIds;
        int partIndex = -1;
        QString parentId, partLabel, parentLabel, selectionSummary;
        QVector<Builder::Owner> children;
        int mocVersion = 0;
        Builder::Warp warp;
        std::optional<QRectF> warpBounds;
        Builder::Rotation rotation;
        bool startWithRotation = false;
        bool lockWarpGrid = false;
        QString warpGridHint;
    };
    using Prepare = std::function<bool(const Builder::InsertRequest&, QString* summary, QString* error)>;
    using Submit = std::function<bool(const Builder::InsertRequest&, QString* error)>;

    CDeformerCreateDialog(Context context, Prepare prepare, Submit submit, QWidget* parent = nullptr);
    ~CDeformerCreateDialog() override;
    bool BuildRequest(Builder::InsertRequest& request, QString* error = nullptr) const;
    void RefreshAvailability();
    void SetError(const QString& error);

public slots:
    void accept() override;
    void reject() override;

protected:
    void showEvent(QShowEvent* event) override;

private:
    class State;
    std::unique_ptr<State> m_state;
    void DraftChanged();
    void QueueRefresh();
    void Evaluate(bool submit);
    void ShowError(const QString& error);
};

#endif
