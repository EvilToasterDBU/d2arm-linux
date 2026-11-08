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
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <string>
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

struct SkillFocusPoint {
    uint16_t skillId = 0;
    uint8_t row = 0;
    uint8_t column = 0;
    float x = 0;
    float y = 0;
};

struct FocusPoint { float x, y; };

int resolveButtonStyle(SDL_GameController *controller, int preference) {
    if (preference == 1) return 0;
    if (preference == 2) return 1;
    const char *rawName = controller ? SDL_GameControllerName(controller) : nullptr;
    if (!rawName) return 0;
    std::string name(rawName);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return name.find("playstation") != std::string::npos || name.find("dualsense") != std::string::npos ||
           name.find("dualshock") != std::string::npos || name.find("sony") != std::string::npos ||
           name.find("ps4") != std::string::npos || name.find("ps5") != std::string::npos ? 1 : 0;
}

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
    int leftSkill = -1;
    int rightSkill = -1;
    uint64_t aimTargetKey = 0;
    std::vector<AimTarget> targets;
    std::vector<AimTarget> corpses;
    uint16_t hotkeySkills[16]{};
    uint8_t hotkeyHands[16]{};
    bool skillFocusOpen = false;
    bool skillFocusReady = false;
    int skillFocusClass = -1;
    int skillFocusTab = 0;
    uint32_t skillFocusRevision = 0;
    std::vector<SkillFocusPoint> skillFocusPoints;
    uint32_t uiFocusRevision = 0;
    std::vector<FocusPoint> uiFocusPoints;

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
    state.corpses.clear();
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
    // The packet follows the target lists with active skill ids and hotkeys.
    if (offset + 53 <= length) {
        state.leftSkill = readU16(packet + offset + 1);
        state.rightSkill = readU16(packet + offset + 3);
        offset += 5;
        for (int i = 0; i < 16; ++i) {
            state.hotkeySkills[i] = readU16(packet + offset);
            state.hotkeyHands[i] = packet[offset + 2];
            offset += 3;
        }
        if (offset >= length) return;
        const size_t damaged = packet[offset++];
        if (offset + damaged * 2 + 10 > length) return;
        offset += damaged * 2 + 10; // durability, vitals, game-state bits and portal charges
        if (offset >= length) return;
        const size_t corpseCount = packet[offset++];
        if (corpseCount > 64 || offset + corpseCount * 8 > length) return;
        for (size_t i = 0; i < corpseCount; ++i) {
            AimTarget corpse;
            corpse.type = 1;
            corpse.kind = 7;
            corpse.id = readU32(packet + offset);
            corpse.x = readI16(packet + offset + 4);
            corpse.y = readI16(packet + offset + 6);
            state.corpses.push_back(corpse);
            offset += 8;
        }
    }
}

