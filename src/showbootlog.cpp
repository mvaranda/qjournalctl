/**
 * qjournalctl: A Qt-based GUI for systemd's journalctl command
 *
 * Copyright (c) 2016-2021 by Patrick Eigensatz <patrick.eigensatz@gmail.com>
 * Some rights reserved. See LICENSE.
 */


#include "showbootlog.h"
#include "ui_showbootlog.h"

#include "local.h"
#include "connection.h"


#include <iostream>
#include <QProcess>
#include <QMessageBox>
#include <QDebug>
#include <QIcon>
#include <QStyle>
#include <QPushButton>
#include <QFileDialog>
#include <QPlainTextEdit>
#include <QShortcut>
#include <QLabel>
#include <QLineEdit>
#include <QTextDocument>
#include <QCheckBox>
#include <QSet>
#include <QRegularExpression>
#include <QCompleter>
#include <QThread>

#define UNIT_ALL "all"
#define UNIT_VIGIL_ALL "vigil all"

#define HIDE_SYSLOG_FILTER

const char * unties[] = {
    UNIT_ALL,
    UNIT_VIGIL_ALL,
#include "unities.h"
    nullptr
};

ShowBootLog::ShowBootLog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ShowBootLog)
{
    ui->setupUi(this);
}


// todo: Fix this mess!
ShowBootLog::ShowBootLog(QWidget *parent, bool completeJournal, bool realtime, bool reverse, QString bootid, Connection *connection) :
    QDialog(parent),
    ui(new Ui::ShowBootLog),
    maxPriority(7),
    unitOption(""),
    grepFilterText(""),
    grepIncompleteLine("")
{
    // Call simple constructor first
    ui->setupUi(this);
    this->connection = connection;

    // Set save icon for the export buttons
    ui->exportButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    ui->exportSelectionButton->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));

  #ifdef HIDE_SYSLOG_FILTER
    ui->identifiersLineEdit->hide();
    ui->filterButton->hide();
    ui->clearButton->hide();
    ui->filterSpacer->changeSize(0,0);
    ui->filterLabel->setText("Grep Filter");
  #endif

    // Create find and escape shortcut
    new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_F), this, SLOT(on_find_keyshortcut_triggered()));
    new QShortcut(QKeySequence(Qt::Key_Escape), this, SLOT(on_escape_keyshortcut_triggered()));

    // Set local display variables
    this->bootid = bootid;
    this->completeJournal = completeJournal;
    this->realtime = realtime;
    this->reverse = reverse;

    // UI default values
    ui->sinceDateTimeEdit->setDateTime(QDateTime::currentDateTime().addSecs(-60));
    ui->untilDateTimeEdit->setDateTime(QDateTime::currentDateTime());

    if(completeJournal){
        ui->label->setText("Complete systemd journal");
    } else {
        if(realtime){
            ui->label->setText("Showing journal of the current boot  (realtime following enabled)");
        } else {
            ui->label->setText("Showing journal of boot #" + bootid);
        }
    }

    // load unit combo from from file
    const char  ** u = &unties[0];
    while(*u) {
        ui->unitCombo->addItem(*u);
        u++;
    }

    // load greb combo
    ui->grepCombo->addItem("None");
    ui->grepCombo->addItem("sql");
    ui->grepCombo->addItem("SQL");
    grepView = ui->grepCombo->view();
    grepView->viewport()->installEventFilter(this);
    grepView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(grepView, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(list_context_menu(QPoint)));

    // Remote connections require initial setup
    if(connection->isRemote()){
        connect(connection, SIGNAL(connectionDataAvailable(QString)), this, SLOT(appendToBootLog(QString)));
    }

    updateBootLog();
}

ShowBootLog::~ShowBootLog()
{
    // Close ssh channels to stop server from sending data to us
    if(connection->isRemote()){
        connection->close();
    }

    delete ui;
}

void ShowBootLog::on_closeButton_clicked()
{
    close();
}


