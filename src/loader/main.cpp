// SPDX-FileCopyrightText: 2023 - 2026 UnionTech Software Technology Co., Ltd.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "setproctitle.h"
#include "pluginsiteminterface_v2.h"
#include "pluginmanager.h"

#include <DDBusSender>
#include <DApplication>

#include <QCommandLineOption>
#include <QCommandLineParser>

#include <QStringLiteral>

#include <DGuiApplicationHelper>
#include <DStandardPaths>
#include <DPathBuf>
#include <LogManager.h>
#include <qglobal.h>
#ifndef QT_DEBUG
#include <signal.h>
#endif

#include "dqwaylandplatform.h"

DGUI_USE_NAMESPACE
DCORE_USE_NAMESPACE

static char pluginDisplayNameC[256] = {0};

void sig_crash(int signum)
{
    // Only use async-signal-safe operations here
    const char prefix[] = "Tray plugin crashed (signal ";
    char buf[384];
    size_t pos = 0;
    while (pos < sizeof(prefix) - 1 && prefix[pos]) { buf[pos] = prefix[pos]; pos++; }
    // append signal number as decimal
    int n = signum;
    if (n >= 100) { buf[pos++] = '0' + n / 100; n %= 100; }
    if (n >= 10)  { buf[pos++] = '0' + n / 10; n %= 10; }
    buf[pos++] = '0' + n;
    buf[pos++] = ')';
    buf[pos++] = ':';
    buf[pos++] = ' ';
    // append plugin display name if set
    int i = 0;
    while (i < 255 && pluginDisplayNameC[i]) { buf[pos++] = pluginDisplayNameC[i++]; }
    buf[pos++] = '\n';
    write(STDERR_FILENO, buf, pos);

#ifndef QT_DEBUG
    signal(signum, SIG_DFL);
    raise(signum);
#endif
}

class LoaderApplication : public Dtk::Widget::DApplication
{
public:
    LoaderApplication(int &argc, char **argv) : Dtk::Widget::DApplication(argc, argv) {}

    bool notify(QObject *obj, QEvent *event) Q_DECL_OVERRIDE {
        // fix plugin menu cannot show
        if (event->type() == QEvent::OrientationChange) {
            return true;
        }

        return Dtk::Widget::DApplication::notify(obj, event);
    }
};

