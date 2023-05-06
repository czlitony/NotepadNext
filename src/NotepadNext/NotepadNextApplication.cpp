/*
 * This file is part of Notepad Next.
 * Copyright 2019 Justin Dailey
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */


#include "MainWindow.h"
#include "NotepadNextApplication.h"
#include "RecentFilesListManager.h"
#include "EditorManager.h"
#include "LuaExtension.h"
#include "DebugManager.h"
#include "SessionManager.h"

#include "LuaState.h"
#include "lua.hpp"
#include "LuaBridge.h"

#include "EditorConfigAppDecorator.h"

#include "ILexer.h"
#include "Lexilla.h"

#include <QCommandLineParser>
#include <QSettings>

#ifdef Q_OS_WIN
#include <Windows.h>
#endif

const SingleApplication::Options opts = SingleApplication::ExcludeAppPath | SingleApplication::ExcludeAppVersion | SingleApplication::SecondaryNotification;

template <>
struct luabridge::Stack <QString const&>
{
    static void push (lua_State* L, QString const &s)
    {
        lua_pushlstring(L, s.toLatin1().constData(), s.toLatin1().size());
    }

    static QString get (lua_State* L, int index)
    {
        return QString(luaL_checkstring (L, index));
    }
};

void parseCommandLine(QCommandLineParser &parser, const QStringList &args)
{
    parser.setApplicationDescription("Notepad Next");
    parser.addHelpOption();
    parser.addVersionOption();

    parser.addPositionalArgument("files", "Files to open.");

    parser.addOptions({
        {"translation", "Overrides the system default translation.", "translation"},
        {"reset-settings", "Resets all application settings."},
    });

    parser.process(args);
}

NotepadNextApplication::NotepadNextApplication(int &argc, char **argv)
    : SingleApplication(argc, argv, true, opts)
{
#ifdef Q_OS_WIN
    // Create a system-wide mutex so the installer can detect if it is running
    CreateMutex(NULL, false, L"NotepadNextMutex");
#endif
    parseCommandLine(parser, arguments());

    DebugManager::manageDebugOutput();
    DebugManager::pauseDebugOutput();
}

