#pragma once

#include <QEvent>
#include <QQuickItem>
#include <QQuickWidget>
#include <QStringList>
#include <functional>
#include <utility>

class DockableQuick3DWidget final : public QQuickWidget {
public:
    DockableQuick3DWidget(QWidget *parent, QStringList sceneProperties)
        : QQuickWidget(parent), m_sceneProperties(std::move(sceneProperties)) {}

    std::function<void()> renderWindowChanged;

protected:
    bool event(QEvent *event) override
    {
        if (event->type() != QEvent::WindowChangeInternal || !rootObject())
            return QQuickWidget::event(event);

        // Qt 6.11.1 replaces the offscreen QQuickWindow when docking. Recreate
        // the scene so spatial nodes never migrate between scene managers.
        const auto url = source();
        QVariantMap state;
        for (const auto &name : m_sceneProperties)
            state.insert(name, rootObject()->property(name.toLatin1().constData()));
        setSource({});
        const bool handled = QQuickWidget::event(event);
        setInitialProperties(state);
        setSource(url);
        if (renderWindowChanged)
            renderWindowChanged();
        return handled;
    }

private:
    const QStringList m_sceneProperties;
};
