#include "right-hand-dialog.h"
#include "ui_right-hand-dialog.h"

#include <QDebug>
#include <QPushButton>

RightHandDialog::RightHandDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::RightHandDialog) {
    ui->setupUi(this);
    setWindowFlags(Qt::Popup);

    connect(ui->btn_right_hand_pinch, &QPushButton::clicked, this, &RightHandDialog::on_btn_right_hand_pinch_clicked);
    connect(ui->btn_right_hand_grab, &QPushButton::clicked, this, &RightHandDialog::on_btn_right_hand_grab_clicked);
    connect(ui->btn_right_hand_poke, &QPushButton::clicked, this, &RightHandDialog::on_btn_right_hand_poke_clicked);
}

RightHandDialog::~RightHandDialog() {
    delete ui;
}

void RightHandDialog::on_btn_right_hand_pinch_clicked() {
    qDebug() << "Right hand gesture: Pinch";
    emit rightHandGestureSelected("pinch");
    accept();
}
void RightHandDialog::on_btn_right_hand_grab_clicked() {
    qDebug() << "Right hand gesture: Grab";
    emit rightHandGestureSelected("grab");
    accept();
}
void RightHandDialog::on_btn_right_hand_poke_clicked() {
    qDebug() << "Right hand gesture: Poke";
    emit rightHandGestureSelected("poke");
    accept();
}