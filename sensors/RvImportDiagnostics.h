#pragma once
#include <QtCore/QJsonObject>
#include <QtCore/QString>

class QWidget;

namespace RvImportDiagnostics {
QString writeReport(const QString &directory, const QString &operation, const QString &reason,
                    const QJsonObject &details);
void showFailure(QWidget *parent, const QString &operation, const QString &reason, const QJsonObject &details);
}

