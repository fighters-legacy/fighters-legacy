// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClientNetEventHandler.h"
#include "ServerNotice.h"

#include "ILogger.h"
#include "INetwork.h"
#include "console/GameConsole.h"
#include "entity/EntityDef.h"
#include "entity/EntityTypeRegistry.h"
#include "net/GameProtocol.h"
#include "net/WireCodec.h"
#include "render/RenderSnapshot.h"
#include "render/SimRenderBridge.h"
#include "weather/WeatherController.h"

#include <cstring>
#include <glm/gtc/quaternion.hpp>
#include <sstream>

void ClientNetEventHandler::onConnect(uint32_t /*peerId*/) {
    m_connected = true;
    logger.log(LogLevel::Info, __FILE__, __LINE__, "connected to local fl-server");
}

void ClientNetEventHandler::signalFailure(SessionFailure f) {
    if (!sessionFailure)
        return;
    SessionFailure expected = SessionFailure::None;
    sessionFailure->compare_exchange_strong(expected, f, std::memory_order_release, std::memory_order_relaxed);
}

void ClientNetEventHandler::onDisconnect(uint32_t /*peerId*/) {
    logger.log(LogLevel::Info, __FILE__, __LINE__, "disconnected from local fl-server");
    // ENet-level rejection before MsgConnectAck — generic fallback (a specific reason set earlier by
    // the MsgHello/MsgConnectRefusal handlers wins via signalFailure's first-writer-wins CAS).
    if (m_connected && assignedEntityIdx == 0)
        signalFailure(SessionFailure::ConnectionRefused);
}

