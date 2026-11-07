#include "gamepad.h"

#include <SDL.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

#include <QThread>
#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {
void key(Display *display, KeySym sym, bool down) {
    const KeyCode code = XKeysymToKeycode(display, sym);
    if (code) XTestFakeKeyEvent(display, code, down, CurrentTime);
}
void mouseButton(Display *display, unsigned int button, bool down) {
    XTestFakeButtonEvent(display, button, down, CurrentTime);
}

struct AimTarget {
    uint32_t id = 0;
    uint8_t type = 0;
    uint8_t kind = 0;
    uint8_t flags = 0;
    int x = 0;
    int y = 0;
};

struct ModState {
    int commandPort = 0;
    int gameWidth = 800;
    int gameHeight = 600;
    Uint32 stateAt = 0;
    uint64_t uiFlags = 0;
    uint32_t hoverId = 0xffffffffu;
    uint8_t hoverType = 0xff;
    bool canWalk = false;
    bool inTown = false;
    int playerX = 0;
    int playerY = 0;
    uint64_t aimTargetKey = 0;
    std::vector<AimTarget> targets;

    bool fresh(Uint32 now) const { return stateAt && now - stateAt < 500; }
    bool panelOpen() const {
        constexpr std::array<unsigned, 21> panels = {1, 2, 3, 4, 5, 8, 9, 11, 12, 14, 15,
            16, 20, 22, 23, 24, 25, 26, 33, 36, 37};
        for (unsigned panel : panels) if (uiFlags & (uint64_t{1} << panel)) return true;
        return false;
    }
    bool escMenu() const { return (uiFlags & (uint64_t{1} << 9)) != 0; }
    bool npcMenu() const { return (uiFlags & (uint64_t{1} << 8)) != 0; }
};

uint16_t readU16(const uint8_t *p) {
    return static_cast<uint16_t>(p[0] | (uint16_t(p[1]) << 8));
}

