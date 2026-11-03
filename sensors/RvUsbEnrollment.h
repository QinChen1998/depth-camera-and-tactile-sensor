#pragma once

#include <QtCore/QJsonObject>

class RvSessionController;
class QWidget;

void watchRvUsbDevices(RvSessionController *session, QWidget *window);
QJsonObject checkRvUsbEnrollment();
