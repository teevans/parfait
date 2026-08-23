#include "theme/ThemeService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFontDatabase>
#include <QHash>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

namespace gromarch {
namespace {

// The header is a frozen contract with no data members, so per-instance state
// lives here and is torn down when the object is destroyed.
struct Palette {
    QColor background;
    QColor darkBackground;
    QColor darkerBackground;
    QColor lighterBackground;
    QColor foreground;
    QColor mutedForeground;
    QColor brightForeground;
    QColor accent;
    QColor selection;
    QColor muted;
    QColor red;
    QColor blue;
};

struct Priv {
    Palette palette;
    QString path;                 // resolved colors.toml (may be empty)
    QString monoFont;
    QString uiFont;
    QFileSystemWatcher* watcher = nullptr;
    QTimer* debounce = nullptr;
};

QHash<const ThemeService*, Priv*>& registry() {
    static QHash<const ThemeService*, Priv*> map;
    return map;
}

Priv* priv(const ThemeService* self) {
    return registry().value(self);
}

// ---- built-in fallback (Tokyo Night) ----------------------------------------

Palette builtinPalette() {
    Palette p;
    p.background = QColor("#1a1b26");
    p.darkBackground = QColor("#16161e");
    p.darkerBackground = QColor("#101014");
    p.lighterBackground = QColor("#24283b");
    p.foreground = QColor("#c0caf5");
    p.brightForeground = QColor("#e8ecfb");
    p.accent = QColor("#7aa2f3");
    p.selection = QColor("#33467c");
    p.muted = QColor("#565f89");
    p.red = QColor("#f7768e");
    p.blue = QColor("#7aa2f7");
    p.mutedForeground = QColor("#7f88ad");
    return p;
}

// ---- tiny TOML subset parser -------------------------------------------------

// Accepts `key = "value"` / `key = 'value'` / bare values, ignores comments and
// section headers (sections are flattened: the last key wins, and `section.key`
// is also recorded so `colors.background` style files still resolve).
QHash<QString, QString> parseToml(const QString& path) {
    QHash<QString, QString> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return out;

    QTextStream in(&f);
    QString section;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        if (line.startsWith('[')) {
            const int close = line.indexOf(']');
            if (close > 1) section = line.mid(1, close - 1).trimmed();
            continue;
        }
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        QString key = line.left(eq).trimmed();
        QString value = line.mid(eq + 1).trimmed();

        // strip trailing comment on unquoted values
        if (!value.startsWith('"') && !value.startsWith('\'')) {
            const int hash = value.indexOf('#', 1);
            if (hash > 0) value = value.left(hash).trimmed();
        }
        if (value.size() >= 2 &&
            ((value.startsWith('"') && value.endsWith('"')) ||
             (value.startsWith('\'') && value.endsWith('\'')))) {
            value = value.mid(1, value.size() - 2);
        }
        key = key.remove('"').remove('\'').toLower();
        if (key.isEmpty() || value.isEmpty()) continue;

        out.insert(key, value);                                  // flattened
        if (!section.isEmpty()) out.insert(section.toLower() + '.' + key, value);
    }
    return out;
}

QColor colorFor(const QHash<QString, QString>& kv, const QStringList& keys) {
    for (const QString& k : keys) {
        const QString v = kv.value(k);
        if (v.isEmpty()) continue;
        QString hex = v.trimmed();
        if (!hex.startsWith('#') &&
            QRegularExpression(QStringLiteral("^[0-9a-fA-F]{6}([0-9a-fA-F]{2})?$")).match(hex).hasMatch()) {
            hex.prepend('#');
        }
        const QColor c(hex);
        if (c.isValid()) return c;
    }
    return QColor();
}

QColor blend(const QColor& a, const QColor& b, qreal t) {
    return QColor::fromRgbF(a.redF() * (1.0 - t) + b.redF() * t,
                            a.greenF() * (1.0 - t) + b.greenF() * t,
                            a.blueF() * (1.0 - t) + b.blueF() * t);
}

bool isDark(const QColor& c) {
    return c.lightnessF() < 0.5;
}

// ---- theme file resolution ---------------------------------------------------

QString resolveThemePath() {
    const QString env = QProcessEnvironment::systemEnvironment().value(
        QStringLiteral("GROMARCH_THEME"));
    if (!env.isEmpty() && QFileInfo::exists(env)) return env;

    const QString home = QDir::homePath();
    const QString omarchy =
        home + QStringLiteral("/.config/omarchy/current/theme/colors.toml");
    if (QFileInfo::exists(omarchy)) return omarchy;
    return QString();
}

QString pickFont(const QStringList& candidates, const QString& fallback) {
    const QStringList families = QFontDatabase::families();
    for (const QString& c : candidates) {
        if (families.contains(c, Qt::CaseInsensitive)) return c;
    }
    return fallback;
}

Palette loadPalette(const QString& path) {
    if (path.isEmpty()) return builtinPalette();
    const QHash<QString, QString> kv = parseToml(path);
    if (kv.isEmpty()) return builtinPalette();

    const Palette fb = builtinPalette();
    Palette p;

    p.background = colorFor(kv, {"background", "bg", "base", "color0"});
    if (!p.background.isValid()) p.background = fb.background;
    p.foreground = colorFor(kv, {"foreground", "fg", "text", "color7"});
    if (!p.foreground.isValid()) p.foreground = fb.foreground;

    const bool dark = isDark(p.background);

    p.darkBackground = colorFor(kv, {"dark_background", "background_dark", "mantle"});
    if (!p.darkBackground.isValid())
        p.darkBackground = dark ? p.background.darker(115) : p.background.darker(105);

    p.darkerBackground = colorFor(kv, {"darker_background", "crust"});
    if (!p.darkerBackground.isValid())
        p.darkerBackground = dark ? p.background.darker(140) : p.background.darker(112);

    p.lighterBackground =
        colorFor(kv, {"lighter_background", "light_background", "background_light",
                      "surface", "surface0", "color8"});
    if (!p.lighterBackground.isValid())
        p.lighterBackground = dark ? p.background.lighter(150) : p.background.darker(108);

    p.brightForeground =
        colorFor(kv, {"bright_foreground", "light_foreground", "foreground_bright", "color15"});
    if (!p.brightForeground.isValid())
        p.brightForeground = dark ? p.foreground.lighter(115) : p.foreground.darker(120);

    p.accent = colorFor(kv, {"accent", "primary", "color4"});
    if (!p.accent.isValid()) p.accent = fb.accent;

    p.selection = colorFor(kv, {"selection", "selection_background", "highlight"});
    if (!p.selection.isValid()) p.selection = blend(p.background, p.accent, 0.35);

    p.muted = colorFor(kv, {"muted", "dark_foreground", "foreground_dark", "comment", "overlay0"});
    if (!p.muted.isValid())
        p.muted = dark ? p.foreground.darker(160) : p.foreground.lighter(160);

    p.red = colorFor(kv, {"red", "error", "color1"});
    if (!p.red.isValid()) p.red = fb.red;

    p.blue = colorFor(kv, {"blue", "color4"});
    if (!p.blue.isValid()) p.blue = p.accent;

    // mutedForeground: explicit `muted` wins, else 60% of foreground over background.
    const QColor explicitMuted = colorFor(kv, {"muted"});
    p.mutedForeground = explicitMuted.isValid() ? explicitMuted
                                                : blend(p.background, p.foreground, 0.6);
    return p;
}

// Watch the file plus every parent directory up to ~/.config so that a
// `current/theme` symlink swap (which replaces a directory, not the file)
// still wakes us up.
void rewatch(Priv* d) {
    if (!d->watcher) return;
    if (!d->watcher->files().isEmpty()) d->watcher->removePaths(d->watcher->files());
    if (!d->watcher->directories().isEmpty()) d->watcher->removePaths(d->watcher->directories());

    QStringList paths;
    if (!d->path.isEmpty() && QFileInfo::exists(d->path)) paths << d->path;

    QString base = d->path.isEmpty()
                       ? QDir::homePath() + QStringLiteral("/.config/omarchy/current/theme/colors.toml")
                       : d->path;
    QDir dir = QFileInfo(base).dir();
    const QString stopAt = QDir::homePath() + QStringLiteral("/.config");
    for (int i = 0; i < 6; ++i) {
        const QString p = dir.absolutePath();
        if (QFileInfo::exists(p)) paths << p;
        if (p == stopAt || p == QDir::homePath() || p == QStringLiteral("/")) break;
        if (!dir.cdUp()) break;
    }
    paths.removeDuplicates();
    if (!paths.isEmpty()) d->watcher->addPaths(paths);
}

} // namespace

