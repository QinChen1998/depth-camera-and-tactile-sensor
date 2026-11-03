#pragma once

#include <QString>

class QApplication;
class QAbstractSpinBox;
class QColor;
class QLayout;
class QPalette;
class QWidget;

// Shared Widgets presentation. Values, focus policy and command routing remain
// the responsibility of each existing controller.
namespace UiStyle {
enum class ButtonState { Normal, Hovered, Pressed };
QColor actionButtonColor(const QPalette &palette, ButtonState state);
void install(QApplication &application);
QLayout *wrappingRow(QWidget *parent, int spacing);
QWidget *numberField(QAbstractSpinBox *editor);
void setUnit(QAbstractSpinBox *editor, const QString &unit);
QWidget *formField(QWidget *editor);
void card(QWidget *widget);
QString depthErrorText(QString detail);
}
