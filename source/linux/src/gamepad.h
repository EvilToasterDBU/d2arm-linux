#pragma once

#include <QThread>
#include <atomic>

class Gamepad final : public QThread {
    Q_OBJECT
public:
    explicit Gamepad(QObject *parent = nullptr);
    ~Gamepad() override;
    void setEnabled(bool enabled);
    void setLauncherMode(bool enabled);
    void setRussian(bool russian);
    void setAimAssist(bool enabled);
    void setQuickCast(bool enabled);
    void setStickDeadzone(int percent);
    void setStickSensitivity(int percent);
    void setButtonStyle(int style);
    void stop();

signals:
    void statusChanged(const QString &status);
    void launcherAction(int action);

protected:
    void run() override;

private:
    std::atomic_bool running_{false};
    std::atomic_bool enabled_{false};
    std::atomic_bool launcherMode_{true};
    std::atomic_bool russian_{false};
    std::atomic_bool aimAssist_{true};
    std::atomic_bool quickCast_{true};
    std::atomic_int stickDeadzonePercent_{22};
    std::atomic_int stickSensitivityPercent_{100};
    // 0 = auto detect, 1 = Xbox, 2 = PlayStation.
    std::atomic_int buttonStyle_{0};
};
