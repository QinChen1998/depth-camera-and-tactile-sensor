#pragma once

#include "DepthSdkExports.h"

class QObject;
class RvSessionController;

namespace DepthSdkQt {
// The returned controller and its SDK session are owned by parent.
DCTS_DEPTH_INTERNAL RvSessionController *attach(QObject *parent);
}