std::vector<FocusPoint> inventoryFocusPoints(const ModState &state) {
    const float left = state.gameWidth / 2.0f;
    const float top = (state.gameHeight - 480) / 2.0f;
    std::vector<FocusPoint> points;
    auto add = [&points](float x, float y) {
        for (const auto &point : points)
            if (std::abs(point.x - x) < 0.5f && std::abs(point.y - y) < 0.5f) return;
        points.push_back({x, y});
    };
    const uint64_t flags = state.uiFlags;
    const auto open = [flags](unsigned index) { return (flags & (uint64_t{1} << index)) != 0; };

    // The reference captures are 1920x1080. Convert their measured control
    // centers into the game's current client coordinates so the maps remain
    // aligned at other resolutions and aspect ratios.
    const auto addRef = [&add, &state](float x, float y) {
        add(x * state.gameWidth / 1920.0f, y * state.gameHeight / 1080.0f);
    };

    // These overlays own the cursor while open; never mix their targets with
    // the player inventory or another panel's focus map.
    if (state.skillTreeOpen()) return points;
    if (state.quickSkillsOpen()) {
        addRef(1620, 660); addRef(1710, 660); addRef(1710, 757);
        addRef(1548, 857); addRef(1638, 857); addRef(1728, 857);
        return points;
    }

    const bool leftPanelOpen = open(25) || open(26) || open(12) || open(23) || open(2) ||
        open(15) || open(17) || open(20);

    if (open(1) || leftPanelOpen) {
        // Keep the established player inventory and equipment focus map unchanged.
        const FocusPoint controls[] = {
            {32,35}, {65,35}, {263,35}, {295,35},
            {46,104}, {276,104}, {162,33}, {219,47}, {162,121},
            {46,207}, {105,191}, {162,191}, {219,191}, {276,207},
            {34,403}, {92,403}
        };
        for (auto p : controls) add(left + p.x, top + p.y);
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 10; ++col)
                add(left + 31.0f + col * 29, top + 269.5f + row * 29);
        // The belt is still visible alongside the left-side panels in the
        // reference captures. Keep its points separate from the protected
        // player-inventory grid above.
        if (!open(1)) {
            constexpr float beltX[] = {1037, 1098, 1158, 1218};
            for (float x : beltX) addRef(x, 1040);
        }
    }

    if (open(25) || open(26)) {
        constexpr float firstX = 493, firstY = 249, cell = 59.3f;
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 6; ++col)
                addRef(firstX + col * cell, firstY + row * cell);
        addRef(490, 126); // stash gold control
        addRef(897, 864); // close stash
    }

    if (open(12) || open(23)) {
        constexpr float tabX[] = {400, 555, 719, 882};
        for (float x : tabX) addRef(x, 89); // shop tabs
        for (int row = 0; row < 8; ++row)
            for (int col = 0; col < 8; ++col)
                addRef(385 + col * 74.5f, 220 + row * 73.5f);
        constexpr float actionX[] = {582, 685, 790, 895};
        for (float x : actionX) addRef(x, 862);
    }

    if (open(2)) {
        constexpr float statY[] = {245, 369, 519, 669};
        for (float y : statY) addRef(598, y); // four attribute allocation buttons
        addRef(610, 870); // close character sheet
    }

    if (open(15) || open(17)) {
        constexpr float tabX[] = {395, 519, 643, 767, 891};
        for (float x : tabX) addRef(x, 89);
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 3; ++col)
                addRef(442 + col * 180, 237 + row * 190);
        addRef(802, 874);
        addRef(909, 874);
    }

    if (open(20)) {
        constexpr float tabX[] = {395, 519, 643, 767, 891};
        constexpr float rowY[] = {210, 281, 351, 422, 493, 564, 635, 705, 775};
        for (float x : tabX) addRef(x, 89);
        for (float y : rowY) addRef(383, y);
        addRef(897, 865); // close waypoint window
    }

    return points;
}

FocusPoint inventoryFocus(const ModState &state, float x, float y, int direction = -1) {
    const auto points = inventoryFocusPoints(state);
    size_t current = 0;
    double nearest = std::numeric_limits<double>::max();
    for (size_t n = 0; n < points.size(); ++n) {
        const double d = std::hypot(points[n].x - x, points[n].y - y);
        if (d < nearest) nearest = d, current = n;
    }
    if (direction < 0) return points[current];
    const int dx[] = {0,0,-1,1}, dy[] = {-1,1,0,0};
    size_t next = current;
    double best = std::numeric_limits<double>::max();
    for (size_t n = 0; n < points.size(); ++n) {
        const float xdiff = points[n].x - points[current].x;
        const float ydiff = points[n].y - points[current].y;
        const double along = xdiff * dx[direction] + ydiff * dy[direction];
        const double across = std::abs(xdiff * dy[direction] - ydiff * dx[direction]);
        if (along <= 1 || across > along * 2.5) continue;
        const double score = along + across * 3.0;
        if (score < best) best = score, next = n;
    }
    return points[next];
}

FocusPoint skillTreeFocus(const ModState &state, float x, float y, int direction = -1) {
    std::vector<FocusPoint> points;
    for (const auto &point : state.skillFocusPoints) points.push_back({point.x, point.y});
    for (float y : {381.0f, 601.0f, 813.0f})
        points.push_back({1508.0f * state.gameWidth / 1920.0f, y * state.gameHeight / 1080.0f});
    if (points.empty()) return {x, y};
    size_t current = 0;
    double nearest = std::numeric_limits<double>::max();
    for (size_t n = 0; n < points.size(); ++n) {
        const double d = std::hypot(points[n].x - x, points[n].y - y);
        if (d < nearest) nearest = d, current = n;
    }
    if (direction < 0) return points[current];
    const int dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
    size_t next = current;
    double best = std::numeric_limits<double>::max();
    const auto &origin = points[current];
    for (size_t n = 0; n < points.size(); ++n) {
        const auto &point = points[n];
        const float xdiff = point.x - origin.x, ydiff = point.y - origin.y;
        const double along = xdiff * dx[direction] + ydiff * dy[direction];
        const double across = std::abs(xdiff * dy[direction] - ydiff * dx[direction]);
        if (along <= 1 || across > along * 2.5) continue;
        const double score = along + across * 3.0;
        if (score < best) best = score, next = n;
    }
    return points[next];
}