bool NotepadNextApplication::init()
{
    qInfo(Q_FUNC_INFO);

    setWindowIcon(QIcon(QStringLiteral(":/icons/NotepadNext.png")));

    if (parser.isSet("reset-settings")) {
        QSettings settings;
        settings.clear();
    }

    if (!parser.value("translation").isEmpty()) {
        loadTranslation(QLocale(parser.value("translation")));
    }
    else {
        loadSystemDefaultTranslation();
    }

    luaState = new LuaState();

    recentFilesListManager = new RecentFilesListManager(this);
    editorManager = new EditorManager(this);
    settings = new Settings(this);
    sessionManager = new SessionManager();

    connect(editorManager, &EditorManager::editorCreated, recentFilesListManager, [=](ScintillaNext *editor) {
        if (editor->isFile()) {
            recentFilesListManager->removeFile(editor->getFilePath());
        }
    });

    connect(editorManager, &EditorManager::editorClosed, recentFilesListManager, [=](ScintillaNext *editor) {
        if (editor->isFile()) {
            recentFilesListManager->addFile(editor->getFilePath());
        }
    });

    loadSettings();

    connect(this, &NotepadNextApplication::aboutToQuit, this, &NotepadNextApplication::saveSettings);

    EditorConfigAppDecorator *ecad = new EditorConfigAppDecorator(this);
    ecad->setEnabled(true);

    luaState->executeFile(":/scripts/init.lua");
    LuaExtension::Instance().Initialise(luaState->L, Q_NULLPTR);

    // LuaBridge is not a long term solution
    // This is probably temporary, but it is quick and works
    luabridge::setHideMetatables(false);
    luabridge::getGlobalNamespace(luaState->L)
        .beginNamespace("nn")
            .beginClass<Settings>("Settings")
                .addFunction("showMenuBar", &Settings::setShowMenuBar)
                .addFunction("showToolBar", &Settings::setShowToolBar)
                .addFunction("showTabBar", &Settings::setShowTabBar)
                .addFunction("showStatusBar", &Settings::setShowStatusBar)
            .endClass()
        .endNamespace();
    luabridge::setGlobal(luaState->L, settings, "settings");

    createNewWindow();
    connect(editorManager, &EditorManager::editorCreated, windows.first(), &MainWindow::addEditor);

    luabridge::getGlobalNamespace(luaState->L)
        .beginNamespace("nn")
            .beginClass<QWidget>("QWidget")
                .addFunction("exit", &QWidget::close)
            .endClass()
            .deriveClass<MainWindow, QWidget>("MainWindow")
                .addFunction("newFile", &MainWindow::newFile)
                .addFunction("openFile", &MainWindow::openFile)
                .addFunction("openFileDialog", &MainWindow::openFileDialog)
                .addFunction("reloadFile", &MainWindow::reloadFile)
                .addFunction("saveFile", &MainWindow::saveCurrentFile)
                .addFunction("saveFileAs", &MainWindow::saveCurrentFileAs)
                .addFunction("closeFile", &MainWindow::closeCurrentFile)
            .endClass()
        .endNamespace();
    luabridge::setGlobal(luaState->L, windows.first(), "window");

    // If the application is activated (e.g. user switching to another program and them back) the focus
    // needs to be reset on whatever object previously had focus (e.g. the find dialog)
    connect(this, &NotepadNextApplication::focusChanged, this, [&](QWidget *old, QWidget *now) {
        Q_UNUSED(old);
        if (now) {
            currentlyFocusedWidget = now;
        }
    });

    connect(this, &SingleApplication::instanceStarted, windows.first(), &MainWindow::bringWindowToForeground);

    connect(this, &SingleApplication::receivedMessage, this, [&](quint32 instanceId, QByteArray message) {
        Q_UNUSED(instanceId)

        QDataStream stream(&message, QIODevice::ReadOnly);
        QStringList args;

        stream >> args;

        QCommandLineParser parser;
        parseCommandLine(parser, args);

        openFiles(parser.positionalArguments());
    }, Qt::QueuedConnection);

    connect(this, &NotepadNextApplication::applicationStateChanged, this, [&](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {

            // Make sure it is active...
            // The application can be active without the main window being show e.g. if there is a
            // message box that pops up before the main window
            if (windows.first()->isActiveWindow()) {
                windows.first()->focusIn();
            }

            if (!currentlyFocusedWidget.isNull()) {
                currentlyFocusedWidget->activateWindow();
            }
        }
    });

    if (settings->restorePreviousSession()) {
        qInfo("Restoring previous session");

        sessionManager->loadSession(windows.first(), editorManager);
    }

    openFiles(parser.positionalArguments());

    // If the window does not have any editors (meaning the no files were
    // specified on the command line) then create a new empty file
    if (windows.first()->editorCount() == 0) {
        windows.first()->newFile();
    }

    // Everything should be ready at this point

    windows.first()->restoreWindowState();
    windows.first()->show();

    DebugManager::resumeDebugOutput();

    return true;
}

SessionManager *NotepadNextApplication::getSessionManager() const
{
    SessionManager::SessionFileTypes fileTypes;

    if (settings->restorePreviousSession()) {
        fileTypes |= SessionManager::SavedFile;
    }

    if (settings->restoreUnsavedFiles()) {
        fileTypes |= SessionManager::UnsavedFile;
    }

    if (settings->restoreTempFiles()) {
        fileTypes |= SessionManager::TempFile;
    }

    // Update the file types supported in case something has changed in the settings
    sessionManager->setSessionFileTypes(fileTypes);

    return sessionManager;
}

QString NotepadNextApplication::getFileDialogFilter() const
{
    return getLuaState()->executeAndReturn<QString>("return DialogFilters()");
}

QStringList NotepadNextApplication::getLanguages() const
{
    return getLuaState()->executeAndReturn<QStringList>(
                R"(
                local names = {}
                for k in pairs(languages) do table.insert(names, k) end
                table.sort(names, function (a, b) return string.lower(a) < string.lower(b) end)
                return names
                )");
}

