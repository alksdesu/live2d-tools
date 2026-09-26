#ifndef CBINDINGKEYSDIALOG_H
#define CBINDINGKEYSDIALOG_H

#include "live2dstruct/ckeyformgridsnapshot.h"
#include "live2dstruct/cmocparameterstructurebuilder.h"
#include <QDialog>
#include <memory>

class CBindingKeysDialog : public QDialog
{
    Q_OBJECT
public:
    CBindingKeysDialog(const live2dstruct::CKeyformGridSnapshot& snapshot, int activeKeyform,
                       QWidget* parent = nullptr);
    ~CBindingKeysDialog() override;
    bool BuildRequest(live2dstruct::CMocParameterStructureBuilder::EditBindingKeysRequest& request,
                      QString* error = nullptr) const;
    int SelectedAxis() const;
    void SetError(const QString& error);

signals:
    void ApplyRequested();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    class State;
    std::unique_ptr<State> m_state;
    void SelectAxis(int index);
    void AddKey();
    void DeleteKeys();
    void Refresh();
    void Apply();
};

#endif
