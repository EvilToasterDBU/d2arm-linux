#pragma once

#include "gamepad.h"

#include <QMainWindow>
#include <QProcess>

class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class QComboBox;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    void browseGameDirectory();
    void browseBox64();
    void browseWine();
    void launchGame();
    void stopGame();
    void processOutput();
    void processFinished(int exitCode, QProcess::ExitStatus status);
    void onGamepadStatus(const QString &status);
    void onLauncherAction(int action);
    void onRadialMenuChanged(int selection);

private:
    QString trText(const char *en, const char *ru) const;
    void restoreSettings();
    void saveSettings() const;
    QString gameDirectory() const;
    QString appDataDirectory() const;
    QString runtimeFile(const QString &name) const;
    QString prepareGameShadow(const QString &sourceDirectory);
    void appendLog(const QString &line);
    void applyStyle();

    bool russian_ = false;
    QLineEdit *gameDir_ = nullptr;
    QLineEdit *box64_ = nullptr;
    QLineEdit *wine64_ = nullptr;
    QCheckBox *gamepadEnabled_ = nullptr;
    QComboBox *interfaceScale_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *gamepadOverlay_ = nullptr;
    QPushButton *launch_ = nullptr;
    QPushButton *stop_ = nullptr;
    QPlainTextEdit *log_ = nullptr;
    QProcess process_;
    Gamepad gamepad_;
};