int main(int argc, char *argv[], char *envp[])
{
#ifndef QT_DEBUG
    // 设置信号处理函数
    struct sigaction sa;
    sa.sa_handler = sig_crash;
    sigemptyset(&sa.sa_mask);
    // 在处理完信号后恢复默认信号处理
    sa.sa_flags = SA_RESETHAND;

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
#endif

    DGuiApplicationHelper::setAttribute(DGuiApplicationHelper::UseInactiveColorGroup, false);
    DGuiApplicationHelper::setAttribute(DGuiApplicationHelper::DontSaveApplicationTheme, true);
    init_setproctitle(argv, envp);
    QMap<QByteArray, QByteArray> oldEnvs;
    oldEnvs["DSG_APP_ID"] = qgetenv("DSG_APP_ID");
    oldEnvs["WAYLAND_DISPLAY"] = qgetenv("WAYLAND_DISPLAY");
    oldEnvs["QT_WAYLAND_SHELL_INTEGRATION"] = qgetenv("QT_WAYLAND_SHELL_INTEGRATION");
    oldEnvs["QT_WAYLAND_RECONNECT"] = qgetenv("QT_WAYLAND_RECONNECT");
    oldEnvs["QT_IM_MODULE"] = qgetenv("QT_IM_MODULE");

    qputenv("DSG_APP_ID", "org.deepin.dde.tray-loader");
    qputenv("WAYLAND_DISPLAY", "dockplugin");
    qputenv("QT_WAYLAND_SHELL_INTEGRATION", "plugin-shell");
    qputenv("QT_WAYLAND_RECONNECT", "1");
    // Force native Wayland text_input protocol as the input method channel.
    // The plugin process connects to the internal dockplugin compositor and cannot
    // directly access the system fcitx/ibus DBus interfaces.
    // The dockplugin compositor proxies IME requests to the outer real compositor
    // via the zwp_text_input family of protocols.
    qputenv("QT_IM_MODULE", "wayland");


    qAddPreRoutine([] () {
        if (!DGuiApplicationHelper::testAttribute(DGuiApplicationHelper::IsXWindowPlatform)) {
            qDebug() << "Register platformInterface.";
            DPlatformInterfaceFactory::registerInterface([] (DPlatformTheme* theme) -> DPlatformInterface *{
                return new DQWaylandPlatformInterface(theme);
            });
        }
    });

    LoaderApplication app(argc, argv);
    app.setOrganizationName("deepin");
    app.setApplicationName("org.deepin.dde.tray-loader");
    app.setQuitOnLastWindowClosed(false);

    QList<QString> translateDirs;
    auto dataDirs = DStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (const auto &path : dataDirs) {
        DPathBuf DPathBuf(path);
        translateDirs << (DPathBuf / "dde-dock/translations").toString();
        translateDirs << (DPathBuf / "trayplugin-loader/translations").toString();
    }
    DGuiApplicationHelper::loadTranslator("dde-dock", translateDirs, QList<QLocale>() << QLocale::system());
    DGuiApplicationHelper::loadTranslator("trayplugin-loader", translateDirs, QList<QLocale>() << QLocale::system());

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption pluginPathsOption(
        "p",
        "plugin path (single path or multiple paths separated by ';').",
        "plugin path(s)"
    );
    QCommandLineOption pluginGroupNameOption(
        "g",
        "Group name for the specified plugin path(s).",
        "group name"
    );

    parser.addOption(pluginPathsOption);
    parser.addOption(pluginGroupNameOption);
    parser.process(app);

    if (!parser.isSet(pluginPathsOption)) {
        qCritical() << "Error: -p is required.";
        parser.showHelp(0);
    }

    auto paths = parser.value(pluginPathsOption);
    auto pluginPaths = paths.split(';', Qt::SkipEmptyParts);

#ifdef QT_DEBUG
    const QDir shellDir(QString("%1/../../plugins/").arg(QCoreApplication::applicationDirPath()));
    if (shellDir.exists()) {
        QCoreApplication::addLibraryPath(shellDir.absolutePath());
    }
#endif

    DLogManager::setLogFormat("%{time}{yy-MM-ddTHH:mm:ss.zzz} [%{type}] [%{category}] <%{function}> %{message}");

    DLogManager::registerConsoleAppender();
    DLogManager::registerFileAppender();

    PluginManager pluginManager;
    pluginManager.setPluginPaths(pluginPaths);
    if (!pluginManager.loadPlugins()) {
        qWarning() << "No valid plugins were loaded.";
        return -1;
    }

    QString pluginGroupName;
    if (parser.isSet(pluginPathsOption)) {
        pluginGroupName = parser.value(pluginGroupNameOption);
    }

    if (pluginGroupName.isEmpty()) {
        pluginGroupName = pluginManager.loadedPlugins()[0]->pluginName();
    }

    app.setApplicationName(pluginGroupName);
    app.setApplicationDisplayName(pluginGroupName);
    setproctitle((QStringLiteral("tray plugin: ") + pluginGroupName).toStdString().c_str());
    {
        QByteArray name = pluginGroupName.toLocal8Bit();
        int copyLen = qMin(name.size(), 255);
        memcpy(pluginDisplayNameC, name.constData(), copyLen);
        pluginDisplayNameC[copyLen] = '\0';
    }
    qunsetenv("QT_SCALE_FACTOR");
    for (auto iter = oldEnvs.begin(); iter != oldEnvs.end(); iter++) {
        if (iter.value().isEmpty()) {
            qunsetenv(iter.key());
        } else {
            qputenv(iter.key(), iter.value());
        }
    }
    return app.exec();
}
