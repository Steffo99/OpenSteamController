/**
 * \file mainwindow.cpp
 * \brief GUI for customizing Steam Controller Jingles.
 *
 * MIT License
 *
 * Copyright (c) 2019 Gregory Gluszek
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "scserial.h"

#include <QSerialPortInfo>
#include <QSerialPort>
#include <QDebug>
#include <QMessageBox>
#include <QFileDialog>

/**
 * @brief MainWindow::MainWindow Constructor.
 *
 * @param parent
 */
MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    const auto infos = QSerialPortInfo::availablePorts();

    for (const QSerialPortInfo &info : infos) {
        ui->serialPortComboBox->addItem(info.portName());
    }

    ui->delJingleToolButton->setIcon(ui->delJingleToolButton->style()->standardIcon(QStyle::SP_TrashIcon));
    ui->mvJingleUpToolButton->setIcon(ui->mvJingleUpToolButton->style()->standardIcon(QStyle::SP_ArrowUp));
    ui->mvJingleDownToolButton->setIcon(ui->mvJingleDownToolButton->style()->standardIcon(QStyle::SP_ArrowDown));
}

/**
 * @brief MainWindow::~MainWindow Desctructor.
 */
MainWindow::~MainWindow() {
    delete ui;
}

/**
 * @brief MainWindow::getSelectedComposition
 *
 * @return A pointer to the selected composition or nullptr if
 *      no valid Composition is selected. In case of nullptr
 *      note that this function will produce a pop-up to
 *      communicate the issue to the user.
 */
Composition* MainWindow::getSelectedComposition() {
    const int comp_idx = ui->jingleListWidget->currentRow();

    if (comp_idx > static_cast<int>(compositions.size()) || comp_idx < 0) {
        QMessageBox::information(this, tr("Error"),
            tr("Invalid Composition selected"));
        return nullptr;
    }

    return &compositions[static_cast<uint32_t>(comp_idx)];
}

void MainWindow::on_playJinglePushButton_clicked() {
    QString serial_port_name = ui->serialPortComboBox->currentText();
    SCSerial serial(serial_port_name);
    if (!compositions.size()) {
        QMessageBox::information(this, tr("Error"),
            tr("No Compositions to Play"));
        return;
    }

    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    SCSerial::ErrorCode serial_err_code = serial.open();
    if (serial_err_code != SCSerial::NO_ERROR) {
        QMessageBox::information(this, tr("Error"),
            tr("Cannot open %1.\n\nError: %2")
            .arg(serial_port_name)
            .arg(SCSerial::getErrorString(serial_err_code)));
        return;
    }

    // Make sure there is enough memory to download Jingle...
    uint32_t num_bytes = Composition::EEPROM_HDR_NUM_BYTES +
            composition->getMemUsage();

    if (num_bytes > Composition::MAX_EEPROM_BYTES) {
        QMessageBox::information(this, tr("Error"),
            tr("Jingle is too large (%1/%2 bytes). Try using configuration "
               "options to reduce size.")
            .arg(num_bytes)
            .arg(Composition::MAX_EEPROM_BYTES));
        return;
    }

    QString cmd("jingle clear\n");
    QString resp = cmd + "\rJingle data cleared successfully.\n\r";
    if (serial.send(cmd, resp)) {
        QMessageBox::information(this, tr("Error"),
            tr("Failed to clear Jingle Data."));
        return;
    }

    Composition::ErrorCode comp_err_code = composition->download(serial, 0);
    if (comp_err_code != Composition::NO_ERROR) {
        QMessageBox::information(this, tr("Error"),
            tr("Cannot download to %1.\n\nError: %2")
            .arg(serial_port_name)
            .arg(Composition::getErrorString(comp_err_code)));
        return;
    }

    // Since we clear before add Jingle, it will always be at index 0
    cmd = "jingle play 0\n";
    resp = cmd + "\rJingle play started successfully.\n\r";
    if (serial.send(cmd, resp)) {
        QMessageBox::information(this, tr("Error"),
            tr("Failed to send play command."));
        return;
    }
}

void MainWindow::on_browsePushButton_clicked()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        ("Open musicXML File"),
        QDir::homePath(),
        ("musixXML (*.musicxml)"));

    ui->musicXmlPathLineEdit->setText(fileName);
}

