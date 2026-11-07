#include "main_window.h"

#include <QCoreApplication>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QSettings>
#include <QLocale>
#include <QProcessEnvironment>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QScreen>
#include <QVBoxLayout>
#include <QWidget>

#include <stdexcept>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    russian_ = QLocale::system().language() == QLocale::Russian;
    gamepad_.setRussian(russian_);
    setWindowTitle(trText("D2ARM Linux", "D2ARM Linux"));
    setMinimumSize(720, 500);

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    auto *intro = new QLabel(trText(
        "ARM64 desktop launcher for your own Diablo II: Lord of Destruction 1.13d installation. "
        "The game runs through Box64 and Wine.",
        "ARM64-лаунчер для вашей копии Diablo II: Lord of Destruction 1.13d. "
        "Игра запускается через Box64 и Wine."), central);
    intro->setWordWrap(true);
    root->addWidget(intro);

    auto *form = new QFormLayout();
    auto makePathRow = [this, form](const QString &label, QLineEdit *&edit, const QString &hint,
                                    const char *browseText, auto browseSlot) {
        auto *row = new QWidget(this);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, 0, 0, 0);
        edit = new QLineEdit(row);
        edit->setPlaceholderText(hint);
        auto *browse = new QPushButton(trText("Browse…", "Обзор…"), row);
        connect(browse, &QPushButton::clicked, this, browseSlot);
        layout->addWidget(edit, 1);
        layout->addWidget(browse);
        form->addRow(label, row);
    };
    makePathRow(trText("Game folder", "Папка игры"), gameDir_,
                trText("Folder containing Game.exe", "Папка с Game.exe"), "Browse…", &MainWindow::browseGameDirectory);
    makePathRow(trText("Box64 (bundled)", "Box64 (в комплекте)"), box64_, QStringLiteral("box64"), "Browse…", &MainWindow::browseBox64);
    makePathRow(trText("Wine WoW64 (bundled)", "Wine WoW64 (в комплекте)"), wine64_, QStringLiteral("wine"),
                "Browse…", &MainWindow::browseWine);
    root->addLayout(form);

    interfaceScale_ = new QComboBox(central);
    interfaceScale_->addItem(trText("Automatic (D2DX)", "Автоматически (D2DX)"), 0);
    interfaceScale_->addItem(trText("Smaller interface (600p)", "Интерфейс поменьше (600p)"), 600);
    interfaceScale_->addItem(trText("Standard interface (540p)", "Обычный интерфейс (540p)"), 540);
    interfaceScale_->addItem(trText("Larger interface (480p)", "Крупный интерфейс (480p)"), 480);
    interfaceScale_->setToolTip(trText("Changes the game's render resolution and applies on the next launch.",
                                       "Меняет разрешение рендеринга игры и применяется при следующем запуске."));
    form->addRow(trText("In-game interface size", "Размер интерфейса игры"), interfaceScale_);

    auto *featureNote = new QLabel(trText(
        "Widescreen rendering uses the project's D2DX patches. A native gamepad profile is enabled while the game is focused.",
        "Широкий формат экрана использует патчи D2DX проекта. Профиль геймпада работает, когда окно игры активно."), central);
    featureNote->setWordWrap(true);
    root->addWidget(featureNote);

    gamepadEnabled_ = new QCheckBox(trText("Enable gamepad controls", "Включить управление геймпадом"), central);
    gamepadEnabled_->setChecked(true);
    root->addWidget(gamepadEnabled_);

    auto *buttons = new QHBoxLayout();
    launch_ = new QPushButton(trText("Launch game", "Запустить игру"), central);
    stop_ = new QPushButton(trText("Stop", "Остановить"), central);
    stop_->setEnabled(false);
    buttons->addWidget(launch_);
    buttons->addWidget(stop_);
    buttons->addStretch(1);
    status_ = new QLabel(trText("Choose your game folder to begin.", "Укажите папку игры."), central);
    buttons->addWidget(status_);
    root->addLayout(buttons);

    log_ = new QPlainTextEdit(central);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(3000);
    root->addWidget(log_, 1);
    setCentralWidget(central);
    launch_->setFocus();

    connect(launch_, &QPushButton::clicked, this, &MainWindow::launchGame);
    connect(stop_, &QPushButton::clicked, this, &MainWindow::stopGame);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &MainWindow::processOutput);
    connect(&process_, &QProcess::readyReadStandardError, this, &MainWindow::processOutput);
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, &MainWindow::processFinished);
    connect(&gamepad_, &Gamepad::statusChanged, this, &MainWindow::onGamepadStatus);
    connect(&gamepad_, &Gamepad::launcherAction, this, &MainWindow::onLauncherAction);

    restoreSettings();
    applyStyle();
    gamepad_.start();
}

