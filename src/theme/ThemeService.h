#pragma once
#include <QColor>
#include <QObject>

namespace gromarch {

// Reads the active Omarchy theme (~/.config/omarchy/current/theme/colors.toml,
// semantic keys with legacy bg/fg fallbacks) and watches for theme switches
// (symlink + file watcher). Falls back to a built-in dark palette when no
// Omarchy theme is present, or honors $GROMARCH_THEME pointing at a colors.toml.
// Exposed to QML as the singleton `Theme`.
class ThemeService : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor background READ background NOTIFY themeChanged)
    Q_PROPERTY(QColor darkBackground READ darkBackground NOTIFY themeChanged)
    Q_PROPERTY(QColor darkerBackground READ darkerBackground NOTIFY themeChanged)
    Q_PROPERTY(QColor lighterBackground READ lighterBackground NOTIFY themeChanged)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY themeChanged)
    Q_PROPERTY(QColor mutedForeground READ mutedForeground NOTIFY themeChanged)
    Q_PROPERTY(QColor brightForeground READ brightForeground NOTIFY themeChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY themeChanged)
    Q_PROPERTY(QColor selection READ selection NOTIFY themeChanged)
    Q_PROPERTY(QColor muted READ muted NOTIFY themeChanged)
    Q_PROPERTY(QColor red READ red NOTIFY themeChanged)
    Q_PROPERTY(QColor blue READ blue NOTIFY themeChanged)
    Q_PROPERTY(QString monoFont READ monoFont NOTIFY themeChanged)
    Q_PROPERTY(QString uiFont READ uiFont NOTIFY themeChanged)
public:
    explicit ThemeService(QObject* parent = nullptr);
    ~ThemeService() override;

    QColor background() const;
    QColor darkBackground() const;
    QColor darkerBackground() const;
    QColor lighterBackground() const;
    QColor foreground() const;
    QColor mutedForeground() const;
    QColor brightForeground() const;
    QColor accent() const;
    QColor selection() const;
    QColor muted() const;
    QColor red() const;
    QColor blue() const;
    QString monoFont() const;   // "CaskaydiaMono Nerd Font" fallback "monospace"
    QString uiFont() const;

public slots:
    void reload();              // also invoked by `gromarch --retint` via local socket

signals:
    void themeChanged();
};

} // namespace gromarch