FocusPoint uiControlFocus(const ModState &state, float x, float y, int direction = -1) {
    if (state.uiFocusPoints.empty()) return {x, y};
    size_t current = 0;
    double nearest = std::numeric_limits<double>::max();
    for (size_t n = 0; n < state.uiFocusPoints.size(); ++n) {
        const double d = std::hypot(state.uiFocusPoints[n].x - x, state.uiFocusPoints[n].y - y);
        if (d < nearest) nearest = d, current = n;
    }
    if (direction < 0) return state.uiFocusPoints[current];
    const int dx[] = {0, 0, -1, 1}, dy[] = {-1, 1, 0, 0};
    size_t next = current;
    double best = std::numeric_limits<double>::max();
    const auto &origin = state.uiFocusPoints[current];
    for (size_t n = 0; n < state.uiFocusPoints.size(); ++n) {
        const auto &point = state.uiFocusPoints[n];
        const float xdiff = point.x - origin.x, ydiff = point.y - origin.y;
        const double along = xdiff * dx[direction] + ydiff * dy[direction];
        const double across = std::abs(xdiff * dy[direction] - ydiff * dx[direction]);
        if (along <= 1 || across > along * 2.5) continue;
        const double score = along + across * 3.0;
        if (score < best) best = score, next = n;
    }
    return state.uiFocusPoints[next];
}

void receiveUiFocusPacket(const uint8_t *packet, size_t length, ModState &state) {
    if (length < 9) return;
    const size_t count = packet[8];
    if (count > 48 || length < 9 + count * 4) return;
    state.uiFocusPoints.clear();
    for (size_t i = 0; i < count; ++i) {
        const uint8_t *point = packet + 9 + i * 4;
        const int x = readU16(point), y = readU16(point + 2);
        if (x >= 0 && x < state.gameWidth && y >= 0 && y < state.gameHeight)
            state.uiFocusPoints.push_back({static_cast<float>(x), static_cast<float>(y)});
    }
    ++state.uiFocusRevision;
}

