#pragma once

#include "../sdk/DepthSdkExports.h"
#include <QtCore/QJsonObject>
#include <functional>

class QObject;
class RvSessionController;
class SensorMainWindow;
class RvUsbEnrollment;

struct RvUsbEnrollmentHooks {
    std::function<bool()> canOffer;
    std::function<bool()> canOfferAutomatically;
    std::function<void(bool, bool, const QJsonObject &)> offer;
    std::function<void(const QString &)> status;
    std::function<void(const QString &, const QJsonObject &, bool)> failure;
    std::function<void()> started;
    std::function<void(bool, bool)> finished;
    std::function<void()> changed;
};

DCTS_DEPTH_INTERNAL RvUsbEnrollment *createRvUsbEnrollment(
    RvSessionController *session, QObject *owner, RvUsbEnrollmentHooks hooks, bool watch);
DCTS_DEPTH_INTERNAL bool beginRvUsbEnrollment(RvUsbEnrollment *operation, const QJsonObject &identity);
bool checkRvUsbDevice(RvUsbEnrollment *operation);
bool enrollCheckedRvUsbDevice(RvUsbEnrollment *operation, quint64 checkId, bool startAfter);
bool rvUsbOperationBusy(const RvUsbEnrollment *operation);
QJsonObject rvUsbOperationStatus(const RvUsbEnrollment *operation);
void watchRvUsbDevices(RvSessionController *session, SensorMainWindow *window);
DCTS_DEPTH_INTERNAL QJsonObject checkRvUsbEnrollmentCore();
QJsonObject checkRvUsbEnrollment();
