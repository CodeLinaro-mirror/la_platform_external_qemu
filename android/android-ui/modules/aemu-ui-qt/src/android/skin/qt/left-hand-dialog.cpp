#include "left-hand-dialog.h"
#include "ui_left-hand-dialog.h"

#include <QDebug>
#include <QPushButton>

LeftHandDialog::LeftHandDialog(QWidget* parent)
    : QDialog(parent), ui(new Ui::LeftHandDialog) {
    ui->setupUi(this);
    setWindowFlags(Qt::Popup);

    connect(ui->btn_left_hand_pinch, &QPushButton::clicked, this, &LeftHandDialog::on_btn_left_hand_pinch_clicked);
    connect(ui->btn_left_hand_grab, &QPushButton::clicked, this, &LeftHandDialog::on_btn_left_hand_grab_clicked);
    connect(ui->btn_left_hand_poke, &QPushButton::clicked, this, &LeftHandDialog::on_btn_left_hand_poke_clicked);
}

LeftHandDialog::~LeftHandDialog() {
    delete ui;
}

void LeftHandDialog::on_btn_left_hand_pinch_clicked() {
    qDebug() << "Left hand gesture: Pinch";
    emit leftHandGestureSelected("pinch");
    accept();
}
void LeftHandDialog::on_btn_left_hand_grab_clicked() {
    qDebug() << "Left hand gesture: Grab";
    emit leftHandGestureSelected("grab");
    accept();
}
void LeftHandDialog::on_btn_left_hand_poke_clicked() {
    qDebug() << "Left hand gesture: Poke";
    emit leftHandGestureSelected("poke");
    accept();
}