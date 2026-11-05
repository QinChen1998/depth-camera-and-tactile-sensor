#include "RvImportDiagnostics.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QSysInfo>
#include <QtCore/QUuid>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>

namespace {
struct ImportText { Q_DECLARE_TR_FUNCTIONS(RvImportDiagnosticsDialog) };

void openReportFolder(QWidget *parent, const QString &report)
{
    const QFileInfo file(report);
    if (!file.isFile()) {
        QMessageBox::warning(parent,ImportText::tr("RV1103 import failed"),
                             ImportText::tr("The diagnostic report is no longer available."));
        return;
    }

    // Use Qt's file view so it has the same filesystem access as the report writer.
    QFileDialog folder(parent,ImportText::tr("Diagnostic report folder"));
    folder.setObjectName("rvImportReportFolder");
    folder.setOption(QFileDialog::DontUseNativeDialog);
    folder.setFileMode(QFileDialog::ExistingFile);
    folder.setNameFilter(ImportText::tr("Diagnostic reports (*.txt)"));
    folder.setDirectory(file.absolutePath());
    folder.selectFile(file.fileName());
    folder.setLabelText(QFileDialog::Accept,ImportText::tr("Save a copy..."));
    folder.setLabelText(QFileDialog::Reject,ImportText::tr("Close"));
    if (folder.exec()!=QDialog::Accepted) return;

    const auto selected=folder.selectedFiles().value(0);
    const auto destination=QFileDialog::getSaveFileName(parent,ImportText::tr("Save a copy..."),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(QFileInfo(selected).fileName()),
        ImportText::tr("Diagnostic reports (*.txt)"),nullptr,QFileDialog::DontUseNativeDialog);
    if (destination.isEmpty() || QFileInfo(destination)==QFileInfo(selected)) return;
    QFile source(selected);
    QSaveFile copy(destination);
    if (source.open(QIODevice::ReadOnly) && copy.open(QIODevice::WriteOnly)) {
        const auto content=source.readAll();
        if (source.error()==QFileDevice::NoError && copy.write(content)==content.size() && copy.commit()) return;
    }
    QMessageBox::warning(parent,ImportText::tr("RV1103 import failed"),
                         ImportText::tr("The diagnostic report copy could not be saved."));
}
}

QString RvImportDiagnostics::writeReport(const QString &directory, const QString &operation,
                                         const QString &reason, const QJsonObject &details)
{
    if (directory.isEmpty() || !QDir().mkpath(directory)) return {};
    const auto now=QDateTime::currentDateTimeUtc();
    const auto name=QString("rv1103-import-%1-%2.txt").arg(
        now.toString("yyyyMMdd-HHmmss-zzz"),QUuid::createUuid().toString(QUuid::Id128));
    QSaveFile file(QDir(directory).absoluteFilePath(name));
    if (!file.open(QIODevice::WriteOnly)) return {};
    const QJsonObject report{
        {"schema","dcts-rv1103-import-failure-v1"},
        {"time_utc",now.toString(Qt::ISODateWithMs)},
        {"application_version",QCoreApplication::applicationVersion()},
        {"application_build",QString::fromLatin1(__DATE__ " " __TIME__)},
        {"os",QSysInfo::prettyProductName()},
        {"operation",operation},
        {"reason",reason},
        {"details",details},
    };
    const auto bytes=QJsonDocument(report).toJson(QJsonDocument::Indented);
    if (file.write(bytes)!=bytes.size() || !file.commit()) return {};
    return file.fileName();
}

void RvImportDiagnostics::showFailure(QWidget *parent, const QString &operation,
                                      const QString &reason, const QJsonObject &details)
{
    const auto local=QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    auto report=writeReport(local.isEmpty()?QString():QDir(local).filePath("DctsDiagnostics/rv1103"),
                            operation,reason,details);
    if (report.isEmpty())
        report=writeReport(QDir(QDir::tempPath()).filePath("Dcts/diagnostics/rv1103"),operation,reason,details);

    QMessageBox box(QMessageBox::Warning,ImportText::tr("RV1103 import failed"),reason,
                    QMessageBox::Close,parent);
    box.button(QMessageBox::Close)->setText(ImportText::tr("Close"));
    box.setTextFormat(Qt::PlainText);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);
    if (report.isEmpty()) {
        box.setInformativeText(ImportText::tr("The diagnostic report could not be saved."));
    } else {
        box.setInformativeText(ImportText::tr("Send this diagnostic report to support:\n%1").arg(QDir::toNativeSeparators(report)));
        auto *open=box.addButton(ImportText::tr("Open containing folder"),QMessageBox::ActionRole);
        open->setObjectName("rvImportOpenReportFolder");
        QObject::connect(open,&QPushButton::clicked,&box,[&box,report] { openReportFolder(&box,report); });
    }
    box.exec();
}