MainWindow::~MainWindow() {
    saveSettings();
    gamepad_.stop();
}

QString MainWindow::trText(const char *en, const char *ru) const {
    return QString::fromUtf8(russian_ ? ru : en);
}

void MainWindow::restoreSettings() {
    QSettings s;
    gameDir_->setText(s.value("gameDirectory").toString());
    const QString bundledBox = runtimeFile("runtime/box64/bin/box64");
    const QString bundledWine = runtimeFile("runtime/wine/bin/wine");
    const QString configuredBox = s.value("box64").toString();
    const QString configuredWine = s.value("wine64").toString();
    box64_->setText(QFileInfo(configuredBox).isExecutable()
                        ? configuredBox
                        : (QFileInfo(bundledBox).isExecutable() ? bundledBox : QStandardPaths::findExecutable("box64")));
    wine64_->setText(QFileInfo(configuredWine).isExecutable()
                         ? configuredWine
                         : (QFileInfo(bundledWine).isExecutable() ? bundledWine : QStandardPaths::findExecutable("wine")));
    gamepadEnabled_->setChecked(s.value("gamepadEnabled", true).toBool());
    const int interfaceHeight = s.value("interfaceHeight", 0).toInt();
    const int scaleIndex = interfaceScale_->findData(interfaceHeight);
    interfaceScale_->setCurrentIndex(scaleIndex >= 0 ? scaleIndex : 0);
}

void MainWindow::saveSettings() const {
    QSettings s;
    s.setValue("gameDirectory", gameDir_->text());
    s.setValue("box64", box64_->text());
    s.setValue("wine64", wine64_->text());
    s.setValue("gamepadEnabled", gamepadEnabled_->isChecked());
    s.setValue("interfaceHeight", interfaceScale_->currentData().toInt());
}

QString MainWindow::gameDirectory() const { return QDir::cleanPath(gameDir_->text().trimmed()); }

