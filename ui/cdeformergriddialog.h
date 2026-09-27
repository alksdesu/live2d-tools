#ifndef CDEFORMERGRIDDIALOG_H
#define CDEFORMERGRIDDIALOG_H

#include <QDialog>
#include <QLineF>
#include <QPointF>
#include <QVector>
#include <functional>
#include <memory>

class CDeformerGridDialog : public QDialog
{
    Q_OBJECT
public:
    struct Context {
        QString id, name;
        int rows = 1, columns = 1;
        int ordinaryKeyforms = 0, blendKeyforms = 0, initialKeyform = 0;
        bool quad = false;
    };
    struct Preview {
        QVector<QPointF> beforeGrid, afterGrid;
        QVector<QLineF> beforeContours, afterContours;
        double maximumDisplacementPixels = 0;
        QString summary, limitations;
    };
    using Prepare = std::function<bool(int rows, int columns, int keyform, Preview&, QString*)>;
    using Apply = std::function<bool(int rows, int columns, QString*)>;
    CDeformerGridDialog(Context context, Prepare prepare, Apply apply, QWidget* parent = nullptr);
    ~CDeformerGridDialog() override;
    bool IsReady() const;
    bool IsPreparing() const;
    bool IsSubmitting() const;
    QString Error() const;

public slots:
    void UpdatePreview();
    void done(int result) override;

private:
    class State;
    std::unique_ptr<State> m_state;
    void DraftChanged();
    void RefreshControls();
    void ShowError(const QString& error);
};

#endif