void MainWindow::on_convertPushButton_clicked()
{
    if (compositions.size() > Composition::MAX_NUM_COMPS) {
        QMessageBox::information(this, tr("Error"),
            tr("Too many Compositions have been added. "
               "Please delete before attemping to add another."));
        return;
    }

    QString filename = ui->musicXmlPathLineEdit->text();

    compositions.push_back(Composition(filename));
    Composition& composition = compositions.back();

    Composition::ErrorCode comp_err_code = composition.parse();
    if (Composition::NO_ERROR != comp_err_code) {
        QMessageBox::information(this, tr("Error"),
            tr("Failed to parse file '%1'.\nError: %2")
            .arg(filename)
            .arg(Composition::getErrorString(comp_err_code)));
        compositions.pop_back();
        return;
    }

    // Create string to identify this Composition
    // Remove path
    QStringList list = filename.split('/');
    filename = list[list.size()-1];
    list = filename.split('\\');
    filename = list[list.size()-1];
    // Remove .musicxml extension
    filename.resize(filename.size() - QString(".musicxml").size());
    // Add identifier string and make sure it is selected
    ui->jingleListWidget->addItem(filename);
    ui->jingleListWidget->setCurrentItem(ui->jingleListWidget->item(ui->jingleListWidget->count()-1));
    ui->jingleListWidget->repaint();

    // Update GUI to show specs on newly added Composition
    updateCompositionDisplay();

    // Update memory usage display since Composition has been added
    updateMemUsage();
}

void MainWindow::updateMemUsage() {
    const int PROG_BAR_MAX = 100;

    uint32_t num_bytes = Composition::EEPROM_HDR_NUM_BYTES;

    for (uint32_t comp_idx = 0; comp_idx < compositions.size(); comp_idx++) {
        num_bytes += compositions[comp_idx].getMemUsage();
    }

    if (num_bytes >= Composition::MAX_EEPROM_BYTES) {
        ui->memUsageProgressBar->setValue(PROG_BAR_MAX);
    } else {
        ui->memUsageProgressBar->setValue(PROG_BAR_MAX * num_bytes
            / Composition::MAX_EEPROM_BYTES);
    }
    ui->memUsageProgressBar->update();
    ui->memUsageProgressBar->repaint();

    QString bytes_used_str = QString::number(num_bytes) +
            "/" + QString::number(Composition::MAX_EEPROM_BYTES) +
            " bytes used";
    ui->memUsageCurrBytesLabel->setText(bytes_used_str);
    ui->memUsageCurrBytesLabel->update();
    ui->memUsageCurrBytesLabel->repaint();
}

void MainWindow::updateCompositionDisplay() {
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    ui->startMeasComboBox->clear();
    ui->endMeasComboBox->clear();
    for (uint32_t meas_idx = 0; meas_idx < composition->getNumMeasures(); meas_idx++) {
        ui->startMeasComboBox->addItem(QString::number(meas_idx));
        ui->endMeasComboBox->addItem(QString::number(meas_idx));
    }
    ui->startMeasComboBox->setCurrentIndex(static_cast<int>(composition->getMeasStartIdx()));
    ui->endMeasComboBox->setCurrentIndex(static_cast<int>(composition->getMeasEndIdx()));
    ui->startMeasComboBox->update();
    ui->startMeasComboBox->repaint();
    ui->endMeasComboBox->update();
    ui->endMeasComboBox->repaint();

    ui->bpmLineEdit->setText(QString::number(composition->getBpm()));
    ui->bpmLineEdit->update();
    ui->bpmLineEdit->repaint();
    ui->octaveAdjustLineEdit->setText(QString().setNum(composition->getOctaveAdjust(), 'f', 2));
    ui->octaveAdjustLineEdit->update();
    ui->octaveAdjustLineEdit->repaint();

    std::vector<QString> voice_strs = composition->getVoiceStrs();
    ui->chanSourceLeftComboBox->clear();
    ui->chanSourceRightComboBox->clear();
    ui->chanSourceLeftComboBox->addItem(Composition::getNoVoiceStr());
    ui->chanSourceRightComboBox->addItem(Composition::getNoVoiceStr());
    ui->chanSourceLeftComboBox->setCurrentIndex(0);
    ui->chanSourceRightComboBox->setCurrentIndex(0);
    for (uint32_t voice_idx = 0; voice_idx < voice_strs.size(); voice_idx++) {
        ui->chanSourceLeftComboBox->addItem(voice_strs[voice_idx]);
        if (voice_strs[voice_idx] == composition->getVoice(Composition::LEFT)) {
            ui->chanSourceLeftComboBox->setCurrentIndex(static_cast<int>(voice_idx+1));
        }
        ui->chanSourceRightComboBox->addItem(voice_strs[voice_idx]);
        if (voice_strs[voice_idx] == composition->getVoice(Composition::RIGHT)) {
            ui->chanSourceRightComboBox->setCurrentIndex(static_cast<int>(voice_idx+1));
        }
    }
    ui->chanSourceLeftComboBox->update();
    ui->chanSourceLeftComboBox->repaint();
    ui->chanSourceRightComboBox->update();
    ui->chanSourceRightComboBox->repaint();

    updateChordComboBox(Composition::LEFT);
    updateChordComboBox(Composition::RIGHT);
}

