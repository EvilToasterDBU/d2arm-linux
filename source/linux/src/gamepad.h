#pragma once

#include <QThread>
#include <atomic>

class Gamepad final : public QThread {
    Q_OBJECT
public:
    explicit Gamepad(QObject *parent = nullptr);
    ~Gamepad() override;
    void setEnabled(bool enabled);
    void setRussian(bool russian);
    void stop();

signals:
    void statusChanged(const QString &status);

protected:
    void run() override;

private:
    std::atomic_bool running_{false};
    std::atomic_bool enabled_{false};
    std::atomic_bool russian_{false};
};
