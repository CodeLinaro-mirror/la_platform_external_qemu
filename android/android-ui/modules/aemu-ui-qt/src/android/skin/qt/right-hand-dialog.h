#pragma once

#include <QDialog>

namespace Ui {
class RightHandDialog;
}

class RightHandDialog : public QDialog {
    Q_OBJECT

public:
    explicit RightHandDialog(QWidget* parent = nullptr);
    ~RightHandDialog();

signals:
    void rightHandGestureSelected(const QString& gesture);

private slots:
    void on_btn_right_hand_pinch_clicked();
    void on_btn_right_hand_grab_clicked();
    void on_btn_right_hand_poke_clicked();

private:
    Ui::RightHandDialog* ui;
};