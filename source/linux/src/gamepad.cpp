#include "gamepad.h"

#include <SDL.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <QThread>
#include <cmath>

namespace {
void key(Display *display, KeySym sym, bool down) {
    const KeyCode code = XKeysymToKeycode(display, sym);
    if (code) XTestFakeKeyEvent(display, code, down, CurrentTime);
}
void mouseButton(Display *display, unsigned int button, bool down) {
    XTestFakeButtonEvent(display, button, down, CurrentTime);
}
}

Gamepad::Gamepad(QObject *parent) : QThread(parent) {}
Gamepad::~Gamepad() { stop(); }

void Gamepad::setEnabled(bool enabled) { enabled_.store(enabled); }
void Gamepad::setRussian(bool russian) { russian_.store(russian); }
void Gamepad::stop() {
    running_.store(false);
    if (isRunning()) wait(2000);
}

void Gamepad::run() {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        emit statusChanged(russian_ ? QStringLiteral("Не удалось запустить SDL для геймпада: %1").arg(SDL_GetError())
                                    : QStringLiteral("SDL gamepad init failed: %1").arg(SDL_GetError()));
        return;
    }
    Display *display = XOpenDisplay(nullptr);
    if (!display) {
        emit statusChanged(russian_ ? QStringLiteral("Для ввода с геймпада требуется X11 или XWayland.")
                                    : QStringLiteral("Cannot connect to X11 for gamepad input injection."));
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
        return;
    }
    running_.store(true);
    SDL_GameController *controller = nullptr;
    bool moving = false, attack = false, skill = false, leftHeld = false;
    bool menuWasDown = false, inventoryWasDown = false, showItemsWasDown = false;
    bool skillWasDown[4] = {false, false, false, false};
    bool runWasDown = false, swapWasDown = false, triggerWasDown = false;
    bool beltWasDown[4] = {false, false, false, false};
    float cursorX = DisplayWidth(display, DefaultScreen(display)) / 2.0f;
    float cursorY = DisplayHeight(display, DefaultScreen(display)) / 2.0f;
    emit statusChanged(russian_ ? QStringLiteral("Геймпад: стик — движение, A — атака, LB — названия, X/B/Y/RB — F1–F4, LT — второй ряд.")
                                : QStringLiteral("Gamepad ready: stick moves, A attacks, LB shows names, X/B/Y/RB select F1–F4, LT switches rows."));

    while (running_.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED && !controller) {
                controller = SDL_GameControllerOpen(event.cdevice.which);
                if (controller) emit statusChanged((russian_ ? QStringLiteral("Геймпад подключён: %1") : QStringLiteral("Controller connected: %1"))
                                                    .arg(SDL_GameControllerName(controller)));
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller &&
                       SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == event.cdevice.which) {
                mouseButton(display, 1, false);
                mouseButton(display, 3, false);
                SDL_GameControllerClose(controller);
                controller = nullptr;
                emit statusChanged(russian_ ? QStringLiteral("Геймпад отключён.") : QStringLiteral("Controller disconnected."));
            }
        }

        if (!enabled_.load() || !controller) {
            if (leftHeld || skill) {
                mouseButton(display, 1, false);
                mouseButton(display, 3, false);
                moving = attack = skill = leftHeld = false;
                XFlush(display);
            }
            QThread::msleep(15);
            continue;
        }

        const Sint16 lx = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        const Sint16 ly = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
        const float x = std::abs(lx) > 9000 ? lx / 32767.0f : 0.0f;
        const float y = std::abs(ly) > 9000 ? ly / 32767.0f : 0.0f;
        const bool moveNow = x != 0 || y != 0;
        if (moveNow) {
            cursorX += x * 10.0f;
            cursorY += y * 10.0f;
            cursorX = std::fmax(0, std::fmin(cursorX, DisplayWidth(display, DefaultScreen(display)) - 1));
            cursorY = std::fmax(0, std::fmin(cursorY, DisplayHeight(display, DefaultScreen(display)) - 1));
            XTestFakeMotionEvent(display, DefaultScreen(display), static_cast<int>(cursorX), static_cast<int>(cursorY), CurrentTime);
        }
        moving = moveNow;

        const bool attackNow = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A);
        const bool layer2 = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
        const bool triggerDown = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
        const bool takeNow = layer2 && triggerDown;
        const bool skillNow = false;
        attack = attackNow;
        const bool wantLeft = moving || attack || takeNow;
        if (wantLeft != leftHeld) mouseButton(display, 1, wantLeft), leftHeld = wantLeft;
        skill = skillNow;
        if (triggerDown && !triggerWasDown && !layer2) key(display, XK_F5, true), key(display, XK_F5, false);
        triggerWasDown = triggerDown;

        const bool menuDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START);
        const bool inventoryDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK);
        const bool showItemsDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        if (menuDown && !menuWasDown) key(display, XK_Escape, true), key(display, XK_Escape, false);
        if (inventoryDown && !inventoryWasDown) key(display, XK_i, true), key(display, XK_i, false);
        if (showItemsDown != showItemsWasDown) key(display, XK_Alt_L, showItemsDown);
        menuWasDown = menuDown;
        inventoryWasDown = inventoryDown;
        showItemsWasDown = showItemsDown;
        const SDL_GameControllerButton skillButtons[4] = {SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
        const KeySym firstRow[4] = {XK_F1, XK_F2, XK_F3, XK_F4};
        const KeySym secondRow[4] = {XK_F6, XK_F7, XK_F8, XK_F4};
        for (int i = 0; i < 4; ++i) {
            const bool down = SDL_GameControllerGetButton(controller, skillButtons[i]);
            if (down && !skillWasDown[i]) {
                const KeySym sym = layer2 ? secondRow[i] : firstRow[i];
                key(display, sym, true);
                key(display, sym, false);
            }
            skillWasDown[i] = down;
        }
        const bool runNow = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSTICK);
        const bool swapNow = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
        if (runNow && !runWasDown) key(display, XK_r, true), key(display, XK_r, false);
        if (swapNow && !swapWasDown) key(display, XK_w, true), key(display, XK_w, false);
        runWasDown = runNow;
        swapWasDown = swapNow;
        const SDL_GameControllerButton buttons[4] = {SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
            SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_UP};
        const KeySym beltKeys[4] = {XK_1, XK_2, XK_3, XK_4};
        for (int i = 0; i < 4; ++i) {
            const bool down = SDL_GameControllerGetButton(controller, buttons[i]);
            if (down && !beltWasDown[i]) key(display, beltKeys[i], true), key(display, beltKeys[i], false);
            beltWasDown[i] = down;
        }
        XFlush(display);
        QThread::msleep(16);
    }

    if (leftHeld) mouseButton(display, 1, false);
    if (skill) mouseButton(display, 3, false);
    XFlush(display);
    if (controller) SDL_GameControllerClose(controller);
    XCloseDisplay(display);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
}