ThemeService::ThemeService(QObject* parent) : QObject(parent) {
    auto* d = new Priv;
    registry().insert(this, d);

    d->monoFont = pickFont({QStringLiteral("CaskaydiaMono Nerd Font"),
                            QStringLiteral("CaskaydiaCove Nerd Font"),
                            QStringLiteral("Cascadia Mono"),
                            QStringLiteral("JetBrainsMono Nerd Font")},
                           QStringLiteral("monospace"));
    d->uiFont = pickFont({QStringLiteral("Inter"),
                          QStringLiteral("Inter Display"),
                          QStringLiteral("Noto Sans")},
                         QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());

    d->path = resolveThemePath();
    d->palette = loadPalette(d->path);

    d->watcher = new QFileSystemWatcher(this);
    d->debounce = new QTimer(this);
    d->debounce->setSingleShot(true);
    d->debounce->setInterval(100);
    connect(d->debounce, &QTimer::timeout, this, &ThemeService::reload);

    const auto kick = [this] {
        if (Priv* p = priv(this)) p->debounce->start();
    };
    connect(d->watcher, &QFileSystemWatcher::fileChanged, this, kick);
    connect(d->watcher, &QFileSystemWatcher::directoryChanged, this, kick);
    rewatch(d);
}

ThemeService::~ThemeService() {
    delete registry().take(this);
}