QString MainWindow::appDataDirectory() const {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

QString MainWindow::runtimeFile(const QString &name) const {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString bundled = QDir(appDir).filePath("../share/d2arm-linux/assets/" + name);
    return QFileInfo::exists(bundled) ? bundled : QDir(appDir).filePath("../assets/" + name);
}

QString MainWindow::prepareGameShadow(const QString &sourceDirectory) {
    const QString base = QDir(appDataDirectory()).filePath("game-shadow");
    const QString sourceCanonical = QDir(sourceDirectory).canonicalPath();
    const QString sourceAbsolute = QDir::cleanPath(QFileInfo(sourceDirectory).absoluteFilePath());
    const QString baseAbsolute = QDir::cleanPath(QFileInfo(base).absoluteFilePath());
    const QString baseParentCanonical = QFileInfo(QFileInfo(base).absolutePath()).canonicalFilePath();
    const bool lexicalOverlap = baseAbsolute == sourceAbsolute || baseAbsolute.startsWith(sourceAbsolute + QDir::separator()) ||
        sourceAbsolute.startsWith(baseAbsolute + QDir::separator());
    if (sourceCanonical.isEmpty() || lexicalOverlap || sourceCanonical == QFileInfo(base).canonicalFilePath() ||
        sourceCanonical == baseParentCanonical || baseParentCanonical.startsWith(sourceCanonical + QDir::separator()))
        throw std::runtime_error("the selected game folder overlaps the launcher data folder");
    if (QFileInfo(base).isSymLink()) throw std::runtime_error("the game shadow path must not be a symbolic link");
    QDir old(base);
    if (old.exists() && !old.removeRecursively()) throw std::runtime_error("cannot clear the old game shadow");
    if (!QDir().mkpath(base)) throw std::runtime_error("cannot create the game shadow directory");

    QDir source(sourceDirectory);
    QDirIterator it(source.absolutePath(), QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString item = it.next();
        const QFileInfo info(item);
        const QString target = QDir(base).filePath(info.fileName());
        if (!QFile::link(info.absoluteFilePath(), target))
            throw std::runtime_error(QString("cannot link game file: %1").arg(info.fileName()).toStdString());
    }

    const QStringList files = {"mod/D2Android.exe", "mod/d2android-mod.dll", "glide3x.dll", "d2dx.cfg"};
    for (const QString &relative : files) {
        const QString from = runtimeFile(relative);
        const QString to = QDir(base).filePath(QFileInfo(relative).fileName());
        if (!QFileInfo(from).isFile())
            throw std::runtime_error(QString("missing packaged file: %1").arg(relative).toStdString());
        if (QFileInfo::exists(to) && !QFile::remove(to)) throw std::runtime_error("cannot replace a game shadow file");
        if (!QFile::copy(from, to)) throw std::runtime_error(QString("cannot copy: %1").arg(relative).toStdString());
    }
    const int targetHeight = interfaceScale_->currentData().toInt();
    if (targetHeight > 0) {
        QFile config(QDir(base).filePath("d2dx.cfg"));
        if (!config.open(QIODevice::ReadOnly | QIODevice::Text))
            throw std::runtime_error("cannot read D2DX configuration");
        QString contents = QString::fromUtf8(config.readAll());
        config.close();
        const QSize desktop = QGuiApplication::primaryScreen()->geometry().size();
        const int gameHeight = qMin(targetHeight, qMin(desktop.width(), desktop.height()));
        const int gameWidth = (qMax(desktop.width(), desktop.height()) * gameHeight /
                               qMax(1, qMin(desktop.width(), desktop.height()))) & ~1;
        const QString sizeLine = QString("size=[%1,%2]").arg(gameWidth).arg(gameHeight);
        contents.replace("size=[-1,-1]", sizeLine);
        const QByteArray updatedConfig = contents.toUtf8();
        if (!config.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate) ||
            config.write(updatedConfig) != updatedConfig.size())
            throw std::runtime_error("cannot write D2DX configuration");
    }
    return base;
}

void MainWindow::browseGameDirectory() {
    const QString path = QFileDialog::getExistingDirectory(this, trText("Select Diablo II folder", "Выберите папку Diablo II"), gameDir_->text());
    if (!path.isEmpty()) gameDir_->setText(path);
}

void MainWindow::browseBox64() {
    const QString path = QFileDialog::getOpenFileName(this, trText("Select Box64", "Выберите Box64"), box64_->text());
    if (!path.isEmpty()) box64_->setText(path);
}

void MainWindow::browseWine() {
    const QString path = QFileDialog::getOpenFileName(this, trText("Select Wine WoW64", "Выберите Wine WoW64"), wine64_->text());
    if (!path.isEmpty()) wine64_->setText(path);
}