void MainWindow::updateChordComboBox(Composition::Channel chan) {
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    QComboBox* combo_box = nullptr;

    switch (chan) {
    case Composition::LEFT:
        combo_box = ui->chanChordLeftComboBox;
        break;

    case Composition::RIGHT:
        combo_box = ui->chanChordRightComboBox;
        break;
    }

    combo_box->clear();

    const QString voice_str = composition->getVoice(chan);
    if (voice_str == Composition::getNoVoiceStr()) {
        combo_box->update();
        combo_box->repaint();
        return;
    }

    const uint32_t meas_start_idx = composition->getMeasStartIdx();
    const uint32_t meas_end_idx = composition->getMeasEndIdx();
    const uint32_t num_chords = composition->getNumChords(voice_str, meas_start_idx, meas_end_idx);

    for (uint32_t chord_idx = 0; chord_idx < num_chords; chord_idx++) {
        combo_box->addItem(QString::number(chord_idx));
    }
    combo_box->setCurrentIndex(static_cast<int>(composition->getChordIdx(chan)));
    combo_box->update();
    combo_box->repaint();
}

void MainWindow::on_delJingleToolButton_clicked()
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    const int comp_idx = ui->jingleListWidget->currentRow();

    compositions.erase(compositions.begin() + comp_idx);
    ui->jingleListWidget->takeItem(comp_idx);
    ui->jingleListWidget->setCurrentItem(nullptr);
    ui->jingleListWidget->update();
    ui->jingleListWidget->repaint();

    // Clear out all ui elements related to Composition
    ui->startMeasComboBox->clear();
    ui->startMeasComboBox->update();
    ui->startMeasComboBox->repaint();
    ui->endMeasComboBox->clear();
    ui->endMeasComboBox->update();
    ui->endMeasComboBox->repaint();

    ui->bpmLineEdit->clear();
    ui->bpmLineEdit->update();
    ui->bpmLineEdit->repaint();
    ui->octaveAdjustLineEdit->clear();
    ui->octaveAdjustLineEdit->update();
    ui->octaveAdjustLineEdit->repaint();

    ui->chanSourceLeftComboBox->clear();
    ui->chanSourceLeftComboBox->update();
    ui->chanSourceLeftComboBox->repaint();
    ui->chanSourceRightComboBox->clear();
    ui->chanSourceRightComboBox->update();
    ui->chanSourceRightComboBox->repaint();

    ui->chanChordRightComboBox->clear();
    ui->chanChordRightComboBox->update();
    ui->chanChordRightComboBox->repaint();
    ui->chanChordLeftComboBox->clear();
    ui->chanChordLeftComboBox->update();
    ui->chanChordLeftComboBox->repaint();

    updateMemUsage();
}

