#pragma once
#include <QCoreApplication>
#include <QRegularExpression>

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
inline QString depthErrorText(QString detail)
{
    static const QRegularExpression hostModel(
        R"([a-z0-9_.-]*unidepth[a-z0-9_.-]*(?:[ \t]+model\b)?)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression boardModel(
        R"([a-z0-9_.-]*litesr[a-z0-9_.-]*(?:[ \t]+model\b)?)", QRegularExpression::CaseInsensitiveOption);
    detail.replace(hostModel, QCoreApplication::translate("UiStyle", "host-assisted inference model"));
    detail.replace(boardModel, QCoreApplication::translate("UiStyle", "on-board inference model"));
    return detail;
}
}
