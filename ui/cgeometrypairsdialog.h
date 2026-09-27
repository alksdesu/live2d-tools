#ifndef CGEOMETRYPAIRSDIALOG_H
#define CGEOMETRYPAIRSDIALOG_H

#include <QDialog>
#include <QPair>
#include <functional>
#include <memory>
#include "live2dstruct/cgeometryobjectpairs.h"

class CCidJson;
namespace live2dstruct { class CModelDocument; }

class CGeometryPairsDialog : public QDialog
{
    Q_OBJECT
public:
    using Pairs = live2dstruct::CGeometryObjectPairs;
    struct Preview {
        QVector<QPointF> firstPoints, secondPoints, proposedSecondPoints;
        QVector<quint16> firstTriangles, secondTriangles;
        QString firstCorner, secondCorner;
        QVector<QPair<int, int>> firstEdges, secondEdges;
    };
    using SubmissionHandler = std::function<bool(const Pairs::State&, QString*)>;
    using PairInitializer = std::function<bool(Pairs::Pair&, int, QString*)>;
    using PreviewProvider = std::function<bool(const Pairs::Pair&, int, Preview&, QString*)>;
    explicit CGeometryPairsDialog(const live2dstruct::CModelDocument& document, const CCidJson* displayInfo,
                                  const QVector<Pairs::Object>& selection = {}, QWidget* parent = nullptr);
    ~CGeometryPairsDialog() override;
    const Pairs::State& DraftResult() const;
    void SetSubmissionHandler(SubmissionHandler handler);
    void SetPairInitializer(PairInitializer initializer);
    void SetPreviewProvider(PreviewProvider provider);
    void accept() override;
    void reject() override;

private:
    struct State;
    std::shared_ptr<State> m_state;
    void LoadPair(int row);
    bool CommitPair(QString* error);
    bool ReadPair(Pairs::Pair& result, QString* error) const;
    void NewPair();
    void RemovePair();
    void ReadObjects();
    void ConfirmPair();
    void MatchRotation();
    void RefreshPreview();
    void SelectAxis(int index);
    void FillVertices(int mode);
    void FillKeys(bool reversed);
    void FillAxes();
    void RefreshList();
    void RefreshMappingLayout();
    void MarkChanged();
    void ShowError(const QString& error);
};

#endif
