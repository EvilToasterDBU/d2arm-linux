#include "gamepad.h"

#include <SDL.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>

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
    uint16_t hotkeySkills[16]{};
    uint8_t hotkeyHands[16]{};

    bool fresh(Uint32 now) const { return stateAt && now - stateAt < 500; }
    bool panelOpen() const {
        constexpr std::array<unsigned, 21> panels = {1, 2, 3, 4, 5, 8, 9, 11, 12, 14, 15,
            16, 20, 22, 23, 24, 25, 26, 33, 36, 37};
        for (unsigned panel : panels) if (uiFlags & (uint64_t{1} << panel)) return true;
        return false;
    }
    bool inventoryGridOpen() const {
        return (uiFlags & ((uint64_t{1} << 1) | (uint64_t{1} << 25) | (uint64_t{1} << 26))) != 0;
    }
    bool quickSkillsOpen() const { return (uiFlags & (uint64_t{1} << 3)) != 0; }
    bool skillTreeOpen() const { return (uiFlags & (uint64_t{1} << 4)) != 0; }
    uint64_t panelSignature() const {
        constexpr std::array<unsigned, 21> panels = {1, 2, 3, 4, 5, 8, 9, 11, 12, 14, 15,
            16, 20, 22, 23, 24, 25, 26, 33, 36, 37};
        uint64_t result = 0;
        for (unsigned panel : panels) result |= uiFlags & (uint64_t{1} << panel);
        return result;
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

    // The state header sends a raw monster count before the filtered list size.
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
    // The state packet ends its target lists with class/current skills and 16 hotkey slots.
    if (offset + 53 <= length) {
        offset += 5; // class, left hand skill, right hand skill
        for (int i = 0; i < 16; ++i) {
            state.hotkeySkills[i] = readU16(packet + offset);
            state.hotkeyHands[i] = packet[offset + 2];
            offset += 3;
        }
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
    const bool directional = aimLength >= 0.18;
    if (directional) {
        x = static_cast<float>(x / aimLength);
        y = static_cast<float>(y / aimLength);
    }
    const AimTarget *best = nullptr;
    double bestScore = std::numeric_limits<double>::max();
    double previousScore = std::numeric_limits<double>::max();
    const AimTarget *previous = nullptr;
    for (const AimTarget &target : state.targets) {
        const double dx = target.x - state.playerX;
        const double dy = target.y - state.playerY;
        const double distance = std::hypot(dx, dy);
        if (target.x < 0 || target.x >= state.gameWidth || target.y < 0 || target.y >= state.gameHeight - 48) continue;
        const bool npc = target.type == 1 && (target.flags & 16);
        const bool enemy = target.type == 1 && !npc && target.kind != 7;
        const bool object = target.type == 2 || target.type == 5;
        // World sprites keep their pixel size as render resolution changes.
        // Undo the isometric projection for a short, roughly one-metre interaction radius.
        const double interactionDistance = std::hypot(dx / 32.0, dy / 16.0);
        if ((object || target.type == 4) && interactionDistance > 3.0) continue;
        if (target.type == 2 && target.kind == 0) continue;
        const double dot = directional && distance > 1.0 ? (dx * x + dy * y) / distance : 1.0;
        const int priority = enemy ? 0 : object ? 1 : npc ? 2 : 3;
        const double score = priority * 1000000.0 + distance * (directional ? (1.5 - dot) : 1.0);
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

    if (previous && static_cast<int>(previousScore / 1000000.0) == static_cast<int>(bestScore / 1000000.0) &&
        std::fmod(previousScore, 1000000.0) <= std::fmod(bestScore, 1000000.0) * 1.15) best = previous;
    state.aimTargetKey = best ? (uint64_t(best->type) << 32) | best->id : 0;
    return best;
}

void sendUnit(int fd, const ModState &state, const AimTarget &target, int phase, int button = 0) {
    const uint8_t payload[12] = {static_cast<uint8_t>(button), static_cast<uint8_t>(phase), 0, 0,
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

void selectBoundSkill(int fd, const ModState &state, int slot) {
    if (slot < 0 || slot >= 16 || state.hotkeySkills[slot] == 0xffff || state.commandPort <= 0) return;
    const uint16_t skill = state.hotkeySkills[slot];
    const uint8_t payload[3] = {static_cast<uint8_t>(state.hotkeyHands[slot] == 1 ? 0 : 1),
        static_cast<uint8_t>(skill), static_cast<uint8_t>(skill >> 8)};
    sendModCommand(fd, state.commandPort, 13, payload, sizeof(payload));
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
    bool runWasDown = false, swapWasDown = false;
    bool potionWasDown[2] = {false, false};
    bool rtChordWasDown[4] = {false, false, false, false};
    uint8_t lastHintMode = 0xff, lastHintButtons = 0xff;
    auto clearGamepadHints = [&]() {
        if (modState.commandPort > 0 && (lastHintMode != 0 || lastHintButtons != 0)) {
            const uint8_t payload[2] = {0, 0};
            sendModCommand(modSocket, modState.commandPort, 27, payload, sizeof(payload));
        }
        lastHintMode = lastHintButtons = 0;
        for (bool &down : rtChordWasDown) down = false;
    };
    bool pointerHidden = false;
    bool gameCursorHidden = false;
    Uint32 cursorHudRefreshAt = 0;
    uint64_t lastPanelSignature = 0;
    bool dpadWasDown[4] = {false, false, false, false};
    bool dpadArrowWasDown[4] = {false, false, false, false};
    float cursorX = DisplayWidth(display, DefaultScreen(display)) / 2.0f;
    float cursorY = DisplayHeight(display, DefaultScreen(display)) / 2.0f;
    emit statusChanged(russian_ ? QStringLiteral("Геймпад: левый стик — движение; цель выделяется автоматически; A — действие, LT+A — навык, LT+Y — быстрый выбор навыка; RT+A/B/X/Y — инвентарь/персонаж/задания/навыки.")
                                : QStringLiteral("Gamepad: left stick moves; nearest target is highlighted; A interacts, LT+A casts, LT+Y opens the quick skill picker; RT+A/B/X/Y open inventory/character/quests/skills."));

    while (running_.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED && !controller) {
                openController(event.cdevice.which);
            } else if (event.type == SDL_CONTROLLERDEVICEREMOVED && controller &&
                       SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller)) == event.cdevice.which) {
                clearGamepadHints();
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
                for (bool &wasDown : potionWasDown) wasDown = false;
                XFlush(display);
                SDL_GameControllerClose(controller);
                controller = nullptr;
                emit statusChanged(russian_ ? QStringLiteral("Геймпад отключён.") : QStringLiteral("Controller disconnected."));
            }
        }

        const Uint32 now = SDL_GetTicks();
        receiveModPackets(modSocket, modState, now);
        if (modState.fresh(now)) {
            const uint64_t panelSignature = modState.panelSignature();
            if (panelSignature != lastPanelSignature && panelSignature) {
                const int displayWidth = DisplayWidth(display, DefaultScreen(display));
                const int displayHeight = DisplayHeight(display, DefaultScreen(display));
                const double sx = static_cast<double>(displayWidth) / std::max(1, modState.gameWidth);
                const double sy = static_cast<double>(displayHeight) / std::max(1, modState.gameHeight);
                const bool stashPanel = (modState.uiFlags & ((uint64_t{1} << 25) | (uint64_t{1} << 26))) != 0;
                const bool rightPanel = !stashPanel && ((modState.uiFlags & (uint64_t{1} << 1)) || modState.skillTreeOpen());
                const bool leftPanel = stashPanel || (modState.uiFlags & ((uint64_t{1} << 2) | (uint64_t{1} << 15) |
                    (uint64_t{1} << 17) | (uint64_t{1} << 36)));
                const double gameX = modState.quickSkillsOpen() ? modState.gameWidth - 90 : rightPanel ? modState.gameWidth * 0.78 : leftPanel ? modState.gameWidth * 0.22 : modState.gameWidth * 0.5;
                const double gameY = modState.quickSkillsOpen() ? modState.gameHeight - 90 : modState.inventoryGridOpen() ? modState.gameHeight * 0.60 :
                    modState.skillTreeOpen() ? modState.gameHeight * 0.48 : modState.gameHeight * 0.50;
                cursorX = std::clamp(static_cast<float>(gameX * sx), 0.0f, static_cast<float>(displayWidth - 1));
                cursorY = std::clamp(static_cast<float>(gameY * sy), 0.0f, static_cast<float>(displayHeight - 1));
                XTestFakeMotionEvent(display, DefaultScreen(display), static_cast<int>(cursorX), static_cast<int>(cursorY), CurrentTime);
            }
            lastPanelSignature = panelSignature;
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
            clearGamepadHints();
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
            if (pointerHidden) XFixesShowCursor(display, DefaultRootWindow(display)), pointerHidden = false;
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
            clearGamepadHints();
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
            if (pointerHidden) XFixesShowCursor(display, DefaultRootWindow(display)), pointerHidden = false;
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
        const bool hidePointer = stateFresh && !pointerMode;
        if (hidePointer != pointerHidden) {
            if (hidePointer) XFixesHideCursor(display, DefaultRootWindow(display));
            else XFixesShowCursor(display, DefaultRootWindow(display));
            pointerHidden = hidePointer;
        }
        if (modState.commandPort > 0 && (hidePointer != gameCursorHidden || now >= cursorHudRefreshAt)) {
            const uint8_t hud[3] = {static_cast<uint8_t>(hidePointer ? 4 : 0), 0, 0};
            sendModCommand(modSocket, modState.commandPort, 5, hud, sizeof(hud));
            gameCursorHidden = hidePointer;
            cursorHudRefreshAt = now + 1000;
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
        const SDL_GameControllerButton chordButtons[4] = {SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
            SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y};
        const KeySym rtKeys[4] = {XK_i, XK_c, XK_q, XK_t};
        uint8_t hintButtons = 0;
        for (int i = 0; i < 4; ++i) {
            const bool down = SDL_GameControllerGetButton(controller, chordButtons[i]);
            if (down) hintButtons |= static_cast<uint8_t>(1u << i);
            const bool chordDown = triggerDown && down;
            if (chordDown && !rtChordWasDown[i]) key(display, rtKeys[i], true), key(display, rtKeys[i], false);
            rtChordWasDown[i] = chordDown;
        }
        const uint8_t hintMode = static_cast<uint8_t>((layer2 ? 1 : 0) | (triggerDown ? 2 : 0));
        if (modState.commandPort > 0 && (hintMode != lastHintMode || hintButtons != lastHintButtons)) {
            const uint8_t payload[2] = {hintMode, hintButtons};
            sendModCommand(modSocket, modState.commandPort, 27, payload, sizeof(payload));
            lastHintMode = hintMode;
            lastHintButtons = hintButtons;
        }
        if (attackNow && !attackWasDown && layer2 && !triggerDown && !pointerMode && stateFresh && modState.commandPort > 0) {
            if (selectedTarget && selectedTarget->type != 4) sendUnit(modSocket, modState, *selectedTarget, 3, 1);
            else { mouseButton(display, 3, true); mouseButton(display, 3, false); }
        }
        const bool effectiveAttack = attackNow && !triggerDown;
        if (menuNavigation && effectiveAttack && !attackWasDown) key(display, XK_Return, true), key(display, XK_Return, false);
        attack = effectiveAttack;
        const bool fallbackClickMove = fallbackMove;
        const bool directTarget = !triggerDown && !menuNavigation && !pointerMode && stateFresh && modState.commandPort > 0 && selectedTarget;
        const uint64_t selectedKey = directTarget ? (uint64_t(selectedTarget->type) << 32) | selectedTarget->id : 0;
        if (directAttackKey && (!attack || !directTarget || selectedKey != directAttackKey)) {
            releaseDirectAttack();
        }
        const bool targetAction = attack && !layer2 && directTarget && selectedTarget->type != 4;
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
        if (attack && !attackWasDown && !layer2 && directTarget && selectedTarget->type == 4) {
            const uint8_t payload[8] = {selectedTarget->type, 0, 0, 0,
                static_cast<uint8_t>(selectedTarget->id), static_cast<uint8_t>(selectedTarget->id >> 8),
                static_cast<uint8_t>(selectedTarget->id >> 16), static_cast<uint8_t>(selectedTarget->id >> 24)};
            sendModCommand(modSocket, modState.commandPort, 15, payload, sizeof(payload));
        }
        const bool wantLeft = (attack && !layer2 && pointerMode && !menuNavigation) || fallbackClickMove ||
                              (attack && !stateFresh && !triggerDown && !layer2);
        if (wantLeft != leftHeld) mouseButton(display, 1, wantLeft), leftHeld = wantLeft;

        const bool menuDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_START);
        const bool inventoryDown = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_BACK);
        const bool showItemsDown = false;
        const SDL_GameControllerButton potionButtons[2] = {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER};
        for (int i = 0; i < 2; ++i) {
            const bool down = SDL_GameControllerGetButton(controller, potionButtons[i]);
            if (down && !potionWasDown[i] && stateFresh && modState.commandPort > 0) {
                const uint8_t potion[2] = {static_cast<uint8_t>(i), 0};
                sendModCommand(modSocket, modState.commandPort, 1, potion, sizeof(potion));
            }
            potionWasDown[i] = down;
        }
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
            const bool down = i < 3 && SDL_GameControllerGetButton(controller, skillButtons[i]);
            if (down && !skillWasDown[i]) {
                if (triggerDown && i < 4) {
                    // RT+A/B/X/Y were handled as game panel chords above.
                } else if (i == 0 && pointerMode) {
                    mouseButton(display, 3, true);
                    rightHeld = true;
                } else if (i == 1 && (menuNavigation || (pointerMode && stateFresh))) {
                    key(display, XK_Escape, true);
                    key(display, XK_Escape, false);
                } else if (layer2 && i == 2) {
                    key(display, XK_s, true);
                    key(display, XK_s, false);
                } else if (layer2 && i < 3 && stateFresh && modState.commandPort > 0 && modState.hotkeySkills[i + 4] != 0xffff) {
                    selectBoundSkill(modSocket, modState, i + 4);
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
            const bool arrowMode = menuNavigation && !modState.escMenu();
            const bool modeChanged = down && dpadWasDown[i] && arrowMode != dpadArrowWasDown[i];
            if (modeChanged && dpadArrowWasDown[i]) key(display, arrows[i], false);
            if (down && (!dpadWasDown[i] || modeChanged)) {
                if (arrowMode) key(display, arrows[i], true);
                else if (pointerMode) {
                    const float displayW = static_cast<float>(DisplayWidth(display, DefaultScreen(display)));
                    const float displayH = static_cast<float>(DisplayHeight(display, DefaultScreen(display)));
                    const int step = std::max(20, static_cast<int>(displayH / (modState.inventoryGridOpen() ? 20 : modState.skillTreeOpen() ? 10 : 12)));
                    const int cursorStep[4][2] = {{0,-step},{0,step},{-step,0},{step,0}};
                    float left = 0.10f, right = 0.90f, top = 0.20f, bottom = 0.82f;
                    if (modState.inventoryGridOpen()) {
                        const bool stash = (modState.uiFlags & ((uint64_t{1} << 25) | (uint64_t{1} << 26))) != 0;
                        left = stash ? 0.04f : 0.62f; right = stash ? 0.39f : 0.98f;
                        top = 0.28f; bottom = 0.88f;
                    } else if (modState.skillTreeOpen()) {
                        left = 0.60f; right = 0.98f; top = 0.18f; bottom = 0.78f;
                    } else if (modState.uiFlags & (uint64_t{1} << 2)) {
                        left = 0.04f; right = 0.40f;
                    }
                    cursorX = std::clamp(cursorX + cursorStep[i][0], displayW * left, displayW * right);
                    cursorY = std::clamp(cursorY + cursorStep[i][1], displayH * top, displayH * bottom);
                    XTestFakeMotionEvent(display, DefaultScreen(display), static_cast<int>(cursorX), static_cast<int>(cursorY), CurrentTime);
                }
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
    XFixesShowCursor(display, DefaultRootWindow(display));
    if (modSocket >= 0) close(modSocket);
    if (controller) SDL_GameControllerClose(controller);
    XCloseDisplay(display);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS);
}