void NotepadNextApplication::setEditorLanguage(ScintillaNext *editor, const QString &languageName) const
{
    LuaExtension::Instance().setEditor(editor);

    getLuaState()->execute(QString("languageName = \"%1\"").arg(languageName).toLatin1().constData());
    const QString lexer = getLuaState()->executeAndReturn<QString>("return languages[languageName].lexer");

    editor->languageName = languageName;
    editor->languageSingleLineComment = getLuaState()->executeAndReturn<QString>("return languages[languageName].singleLineComment or \"\"").toUtf8();

    auto lexerInstance = CreateLexer(lexer.toLatin1().constData());
    editor->setILexer((sptr_t) lexerInstance);
    editor->clearDocumentStyle(); // Remove all previous style information, setting the lexer does not guarantee styling information is cleared

    // Not ideal this has to be manually emitted but it works since setILexer() is not widely used
    emit editor->lexerChanged();

    // Dynamic properties can be used to skip part of the default initialization. The value in the
    // property doesn't currently matter, but may be used at a later point.
    getLuaState()->execute(QString("skip_tabs = %1").arg(editor->QObject::property("nn_skip_usetabs").isValid() ? "true" : "false").toLatin1().constData());
    getLuaState()->execute(QString("skip_tabwidth = %1").arg(editor->QObject::property("nn_skip_tabwidth").isValid() ? "true" : "false").toLatin1().constData());

    getLuaState()->execute(R"(
        local L = languages[languageName]

        if not skip_tabs then
            editor.UseTabs = (L.tabSettings or "tabs") == "tabs"
        end
        if not skip_tabwidth then
            editor.TabWidth = L.tabSize or 4
        end

        editor.MarginWidthN[2] = L.disableFoldMargin and 0 or 16
        if L.styles then
            for name, style in pairs(L.styles) do
                editor.StyleFore[style.id] = style.fgColor
                editor.StyleBack[style.id] = style.bgColor

                if style.fontStyle then
                    editor.StyleBold[style.id] = (style.fontStyle & 1 == 1)
                    editor.StyleItalic[style.id] = (style.fontStyle & 2 == 2)
                    editor.StyleUnderline[style.id] = (style.fontStyle & 4 == 4)
                    editor.StyleEOLFilled[style.id] = (style.fontStyle & 8 == 8)
                end
            end
        end
        if L.keywords then
            for id, kw in pairs(L.keywords) do
                editor.KeyWords[id] = kw
            end
        end
        if L.properties then
            for p,v in pairs(L.properties) do
                editor.Property[p] = v
            end
        end

        editor.Property["fold"] = "1"
        editor.Property["fold.compact"] = "0"
    )");
}

QString NotepadNextApplication::detectLanguage(ScintillaNext *editor) const
{
    qInfo(Q_FUNC_INFO);

    QString language_name = QStringLiteral("Text");

    if (editor->isFile()) {
        language_name = detectLanguageFromExtension(editor->getFileInfo().suffix());
    }

    if (language_name == QStringLiteral("Text")) {
        language_name = detectLanguageFromContents(editor);
    }

    return language_name;
}