void ClientNetEventHandler::onReceive(uint32_t /*peerId*/, const void* data, std::size_t size) {
    if (size < 1)
        return;
    const uint8_t msgId = *static_cast<const uint8_t*>(data);

    if (msgId == static_cast<uint8_t>(fl::MsgId::Hello)) {
        fl::MsgHello hello;
        if (!fl::readMsg(data, size, hello))
            return;
        if (hello.protocolVersion != fl::kProtocolVersion) {
            logger.log(LogLevel::Error, __FILE__, __LINE__, "server protocol version mismatch — disconnecting");
            signalFailure(SessionFailure::VersionMismatch);
            net.disconnect();
        }
        return;
    }

    if (msgId == static_cast<uint8_t>(fl::MsgId::ConnectAck)) {
        fl::MsgConnectAck ack;
        if (!fl::readMsg(data, size, ack))
            return;
        assignedEntityIdx = ack.assignedEntityIdx;
        assignedEntityGen = ack.assignedEntityGen;
        m_planetRadiusKm = ack.planetRadiusKm;
        std::size_t off = sizeof(ack);
        for (uint16_t i = 0; i < ack.typeCount; ++i) {
            fl::MsgEntityTypeDef td;
            if (!fl::readRecordAt(data, size, off, td))
                break;
            off += sizeof(td);
            if (registry.findById(td.id))
                continue; // already registered
            fl::EntityDef def;
            def.id = td.id;
            def.mesh = td.mesh;
            def.classicDamageMesh = td.dmgMesh;
            def.maxHp = 100.0f;
            registry.registerType(std::move(def));
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::WorldSnapshot)) {
        fl::MsgWorldSnapshotHeader hdr;
        if (!fl::readMsg(data, size, hdr))
            return;

        fl::RenderSnapshot snap;
        snap.tickIndex = hdr.tickIndex;
        snap.entries.reserve(hdr.entityCount);

        std::size_t off = sizeof(hdr);
        for (uint16_t i = 0; i < hdr.entityCount; ++i) {
            fl::MsgEntityEntry e;
            if (!fl::readRecordAt(data, size, off, e))
                break;
            off += sizeof(e);

            fl::EntityRenderEntry re;
            re.entityIdx = e.entityIdx;
            re.entityGen = e.entityGen;
            re.typeIndex = e.typeIndex;
            re.position = {e.pos[0], e.pos[1], e.pos[2]};
            re.velocity = {e.vel[0], e.vel[1], e.vel[2]};
            // Wire format: x,y,z,w — glm::quat constructor: (w,x,y,z)
            re.orientation = glm::quat(e.ori[3], e.ori[0], e.ori[1], e.ori[2]);
            re.damageLevel = e.damageLevel;
            re.playerOwned = (e.flags & 1u) != 0;
            re.throttle = e.throttle;
            re.fuelPct = e.fuelPct;
            re.abEngaged = e.abEngaged != 0;
            re.engineFailFlags = e.engineFailFlags;
            snap.entries.push_back(re);
        }
        char traceBuf[80];
        std::snprintf(traceBuf, sizeof(traceBuf), "WorldSnapshot: hdr.entityCount=%u, built=%zu", hdr.entityCount,
                      snap.entries.size());
        logger.log(LogLevel::Trace, __FILE__, __LINE__, traceBuf);
        bridge.publishExternal(std::move(snap));
        tickAlpha.markNewTick();
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::WeatherState)) {
        fl::MsgWeatherState ws;
        if (!fl::readMsg(data, size, ws))
            return;
        float tod = static_cast<float>(ws.timeOfDayTenths) / 10.f;
        env.fogDensity = ws.fogDensity;
        env.fogStartDist = ws.fogStartDist;
        env.timeOfDay = tod;
        fl::WeatherController::applyPresetToEnv(static_cast<fl::WeatherPreset>(ws.preset), tod, env);
        env.windX = ws.windX;
        env.windZ = ws.windZ;
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::ServerNotice)) {
        fl::MsgServerNotice sn;
        if (!fl::readMsg(data, size, sn))
            return;
        sn.text[59] = '\0';
        char noticeBuf[72];
        std::snprintf(noticeBuf, sizeof(noticeBuf), "[server] %s", sn.text);
        if (console)
            console->print(std::string(noticeBuf));
        if (notice)
            notice->setNotice(noticeBuf, sn.secondsRemaining);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::AdminResponse)) {
        fl::MsgAdminResponse resp;
        if (!fl::readMsg(data, size, resp))
            return;
        resp.text[sizeof(resp.text) - 1] = '\0';
        if (console && resp.text[0] != '\0')
            console->print(std::string("[admin] ") + resp.text);
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::Motd)) {
        fl::MsgMotdHeader mh;
        if (!fl::readMsg(data, size, mh))
            return;
        const uint32_t effectiveSecs =
            mh.displaySeconds > 0 ? static_cast<uint32_t>(mh.displaySeconds) : motdDisplaySeconds;
        const std::size_t textLen = std::min(size - sizeof(mh), fl::kMaxMotdBytes);
        std::string text(static_cast<const char*>(data) + sizeof(mh), textLen);
        while (!text.empty() && text.back() == '\0')
            text.pop_back();
        std::istringstream stream(text);
        std::string line;
        bool first = true;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            std::string prefixed = std::string("[server] ") + line;
            if (console)
                console->print(prefixed);
            if (notice && first)
                notice->setNotice(prefixed, 0, effectiveSecs);
            first = false;
        }
    } else if (msgId == static_cast<uint8_t>(fl::MsgId::ConnectRefusal)) {
        fl::MsgConnectRefusal ref{};
        if (!fl::readMsg(data, size, ref))
            return;
        SessionFailure f = SessionFailure::ConnectionRefused;
        switch (static_cast<fl::ConnectRefusalCode>(ref.code)) {
        case fl::ConnectRefusalCode::Banned:
            f = SessionFailure::Banned;
            break;
        case fl::ConnectRefusalCode::AccessDenied:
        case fl::ConnectRefusalCode::AdminLockout:
            f = SessionFailure::AccessDenied;
            break;
        case fl::ConnectRefusalCode::RateLimited:
            f = SessionFailure::RateLimited;
            break;
        case fl::ConnectRefusalCode::TooManyConnections:
            f = SessionFailure::TooManyConnections;
            break;
        case fl::ConnectRefusalCode::Generic:
            break; // ConnectionRefused
        }
        signalFailure(f);
    }
    // Unknown msgIds: silently discard
}
