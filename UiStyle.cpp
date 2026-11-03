#include "UiStyle.h"

#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QRegularExpression>
#include <QPushButton>
#include <QScrollBar>
#include <QSlider>
#include <QSpinBox>
#include <QStyleOption>
#include <QToolButton>
#include <QWheelEvent>

QString UiStyle::depthErrorText(QString detail)
{
    static const QRegularExpression hostModel(
        R"([a-z0-9_.-]*unidepth[a-z0-9_.-]*(?:[ \t]+model\b)?)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression boardModel(
        R"([a-z0-9_.-]*litesr[a-z0-9_.-]*(?:[ \t]+model\b)?)", QRegularExpression::CaseInsensitiveOption);
    detail.replace(hostModel, QCoreApplication::translate("UiStyle", "host-assisted inference model"));
    detail.replace(boardModel, QCoreApplication::translate("UiStyle", "on-board inference model"));
    return detail;
}

QColor UiStyle::actionButtonColor(const QPalette &palette, ButtonState state)
{
    const bool dark = palette.color(QPalette::Base).lightness() < 128;
    if (state == ButtonState::Pressed) return QColor(dark ? "#515b68" : "#ced7e1");
    if (state == ButtonState::Hovered) return QColor(dark ? "#464e59" : "#d9dfe6");
    return QColor(dark ? "#3b414a" : "#e3e6ea");
}

