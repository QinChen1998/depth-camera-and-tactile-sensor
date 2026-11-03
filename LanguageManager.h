#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTranslator>

class LanguageManager final : public QObject
{
    Q_OBJECT

public:
    enum class Language
    {
        English,
        SimplifiedChinese
    };
    Q_ENUM(Language)

    explicit LanguageManager(QObject *parent = nullptr);
    ~LanguageManager() override;

    Language language() const;
    QString languageCode() const;
    bool setLanguage(Language language, bool persist = true);
    bool setLanguageCode(const QString &languageCode, bool persist = true);

signals:
    void languageChanged(LanguageManager::Language language);

private:
    static Language defaultLanguage();

    Language m_language = Language::English;
    QTranslator m_applicationTranslator;
};
