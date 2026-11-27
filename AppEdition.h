#pragma once

#include <QStringList>

namespace AppEdition {
#ifdef DCTS_CUSTOMER_BUILD
inline constexpr auto Name = "Customer";
inline constexpr bool Customer = true;
#else
inline constexpr auto Name = "Normal";
inline constexpr bool Customer = false;
#endif

QString label();
QString version();
const QStringList &imagingSchemes();
bool measurementCaptureEnabled();
}