void ShowBootLog::updateBootLog(bool keepIdentifiers)
{
    // Clear log!
    empty = true;
    ui->plainTextEdit->setPlainText("Selected journal seems to be empty using selected filter settings.");
    ui->plainTextEdit->setStyleSheet("font-weight: bold;");

    if(!keepIdentifiers){
        // Reset all previously accepted but also read identifiers   (clear the filter but also do a full reload!)
        this->allIdentifiers.clear();
        this->acceptedIdentifiers.clear();
        identifierFlags = "";

        // Also reset the UI parts
        ui->acceptedIdentifierLabel->setText("");

    } else {
        // Regenerate all flags
        identifierFlags = "";
        QString acceptedIdentifierLabelText = "";
        for(QString identifier : this->acceptedIdentifiers){
            identifierFlags += " -t " + identifier;
            acceptedIdentifierLabelText += identifier + "  ";
        }

        ui->acceptedIdentifierLabel->setText(acceptedIdentifierLabelText);
    }


    // Fixing realtime bug: Maybe there isn't a single entry with
    // the selected properties. Therefore appendToBootLog() never
    // gets called, because it never gets the '-- No Entries --' output
    // from journalctl, leading to an out-of-date numerOfEntriesLabel.
    if(realtime){
        ui->numberOfEntriesLabel->setText("Showing <b>0</b> lines");
    }


    QString sinceStr = "";
    if(sinceFlag){
        sinceStr = " --since \"" + ui->sinceDateTimeEdit->dateTime().toString("yyyy-MM-dd hh:mm:00") + "\"";
    }

    QString untilStr = "";
    if(untilFlag){
        untilStr = " --until \"" + ui->untilDateTimeEdit->dateTime().toString("yyyy-MM-dd hh:mm:00") + "\"";
    }


    QString command = "";
    if(this->completeJournal){
        command = "journalctl -q -a -p " + QString::number(maxPriority) + sinceStr + untilStr;
    } else {
        if(this->realtime){
            command = "journalctl -q -f --no-tail -p " + QString::number(maxPriority) + " -b " + bootid + sinceStr + untilStr;
        } else {
            command = "journalctl -q -a -p " + QString::number(maxPriority) + " -b " + bootid + sinceStr + untilStr;
        }
    }

    // show blob's content
    if (ui->allCheckBox->isChecked())
        command += " --all ";

    // add unit option
    command += unitOption;


    if(this->reverse){
        command = command + " -r";
    }

    // Enable filtering by syslog identifiers
    command += identifierFlags;

    // As soon as the connection has data available, we want to append it to the boot log.
    // If the connection is already open, we close it!
    if(!connection->isRunning()){
        connect(connection, SIGNAL(connectionDataAvailable(QString)), this, SLOT(appendToBootLog(QString)));
    } else {
        connection->close();
    }

    // Reset byte counter
    numberOfBytesRead = 0;
    // qDebug () << "command: " + command;
    connection->run(command);
}

void ShowBootLog::acceptIdentifier(void){
    this->acceptedIdentifiers.insert(ui->identifiersLineEdit->text());
    updateBootLog(true);
    ui->identifiersLineEdit->clear();
    ui->identifiersLineEdit->setFocus();
}