uint32_t readU32(const uint8_t *p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

int16_t readI16(const uint8_t *p) {
    return static_cast<int16_t>(readU16(p));
}

void parseAimTargets(const uint8_t *packet, size_t length, ModState &state) {
    state.targets.clear();
    if (length < 59) return;

    size_t offset = 58;
    const uint8_t monsterCount = packet[offset++];
    for (uint8_t i = 0; i < monsterCount; ++i) {
        if (offset + 11 > length) return;
        AimTarget target;
        target.type = 1;
        target.id = readU32(packet + offset);
        target.x = readI16(packet + offset + 4);
        target.y = readI16(packet + offset + 6);
        target.flags = packet[offset + 9];
        const size_t nameLength = packet[offset + 10];
        offset += 11 + nameLength;
        if (offset > length) return;
        if (state.inTown && (target.flags & 16)) continue;
        if (!state.inTown && (target.flags & 16)) continue;
        state.targets.push_back(target);
    }

    if (offset >= length) return;
    const uint8_t itemCount = packet[offset++];
    for (uint8_t i = 0; i < itemCount; ++i) {
        if (offset + 10 > length) return;
        AimTarget target;
        target.type = 4;
        target.id = readU32(packet + offset);
        target.x = readI16(packet + offset + 4);
        target.y = readI16(packet + offset + 6);
        target.kind = packet[offset + 8];
        state.targets.push_back(target);
        offset += 10;
    }

    if (offset >= length) return;
    const uint8_t objectCount = packet[offset++];
    for (uint8_t i = 0; i < objectCount; ++i) {
        if (offset + 10 > length) return;
        AimTarget target;
        target.id = readU32(packet + offset);
        target.x = readI16(packet + offset + 4);
        target.y = readI16(packet + offset + 6);
        target.kind = packet[offset + 8];
        target.type = target.kind == 6 ? 5 : target.kind == 7 ? 1 : 2;
        state.targets.push_back(target);
        offset += 10;
    }
}

int openModSocket() {
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    local.sin_port = htons(7958);
    if (bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) != 0) {
        close(fd);
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

void receiveModPackets(int fd, ModState &state, Uint32 now) {
    if (fd < 0) return;
    std::array<uint8_t, 4096> packet{};
    for (;;) {
        const ssize_t n = recv(fd, packet.data(), packet.size(), 0);
        if (n <= 0) break;
        if (n < 8 || std::memcmp(packet.data(), "D2AM", 4) != 0 || packet[4] != 2) continue;
        if (packet[5] == 1 && n >= 10) {
            const size_t portOffset = 10 + packet[9];
            if (portOffset + 2 <= static_cast<size_t>(n))
                state.commandPort = packet[portOffset] | (packet[portOffset + 1] << 8);
        } else if (packet[5] == 2 && n >= 39) {
            state.gameWidth = readU16(packet.data() + 8);
            state.gameHeight = packet[10] | (packet[11] << 8);
            const uint32_t low = packet[16] | (uint32_t(packet[17]) << 8) | (uint32_t(packet[18]) << 16) | (uint32_t(packet[19]) << 24);
            const uint32_t high = packet[20] | (uint32_t(packet[21]) << 8) | (uint32_t(packet[22]) << 16) | (uint32_t(packet[23]) << 24);
            state.uiFlags = low | (uint64_t(high) << 32);
            state.canWalk = (packet[27] & 4) != 0;
            state.inTown = packet[26] != 0;
            state.playerX = readI16(packet.data() + 28);
            state.playerY = readI16(packet.data() + 30);
            state.hoverType = packet[34];
            state.hoverId = packet[35] | (uint32_t(packet[36]) << 8) | (uint32_t(packet[37]) << 16) | (uint32_t(packet[38]) << 24);
            parseAimTargets(packet.data(), static_cast<size_t>(n), state);
            state.stateAt = now;
        }
    }
}

void sendModCommand(int fd, int port, uint8_t command, const uint8_t *payload, size_t payloadSize) {
    if (fd < 0 || port <= 0 || port > 65535 || payloadSize > 248) return;
    std::array<uint8_t, 256> packet{};
    std::memcpy(packet.data(), "D2AC", 4);
    packet[4] = 1;
    packet[5] = command;
    if (payloadSize) std::memcpy(packet.data() + 8, payload, payloadSize);
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = htons(static_cast<uint16_t>(port));
    sendto(fd, packet.data(), 8 + payloadSize, 0, reinterpret_cast<sockaddr *>(&target), sizeof(target));
}

void sendWalk(int fd, const ModState &state, int phase, float x = 0, float y = 0) {
    const float distance = std::max(1, state.gameHeight) * 0.30f;
    const int16_t dx = static_cast<int16_t>(std::clamp(x * distance, -32768.0f, 32767.0f));
    const int16_t dy = static_cast<int16_t>(std::clamp(y * distance, -32768.0f, 32767.0f));
    const uint8_t payload[6] = {static_cast<uint8_t>(phase), 0, static_cast<uint8_t>(dx),
        static_cast<uint8_t>(uint16_t(dx) >> 8), static_cast<uint8_t>(dy), static_cast<uint8_t>(uint16_t(dy) >> 8)};
    sendModCommand(fd, state.commandPort, 4, payload, sizeof(payload));
}

const AimTarget *chooseAimTarget(ModState &state, float x, float y) {
    const double aimLength = std::hypot(x, y);
    if (aimLength < 0.01) {
        for (const AimTarget &target : state.targets) {
            if (((uint64_t(target.type) << 32) | target.id) == state.aimTargetKey) return &target;
        }
        state.aimTargetKey = 0;
        return nullptr;
    }
    x = static_cast<float>(x / aimLength);
    y = static_cast<float>(y / aimLength);
    const float height = std::max(1, state.gameHeight);
    const AimTarget *best = nullptr;
    double bestScore = std::numeric_limits<double>::max();
    double previousScore = std::numeric_limits<double>::max();
    const AimTarget *previous = nullptr;
    const double coneCos = std::cos(55.0 * 0.5 * 3.14159265358979323846 / 180.0);

    for (const AimTarget &target : state.targets) {
        const double dx = target.x - state.playerX;
        const double dy = target.y - state.playerY;
        const double distance = std::hypot(dx, 2.0 * dy);
        const bool monster = target.type == 1 && target.kind != 7;
        const double reach = height * (monster ? 0.75 : 0.55);
        if (distance > reach) continue;
        const double flatDistance = std::hypot(dx, dy);
        const double dot = flatDistance > 1.0 ? (dx * x + dy * y) / flatDistance : 1.0;
        if (dot < coneCos) continue;
        const bool special = monster && (target.flags & (1 | 2 | 8));
        const double score = distance * (1.5 - dot) * (special ? 0.9 : 1.0);
        const uint64_t key = (uint64_t(target.type) << 32) | target.id;
        if (key == state.aimTargetKey) {
            previous = &target;
            previousScore = score;
        }
        if (score < bestScore) {
            best = &target;
            bestScore = score;
        }
    }

    if (previous && previousScore <= bestScore * 1.15) best = previous;
    state.aimTargetKey = best ? (uint64_t(best->type) << 32) | best->id : 0;
    return best;
}

void sendUnit(int fd, const ModState &state, const AimTarget &target, int phase) {
    const uint8_t payload[12] = {0, static_cast<uint8_t>(phase), 0, 0,
        static_cast<uint8_t>(target.type), 0, 0, 0,
        static_cast<uint8_t>(target.id), static_cast<uint8_t>(target.id >> 8),
        static_cast<uint8_t>(target.id >> 16), static_cast<uint8_t>(target.id >> 24)};
    sendModCommand(fd, state.commandPort, 2, payload, sizeof(payload));
}

void sendHighlight(int fd, const ModState &state, const AimTarget *target) {
    const uint32_t type = target ? target->type : 0;
    const uint32_t id = target ? target->id : 0xffffffffu;
    const uint8_t payload[8] = {static_cast<uint8_t>(type), static_cast<uint8_t>(type >> 8),
        static_cast<uint8_t>(type >> 16), static_cast<uint8_t>(type >> 24),
        static_cast<uint8_t>(id), static_cast<uint8_t>(id >> 8),
        static_cast<uint8_t>(id >> 16), static_cast<uint8_t>(id >> 24)};
    sendModCommand(fd, state.commandPort, 3, payload, sizeof(payload));
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
    const int modSocket = openModSocket();
    if (modSocket < 0)
        emit statusChanged(russian_ ? QStringLiteral("Не удалось открыть локальный канал управления модом D2.")
                                   : QStringLiteral("Cannot open the local D2 mod input channel."));
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
    bool attack = false, leftHeld = false, rightHeld = false;
    uint64_t directAttackKey = 0;
    Uint32 attackRefreshAt = 0, highlightRefreshAt = 0;
    uint64_t highlightedKey = 0;
    bool directWalking = false;
    Uint32 walkRefreshAt = 0;
    float lastWalkX = 0, lastWalkY = 0;
    ModState modState;
    auto releaseDirectAttack = [&]() {
        if (!directAttackKey) return;
        AimTarget target;
        target.type = static_cast<uint8_t>(directAttackKey >> 32);
        target.id = static_cast<uint32_t>(directAttackKey);
        sendUnit(modSocket, modState, target, 2);
        directAttackKey = 0;
    };
    bool menuWasDown = false, inventoryWasDown = false, showItemsWasDown = false;
    bool launcherAWasDown = false, launcherBWasDown = false;
    bool launcherDpadWasDown[4] = {false, false, false, false};
    Uint32 launcherRepeatAt[4] = {0, 0, 0, 0};
    bool skillWasDown[4] = {false, false, false, false};
    bool runWasDown = false, swapWasDown = false, triggerWasDown = false;
    bool dpadWasDown[4] = {false, false, false, false};
    bool dpadArrowWasDown[4] = {false, false, false, false};
    float cursorX = DisplayWidth(display, DefaultScreen(display)) / 2.0f;
    float cursorY = DisplayHeight(display, DefaultScreen(display)) / 2.0f;
    emit statusChanged(russian_ ? QStringLiteral("Геймпад: левый стик — движение, правый — прицел и выбор цели, A — атаковать/взаимодействовать, LT+Back — карта.")
                                : QStringLiteral("Gamepad: left stick moves, right stick aims/selects a target, A attacks/interacts, LT+Back opens the map."));

    while (running_.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED && !controller) {
                openController(event.cdevice.which);
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller &&
                       SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == event.cdevice.which) {
                releaseDirectAttack();
                if (highlightedKey) sendHighlight(modSocket, modState, nullptr), highlightedKey = 0;
                if (directWalking) sendWalk(modSocket, modState, 2);
                mouseButton(display, 1, false);
                mouseButton(display, 3, false);
                if (showItemsWasDown) key(display, XK_Alt_L, false);
                const KeySym arrows[4] = {XK_Up, XK_Down, XK_Left, XK_Right};
                for (int i = 0; i < 4; ++i) {
                    if (dpadWasDown[i] && dpadArrowWasDown[i]) key(display, arrows[i], false);
                    dpadWasDown[i] = dpadArrowWasDown[i] = false;
                }
                showItemsWasDown = false;
                leftHeld = rightHeld = attack = directWalking = false;
                launcherAWasDown = launcherBWasDown = false;
                menuWasDown = inventoryWasDown = false;
                for (bool &wasDown : skillWasDown) wasDown = false;
                XFlush(display);
                SDL_GameControllerClose(controller);
                controller = nullptr;
                emit statusChanged(russian_ ? QStringLiteral("Геймпад отключён.") : QStringLiteral("Controller disconnected."));
            }
        }

        const Uint32 now = SDL_GetTicks();
        receiveModPackets(modSocket, modState, now);
        if (!controller && SDL_GetTicks() >= rescanAt) {
            for (int i = 0; i < SDL_NumJoysticks() && !controller; ++i) openController(i);
            rescanAt = SDL_GetTicks() + 1000;
        }
        if (!controller) {
            QThread::msleep(15);
            continue;
        }
        if (launcherMode_.load()) {
            releaseDirectAttack();
            if (highlightedKey) sendHighlight(modSocket, modState, nullptr), highlightedKey = 0;
            if (directWalking) sendWalk(modSocket, modState, 2);
            directWalking = false;
            if (leftHeld || rightHeld || showItemsWasDown) {
                mouseButton(display, 1, false);
                mouseButton(display, 3, false);
                if (showItemsWasDown) key(display, XK_Alt_L, false);
                showItemsWasDown = false;
            }
            const KeySym arrows[4] = {XK_Up, XK_Down, XK_Left, XK_Right};
            for (int i = 0; i < 4; ++i) {
                if (dpadWasDown[i] && dpadArrowWasDown[i]) key(display, arrows[i], false);
                dpadWasDown[i] = dpadArrowWasDown[i] = false;
            }
            attack = leftHeld = rightHeld = false;
            XFlush(display);
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
            releaseDirectAttack();
            if (highlightedKey) sendHighlight(modSocket, modState, nullptr), highlightedKey = 0;
            if (directWalking) sendWalk(modSocket, modState, 2);
            directWalking = false;
            if (leftHeld || rightHeld || showItemsWasDown) {
                mouseButton(display, 1, false);
                mouseButton(display, 3, false);
                if (showItemsWasDown) key(display, XK_Alt_L, false);
                showItemsWasDown = false;
            }
            const KeySym arrows[4] = {XK_Up, XK_Down, XK_Left, XK_Right};
            for (int i = 0; i < 4; ++i) {
                if (dpadWasDown[i] && dpadArrowWasDown[i]) key(display, arrows[i], false);
                dpadWasDown[i] = dpadArrowWasDown[i] = false;
            }
            attack = leftHeld = rightHeld = false;
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
        const float stickLength = std::hypot(x, y);
        const bool moveNow = stickLength >= 0.25f;
        const float dirX = moveNow ? x / stickLength : 0.0f;
        const float dirY = moveNow ? y / stickLength : 0.0f;
        const Sint16 rxRaw = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
        const Sint16 ryRaw = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);
        const float rx = stickAxis(rxRaw);
        const float ry = stickAxis(ryRaw);
        const bool stateFresh = modState.fresh(now);
        const bool pointerMode = modSocket >= 0 && (!stateFresh || modState.panelOpen());
        const bool menuNavigation = modSocket >= 0 && (!stateFresh || modState.escMenu() || modState.npcMenu());
        const bool directMove = modSocket >= 0 && stateFresh && modState.commandPort > 0 && modState.canWalk &&
                                !modState.panelOpen() && moveNow;
        if (directMove) {
            const bool turned = std::abs(dirX - lastWalkX) > 0.035f || std::abs(dirY - lastWalkY) > 0.035f;
            if (!directWalking || turned || now >= walkRefreshAt) {
                sendWalk(modSocket, modState, directWalking ? 1 : 0, dirX, dirY);
                directWalking = true;
                walkRefreshAt = now + 200;
                lastWalkX = dirX;
                lastWalkY = dirY;
            }
        } else if (directWalking) {
            sendWalk(modSocket, modState, 2);
            directWalking = false;
        }

        const bool fallbackMove = moveNow && !pointerMode && (modSocket < 0 || (stateFresh && !modState.canWalk));
        float cursorMoveX = pointerMode ? x + rx : (fallbackMove ? x : rx);
        float cursorMoveY = pointerMode ? y + ry : (fallbackMove ? y : ry);
        const AimTarget *selectedTarget = nullptr;
        if (!pointerMode && stateFresh && modState.commandPort > 0)
            selectedTarget = chooseAimTarget(modState, rx, ry);
        if (selectedTarget) {
            const uint64_t key = (uint64_t(selectedTarget->type) << 32) | selectedTarget->id;
            if (key != highlightedKey || now >= highlightRefreshAt) {
                sendHighlight(modSocket, modState, selectedTarget);
                highlightedKey = key;
                highlightRefreshAt = now + 400;
            }
        } else if (highlightedKey && modSocket >= 0 && modState.commandPort > 0) {
            sendHighlight(modSocket, modState, nullptr);
            highlightedKey = 0;
        }
        if (pointerMode) {
            cursorMoveX = x + rx;
            cursorMoveY = y + ry;
        } else {
            cursorMoveX = cursorMoveY = 0;
        }
        if (cursorMoveX != 0 || cursorMoveY != 0) {
            cursorX += cursorMoveX * 8.0f;
            cursorY += cursorMoveY * 8.0f;
            cursorX = std::clamp(cursorX, 0.0f, static_cast<float>(DisplayWidth(display, DefaultScreen(display)) - 1));
            cursorY = std::clamp(cursorY, 0.0f, static_cast<float>(DisplayHeight(display, DefaultScreen(display)) - 1));
            XTestFakeMotionEvent(display, DefaultScreen(display), static_cast<int>(cursorX), static_cast<int>(cursorY), CurrentTime);
        }

        const bool attackNow = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A);
        const bool attackWasDown = attack;
        const bool layer2 = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
        const bool triggerDown = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
        if (menuNavigation && attackNow && !attackWasDown) key(display, XK_Return, true), key(display, XK_Return, false);
        attack = attackNow;
        const bool fallbackClickMove = fallbackMove;
        const bool directTarget = !menuNavigation && !pointerMode && stateFresh && modState.commandPort > 0 && selectedTarget;
        const uint64_t selectedKey = directTarget ? (uint64_t(selectedTarget->type) << 32) | selectedTarget->id : 0;
        if (directAttackKey && (!attack || !directTarget || selectedKey != directAttackKey)) {
            releaseDirectAttack();
        }
        const bool targetAction = attack && directTarget && selectedTarget->type != 4;
        if (targetAction && !directAttackKey) {
            if (selectedTarget->type == 2 || selectedTarget->type == 5) sendUnit(modSocket, modState, *selectedTarget, 3);
            else {
                sendUnit(modSocket, modState, *selectedTarget, 0);
                directAttackKey = selectedKey;
                attackRefreshAt = now + 200;
            }
        } else if (targetAction && directAttackKey && now >= attackRefreshAt) {
            sendUnit(modSocket, modState, *selectedTarget, 1);
            attackRefreshAt = now + 200;
        }
        if (attack && !attackWasDown && directTarget && selectedTarget->type == 4) {
            const uint8_t payload[8] = {selectedTarget->type, 0, 0, 0,
                static_cast<uint8_t>(selectedTarget->id), static_cast<uint8_t>(selectedTarget->id >> 8),
                static_cast<uint8_t>(selectedTarget->id >> 16), static_cast<uint8_t>(selectedTarget->id >> 24)};
            sendModCommand(modSocket, modState.commandPort, 15, payload, sizeof(payload));
        }
        const bool wantLeft = (attack && menuNavigation) || fallbackClickMove || (attack && !stateFresh);
        if (wantLeft != leftHeld) mouseButton(display, 1, wantLeft), leftHeld = wantLeft;
        if (triggerDown && !triggerWasDown && !layer2) key(display, XK_F5, true), key(display, XK_F5, false);
        if (triggerDown && !triggerWasDown && layer2) {
            if (stateFresh && modState.hoverType != 0xff && modState.hoverId != 0xffffffffu) {
                const uint8_t payload[8] = {modState.hoverType, 0, 0, 0, static_cast<uint8_t>(modState.hoverId),
                    static_cast<uint8_t>(modState.hoverId >> 8), static_cast<uint8_t>(modState.hoverId >> 16),
                    static_cast<uint8_t>(modState.hoverId >> 24)};
                sendModCommand(modSocket, modState.commandPort, 15, payload, sizeof(payload));
            } else {
                mouseButton(display, 1, true);
                mouseButton(display, 1, false);
            }
        }
        triggerWasDown = triggerDown;

        const bool menuDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START);
        const bool inventoryDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK);
        const bool showItemsDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
        if (menuDown && !menuWasDown) key(display, XK_Escape, true), key(display, XK_Escape, false);
        if (inventoryDown && !inventoryWasDown) {
            const KeySym action = layer2 ? XK_Tab : XK_i;
            key(display, action, true);
            key(display, action, false);
        }
        if (showItemsDown != showItemsWasDown) key(display, XK_Alt_L, showItemsDown);
        menuWasDown = menuDown;
        inventoryWasDown = inventoryDown;
        showItemsWasDown = showItemsDown;
        const SDL_GameControllerButton skillButtons[4] = {SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
        const KeySym firstRow[4] = {XK_F1, XK_F2, XK_F3, XK_F4};
        const KeySym secondRow[4] = {XK_F6, XK_F7, XK_F8, NoSymbol};
        for (int i = 0; i < 4; ++i) {
            const bool down = SDL_GameControllerGetButton(controller, skillButtons[i]);
            if (down && !skillWasDown[i]) {
                if (i == 0 && pointerMode) {
                    mouseButton(display, 3, true);
                    rightHeld = true;
                } else if (i == 1 && (menuNavigation || (pointerMode && stateFresh))) {
                    key(display, XK_Escape, true);
                    key(display, XK_Escape, false);
                } else if (i == 3 && layer2) {
                    const uint8_t portal = 0;
                    sendModCommand(modSocket, modState.commandPort, 7, &portal, 1);
                } else {
                    const KeySym sym = layer2 ? secondRow[i] : firstRow[i];
                    if (sym != NoSymbol) key(display, sym, true), key(display, sym, false);
                }
            }
            if (!down && skillWasDown[i] && i == 0 && rightHeld) {
                mouseButton(display, 3, false);
                rightHeld = false;
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
        const KeySym beltKeys[4] = {XK_4, XK_3, XK_1, XK_2};
        for (int i = 0; i < 4; ++i) {
            const bool down = SDL_GameControllerGetButton(controller, buttons[i]);
            const bool arrowMode = menuNavigation;
            const bool modeChanged = down && dpadWasDown[i] && arrowMode != dpadArrowWasDown[i];
            if (modeChanged && dpadArrowWasDown[i]) key(display, arrows[i], false);
            if (down && (!dpadWasDown[i] || modeChanged)) {
                if (arrowMode) key(display, arrows[i], true);
                else if (layer2) {
                    key(display, XK_Shift_L, true);
                    key(display, beltKeys[i], true);
                    key(display, beltKeys[i], false);
                    key(display, XK_Shift_L, false);
                } else key(display, beltKeys[i], true), key(display, beltKeys[i], false);
            }
            if (!down && dpadWasDown[i] && dpadArrowWasDown[i]) key(display, arrows[i], false);
            dpadWasDown[i] = down;
            dpadArrowWasDown[i] = down && arrowMode;
        }
        XFlush(display);
        QThread::msleep(16);
    }

    releaseDirectAttack();
    if (highlightedKey) sendHighlight(modSocket, modState, nullptr);
    if (directWalking) sendWalk(modSocket, modState, 2);
    if (leftHeld) mouseButton(display, 1, false);
    if (rightHeld) mouseButton(display, 3, false);
    if (showItemsWasDown) key(display, XK_Alt_L, false);
    const KeySym arrows[4] = {XK_Up, XK_Down, XK_Left, XK_Right};
    for (int i = 0; i < 4; ++i) {
        if (dpadWasDown[i] && dpadArrowWasDown[i]) key(display, arrows[i], false);
    }
    XFlush(display);
    if (modSocket >= 0) close(modSocket);
    if (controller) SDL_GameControllerClose(controller);
    XCloseDisplay(display);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
}
