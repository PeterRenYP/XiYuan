#include "workbench.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QStyleFactory>
#include <QTimer>
#include <QIcon>

using namespace Qt::StringLiterals;

int main(int argc, char **argv) {
    // 使用跨平台原生控件风格，深色外观由工作区统一配置。
    QApplication app(argc, argv);
    app.setWindowIcon(QIcon(u":/ui/assets/xiyuan.ico"_s));
    app.setOrganizationName(u"ModelObservatory"_s);
    app.setApplicationName(u"析元"_s);
    app.setApplicationVersion(u"1.0.4"_s);
    app.setStyle(QStyleFactory::create(u"Fusion"_s));
    // 无窗口验证平台不枚举系统字体，显式加载中文字体保证截图可读。
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == u"offscreen"_s)
        QFontDatabase::addApplicationFont(qEnvironmentVariable("WINDIR") + u"/Fonts/msyh.ttc"_s);
    QFont font(u"Microsoft YaHei UI"_s);
    font.setPixelSize(12);
    app.setFont(font);
    const auto arguments = app.arguments();
    // 自动验证使用独立设置目录，避免测试模型和项目进入用户最近列表。
    auto settingsDirectory = qEnvironmentVariable("MODEL_OBSERVATORY_SETTINGS_DIR");
    if (settingsDirectory.isEmpty())
        for (int i = 1; i + 1 < arguments.size(); ++i)
            if (arguments[i].startsWith(u"--verify"_s)) {
                settingsDirectory = QDir(arguments[i + 1]).absolutePath() + u"/界面设置"_s;
                qputenv("MODEL_OBSERVATORY_SETTINGS_DIR", settingsDirectory.toUtf8());
                break;
            }
    if (!settingsDirectory.isEmpty()) {
        QDir().mkpath(settingsDirectory);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory);
    }
    Workbench window(!arguments.contains(u"--no-processes"_s) && !arguments.contains(u"--verify-scale"_s) &&
                     !arguments.contains(u"--verify-project"_s) && !arguments.contains(u"--verify-maintenance"_s) &&
                     !arguments.contains(u"--verify-plan-ui"_s) && !arguments.contains(u"--verify-neurons"_s) &&
                     !arguments.contains(u"--verify-sessions"_s) && !arguments.contains(u"--verify-ui-v2"_s) && !arguments.contains(u"--probe-scale"_s));
    QString weightFile;
    const int fileIndex=arguments.indexOf(u"--open-file"_s);
    if(fileIndex>=0)weightFile=arguments.value(fileIndex+1);
    else for(const auto &argument:arguments.mid(1))
        if(argument.endsWith(u".safetensors"_s,Qt::CaseInsensitive)||argument.endsWith(u".safetensors.index.json"_s,Qt::CaseInsensitive))weightFile=argument;
    if(weightFile.isEmpty())window.show();
    else {window.setProperty("fileOnly",true);window.openWeightFile(weightFile);}
    const auto modeIndex = arguments.indexOf(u"--mode"_s);
    if (modeIndex >= 0)
        window.setMode(arguments.value(modeIndex + 1, u"observe"_s));
    const auto sizeIndex = arguments.indexOf(u"--size"_s);
    if (sizeIndex >= 0) {
        const auto dimensions = arguments.value(sizeIndex + 1).split(QChar(u'x'));
        if (dimensions.size() == 2)
            window.resize(dimensions[0].toInt(), dimensions[1].toInt());
    }
    const auto probe = arguments.indexOf(u"--probe-scale"_s);
    const auto uiTest = arguments.indexOf(u"--verify-ui-v2"_s);
    if(uiTest>=0){const auto directory=arguments.value(uiTest+1);QTimer::singleShot(200,&window,[&window,directory]{window.startPrototypeVerification(directory);});}
    const auto scaleTest = arguments.indexOf(u"--verify-scale"_s);
    const auto projectTest = arguments.indexOf(u"--verify-project"_s);
    const auto maintenanceTest = arguments.indexOf(u"--verify-maintenance"_s);
    if (maintenanceTest >= 0) {
        const auto directory = arguments.value(maintenanceTest + 1);
        QTimer::singleShot(200, &window, [&window, directory] { window.startMaintenanceVerification(directory); });
    }
    const auto projectRuntime = arguments.indexOf(u"--verify-project-runtime"_s);
    const auto featureTest = arguments.indexOf(u"--verify-features"_s);
    const auto planTest = arguments.indexOf(u"--verify-plan-ui"_s);
    const auto neuronTest = arguments.indexOf(u"--verify-neurons"_s);
    const auto sessionTest = arguments.indexOf(u"--verify-sessions"_s);
    if (sessionTest >= 0) {
        const auto directory = arguments.value(sessionTest + 1);
        QTimer::singleShot(200, &window, [&window, directory] { window.startSessionVerification(directory); });
    }
    const auto sessionRuntimeTest = arguments.indexOf(u"--verify-sessions-runtime"_s);
    if (sessionRuntimeTest >= 0) {
        const auto directory = arguments.value(sessionRuntimeTest + 1);
        QTimer::singleShot(200, &window, [&window, directory] { window.startSessionRuntimeVerification(directory); });
    }
    if(neuronTest>=0){const auto directory=arguments.value(neuronTest+1);QTimer::singleShot(200,&window,[&window,directory]{window.startNeuronVerification(directory);});}
    const auto neuronRuntimeTest = arguments.indexOf(u"--verify-neurons-runtime"_s);
    if(neuronRuntimeTest>=0){const auto directory=arguments.value(neuronRuntimeTest+1);QTimer::singleShot(200,&window,[&window,directory]{window.startNeuronVerification(directory);});}
    if(planTest>=0){const auto directory=arguments.value(planTest+1);QTimer::singleShot(200,&window,[&window,directory]{window.startPlanUiVerification(directory);});}
    if (featureTest >= 0) {
        const auto directory = arguments.value(featureTest + 1);
        QTimer::singleShot(200, &window, [&window, directory] { window.startFeatureVerification(directory); });
    }
    if (projectRuntime >= 0) {
        const auto directory = arguments.value(projectRuntime + 1);
        QTimer::singleShot(200, &window, [&window, directory] { window.startProjectRuntimeVerification(directory); });
    }
    if (projectTest >= 0) {
        const auto directory = arguments.value(projectTest + 1);
        QTimer::singleShot(200, &window, [&window, directory] { window.startProjectVerification(directory); });
    }
    if (probe >= 0) {
        // 独立进程只读启动倍率，供重启记忆回归验证。
        QTimer::singleShot(150, &window, [&window, arguments, probe] {
            QFile file(arguments.value(probe + 1));
            if (file.open(QIODevice::WriteOnly))
                file.write(QJsonDocument(QJsonObject{{u"factor"_s, window.property("uiScale").toDouble()}})
                               .toJson());
            window.close();
        });
    } else if (scaleTest >= 0) {
        const auto directory = arguments.value(scaleTest + 1);
        QTimer::singleShot(200, &window, [&window, directory] { window.startScaleVerification(directory); });
    }
    const auto index = arguments.indexOf(u"--verify"_s);
    if (index >= 0) {
        const QString destination =
            arguments.value(index + 1, QDir::currentPath() + u"/artifacts/cpp-client"_s);
        QTimer::singleShot(300, &window, [&window, destination] { window.startVerification(destination); });
    } else if (arguments.contains(u"--load"_s)) {
        const auto modelIndex = arguments.indexOf(u"--model"_s);
        QSettings settings(QSettings::defaultFormat(), QSettings::UserScope, u"ModelObservatory"_s, u"NativeWorkbench"_s);
        const auto model = modelIndex >= 0
                               ? arguments.value(modelIndex + 1)
                               : settings.value(u"model_path"_s).toString();
        QTimer::singleShot(500, &window, [&window, model] { window.loadModel(model); });
    }
    const auto screenshotIndex = arguments.indexOf(u"--screenshot"_s);
    if (screenshotIndex >= 0) {
        const auto path = arguments.value(screenshotIndex + 1);
        QTimer::singleShot(1500, &window, [&window, path] {
            window.grab().save(path);
            window.close();
        });
    }
    return app.exec();
}