void ThemeService::reload() {
    Priv* d = priv(this);
    if (!d) return;
    d->path = resolveThemePath();
    d->palette = loadPalette(d->path);
    rewatch(d);
    emit themeChanged();
}

QColor ThemeService::background() const {
    Priv* d = priv(this);
    return d ? d->palette.background : builtinPalette().background;
}
QColor ThemeService::darkBackground() const {
    Priv* d = priv(this);
    return d ? d->palette.darkBackground : builtinPalette().darkBackground;
}
QColor ThemeService::darkerBackground() const {
    Priv* d = priv(this);
    return d ? d->palette.darkerBackground : builtinPalette().darkerBackground;
}
QColor ThemeService::lighterBackground() const {
    Priv* d = priv(this);
    return d ? d->palette.lighterBackground : builtinPalette().lighterBackground;
}
QColor ThemeService::foreground() const {
    Priv* d = priv(this);
    return d ? d->palette.foreground : builtinPalette().foreground;
}
QColor ThemeService::mutedForeground() const {
    Priv* d = priv(this);
    return d ? d->palette.mutedForeground : builtinPalette().mutedForeground;
}
QColor ThemeService::brightForeground() const {
    Priv* d = priv(this);
    return d ? d->palette.brightForeground : builtinPalette().brightForeground;
}
QColor ThemeService::accent() const {
    Priv* d = priv(this);
    return d ? d->palette.accent : builtinPalette().accent;
}
QColor ThemeService::selection() const {
    Priv* d = priv(this);
    return d ? d->palette.selection : builtinPalette().selection;
}
QColor ThemeService::muted() const {
    Priv* d = priv(this);
    return d ? d->palette.muted : builtinPalette().muted;
}
QColor ThemeService::red() const {
    Priv* d = priv(this);
    return d ? d->palette.red : builtinPalette().red;
}
QColor ThemeService::blue() const {
    Priv* d = priv(this);
    return d ? d->palette.blue : builtinPalette().blue;
}

QString ThemeService::monoFont() const {
    Priv* d = priv(this);
    return d ? d->monoFont : QStringLiteral("monospace");
}

QString ThemeService::uiFont() const {
    Priv* d = priv(this);
    return d ? d->uiFont : QStringLiteral("sans-serif");
}

} // namespace gromarch