namespace {
QColor border(const QPalette &palette)
{
    return palette.color(QPalette::Base).lightness() < 128 ? QColor("#454950") : QColor("#e7e7e7");
}
QColor surface(const QPalette &palette)
{
    return palette.color(QPalette::Base).lightness() < 128 ? QColor("#30343b") : QColor("#f5f5f5");
}
void rounded(QPainter *painter, const QRect &rect, const QColor &fill, const QColor &edge, qreal radius = 8)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(edge, 1));
    painter->setBrush(fill);
    painter->drawRoundedRect(QRectF(rect).adjusted(.5, .5, -.5, -.5), radius, radius);
    painter->restore();
}
void chevron(QPainter *painter, const QRect &rect, const QColor &color, bool up)
{
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(QPen(color, 1.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF center = QRectF(rect).center();
    const qreal direction = up ? -1 : 1;
    QPainterPath path;
    path.moveTo(center + QPointF(-3, -1.5 * direction));
    path.lineTo(center + QPointF(0, 1.5 * direction));
    path.lineTo(center + QPointF(3, -1.5 * direction));
    painter->drawPath(path);
    painter->restore();
}

// Paint standard Widgets using the tactile settings' palette and geometry.
// A proxy preserves native text, accessibility, keyboard and hit-test behavior;
// local semantic styles (for example Emergency Stop) still take precedence.
class SettingsStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void polish(QWidget *widget) override
    {
        QProxyStyle::polish(widget);
        if (qobject_cast<QAbstractSpinBox *>(widget) || qobject_cast<QComboBox *>(widget)
            || qobject_cast<QPushButton *>(widget) || qobject_cast<QToolButton *>(widget))
            widget->setAttribute(Qt::WA_Hover);
        if (qobject_cast<QAbstractSpinBox *>(widget))
            widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        if (qobject_cast<QComboBox *>(widget) && widget->sizePolicy().horizontalPolicy() == QSizePolicy::Preferred)
            widget->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        if (qobject_cast<QPushButton *>(widget) && widget->sizePolicy().horizontalPolicy() == QSizePolicy::Minimum)
            widget->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    }

    void drawControl(ControlElement element, const QStyleOption *option,
        QPainter *painter, const QWidget *widget) const override
    {
        // Windows 11 paints push buttons directly, bypassing the panel
        // primitive. Use the common label layout so those buttons match too.
        if (element == CE_PushButton || element == CE_PushButtonLabel
            || element == CE_ComboBoxLabel || element == CE_ToolButtonLabel) {
            QCommonStyle::drawControl(element, option, painter, widget);
            return;
        }
        if (element == CE_PushButtonBevel) {
            drawPrimitive(PE_PanelButtonCommand, option, painter, widget);
            return;
        }
        QProxyStyle::drawControl(element, option, painter, widget);
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override
    {
        if (metric == PM_LayoutHorizontalSpacing || metric == PM_LayoutVerticalSpacing) return 8;
        if ((metric == PM_IndicatorWidth || metric == PM_IndicatorHeight) && qobject_cast<const QCheckBox *>(widget))
            return metric == PM_IndicatorWidth ? 36 : 24;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    QSize sizeFromContents(ContentsType type, const QStyleOption *option,
        const QSize &contents, const QWidget *widget) const override
    {
        auto size = QProxyStyle::sizeFromContents(type, option, contents, widget);
        if (type == CT_PushButton || type == CT_ComboBox || type == CT_SpinBox || type == CT_LineEdit)
            size.setHeight(qMax(size.height(), 28));
        if (type == CT_PushButton) size.setWidth(qMax(size.width(), contents.width() + 20));
        if (type == CT_ComboBox || type == CT_SpinBox) size.setWidth(qMax(size.width(), contents.width() + 36));
        return size;
    }

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
        QPainter *painter, const QWidget *widget) const override
    {
        const auto &palette = option->palette;
        const bool enabled = option->state.testFlag(State_Enabled);
        const bool focus = option->state.testFlag(State_HasFocus);
        const bool hover = enabled && option->state.testFlag(State_MouseOver);
        if (element == PE_IndicatorCheckBox && qobject_cast<const QCheckBox *>(widget)) {
            const bool checked = option->state.testFlag(State_On);
            const auto group = enabled ? QPalette::Active : QPalette::Disabled;
            QColor track = checked ? palette.color(group, QPalette::Highlight) : border(palette);
            if (hover) track = track.darker(106);
            const QRectF area(option->rect.center().x() - 16, option->rect.center().y() - 10, 32, 20);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            painter->setPen(Qt::NoPen);
            painter->setBrush(track);
            painter->drawRoundedRect(area, 10, 10);
            painter->setBrush(palette.color(QPalette::Base).lightness() > 128 ? QColor(Qt::white) : palette.color(group, QPalette::Text));
            painter->drawEllipse(QRectF(area.left() + (checked ? 15 : 3), area.top() + 3, 14, 14));
            if (option->state.testFlag(State_NoChange)) {
                painter->setPen(palette.color(group, QPalette::Text));
                painter->drawLine(area.center() + QPointF(-3, 0), area.center() + QPointF(3, 0));
            }
            painter->restore();
            return;
        }
        if (element == PE_PanelButtonCommand || element == PE_PanelButtonTool) {
            const bool checked = option->state.testFlag(State_On);
            const bool pressed = option->state.testFlag(State_Sunken);
            if (element == PE_PanelButtonTool && !hover && !checked && !pressed && !focus) return;
            const bool primary = focus || checked || (qstyleoption_cast<const QStyleOptionButton *>(option)
                && qstyleoption_cast<const QStyleOptionButton *>(option)->features.testFlag(QStyleOptionButton::DefaultButton));
            const auto *button = qobject_cast<const QPushButton *>(widget);
            auto fill = (pressed || hover) ? border(palette) : surface(palette);
            if (element == PE_PanelButtonCommand && button && !button->isFlat() && !button->text().isEmpty()) {
                using UiStyle::ButtonState;
                const auto state = pressed ? ButtonState::Pressed : hover ? ButtonState::Hovered : ButtonState::Normal;
                fill = UiStyle::actionButtonColor(palette, state);
            }
            rounded(painter, option->rect, fill,
                enabled && primary ? palette.color(QPalette::Highlight) : QColor(Qt::transparent));
            return;
        }
        if (element == PE_PanelLineEdit || element == PE_FrameLineEdit) {
            // Embedded editors are painted by their spin/combo owner.
            if (const auto *edit = qobject_cast<const QLineEdit *>(widget); edit && !edit->hasFrame()) return;
            rounded(painter, option->rect, enabled ? palette.color(QPalette::Base) : surface(palette),
                focus ? palette.color(QPalette::Highlight) : border(palette));
            return;
        }
        if (element == PE_FrameGroupBox) {
            rounded(painter, option->rect, palette.color(QPalette::Base), border(palette), 12);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    QRect subControlRect(ComplexControl control, const QStyleOptionComplex *option,
        SubControl sub, const QWidget *widget) const override
    {
        const auto rect = option->rect;
        const auto visual = [&](QRect area) { return visualRect(option->direction, rect, area); };
        if (control == CC_ComboBox) {
            if (sub == SC_ComboBoxArrow) return visual(QRect(rect.right() - 23, rect.top() + 1, 23, rect.height() - 2));
            if (sub == SC_ComboBoxEditField) return visual(rect.adjusted(8, 2, -28, -2));
        }
        if (control == CC_SpinBox) {
            const auto *spin = qstyleoption_cast<const QStyleOptionSpinBox *>(option);
            if (spin) {
                const bool buttons = spin->buttonSymbols != QAbstractSpinBox::NoButtons;
                const int half = rect.height() / 2;
                if (sub == SC_SpinBoxUp) return buttons ? visual(QRect(rect.right() - 20, rect.top() + 1, 20, half - 1)) : QRect();
                if (sub == SC_SpinBoxDown) return buttons ? visual(QRect(rect.right() - 20, rect.top() + half, 20, rect.height() - half - 1)) : QRect();
                if (sub == SC_SpinBoxEditField) return visual(rect.adjusted(8, 2, buttons ? -26 : -8, -2));
            }
        }
        return QProxyStyle::subControlRect(control, option, sub, widget);
    }

    SubControl hitTestComplexControl(ComplexControl control, const QStyleOptionComplex *option,
        const QPoint &point, const QWidget *widget) const override
    {
        if (control == CC_ComboBox || control == CC_SpinBox) {
            const auto buttons = control == CC_ComboBox ? QList<SubControl>{SC_ComboBoxArrow}
                : QList<SubControl>{SC_SpinBoxUp, SC_SpinBoxDown};
            for (const auto sub : buttons)
                if (subControlRect(control, option, sub, widget).contains(point)) return sub;
            return control == CC_ComboBox ? SC_ComboBoxEditField : SC_SpinBoxEditField;
        }
        return QProxyStyle::hitTestComplexControl(control, option, point, widget);
    }

    void drawComplexControl(ComplexControl control, const QStyleOptionComplex *option,
        QPainter *painter, const QWidget *widget) const override
    {
        if (control != CC_ComboBox && control != CC_SpinBox) {
            QProxyStyle::drawComplexControl(control, option, painter, widget);
            return;
        }
        const auto &palette = option->palette;
        const bool enabled = option->state.testFlag(State_Enabled);
        const bool focus = option->state.testFlag(State_HasFocus) || (widget && widget->hasFocus());
        rounded(painter, option->rect, enabled ? palette.color(QPalette::Base) : surface(palette),
            focus ? palette.color(QPalette::Highlight) : border(palette));
        const auto color = [&](bool active) { return palette.color(enabled && active ? QPalette::Active : QPalette::Disabled, QPalette::Text); };
        if (control == CC_ComboBox) {
            chevron(painter, subControlRect(control, option, SC_ComboBoxArrow, widget), color(true), false);
            return;
        }
        const auto *spin = qstyleoption_cast<const QStyleOptionSpinBox *>(option);
        if (!spin || spin->buttonSymbols == QAbstractSpinBox::NoButtons) return;
        for (const auto sub : {SC_SpinBoxUp, SC_SpinBoxDown}) {
            const bool up = sub == SC_SpinBoxUp;
            const auto area = subControlRect(control, option, sub, widget);
            const bool canStep = spin->stepEnabled.testFlag(up ? QAbstractSpinBox::StepUpEnabled : QAbstractSpinBox::StepDownEnabled);
            if (enabled && canStep && option->activeSubControls.testFlag(sub) && option->state.testFlag(State_MouseOver))
                rounded(painter, area, surface(palette), Qt::transparent, 4);
            painter->save();
            painter->setPen(border(palette));
            const int x = option->direction == Qt::RightToLeft ? area.right() : area.left();
            painter->drawLine(x, area.top(), x, area.bottom());
            if (spin->buttonSymbols == QAbstractSpinBox::PlusMinus) {
                painter->setPen(color(canStep));
                painter->drawText(area, Qt::AlignCenter, up ? "+" : QString::fromUtf8("−"));
            } else chevron(painter, area, color(canStep), up);
            painter->restore();
        }
    }
};

class OptionWheelGuard final : public QObject {
public:
    explicit OptionWheelGuard(QApplication &app) : QObject(&app) {}
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() != QEvent::Wheel) return false;
        auto *target = qobject_cast<QWidget *>(object);
        QWidget *control = nullptr;
        for (auto *parent = target; parent; parent = parent->parentWidget()) {
            if (qobject_cast<QScrollBar *>(parent)) return false;
            if (qobject_cast<QComboBox *>(parent) || qobject_cast<QAbstractSpinBox *>(parent)
                || qobject_cast<QSlider *>(parent)) { control = parent; break; }
        }
        if (!control) return false; // Includes 3D cameras, tables and ordinary page scrolling.
        const auto *wheel = static_cast<QWheelEvent *>(event);
        // Popup lists consume wheels locally; they must not edit the selection
        // or scroll the page behind the popup.
        for (auto *parent = target->window() == control->window() ? control->parentWidget() : nullptr;
            parent; parent = parent->parentWidget()) {
            if (auto *scroll = qobject_cast<QAbstractScrollArea *>(parent)) {
                QWheelEvent pageWheel(scroll->viewport()->mapFromGlobal(wheel->globalPosition()), wheel->globalPosition(),
                    wheel->pixelDelta(), wheel->angleDelta(), wheel->buttons(), wheel->modifiers(), wheel->phase(), wheel->inverted(),
                    wheel->source(), wheel->pointingDevice());
                QApplication::sendEvent(scroll->viewport(), &pageWheel);
                break;
            }
        }
        event->accept();
        return true;
    }
};

class UnitLabel final : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setPen(palette().color(QPalette::Base).lightness() < 128 ? QColor("#999da5") : QColor("#858585"));
        painter.drawText(contentsRect(), alignment(), text());
    }
};