void MainWindow::on_mvJingleDownToolButton_clicked()
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    const int comp_idx = ui->jingleListWidget->currentRow();

    if (comp_idx + 1 >= static_cast<int>(compositions.size()) ) {
        return;
    }

    QListWidgetItem* item = ui->jingleListWidget->takeItem(comp_idx);
    ui->jingleListWidget->insertItem(comp_idx+1, item);
    ui->jingleListWidget->setCurrentItem(item);
    ui->jingleListWidget->update();
    ui->jingleListWidget->repaint();

    Composition comp = compositions.at(static_cast<uint32_t>(comp_idx));
    compositions[static_cast<uint32_t>(comp_idx)] = compositions[static_cast<uint32_t>(comp_idx)+1];
    compositions[static_cast<uint32_t>(comp_idx)+1] = comp;
}

void MainWindow::on_mvJingleUpToolButton_clicked()
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    const int comp_idx = ui->jingleListWidget->currentRow();

    if (comp_idx == 0) {
        return;
    }

    QListWidgetItem* item = ui->jingleListWidget->takeItem(comp_idx);
    ui->jingleListWidget->insertItem(comp_idx-1, item);
    ui->jingleListWidget->setCurrentItem(item);
    ui->jingleListWidget->update();
    ui->jingleListWidget->repaint();

    Composition comp = compositions.at(static_cast<uint32_t>(comp_idx));
    compositions[static_cast<uint32_t>(comp_idx)] = compositions[static_cast<uint32_t>(comp_idx)-1];
    compositions[static_cast<uint32_t>(comp_idx)-1] = comp;
}

void MainWindow::on_jingleListWidget_clicked(const QModelIndex &index)
{
    Q_UNUSED(index);

    if (ui->jingleListWidget->currentRow() < 0)
        return;

    updateCompositionDisplay();
}

void MainWindow::on_startMeasComboBox_activated(int index)
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    if (index < 0) {
        QMessageBox::information(this, tr("Error"),
            tr("Invalid Start Measure selected"));
        return;
    }

    //TODO: check error
    composition->setMeasStartIdx(static_cast<uint32_t>(index));

    updateChordComboBox(Composition::LEFT);
    updateChordComboBox(Composition::RIGHT);

    updateMemUsage();
}

void MainWindow::on_endMeasComboBox_activated(int index)
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    if (index < 0) {
        QMessageBox::information(this, tr("Error"),
            tr("Invalid Start Measure selected"));
        return;
    }

    //TODO: check error
    composition->setMeasEndIdx(static_cast<uint32_t>(index));

    updateChordComboBox(Composition::LEFT);
    updateChordComboBox(Composition::RIGHT);

    updateMemUsage();
}

void MainWindow::on_octaveAdjustLineEdit_editingFinished()
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    float octave_adjust = ui->octaveAdjustLineEdit->text().toFloat();

    qDebug() << "Adjusting Octave scaling factor to " << octave_adjust;

    composition->setOctaveAdjust(octave_adjust);
}

void MainWindow::on_bpmLineEdit_editingFinished()
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    uint32_t bpm = ui->bpmLineEdit->text().toUInt();

    qDebug() << "Adjusting BPM to " << bpm;

    composition->setBpm(bpm);
}

void MainWindow::on_chanChordLeftComboBox_activated(int index)
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    if (index < 0) {
        QMessageBox::information(this, tr("Error"),
            tr("Invalid Left Chord Index selected"));
        return;
    }

    qDebug() << "Left Chord Idx Set to " << index;

    //TODO: check error
    composition->setChordIdx(Composition::LEFT, static_cast<uint32_t>(index));
}

void MainWindow::on_chanChordRightComboBox_activated(int index)
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    if (index < 0) {
        QMessageBox::information(this, tr("Error"),
            tr("Invalid Right Chord Index selected"));
        return;
    }

    //TODO: check error
    composition->setChordIdx(Composition::RIGHT, static_cast<uint32_t>(index));
}

void MainWindow::on_chanSourceLeftComboBox_activated(const QString& voiceStr)
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    //TODO: check error
    composition->setVoice(Composition::LEFT, voiceStr);

    updateChordComboBox(Composition::LEFT);

    updateMemUsage();
}

void MainWindow::on_chanSourceRightComboBox_activated(const QString& voiceStr)
{
    Composition* composition = getSelectedComposition();
    if (!composition) {
        return;
    }

    //TODO: check error
    composition->setVoice(Composition::RIGHT, voiceStr);

    updateChordComboBox(Composition::RIGHT);

    updateMemUsage();
}
