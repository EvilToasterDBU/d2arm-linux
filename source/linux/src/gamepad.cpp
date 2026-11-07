#include "gamepad.h"

#include <SDL.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <QThread>
#include <algorithm>
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
void Gamepad::setLauncherMode(bool enabled) { launcherMode_.store(enabled); }
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
    auto openController = [&](int deviceIndex) {
        if (controller || !SDL_IsGameController(deviceIndex)) return;
        controller = SDL_GameControllerOpen(deviceIndex);
        if (controller) emit statusChanged((russian_ ? QStringLiteral("Геймпад подключён: %1") : QStringLiteral("Controller connected: %1"))
                                           .arg(SDL_GameControllerName(controller)));
    };
    for (int i = 0; i < SDL_NumJoysticks() && !controller; ++i) openController(i);
    Uint32 rescanAt = 0;
    bool moving = false, attack = false, skill = false, leftHeld = false;
    bool menuWasDown = false, inventoryWasDown = false, showItemsWasDown = false;
    bool launcherAWasDown = false, launcherBWasDown = false;
    bool launcherDpadWasDown[4] = {false, false, false, false};
    Uint32 launcherRepeatAt[4] = {0, 0, 0, 0};
    bool skillWasDown[4] = {false, false, false, false};
    bool runWasDown = false, swapWasDown = false, triggerWasDown = false;
    bool dpadWasDown[4] = {false, false, false, false};
    bool dpadLayer2WasDown[4] = {false, false, false, false};
    float cursorX = DisplayWidth(display, DefaultScreen(display)) / 2.0f;
    float cursorY = DisplayHeight(display, DefaultScreen(display)) / 2.0f;
    emit statusChanged(russian_ ? QStringLiteral("Управление: левый стик — движение, A — атака, крестовина — меню, LT+крестовина — зелья, X/B/Y/RB — навыки.")
                                : QStringLiteral("Controls: left stick moves, A attacks, D-pad navigates, LT+D-pad uses potions, X/B/Y/RB select skills."));

    while (running_.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED && !controller) {
                openController(event.cdevice.which);
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller &&
                       SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == event.cdevice.which) {
                mouseButton(display, 1, false);
                mouseButton(display, 3, false);
                if (showItemsWasDown) key(display, XK_Alt_L, false);
                const KeySym arrows[4] = {XK_Up, XK_Down, XK_Left, XK_Right};
                for (int i = 0; i < 4; ++i) {
                    if (dpadWasDown[i] && !dpadLayer2WasDown[i]) key(display, arrows[i], false);
                    dpadWasDown[i] = dpadLayer2WasDown[i] = false;
                }
                showItemsWasDown = false;
                leftHeld = skill = moving = attack = false;
                XFlush(display);
                SDL_GameControllerClose(controller);
                controller = nullptr;
                emit statusChanged(russian_ ? QStringLiteral("Геймпад отключён.") : QStringLiteral("Controller disconnected."));
            }
        }

        if (!controller && SDL_GetTicks() >= rescanAt) {
            for (int i = 0; i < SDL_NumJoysticks() && !controller; ++i) openController(i);
            rescanAt = SDL_GetTicks() + 1000;
        }
        if (!controller) {
            QThread::msleep(15);
            continue;
        }
        if (launcherMode_.load()) {
            if (leftHeld || skill || showItemsWasDown) {
                mouseButton(display, 1, false);
                mouseButton(display, 3, false);
                if (showItemsWasDown) key(display, XK_Alt_L, false);
                showItemsWasDown = false;
            }
            const KeySym arrows[4] = {XK_Up, XK_Down, XK_Left, XK_Right};
            for (int i = 0; i < 4; ++i) {
                if (dpadWasDown[i] && !dpadLayer2WasDown[i]) key(display, arrows[i], false);
                dpadWasDown[i] = dpadLayer2WasDown[i] = false;
            }
            moving = attack = skill = leftHeld = false;
            XFlush(display);
            const Uint32 now = SDL_GetTicks();
            const SDL_GameControllerButton navButtons[4] = {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
                SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
            for (int i = 0; i < 4; ++i) {
                const bool down = SDL_GameControllerGetButton(controller, navButtons[i]);
                if (down && (!launcherDpadWasDown[i] || now >= launcherRepeatAt[i])) {
                    emit launcherAction((i == 0 || i == 2) ? 0 : 1);
                    launcherRepeatAt[i] = now + 300;
                }
                launcherDpadWasDown[i] = down;
            }
            const Sint16 ly = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
            const bool stickUp = ly < -18000;
            const bool stickDown = ly > 18000;
            if ((stickUp || stickDown) && now >= launcherRepeatAt[0]) {
                emit launcherAction(stickUp ? 0 : 1);
                launcherRepeatAt[0] = now + 300;
            }
            const bool aDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A);
            const bool bDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_B);
            if (aDown && !launcherAWasDown) emit launcherAction(2);
            if (bDown && !launcherBWasDown) emit launcherAction(4);
            launcherAWasDown = aDown;
            launcherBWasDown = bDown;
            const bool startDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START);
            const bool backDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK);
            if (startDown && !menuWasDown) emit launcherAction(3);
            if (backDown && !inventoryWasDown) emit launcherAction(4);
            menuWasDown = startDown;
            inventoryWasDown = backDown;
            QThread::msleep(16);
            continue;
        }
        if (!enabled_.load()) {
            if (leftHeld || skill || showItemsWasDown) {
                mouseButton(display, 1, false);
                mouseButton(display, 3, false);
                if (showItemsWasDown) key(display, XK_Alt_L, false);
                showItemsWasDown = false;
            }
            const KeySym arrows[4] = {XK_Up, XK_Down, XK_Left, XK_Right};
            for (int i = 0; i < 4; ++i) {
                if (dpadWasDown[i] && !dpadLayer2WasDown[i]) key(display, arrows[i], false);
                dpadWasDown[i] = dpadLayer2WasDown[i] = false;
            }
            moving = attack = skill = leftHeld = false;
            XFlush(display);
            QThread::msleep(16);
            continue;
        }

        const Sint16 lx = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        const Sint16 ly = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);
        auto stickAxis = [](Sint16 axis) {
            constexpr float deadzone = 0.22f;
            const float value = axis / 32767.0f;
            const float magnitude = std::abs(value);
            if (magnitude <= deadzone) return 0.0f;
            return std::copysign((magnitude - deadzone) / (1.0f - deadzone), value);
        };
        const float x = stickAxis(lx);
        const float y = stickAxis(ly);
        const bool moveNow = x != 0 || y != 0;
        if (moveNow) {
            const int screenWidth = DisplayWidth(display, DefaultScreen(display));
            const int screenHeight = DisplayHeight(display, DefaultScreen(display));
            cursorX = screenWidth * 0.5f + x * std::min(screenWidth * 0.22f, 420.0f);
            cursorY = screenHeight * 0.5f + y * std::min(screenHeight * 0.22f, 300.0f);
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
        const SDL_GameControllerButton buttons[4] = {SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
            SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT};
        const KeySym arrows[4] = {XK_Up, XK_Down, XK_Left, XK_Right};
        const KeySym beltKeys[4] = {XK_1, XK_2, XK_3, XK_4};
        const bool layer2Now = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
        for (int i = 0; i < 4; ++i) {
            const bool down = SDL_GameControllerGetButton(controller, buttons[i]);
            const bool modifierChanged = down && dpadWasDown[i] && layer2Now != dpadLayer2WasDown[i];
            if (modifierChanged && !dpadLayer2WasDown[i]) key(display, arrows[i], false);
            if (down && (!dpadWasDown[i] || modifierChanged)) {
                if (layer2Now) key(display, beltKeys[i], true), key(display, beltKeys[i], false);
                else key(display, arrows[i], true);
            }
            if (!down && dpadWasDown[i] && !dpadLayer2WasDown[i]) key(display, arrows[i], false);
            dpadWasDown[i] = down;
            dpadLayer2WasDown[i] = down && layer2Now;
        }
        XFlush(display);
        QThread::msleep(16);
    }

    if (leftHeld) mouseButton(display, 1, false);
    if (skill) mouseButton(display, 3, false);
    if (showItemsWasDown) key(display, XK_Alt_L, false);
    const KeySym arrows[4] = {XK_Up, XK_Down, XK_Left, XK_Right};
    for (int i = 0; i < 4; ++i) {
        if (dpadWasDown[i] && !dpadLayer2WasDown[i]) key(display, arrows[i], false);
    }
    XFlush(display);
    if (controller) SDL_GameControllerClose(controller);
    XCloseDisplay(display);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
}