class WrappingRow final : public QLayout {
public:
    WrappingRow(QWidget *parent, int gap) : QLayout(parent)
    {
        setContentsMargins(0, 0, 0, 0);
        setSpacing(gap);
    }
    ~WrappingRow() override
    {
        while (auto *item = takeAt(0)) delete item;
    }
    void addItem(QLayoutItem *item) override { m_items.append(item); invalidate(); }
    int count() const override { return m_items.size(); }
    QLayoutItem *itemAt(int index) const override { return m_items.value(index); }
    QLayoutItem *takeAt(int index) override
    {
        if (!itemAt(index)) return nullptr;
        auto *item = m_items.takeAt(index);
        invalidate();
        return item;
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize sizeHint() const override
    {
        QSize hint(0, 0);
        for (auto *item : m_items) {
            if (item->isEmpty()) continue;
            const auto size = item->sizeHint().expandedTo(item->minimumSize());
            hint.setWidth(hint.width() + (hint.width() ? spacing() : 0) + size.width());
            hint.setHeight(qMax(hint.height(), size.height()));
        }
        return hint;
    }
    QSize minimumSize() const override
    {
        QSize result(0, 0);
        for (auto *item : m_items)
            if (!item->isEmpty()) result = result.expandedTo(item->minimumSize());
        return result;
    }
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        arrange(rect, true);
    }

private:
    int arrange(const QRect &rect, bool place) const
    {
        int x = 0, y = 0, lineHeight = 0;
        QList<QPair<QLayoutItem *, QRect>> row;
        const auto placeRow = [&] {
            for (const auto &[item, cell] : row)
                item->setGeometry(cell.translated(rect.x(), rect.y() + y + (lineHeight - cell.height()) / 2));
            row.clear();
        };
        for (auto *item : m_items) {
            if (item->isEmpty()) continue;
            const auto minimum = item->minimumSize();
            auto size = item->sizeHint().expandedTo(minimum);
            size.setWidth(qBound(minimum.width(), size.width(), qMax(minimum.width(), rect.width())));
            if (x > 0 && x + size.width() > rect.width()) {
                placeRow();
                x = 0;
                y += lineHeight + spacing();
                lineHeight = 0;
            }
            if (place) row.append({item, QRect(QPoint(x, 0), size)});
            x += size.width() + spacing();
            lineHeight = qMax(lineHeight, size.height());
        }
        placeRow();
        return y + lineHeight;
    }
    QList<QLayoutItem *> m_items;
};
}

