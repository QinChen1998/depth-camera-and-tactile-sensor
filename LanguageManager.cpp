#include "LanguageManager.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QLocale>
#include <QtCore/QSettings>

namespace {
constexpr auto LanguageSettingKey = "ui/language";

QSettings applicationSettings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QCoreApplication::organizationName(),
                     QCoreApplication::applicationName());
}
}

LanguageManager::LanguageManager(QObject *parent)
    : QObject(parent)
{
    QSettings settings = applicationSettings();
    const QString configured = settings.value(
        QString::fromLatin1(LanguageSettingKey)).toString();
    if (configured.isEmpty())
        setLanguage(defaultLanguage(), false);
    else
        setLanguageCode(configured, false);
}

LanguageManager::~LanguageManager()
{
    QCoreApplication::removeTranslator(&m_applicationTranslator);
}

LanguageManager::Language LanguageManager::language() const
{
    return m_language;
}

QString LanguageManager::languageCode() const
{
    return m_language == Language::SimplifiedChinese
        ? QStringLiteral("zh_CN") : QStringLiteral("en");
}

bool LanguageManager::setLanguage(Language language, bool persist)
{
    QCoreApplication::removeTranslator(&m_applicationTranslator);
    if (language == Language::SimplifiedChinese) {
        if (!m_applicationTranslator.load(
                QStringLiteral(":/i18n/hand_zh_CN.qm"))) {
            if (m_language == Language::SimplifiedChinese)
                m_language = Language::English;
            return false;
        }
        QCoreApplication::installTranslator(&m_applicationTranslator);
    }

    const bool changed = m_language != language;
    m_language = language;
    QLocale::setDefault(language == Language::SimplifiedChinese
                            ? QLocale(QLocale::Chinese, QLocale::China)
                            : QLocale(QLocale::English, QLocale::UnitedStates));
    if (persist) {
        QSettings settings = applicationSettings();
        settings.setValue(QString::fromLatin1(LanguageSettingKey), languageCode());
        settings.sync();
    }
    if (changed)
        emit languageChanged(m_language);
    return true;
}

bool LanguageManager::setLanguageCode(const QString &languageCode, bool persist)
{
    return setLanguage(languageCode.compare(QStringLiteral("zh_CN"),
                                            Qt::CaseInsensitive) == 0
                           ? Language::SimplifiedChinese
                           : Language::English,
                       persist);
}

LanguageManager::Language LanguageManager::defaultLanguage()
{
    return QLocale::system().language() == QLocale::Chinese
        ? Language::SimplifiedChinese : Language::English;
}