void MainWindow::launchGame() {
    if (process_.state() != QProcess::NotRunning) return;
    const QString dir = gameDirectory();
    const QString game = QDir(dir).filePath("Game.exe");
    const QString box = QFileInfo(box64_->text()).isAbsolute() ? box64_->text() : QStandardPaths::findExecutable(box64_->text());
    const QString wine = QFileInfo(wine64_->text()).isAbsolute() ? wine64_->text() : QStandardPaths::findExecutable(wine64_->text());
    if (!QFileInfo(game).isFile()) {
        QMessageBox::warning(this, trText("Game not found", "Игра не найдена"),
                             trText("Select a Diablo II folder that contains Game.exe.", "Выберите папку Diablo II с файлом Game.exe."));
        return;
    }
    if (box.isEmpty() || wine.isEmpty()) {
        QMessageBox::warning(this, trText("Runtime not found", "Runtime не найден"),
                             trText("The bundled Box64/Wine runtime is missing. Re-download the AppImage or select compatible executables.",
                                    "В AppImage не найден встроенный runtime Box64/Wine. Скачайте AppImage заново или укажите совместимые исполняемые файлы."));
        return;
    }
    try {
        const QString shadow = prepareGameShadow(dir);
        const QString loader = QDir(shadow).filePath("D2Android.exe");
        QDir().mkpath(appDataDirectory());
        const QString prefix = QDir(appDataDirectory()).filePath("wine-prefix");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("WINEPREFIX", prefix);
        env.insert("WINEARCH", "win64");
        env.insert("WINEDEBUG", "-all");
        env.insert("WINEDLLOVERRIDES", "mscoree,mshtml=d;glide3x=n,b");
        env.insert("D2MOD_APP_PORT", "7958");
        env.insert("D2MOD_CMD_PORT", "0");
        const QString wineRoot = runtimeFile("runtime/wine");
        const QString wineLib = QDir(wineRoot).filePath("lib");
        const QString wineUnixLib = QDir(wineLib).filePath("wine/x86_64-unix");
        const QString bundledX64Libs = runtimeFile("runtime/box64/usr/lib/box64-x86_64-linux-gnu");
        const QString box64Lib = runtimeFile("runtime/box64/lib");
        const QString box64MultiarchLib = runtimeFile("runtime/box64/lib/aarch64-linux-gnu");
        QStringList hostLibPaths = {box64Lib, box64MultiarchLib};
        if (env.contains("LD_LIBRARY_PATH")) hostLibPaths.append(env.value("LD_LIBRARY_PATH"));
        env.insert("LD_LIBRARY_PATH", hostLibPaths.join(QLatin1Char(':')));
        if (QFileInfo(runtimeFile("runtime/box64/bin/box64")).isExecutable() &&
            QFileInfo::exists(QDir(wineLib).filePath("wine")))
            env.insert("BOX64_LD_LIBRARY_PATH", bundledX64Libs + QLatin1Char(':') + wineUnixLib + QLatin1Char(':') + wineLib);

        const QString windowsInstallPath = QStringLiteral("Z:") + QDir::toNativeSeparators(shadow);
        auto setWineValue = [&](const QString &key, const QString &name, const QString &type, const QString &value) {
            QProcess registry;
            registry.setProcessEnvironment(env);
            registry.setWorkingDirectory(shadow);
            registry.setProgram(box);
            registry.setArguments({wine, "reg.exe", "add", key, "/v", name, "/t", type, "/d", value, "/f"});
            registry.start();
            if (!registry.waitForStarted(5000))
                throw std::runtime_error(QString("cannot start Wine registry setup: %1").arg(registry.errorString()).toStdString());
            if (!registry.waitForFinished(60000)) {
                registry.kill();
                registry.waitForFinished();
                throw std::runtime_error("Wine registry setup timed out");
            }
            const QString output = QString::fromLocal8Bit(registry.readAllStandardOutput() + registry.readAllStandardError()).trimmed();
            if (!output.isEmpty()) appendLog(output);
            if (registry.exitStatus() != QProcess::NormalExit || registry.exitCode() != 0)
                throw std::runtime_error(output.isEmpty() ? "Wine registry setup failed" : output.toStdString());
        };
        const QString installKey = QStringLiteral("HKCU\\Software\\Blizzard Entertainment\\Diablo II");
        setWineValue(installKey, "InstallPath", "REG_SZ", windowsInstallPath);
        setWineValue(installKey, "Save Path", "REG_SZ", windowsInstallPath);
        setWineValue(installKey, "Mini Panel", "REG_DWORD", "1");
        setWineValue(installKey + QStringLiteral("\\VideoConfig"), "Render", "REG_DWORD", "0");
        setWineValue("HKLM\\Software\\Blizzard Entertainment\\Diablo II", "InstallPath", "REG_SZ", windowsInstallPath);
        setWineValue("HKLM\\Software\\Wow6432Node\\Blizzard Entertainment\\Diablo II", "InstallPath", "REG_SZ", windowsInstallPath);
        setWineValue("HKCU\\Software\\Wine\\AppDefaults\\Game.exe", "Version", "REG_SZ", "win7");
        setWineValue("HKCU\\Software\\Wine\\AppDefaults\\D2Android.exe", "Version", "REG_SZ", "win7");

        process_.setProcessEnvironment(env);
        process_.setWorkingDirectory(shadow);
        process_.setProgram(box);
        process_.setArguments({wine, loader, "-3dfx", "-direct"});
        appendLog(trText("Starting through Box64 + Wine with D2Android mod and D2DX…",
                         "Запуск через Box64 + Wine с модом D2Android и D2DX…"));
        status_->setText(trText("Starting…", "Запуск…"));
        launch_->setEnabled(false);
        stop_->setEnabled(true);
        gamepad_.setLauncherMode(false);
        gamepad_.setEnabled(gamepadEnabled_->isChecked());
        process_.start();
        if (!process_.waitForStarted(5000)) throw std::runtime_error(process_.errorString().toStdString());
        saveSettings();
    } catch (const std::exception &e) {
        gamepad_.setEnabled(false);
        gamepad_.setLauncherMode(true);
        launch_->setEnabled(true);
        stop_->setEnabled(false);
        status_->setText(trText("Launch setup failed", "Ошибка подготовки запуска"));
        QMessageBox::critical(this, trText("Cannot start game", "Не удалось запустить игру"), QString::fromUtf8(e.what()));
    }
}

