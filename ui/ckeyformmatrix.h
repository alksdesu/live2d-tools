#ifndef CKEYFORMMATRIX_H
#define CKEYFORMMATRIX_H

#include "live2dstruct/ckeyformgridsnapshot.h"
#include <QWidget>
#include <memory>

class CKeyformMatrix : public QWidget
{
    Q_OBJECT
public:
    explicit CKeyformMatrix(QWidget* parent = nullptr);
    ~CKeyformMatrix() override;
    void SetGrid(const live2dstruct::CKeyformGridSnapshot& grid, int activeKeyform, bool locked);
    void SetActiveKeyform(int keyform);
    void Clear();
    int ActiveKeyform() const;

signals:
    void KeyformRequested(int keyform);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    class State;
    std::unique_ptr<State> m_state;
    void RebuildAxes();
    void RefreshProjection();
    void RefreshSliceKeys();
    void RefreshStatus();
    void UpdateMetrics();
    void RequestCurrent();
};

#endif