QLayout *UiStyle::wrappingRow(QWidget *parent, int spacing)
{
    return new WrappingRow(parent, spacing);
}

void UiStyle::install(QApplication &application)
{
    if (application.property("uiSettingsStyleInstalled").toBool()) return;
    application.setProperty("uiSettingsStyleInstalled", true);
    application.setStyle(new SettingsStyle);
    application.installEventFilter(new OptionWheelGuard(application));
}

QWidget *UiStyle::numberField(QAbstractSpinBox *editor)
{
    auto *row = new QWidget(editor->parentWidget());
    row->setProperty("uiNumberField", true);
    row->setFocusProxy(editor);
    row->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    editor->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(editor);
    auto *unit = new UnitLabel(editor->property("uiUnitText").toString(), row);
    unit->setObjectName("uiNumberUnit");
    layout->addWidget(unit);
    return row;
}

void UiStyle::setUnit(QAbstractSpinBox *editor, const QString &unit)
{
    editor->setProperty("uiUnitText", unit.trimmed());
    editor->setAccessibleDescription(unit.trimmed());
    if (auto *spin = qobject_cast<QDoubleSpinBox *>(editor)) spin->setSuffix({});
    if (auto *spin = qobject_cast<QSpinBox *>(editor)) spin->setSuffix({});
    if (auto *row = formField(editor); row != editor)
        row->findChild<QLabel *>("uiNumberUnit")->setText(unit.trimmed());
}

QWidget *UiStyle::formField(QWidget *editor)
{
    return editor && editor->parentWidget() && editor->parentWidget()->property("uiNumberField").toBool()
        ? editor->parentWidget() : editor;
}

void UiStyle::card(QWidget *widget)
{
    widget->setObjectName(widget->objectName().isEmpty() ? "uiSettingsCard" : widget->objectName());
    widget->setProperty("uiSettingsCard", true);
    widget->setStyleSheet(QStringLiteral("QWidget[uiSettingsCard=\"true\"] { background: palette(base); border: 1px solid palette(midlight); border-radius: 12px; }"));
}