void ShowBootLog::appendToBootLog(QString readString)
{
    // Since data was available, we're definitely not in the "empty" state anymore
    // --> Prevent the "journal is empty" message to be shown on view update
    if(empty){
        // Remove the "journal is empty" message and reset the style
        ui->plainTextEdit->clear();
        ui->plainTextEdit->setStyleSheet("");

        empty = false;
    }



    if (grepFilterText.length() > 0) {
        bool case_s = ui->caseCheckBox->isChecked();
        QString grepFilterText_lower = grepFilterText.toLower();

        if (grepIncompleteLine.length() > 0) {
            readString = grepIncompleteLine + readString;
            grepIncompleteLine = "";
        }

        int last_newline_idx = readString.lastIndexOf('\n');
        if (last_newline_idx < 0) {
            // qDebug() << "no new_line";
            grepIncompleteLine += readString;
        }
        else if (last_newline_idx == readString.length() - 1) {
            qDebug() << "line is complete";
        }
        else {
            grepIncompleteLine = readString.mid(last_newline_idx + 1);
            readString = readString.mid(0, last_newline_idx);
        }

        QString line, toDisplay;
        QTextStream stream(&readString);
        while (stream.readLineInto(&line)) {
            if (line.length() == 0) continue;
            if (case_s) {
                if (line.indexOf(grepFilterText) >= 0) {
                    toDisplay = toDisplay + line + "\n";
                    // qDebug() << "add: " + line;
                }
            } else {
                QString line_lower = line.toLower();
                if (line_lower.indexOf(grepFilterText_lower) >= 0) {
                    toDisplay = toDisplay + line + "\n";
                    // qDebug() << "add: " + line;
                }
            }
        }
        // Append string to the UI and increment byte counter
        ui->plainTextEdit->appendPlainText(toDisplay.trimmed());

    } else {
        // Append string to the UI and increment byte counter
        ui->plainTextEdit->appendPlainText(readString);
    }
    numberOfBytesRead += readString.size(); // total even if we drop filtered bytes




    ui->plainTextEdit->ensureCursorVisible();

    // Update "numberOfEntries" label
    if(!empty){
        ui->numberOfEntriesLabel->setText("Showing <b>" + QString::number(ui->plainTextEdit->document()->lineCount()-1) + "</b> lines ("+QString::number(numberOfBytesRead) + " bytes)");
    } else {
        ui->numberOfEntriesLabel->setText("Showing <b>0</b> lines (0 bytes)");
    }

    // Collect identifiers for auto completion
    QRegularExpression identifierRegexp = QRegularExpression(" ([a-zA-Z0-9\\_]+)\\[\\d+\\]\\: ", QRegularExpression::MultilineOption);
    QRegularExpressionMatchIterator matches = identifierRegexp.globalMatch(readString);

    QSet<QString> identifierSet;

    // Iterate over all found identifiers
    while(matches.hasNext()){
        QRegularExpressionMatch identifier = matches.next();
        identifierSet.insert(identifier.captured(1));
    }

    this->allIdentifiers += identifierSet;
    QCompleter *completer = new QCompleter(this->allIdentifiers.toList());
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    ui->identifiersLineEdit->setCompleter(completer);

    // Clearing the LineEdit using clear() does not work due to the focus being
    // on the completer popup. (Some strange Qt behaviour)
    // Solution: Stackoverflow, again!  https://stackoverflow.com/a/11905995/2628569
    QObject::connect(completer, SIGNAL(activated(const QString&)),
                     ui->identifiersLineEdit, SLOT(clear()),
                     Qt::QueuedConnection);
}


void ShowBootLog::on_sinceCheckBox_clicked()
{
    sinceFlag = !sinceFlag;
    ui->sinceDateTimeEdit->setEnabled(sinceFlag);
    updateBootLog(true);
}

void ShowBootLog::on_untilCheckBox_clicked()
{
    untilFlag = !untilFlag;
    ui->untilDateTimeEdit->setEnabled(untilFlag);
    updateBootLog(true);
}

void ShowBootLog::on_sinceDateTimeEdit_dateTimeChanged()
{
    updateBootLog(true);
}

void ShowBootLog::on_untilDateTimeEdit_dateTimeChanged()
{
    updateBootLog(true);
}

void ShowBootLog::on_horizontalSlider_sliderMoved(int position)
{
    maxPriority = position;
    updateBootLog(true);
}

void ShowBootLog::on_horizontalSlider_valueChanged(int value)
{
    ShowBootLog::on_horizontalSlider_sliderMoved(value);
}

