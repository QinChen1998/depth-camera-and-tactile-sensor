#pragma once

#include <QtCore/QJsonObject>

class RvSessionController;
class SensorMainWindow;

void watchRvUsbDevices(RvSessionController *session, SensorMainWindow *window);
QJsonObject checkRvUsbEnrollment();