void MainWindow::stopGame() {
    if (process_.state() != QProcess::NotRunning) process_.terminate();
    gamepad_.setEnabled(false);
}

void MainWindow::processOutput() {
    const QByteArray data = process_.readAllStandardOutput() + process_.readAllStandardError();
    const QString text = QString::fromLocal8Bit(data).trimmed();
    if (!text.isEmpty()) appendLog(text);
}

void MainWindow::processFinished(int exitCode, QProcess::ExitStatus status) {
    gamepad_.setEnabled(false);
    gamepad_.setLauncherMode(true);
    launch_->setEnabled(true);
    stop_->setEnabled(false);
    status_->setText(trText("Game closed", "Игра закрыта"));
    appendLog(trText("Wine process ended with code ", "Процесс Wine завершился с кодом ") +
              QString::number(exitCode) + (status == QProcess::CrashExit ? trText(" (crashed)", " (аварийно)") : QString()));
}

void MainWindow::onGamepadStatus(const QString &status) {
    if (status.isEmpty()) return;
    appendLog(status);
}

void MainWindow::onLauncherAction(int action) {
    // Actions: 0/1 = previous/next focus, 2 = activate, 3 = launch, 4 = stop.
    QWidget *focused = QApplication::focusWidget();
    if (process_.state() != QProcess::NotRunning) {
        if (action == 4) stopGame();
        return;
    }
    if (action == 0 || action == 1) {
        if (focused) {
            const int key = action == 0 ? Qt::Key_Backtab : Qt::Key_Tab;
            QKeyEvent press(QEvent::KeyPress, key, action == 0 ? Qt::ShiftModifier : Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, key, action == 0 ? Qt::ShiftModifier : Qt::NoModifier);
            QApplication::sendEvent(focused, &press);
            QApplication::sendEvent(focused, &release);
        }
    } else if (action == 2) {
        if (auto *button = qobject_cast<QPushButton *>(focused)) button->click();
        else if (auto *check = qobject_cast<QCheckBox *>(focused)) check->toggle();
        else if (focused) {
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Return, Qt::NoModifier);
            QApplication::sendEvent(focused, &press);
            QApplication::sendEvent(focused, &release);
        }
    } else if (action == 3) launch_->click();
    else if (action == 4 && QApplication::activeModalWidget() && focused) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(focused, &press);
        QApplication::sendEvent(focused, &release);
    }
}

void MainWindow::appendLog(const QString &line) { log_->appendPlainText(line); }

void MainWindow::applyStyle() {
    setStyleSheet("QMainWindow, QWidget { background: #171717; color: #e9dfc7; }"
                  "QLineEdit, QPlainTextEdit { background: #242424; border: 1px solid #765f37; padding: 6px; }"
                  "QPushButton { background: #42331d; border: 1px solid #a6874b; padding: 7px 14px; }"
                  "QPushButton:hover { background: #574321; } QPushButton:focus, QLineEdit:focus, QCheckBox:focus { outline: 2px solid #e0b44c; }"
                  "QPushButton:disabled { color: #777; }");
}