void receiveSkillFocusPacket(const uint8_t *packet, size_t length, ModState &state) {
    if (length < 17) return;
    state.skillFocusOpen = packet[8] != 0;
    state.skillFocusReady = packet[9] != 0;
    const int width = readU16(packet + 10), height = readU16(packet + 12);
    state.skillFocusClass = packet[14] == 0xff ? -1 : packet[14];
    state.skillFocusTab = packet[15];
    const size_t count = packet[16];
    state.skillFocusPoints.clear();
    if (count > 30 || length < 17 + count * 8) state.skillFocusReady = false;
    else if (state.skillFocusReady && width > 0 && height > 0) {
        for (size_t i = 0; i < count; ++i) {
            const uint8_t *node = packet + 17 + i * 8;
            const int x = readU16(node + 2), y = readU16(node + 4);
            if (x >= width || y >= height || node[6] < 1 || node[6] > 6 || node[7] < 1 || node[7] > 3) continue;
            state.skillFocusPoints.push_back({readU16(node), node[6], node[7], static_cast<float>(x), static_cast<float>(y)});
        }
    }
    ++state.skillFocusRevision;
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
        } else if (packet[5] == 18) {
            receiveSkillFocusPacket(packet.data(), static_cast<size_t>(n), state);
        } else if (packet[5] == 19) {
            receiveUiFocusPacket(packet.data(), static_cast<size_t>(n), state);
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
    const double aimMagnitude = std::clamp(static_cast<double>(std::hypot(x, y)), 0.0, 1.0);
    const bool directional = aimMagnitude >= 0.18;
    const double maxAimDistance = std::min(state.gameWidth, state.gameHeight) * 0.72;
    const double desiredDistance = maxAimDistance * aimMagnitude;
    if (directional) {
        x = static_cast<float>(x / aimMagnitude);
        y = static_cast<float>(y / aimMagnitude);
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
        const bool stash = object && (target.kind == 0 ||
            (state.uiFlags & ((uint64_t{1} << 25) | (uint64_t{1} << 26))));
        if (object && interactionDistance > (stash ? 2.0 : 1.0)) continue;
        if (target.type == 4 && interactionDistance > 1.0) continue;
        if (npc && interactionDistance > 3.0) continue;
        double score = 0;
        if (directional) {
            const double along = dx * x + dy * y;
            const double across = std::abs(dx * y - dy * x);
            if (along < -8 || along > maxAimDistance + 80 || across > std::max(28.0, along * 0.22)) continue;
            score = std::abs(along - desiredDistance) + across * 2.5 + distance * 0.08;
        } else {
            score = distance;
        }
        // Keep a modest class preference for combat targets; aiming direction
        // and stick extension determine the lock when targets are elsewhere.
        score += enemy ? 0.0 : object ? 22.0 : npc ? 30.0 : 40.0;
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

    if (previous && previousScore <= bestScore * 1.15 + 12.0) best = previous;
    state.aimTargetKey = best ? (uint64_t(best->type) << 32) | best->id : 0;
    return best;
}

bool isCorpseSkill(int skill) {
    switch (skill) {
        case 70: case 74: case 80: case 95: case 131: case 142: case 150: return true;
        default: return false;
    }
}

const AimTarget *chooseCorpseTarget(const ModState &state, float x, float y) {
    const double aimLength = std::hypot(x, y);
    const bool directional = aimLength >= 0.18;
    if (directional) x /= static_cast<float>(aimLength), y /= static_cast<float>(aimLength);
    const AimTarget *best = nullptr;
    double bestScore = std::numeric_limits<double>::max();
    for (const AimTarget &corpse : state.corpses) {
        const double dx = corpse.x - state.playerX, dy = corpse.y - state.playerY;
        const double distance = std::hypot(dx, dy);
        const double interactionDistance = std::hypot(dx / 32.0, dy / 16.0);
        if (corpse.x < 0 || corpse.x >= state.gameWidth || corpse.y < 0 || corpse.y >= state.gameHeight - 48 ||
            interactionDistance > 20.0) continue;
        const double dot = directional && distance > 1.0 ? (dx * x + dy * y) / distance : 1.0;
        if (directional && dot < 0.0) continue;
        const double score = interactionDistance * (directional ? (1.5 - dot) : 1.0);
        if (score < bestScore) best = &corpse, bestScore = score;
    }
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

void castBoundSkill(Display *display, int fd, const ModState &state, int slot,
                    const AimTarget *target, float aimX, float aimY, float &cursorX, float &cursorY) {
    if (slot < 0 || slot >= 16 || state.hotkeySkills[slot] == 0xffff || state.commandPort <= 0) return;
    selectBoundSkill(fd, state, slot);
    const int button = state.hotkeyHands[slot] == 1 ? 0 : 1;
    if (target && target->type != 4) {
        sendUnit(fd, state, *target, 3, button);
        return;
    }

    const double aimLength = std::hypot(aimX, aimY);
    if (aimLength > 1.0) aimX /= static_cast<float>(aimLength), aimY /= static_cast<float>(aimLength);
    const double reach = std::min(state.gameWidth, state.gameHeight) * 0.72;
    const double gameX = state.playerX + aimX * reach;
    const double gameY = state.playerY + aimY * reach;
    const int screenW = DisplayWidth(display, DefaultScreen(display));
    const int screenH = DisplayHeight(display, DefaultScreen(display));
    cursorX = std::clamp(static_cast<float>(gameX * screenW / std::max(1, state.gameWidth)), 0.0f, static_cast<float>(screenW - 1));
    cursorY = std::clamp(static_cast<float>(gameY * screenH / std::max(1, state.gameHeight)), 0.0f, static_cast<float>(screenH - 1));
    XTestFakeMotionEvent(display, DefaultScreen(display), static_cast<int>(cursorX), static_cast<int>(cursorY), CurrentTime);
    mouseButton(display, button ? 3 : 1, true);
    mouseButton(display, button ? 3 : 1, false);
}
}

Gamepad::Gamepad(QObject *parent) : QThread(parent) {}
Gamepad::~Gamepad() { stop(); }

void Gamepad::setEnabled(bool enabled) { enabled_.store(enabled); }
void Gamepad::setLauncherMode(bool enabled) { launcherMode_.store(enabled); }
void Gamepad::setRussian(bool russian) { russian_.store(russian); }
void Gamepad::setAimAssist(bool enabled) { aimAssist_.store(enabled); }
void Gamepad::setQuickCast(bool enabled) { quickCast_.store(enabled); }
void Gamepad::setStickDeadzone(int percent) { stickDeadzonePercent_.store(std::clamp(percent, 5, 45)); }
void Gamepad::setStickSensitivity(int percent) { stickSensitivityPercent_.store(std::clamp(percent, 50, 200)); }
void Gamepad::setButtonStyle(int style) { buttonStyle_.store(std::clamp(style, 0, 2)); }
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
    Uint32 attackRefreshAt = 0, highlightRefreshAt = 0, corpseCastRefreshAt = 0;
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
    Uint32 potionReadyAt = 0;
    bool rtChordWasDown[4] = {false, false, false, false};
    uint8_t lastHintMode = 0xff, lastHintButtons = 0xff, lastHintStyle = 0xff;
    int activeButtonStyle = 0;
    auto clearGamepadHints = [&]() {
        if (modState.commandPort > 0 && (lastHintMode != 0 || lastHintButtons != 0)) {
            const uint8_t payload[3] = {0, 0, static_cast<uint8_t>(activeButtonStyle)};
            sendModCommand(modSocket, modState.commandPort, 27, payload, sizeof(payload));
        }
        lastHintMode = lastHintButtons = 0;
        for (bool &down : rtChordWasDown) down = false;
    };
    bool pointerHidden = false;
    bool gameCursorHidden = false;
    Uint32 cursorHudRefreshAt = 0;
    uint64_t lastPanelSignature = 0;
    uint32_t seenSkillFocusRevision = 0;
    uint32_t seenUiFocusRevision = 0;
    bool skillTreeCursorPending = false;
    bool dpadWasDown[4] = {false, false, false, false};
    bool dpadArrowWasDown[4] = {false, false, false, false};
    float cursorX = DisplayWidth(display, DefaultScreen(display)) / 2.0f;
    float cursorY = DisplayHeight(display, DefaultScreen(display)) / 2.0f;
    bool controllerMode = false;
    bool controllerSampled = false;
    Sint16 previousAxes[6]{};
    bool previousButtons[SDL_CONTROLLER_BUTTON_MAX]{};
    char previousKeymap[32]{};
    int previousPointerX = -1, previousPointerY = -1;
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
        if (modState.uiFocusRevision != seenUiFocusRevision) {
            seenUiFocusRevision = modState.uiFocusRevision;
            const bool inventoryPanel = modState.fresh(now) && (modState.uiFlags & (uint64_t{1} << 1));
            const bool skillPanel = modState.fresh(now) && modState.skillTreeOpen();
            if (!modState.uiFocusPoints.empty() && !inventoryPanel && !skillPanel) {
                const int displayWidth = DisplayWidth(display, DefaultScreen(display));
                const int displayHeight = DisplayHeight(display, DefaultScreen(display));
                const double sx = static_cast<double>(displayWidth) / std::max(1, modState.gameWidth);
                const double sy = static_cast<double>(displayHeight) / std::max(1, modState.gameHeight);
                const auto focus = uiControlFocus(modState, cursorX / sx, cursorY / sy);
                cursorX = focus.x * sx;
                cursorY = focus.y * sy;
                XTestFakeMotionEvent(display, DefaultScreen(display), static_cast<int>(cursorX), static_cast<int>(cursorY), CurrentTime);
            }
        }
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
                if (modState.skillTreeOpen()) skillTreeCursorPending = true;
                if (!inventoryFocusPoints(modState).empty()) {
                    const auto focus = inventoryFocus(modState, cursorX / sx, cursorY / sy);
                    cursorX = focus.x * sx;
                    cursorY = focus.y * sy;
                } else if (!modState.skillTreeOpen() && !modState.uiFocusPoints.empty()) {
                    const auto focus = uiControlFocus(modState, cursorX / sx, cursorY / sy);
                    cursorX = focus.x * sx;
                    cursorY = focus.y * sy;
                }
                XTestFakeMotionEvent(display, DefaultScreen(display), static_cast<int>(cursorX), static_cast<int>(cursorY), CurrentTime);
            }
            lastPanelSignature = panelSignature;
            if (!modState.skillTreeOpen()) skillTreeCursorPending = false;
            if (modState.skillTreeOpen() && modState.skillFocusReady &&
                modState.skillFocusRevision != seenSkillFocusRevision) {
                seenSkillFocusRevision = modState.skillFocusRevision;
                if (skillTreeCursorPending && !modState.skillFocusPoints.empty()) {
                    const double sx = static_cast<double>(DisplayWidth(display, DefaultScreen(display))) / std::max(1, modState.gameWidth);
                    const double sy = static_cast<double>(DisplayHeight(display, DefaultScreen(display))) / std::max(1, modState.gameHeight);
                    const auto focus = skillTreeFocus(modState, cursorX / sx, cursorY / sy);
                    cursorX = focus.x * sx;
                    cursorY = focus.y * sy;
                    XTestFakeMotionEvent(display, DefaultScreen(display), static_cast<int>(cursorX), static_cast<int>(cursorY), CurrentTime);
                    skillTreeCursorPending = false;
                }
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
        auto stickAxis = [this](Sint16 axis) {
            const float deadzone = stickDeadzonePercent_.load() / 100.0f;
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
        const Sint16 axes[6] = {lx, ly, rxRaw, ryRaw,
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT),
            SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT)};
        bool controllerInput = false;
        for (int i = 0; i < 6; ++i) {
            if (controllerSampled && std::abs(static_cast<int>(axes[i]) - previousAxes[i]) > 5000) controllerInput = true;
            previousAxes[i] = axes[i];
        }
        for (int i = 0; i < SDL_CONTROLLER_BUTTON_MAX; ++i) {
            const bool down = SDL_GameControllerGetButton(controller, static_cast<SDL_GameControllerButton>(i));
            if (down && (!controllerSampled || !previousButtons[i])) controllerInput = true;
            previousButtons[i] = down;
        }
        char keymap[32]{};
        XQueryKeymap(display, keymap);
        bool keyboardInput = false;
        for (int code = 8; code < 256; ++code) {
            const unsigned mask = 1u << (code & 7);
            if ((static_cast<unsigned char>(keymap[code >> 3]) & mask) &&
                !(static_cast<unsigned char>(previousKeymap[code >> 3]) & mask)) {
                keyboardInput = true;
                break;
            }
        }
        std::memcpy(previousKeymap, keymap, sizeof(previousKeymap));
        Window rootWindow = DefaultRootWindow(display), childWindow = 0;
        int rootX = 0, rootY = 0, windowX = 0, windowY = 0;
        unsigned pointerMask = 0;
        if (XQueryPointer(display, rootWindow, &rootWindow, &childWindow, &rootX, &rootY,
                          &windowX, &windowY, &pointerMask)) {
            const bool moved = previousPointerX >= 0 &&
                (std::abs(rootX - previousPointerX) > 2 || std::abs(rootY - previousPointerY) > 2);
            const bool matchesPadCursor = std::abs(rootX - static_cast<int>(cursorX)) <= 2 &&
                                          std::abs(rootY - static_cast<int>(cursorY)) <= 2;
            if (moved && !matchesPadCursor) keyboardInput = true;
            previousPointerX = rootX;
            previousPointerY = rootY;
        }
        controllerSampled = true;
        if (controllerInput) controllerMode = true;
        if (keyboardInput) controllerMode = false;
        if (!controllerMode) {
            clearGamepadHints();
            releaseDirectAttack();
            if (highlightedKey) sendHighlight(modSocket, modState, nullptr), highlightedKey = 0;
            if (directWalking) sendWalk(modSocket, modState, 2), directWalking = false;
            if (leftHeld) mouseButton(display, 1, false), leftHeld = false;
            if (rightHeld) mouseButton(display, 3, false), rightHeld = false;
            if (showItemsWasDown) key(display, XK_Alt_L, false), showItemsWasDown = false;
            const KeySym arrows[4] = {XK_Up, XK_Down, XK_Left, XK_Right};
            for (int i = 0; i < 4; ++i) {
                if (dpadWasDown[i] && dpadArrowWasDown[i]) key(display, arrows[i], false);
                dpadWasDown[i] = dpadArrowWasDown[i] = false;
            }
            attack = menuWasDown = inventoryWasDown = false;
            for (bool &wasDown : skillWasDown) wasDown = false;
            for (bool &wasDown : potionWasDown) wasDown = false;
            for (bool &wasDown : rtChordWasDown) wasDown = false;
            if (pointerHidden) XFixesShowCursor(display, DefaultRootWindow(display)), pointerHidden = false;
            if (gameCursorHidden && modState.commandPort > 0) {
                const uint8_t hud[3] = {0, 0, 0};
                sendModCommand(modSocket, modState.commandPort, 5, hud, sizeof(hud));
                gameCursorHidden = false;
            }
            XFlush(display);
            QThread::msleep(16);
            continue;
        }
        const bool stateFresh = modState.fresh(now);
        // Quick Cast changes when a skill is triggered, not how the stick aims.
        // Disabling aim assist alone restores the visible, manually controlled pointer.
        const bool manualAim = stateFresh && !modState.panelOpen() && !aimAssist_.load();
        const bool pointerMode = modSocket >= 0 && (!stateFresh || modState.panelOpen() || manualAim);
        const bool menuNavigation = modSocket >= 0 && ((!stateFresh && modState.uiFocusPoints.empty()) ||
            modState.escMenu() || modState.npcMenu());
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
            cursorMoveX = manualAim ? rx : x + rx;
            cursorMoveY = manualAim ? ry : y + ry;
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
            const float sensitivity = stickSensitivityPercent_.load() / 100.0f;
            cursorX += cursorMoveX * 8.0f * sensitivity;
            cursorY += cursorMoveY * 8.0f * sensitivity;
            cursorX = std::clamp(cursorX, 0.0f, static_cast<float>(DisplayWidth(display, DefaultScreen(display)) - 1));
            cursorY = std::clamp(cursorY, 0.0f, static_cast<float>(DisplayHeight(display, DefaultScreen(display)) - 1));
            XTestFakeMotionEvent(display, DefaultScreen(display), static_cast<int>(cursorX), static_cast<int>(cursorY), CurrentTime);
        }

        const bool attackNow = SDL_GameControllerGetButton(controller, SDL_CONTROLLER_BUTTON_A);
        const bool attackWasDown = attack;
        const bool layer2 = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
        const bool triggerDown = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
        const bool corpseSkill = isCorpseSkill(layer2 ? modState.rightSkill : modState.leftSkill);
        const AimTarget *corpseTarget = corpseSkill && !pointerMode && stateFresh && !triggerDown &&
            !menuNavigation && modState.commandPort > 0 ? chooseCorpseTarget(modState, rx, ry) : nullptr;
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
        activeButtonStyle = resolveButtonStyle(controller, buttonStyle_.load());
        const uint8_t hintMode = static_cast<uint8_t>((layer2 ? 1 : 0) | (triggerDown ? 2 : 0));
        if (modState.commandPort > 0 && (hintMode != lastHintMode || hintButtons != lastHintButtons ||
                                         activeButtonStyle != lastHintStyle)) {
            const uint8_t payload[3] = {hintMode, hintButtons, static_cast<uint8_t>(activeButtonStyle)};
            sendModCommand(modSocket, modState.commandPort, 27, payload, sizeof(payload));
            lastHintMode = hintMode;
            lastHintButtons = hintButtons;
            lastHintStyle = static_cast<uint8_t>(activeButtonStyle);
        }
        if (attackNow && !attackWasDown && layer2 && !triggerDown && !pointerMode && stateFresh && modState.commandPort > 0) {
            if (!corpseSkill && selectedTarget && selectedTarget->type != 4) sendUnit(modSocket, modState, *selectedTarget, 3, 1);
            else if (!corpseSkill) {
                mouseButton(display, 3, true);
                mouseButton(display, 3, false);
            }
        }
        if (attackNow && !attackWasDown && layer2 && !triggerDown && manualAim) {
            mouseButton(display, 3, true);
            mouseButton(display, 3, false);
        }
        const bool effectiveAttack = attackNow && !triggerDown;
        if (menuNavigation && effectiveAttack && !attackWasDown) key(display, XK_Return, true), key(display, XK_Return, false);
        attack = effectiveAttack;
        if (!attack) corpseCastRefreshAt = 0;
        else if (corpseTarget && (!attackWasDown || now >= corpseCastRefreshAt)) {
            // Cast toward a corpse without replacing the normal enemy aim/highlight.
            sendUnit(modSocket, modState, *corpseTarget, 3, layer2 ? 1 : 0);
            corpseCastRefreshAt = now + 400;
        }
        const bool fallbackClickMove = fallbackMove;
        const bool directTarget = !triggerDown && !menuNavigation && !pointerMode && stateFresh && modState.commandPort > 0 && selectedTarget;
        const uint64_t selectedKey = directTarget ? (uint64_t(selectedTarget->type) << 32) | selectedTarget->id : 0;
        if (directAttackKey && (!attack || !directTarget || selectedKey != directAttackKey)) {
            releaseDirectAttack();
        }
        const bool targetAction = attack && !layer2 && !corpseSkill && directTarget && selectedTarget->type != 4;
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
        if (attack && !attackWasDown && !layer2 && !corpseSkill && directTarget && selectedTarget->type == 4) {
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
            if (down && !potionWasDown[i] && stateFresh && modState.commandPort > 0 && now >= potionReadyAt) {
                const uint8_t potion[2] = {static_cast<uint8_t>(i), 0};
                sendModCommand(modSocket, modState.commandPort, 1, potion, sizeof(potion));
                potionReadyAt = now + 350;
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
                    if (quickCast_.load())
                        castBoundSkill(display, modSocket, modState, i + 4, selectedTarget, rx, ry, cursorX, cursorY);
                    else
                        selectBoundSkill(modSocket, modState, i + 4);
                } else if (i == 3 && layer2) {
                    const uint8_t portal = 0;
                    sendModCommand(modSocket, modState.commandPort, 7, &portal, 1);
                } else {
                    const int slot = layer2 ? i + 4 : i;
                    const KeySym sym = layer2 ? secondRow[i] : firstRow[i];
                    if (stateFresh && modState.commandPort > 0 && slot < 16 &&
                        modState.hotkeySkills[slot] != 0xffff) {
                        if (quickCast_.load())
                            castBoundSkill(display, modSocket, modState, slot, selectedTarget, rx, ry, cursorX, cursorY);
                        else
                            selectBoundSkill(modSocket, modState, slot);
                    } else if (sym != NoSymbol) {
                        key(display, sym, true);
                        key(display, sym, false);
                    }
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
            const bool mappedControls = stateFresh && (modState.skillTreeOpen()
                ? (modState.skillFocusReady && !modState.skillFocusPoints.empty())
                : !inventoryFocusPoints(modState).empty());
            const bool arrowMode = menuNavigation && !modState.escMenu() && !mappedControls;
            const bool modeChanged = down && dpadWasDown[i] && arrowMode != dpadArrowWasDown[i];
            if (modeChanged && dpadArrowWasDown[i]) key(display, arrows[i], false);
            if (down && (!dpadWasDown[i] || modeChanged)) {
                if (arrowMode) key(display, arrows[i], true);
                else if (pointerMode) {
                    const float displayW = static_cast<float>(DisplayWidth(display, DefaultScreen(display)));
                    const float displayH = static_cast<float>(DisplayHeight(display, DefaultScreen(display)));
                    if (stateFresh && modState.skillTreeOpen() && modState.skillFocusReady &&
                               !modState.skillFocusPoints.empty()) {
                        const float sx = displayW / std::max(1, modState.gameWidth);
                        const float sy = displayH / std::max(1, modState.gameHeight);
                        const auto focus = skillTreeFocus(modState, cursorX / sx, cursorY / sy, i);
                        cursorX = focus.x * sx;
                        cursorY = focus.y * sy;
                    } else if (stateFresh && !inventoryFocusPoints(modState).empty()) {
                        const float sx = displayW / std::max(1, modState.gameWidth);
                        const float sy = displayH / std::max(1, modState.gameHeight);
                        const auto focus = inventoryFocus(modState, cursorX / sx, cursorY / sy, i);
                        cursorX = focus.x * sx;
                        cursorY = focus.y * sy;
                    } else if (!modState.uiFocusPoints.empty() &&
                               (!stateFresh || !modState.inventoryGridOpen())) {
                        const float sx = displayW / std::max(1, modState.gameWidth);
                        const float sy = displayH / std::max(1, modState.gameHeight);
                        const auto focus = uiControlFocus(modState, cursorX / sx, cursorY / sy, i);
                        cursorX = focus.x * sx;
                        cursorY = focus.y * sy;
                    } else {
                        // Unmapped panels use their native navigation until their
                        // interactive controls have an explicit focus map.
                        key(display, arrows[i], true);
                        key(display, arrows[i], false);
                    }
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