void ShowBootLog::on_filterButton_clicked()
{
    acceptIdentifier();
    ui->identifiersLineEdit->clear();

    updateBootLog(true);
}

void writeToExportFile(QString fileName, const char *data){
    if(fileName != ""){
        QFile *exportFile = new QFile(fileName);
        exportFile->open(QFile::ReadWrite);
        exportFile->write(data);
        exportFile->close();
    }
}


void ShowBootLog::on_exportButton_clicked()
{
    QString fileName = QFileDialog::getSaveFileName(this, "Export filtered journal entries");
    writeToExportFile(fileName, ui->plainTextEdit->toPlainText().toLocal8Bit().data());
}


void ShowBootLog::on_find_keyshortcut_triggered()
{
    ui->findBox->setVisible(true);
    ui->findLineEdit->setFocus();
}

void ShowBootLog::on_escape_keyshortcut_triggered()
{
    if(ui->identifiersLineEdit->hasFocus()){
        on_clearButton_clicked();
    } else if(ui->findLineEdit->hasFocus() || ui->useRegexpCheckBox->hasFocus() || ui->ignoreCaseCheckBox->hasFocus()){
        ui->findBox->setVisible(false);
    }
}

void ShowBootLog::execute_find(QRegExp regexp, QTextDocument::FindFlags findFlags) {
    if(!regexp.isValid()){
        ui->findStatusLabel->setText("Invalid RegExp!");
        ui->findStatusLabel->setStyleSheet("color: #F00;");
        return;
    }

    if(!ui->plainTextEdit->find(regexp, findFlags)){
        QTextCursor cur = ui->plainTextEdit->textCursor();
        cur.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor, 1);
        ui->plainTextEdit->setTextCursor(cur);
        ui->findStatusLabel->setText("Search started from the beginning");

        if(!ui->plainTextEdit->find(regexp, findFlags)){
            ui->findStatusLabel->setText("Not found");
            ui->findStatusLabel->setStyleSheet("color: #F00;");
        }
    }
}

void ShowBootLog::execute_find(QString string, QTextDocument::FindFlags findFlags) {
    if(!ui->plainTextEdit->find(string, findFlags)){
        QTextCursor cur = ui->plainTextEdit->textCursor();
        cur.movePosition(QTextCursor::Start, QTextCursor::MoveAnchor, 1);
        ui->plainTextEdit->setTextCursor(cur);
        ui->findStatusLabel->setText("Search started from the beginning");

        if(!ui->plainTextEdit->find(string, findFlags)){
            ui->findStatusLabel->setText("Not found");
            ui->findStatusLabel->setStyleSheet("color: #F00;");
        }
    }
}

void ShowBootLog::on_findLineEdit_returnPressed()
{
    ui->findStatusLabel->setText("");
    ui->findStatusLabel->setStyleSheet("color: #000;");

    bool ignoreCase = (ui->ignoreCaseCheckBox->checkState() == Qt::Checked);
    bool useRegexp = (ui->useRegexpCheckBox->checkState() == Qt::Checked);

    QTextDocument::FindFlags ignoreCaseFlags;
    if(!ignoreCase){
        ignoreCaseFlags = QTextDocument::FindCaseSensitively;
    }

    if(useRegexp) {
        QRegExp regexp = QRegExp(ui->findLineEdit->text());
        if(ignoreCase) {
            regexp.setCaseSensitivity(Qt::CaseInsensitive);
        } else {
            regexp.setCaseSensitivity(Qt::CaseSensitive);
        }

        execute_find(regexp, ignoreCaseFlags);
    } else {
        execute_find(ui->findLineEdit->text(), ignoreCaseFlags);
    }
}


void ShowBootLog::on_identifiersLineEdit_returnPressed()
{
    acceptIdentifier();
}


void ShowBootLog::on_clearButton_clicked()
{
    ui->acceptedIdentifierLabel->setText("");
    ui->identifiersLineEdit->clear();
    this->acceptedIdentifiers.clear();
    updateBootLog(false);
}