QString NotepadNextApplication::detectLanguageFromExtension(const QString &extension) const
{
    qInfo(Q_FUNC_INFO);

    return getLuaState()->executeAndReturn<QString>(QString(R"(
    local ext = "%1"
    for name, L in pairs(languages) do
        if L.extensions then
            for _, v in ipairs(L.extensions) do
                if v == ext then
                    return name
                end
            end
        end
    end
    return "Text"
    )").arg(extension).toLatin1().constData());
}

QString NotepadNextApplication::detectLanguageFromContents(ScintillaNext *editor) const
{
    qInfo(Q_FUNC_INFO);

    LuaExtension::Instance().setEditor(editor);

    return getLuaState()->executeAndReturn<QString>(QString(R"(
    -- Grab a small chunk
    if editor.Length > 0 then
        editor:SetTargetRange(0, math.min(64, editor.Length))
        return detectLanguageFromContents(editor.TargetText)
    end

    return "Text"
    )").toLatin1().constData());
}

void NotepadNextApplication::loadSystemDefaultTranslation()
{
    loadTranslation(QLocale(QLocale::system().name()));
}

void NotepadNextApplication::loadTranslation(QLocale locale)
{
    qInfo(Q_FUNC_INFO);

    // Translation files are stored as a qresource
    const QString languagePath = QStringLiteral(":/i18n/");

    // Load translation for NotepadNext e.g. "i18n/NotepadNext.en.qm"
    if (translatorNpn.load(locale, QApplication::applicationName(), QString("_"), languagePath)) {
        installTranslator(&translatorNpn);
        qInfo("Loaded %s translation %s for Notepad Next", qUtf8Printable(locale.name()), qUtf8Printable(translatorNpn.filePath()));
    } else {
        qInfo("%s translation not found for Notepad Next", qUtf8Printable(locale.name()));
    }

    // Load translation for Qt components e.g. "i18n/qt_en.qm"
    if (translatorQt.load(locale, QString("qt"), QString("_"), languagePath)) {
        installTranslator(&translatorQt);
        qInfo("Loaded %s translation %s for Qt components", qUtf8Printable(locale.name()), qUtf8Printable(translatorQt.filePath()));
    } else {
        qInfo("%s translation not found for Qt components", qUtf8Printable(locale.name()));
    }
}

void NotepadNextApplication::sendInfoToPrimaryInstance()
{
    QByteArray buffer;
    QDataStream stream(&buffer, QIODevice::WriteOnly);

    stream << arguments();
    sendMessage(buffer);
}

bool NotepadNextApplication::event(QEvent *event)
{
    // Handle the QFileOpenEvent to open files on MacOS X.
    if (event->type() == QEvent::FileOpen) {
        QFileOpenEvent *fileOpenEvent = static_cast<QFileOpenEvent*>(event);

        qInfo("QFileOpenEvent %s", qUtf8Printable(fileOpenEvent->file()));

        openFiles(QStringList() << fileOpenEvent->file());

        return true;
    }

    return SingleApplication::event(event);
}

void NotepadNextApplication::openFiles(const QStringList &files)
{
    qInfo(Q_FUNC_INFO);

    for (const QString &file : files) {
        windows.first()->openFile(file);
    }
}

void NotepadNextApplication::loadSettings()
{
    QSettings qsettings;

    settings->setRestorePreviousSession(qsettings.value("App/RestorePreviousSession", false).toBool());
    settings->setRestoreUnsavedFiles(qsettings.value("App/RestoreUnsavedFiles", false).toBool());
    settings->setRestoreTempFiles(qsettings.value("App/RestoreTempFiles", false).toBool());
    recentFilesListManager->setFileList(qsettings.value("App/RecentFilesList").toStringList());
}

void NotepadNextApplication::saveSettings()
{
    QSettings qsettings;

    qsettings.setValue("App/RestorePreviousSession", settings->restorePreviousSession());
    qsettings.setValue("App/RestoreUnsavedFiles", settings->restoreUnsavedFiles());
    qsettings.setValue("App/RestoreTempFiles", settings->restoreTempFiles());
    qsettings.setValue("App/RecentFilesList", recentFilesListManager->fileList());
}

MainWindow *NotepadNextApplication::createNewWindow()
{
    MainWindow *w = new MainWindow(this);

    windows.append(w);

    // Keep Lua's editor reference up to date
    connect(w, &MainWindow::editorActivated, this, [](ScintillaNext *editor) {
        LuaExtension::Instance().setEditor(editor);
    });

    // Since these editors don't actually get "closed" go ahead and add them to the recent file list
    connect(w, &MainWindow::aboutToClose, this, [=]() {
        for (const auto &editor : w->editors()) {
            if (editor->isFile()) {
                recentFilesListManager->addFile(editor->getFilePath());
            }
        }

        getSessionManager()->saveSession(w);
    });

    return w;
}
