#pragma once

#include <QDialog>

namespace Ui {
class LeftHandDialog;
}

class LeftHandDialog : public QDialog {
    Q_OBJECT

public:
    explicit LeftHandDialog(QWidget* parent = nullptr);
    ~LeftHandDialog();

signals:
    void leftHandGestureSelected(const QString& gesture);

private slots:
    void on_btn_left_hand_pinch_clicked();
    void on_btn_left_hand_grab_clicked();
    void on_btn_left_hand_poke_clicked();

private:
    Ui::LeftHandDialog* ui;
};