void ShowBootLog::on_plainTextEdit_selectionChanged()
{
    // Get the selected text and decide whether to show the the exportSelectionButton or not
    QString selection = ui->plainTextEdit->textCursor().selectedText();
    if(selection != ""){
        ui->exportSelectionButton->setVisible(true);
    } else {
        ui->exportSelectionButton->setVisible(false);
    }
}


void ShowBootLog::on_exportSelectionButton_clicked()
{
    QString selection = ui->plainTextEdit->textCursor().selectedText();
    QString fileName = QFileDialog::getSaveFileName(this, "Export selected journal entries");

    writeToExportFile(fileName, selection.toLocal8Bit().data());
}


void ShowBootLog::on_unitCombo_currentTextChanged(const QString &unit)
{
    // qDebug () << "selected unit: " << unit;

    unitOption = "";
    if (unit.compare(UNIT_VIGIL_ALL) == 0) {
        const char  ** u = &unties[2];
        while(*u) {
            unitOption += " -u ";
            unitOption += *u;
            u++;
        }
    }
    else if (unit.compare(UNIT_ALL ) != 0) {
        unitOption += " -u ";
        unitOption += unit;
    }
    // qDebug () << "unitOption: " << unitOption;
    updateBootLog(true);
}


void ShowBootLog::on_grepCombo_currentTextChanged(const QString &grep_txt)
{
    // qDebug () << "selected Grep: " << grep_txt;
    //grepFilterText = grep_txt;
    if (grep_txt.compare("None")) {
        on_grepFilterBt_clicked();
    }
}


void ShowBootLog::on_grepEditBox_textChanged(const QString &txt)
{
     // qDebug () << "Text Grep: " << txt;
     if (txt.length() > 0) {
         ui->grepCombo->setEnabled(false);
         ui->grepEditBox->setStyleSheet("background-color: yellow;");
         //grepFilterText = txt;
     }
     else {
         ui->grepCombo->setEnabled(true);
         ui->grepEditBox->setStyleSheet("background-color: white;");
         ui->grepCombo->setCurrentIndex(0);
         //grepFilterText.clear();
     }
}

bool ShowBootLog::eventFilter(QObject *o, QEvent *e)
{
    // qDebug () << "Got Grep Event: ";
    return false;
}


void ShowBootLog::on_grepClear_clicked()
{
    QString combo = ui->grepCombo->currentText();
    QString txt = ui->grepEditBox->displayText();
    grepFilterText = "";
    if (txt.length() > 0) {
        grepFilterText = txt;
    }
    else if (combo.compare("None") != 0) {
        grepFilterText = combo;
    }

    if (grepFilterText.length() > 0) {
        grepFilterText = "";
        ui->grepCombo->setEnabled(true);
        ui->grepCombo->setCurrentIndex(0);
        ui->grepEditBox->setEnabled(true);
        ui->grepFilterBt->setEnabled(true);
        ui->grepEditBox->setStyleSheet("background-color: white;");
        ui->grepEditBox->clear();
        updateBootLog(true);
    }

}


void ShowBootLog::on_grepFilterBt_clicked()
{
    QString combo = ui->grepCombo->currentText();
    QString txt = ui->grepEditBox->displayText();
    grepFilterText = "";
    if (txt.length() > 0) {
        grepFilterText = txt;
    }
    else if (combo.compare("None") != 0) {
        grepFilterText = combo;
    }

    if (grepFilterText.length() > 0) {
        ui->grepCombo->setEnabled(false);
        ui->grepEditBox->setEnabled(false);
        ui->grepFilterBt->setEnabled(false);
        updateBootLog(true);
    }

}


void ShowBootLog::on_caseCheckBox_clicked()
{
    if (grepFilterText.length() > 0) {
        updateBootLog(true);
    }
}


void ShowBootLog::on_allCheckBox_clicked()
{
    updateBootLog(true);
}

