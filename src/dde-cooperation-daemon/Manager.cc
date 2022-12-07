#include "Manager.h"

#include <stdexcept>
#include <fstream>

#include <sys/stat.h>

#include <uuid/uuid.h>
#include <fmt/core.h>
#include <spdlog/spdlog.h>

#include <QUdpSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QDebug>

#include "Machine/Machine.h"
#include "Machine/PCMachine.h"
#include "Machine/AndroidMachine.h"
#include "X11/Display.h"
#include "X11/Clipboard.h"
#include "utils/message_helper.h"
#include "utils/net.h"
#include "protocol/message.pb.h"

namespace fs = std::filesystem;

const static fs::path inputDevicePath = "/dev/input";
const static QString dConfigAppID = "org.deepin.cooperation";
const static QString dConfigName = "org.deepin.cooperation";

Manager::Manager(const std::filesystem::path &dataDir)
    : m_bus(QDBusConnection::sessionBus())
    , m_dbusAdaptor(new ManagerDBusAdaptor(this, m_bus))
    , m_dataDir(dataDir)
    , m_mountRoot(m_dataDir / "mr")
    , m_lastMachineIndex(0)
    , m_deviceSharingSwitch(true)
    , m_lastRequestId(0)
    , m_socketScan(new QUdpSocket(this))
    , m_listenPair(new QTcpServer(this))
    , m_powersaverProxy("org.freedesktop.ScreenSaver",
                        "/org/freedesktop/ScreenSaver",
                        "org.freedesktop.ScreenSaver")
    , m_keypair(m_dataDir, KeyPair::KeyType::ED25519)
    , m_dConfig(DConfig::create(dConfigAppID, dConfigName)) {
    ensureDataDirExists();
    initUUID();
    initFileStoragePath();
    initSharedClipboardStatus();
    initSharedDevicesStatus();
    initCooperatedMachines();

    m_keypair.load();

    connect(m_socketScan, &QUdpSocket::readyRead, this, &Manager::handleReceivedSocketScan);
    m_socketScan->bind(QHostAddress::Any, m_scanPort);

    connect(m_listenPair, &QTcpServer::newConnection, this, &Manager::handleNewConnection);
    m_listenPair->listen(QHostAddress::Any);
    m_port = m_listenPair->serverPort();
    spdlog::debug("TCP listening on port: {}", m_port);

    m_displayServer = std::make_unique<X11::Display>(this, this);
    m_clipboard = std::make_unique<X11::Clipboard>(this, this);

    // TODO: inotify
    for (const auto &entry : fs::directory_iterator(inputDevicePath)) {
        if (!entry.is_character_file()) {
            continue;
        }
        if (entry.path().filename().string().rfind("event", 0) != 0) {
            continue;
        }

        m_inputGrabbers.emplace(
            std::make_pair(entry.path().string(),
                           std::make_shared<InputGrabberWrapper>(this, entry.path())));
    }

    scan();
}

Manager::~Manager() {
    m_socketScan->close();
    m_listenPair->close();
}

void Manager::ensureDataDirExists() {
    if (fs::exists(m_dataDir)) {
        if (fs::is_directory(m_dataDir)) {
            return;
        }

        throw std::runtime_error(fmt::format("{} is not a directory", m_dataDir.string()));
    }

    auto oldMask = umask(077);
    if (!fs::create_directory(m_dataDir)) {
        throw std::runtime_error(fmt::format("failed to create directory {}", m_dataDir.string()));
    }
    umask(oldMask);
}

void Manager::initUUID() {
    if (!m_dConfig || !m_dConfig->isValid() || !m_dConfig->keyList().contains("machineId")) {
        spdlog::warn("dConfig is invalid or does not has machineId key!");
        m_uuid = newUUID();
        return;
    }

    QString existedUuid = m_dConfig->value("machineId").toString();
    if (!existedUuid.isEmpty()) {
        m_uuid = existedUuid.toStdString();
        return;
    }

    m_uuid = newUUID();
    m_dConfig->setValue("machineId", QString::fromStdString(m_uuid));
}

std::string Manager::newUUID() const {
    uuid_t uuid;
    uuid_generate(uuid);
    char uuidStr[100];
    uuid_unparse(uuid, uuidStr);

    return uuidStr;
}

bool Manager::isValidUUID(const std::string &str) const noexcept {
    uuid_t uuid;
    int res = uuid_parse_range(str.data(), str.data() + str.size(), uuid);
    return res == 0;
}

QString Manager::addrToString(const QHostAddress &addr) const {
    bool conversionOK = false;
    QHostAddress ip4Addr(addr.toIPv4Address(&conversionOK));
    if (conversionOK) {
        return ip4Addr.toString();
    }

    return addr.toString();
}

void Manager::initFileStoragePath() {
    if (!m_dConfig || !m_dConfig->isValid() || !m_dConfig->keyList().contains("filesStoragePath")) {
        spdlog::warn("dConfig is invalid or does not has filesStoragePath key!");
        m_fileStoragePath = getenv("HOME"); // default
        return;
    }

    QString existedPath = m_dConfig->value("filesStoragePath").toString();
    if (!existedPath.isEmpty()) {
        m_fileStoragePath = existedPath;
        return;
    }

    m_fileStoragePath = getenv("HOME");
}

void Manager::initSharedClipboardStatus() {
    if (!m_dConfig || !m_dConfig->isValid() || !m_dConfig->keyList().contains("shareClipboard")) {
        spdlog::warn("dConfig is invalid or does not has shareClipboard key!");
        m_sharedClipboard = false;
        return;
    }

    m_sharedClipboard = m_dConfig->value("shareClipboard").toBool();
}

void Manager::initSharedDevicesStatus() {
    if (!m_dConfig || !m_dConfig->isValid() || !m_dConfig->keyList().contains("shareDevices")) {
        spdlog::warn("dConfig is invalid or does not has shareDevices key!");
        m_sharedDevices = false;
        return;
    }

    m_sharedDevices = m_dConfig->value("shareDevices").toBool();
}

void Manager::initCooperatedMachines() {
    if (!m_dConfig || !m_dConfig->isValid() ||
        !m_dConfig->keyList().contains("cooperatedMachineIds")) {
        spdlog::warn("dConfig is invalid or does not has cooperatedMachineIds key!");
        return;
    }

    m_cooperatedMachines = m_dConfig->value("cooperatedMachineIds").toStringList();
}

bool Manager::sendFile(const QStringList &files, int osType) noexcept {
    int dstOs = osType;
    bool dstIsPcMachine = dstOs == DEVICE_OS_UOS || dstOs == DEVICE_OS_LINUX ||
                          dstOs == DEVICE_OS_WINDOWS || dstOs == DEVICE_OS_MACOS;
    bool dstIsAndroid = dstOs == DEVICE_OS_ANDROID;

    bool hasSend = false;
    for (const auto &v : m_machines) {
        const std::shared_ptr<Machine> &machine = v.second;
        if (!machine->m_connected) {
            continue;
        }

        if ((dstIsPcMachine && machine->isPcMachine()) || (dstIsAndroid && machine->isAndroid())) {
            machine->sendFiles(files);
            hasSend = true;
            break;
        }
    }

    return hasSend;
}

void Manager::setFileStoragePath(const QString &path) noexcept {
    if (path == m_fileStoragePath) {
        return;
    }

    m_fileStoragePath = path;
    m_dbusAdaptor->updateFileStoragePath(path);

    if (!m_dConfig || !m_dConfig->isValid() || !m_dConfig->keyList().contains("filesStoragePath")) {
        spdlog::warn("dConfig is invalid or does not has filesStoragePath key!");
        return;
    }

    m_dConfig->setValue("filesStoragePath", path);
}

void Manager::openSharedClipboard(bool on) noexcept {
    if (on == m_sharedClipboard) {
        return;
    }

    m_sharedClipboard = on;
    serviceStatusChanged();
    m_dbusAdaptor->updateSharedClipboard(on);

    if (!m_dConfig || !m_dConfig->isValid() || !m_dConfig->keyList().contains("shareClipboard")) {
        spdlog::warn("dConfig is invalid or does not has shareClipboard key!");
        return;
    }

    m_dConfig->setValue("shareClipboard", m_sharedClipboard);
}

void Manager::openSharedDevices(bool on) noexcept {
    if (on == m_sharedDevices) {
        return;
    }

    m_sharedDevices = on;
    serviceStatusChanged();
    m_dbusAdaptor->updateSharedDevices(on);

    if (!m_dConfig || !m_dConfig->isValid() || !m_dConfig->keyList().contains("shareDevices")) {
        spdlog::warn("dConfig is invalid or does not has shareDevices key!");
        return;
    }

    m_dConfig->setValue("shareDevices", m_sharedDevices);
}

QVector<QDBusObjectPath> Manager::getMachinePaths() const noexcept {
    QVector<QDBusObjectPath> machines;
    machines.reserve(m_machines.size());

    for (auto &[_, machine] : m_machines) {
        machines.append(QDBusObjectPath(machine->path()));
    }

    return machines;
}

bool Manager::setDeviceSharingSwitch(bool value) noexcept {
    m_deviceSharingSwitch = value;
    cooperationStatusChanged(m_deviceSharingSwitch);
    m_dbusAdaptor->updateDeviceSharingSwitch(value);
    return true;
}

bool Manager::hasPcMachinePaired() const {
    for (const auto &v : m_machines) {
        const std::shared_ptr<Machine> &machine = v.second;
        if (machine->m_connected && machine->isPcMachine()) {
            return true;
        }
    }

    return false;
}

bool Manager::hasAndroidPaired() const {
    for (const auto &v : m_machines) {
        const std::shared_ptr<Machine> &machine = v.second;
        if (machine->m_connected && machine->isAndroid()) {
            return true;
        }
    }

    return false;
}

void Manager::machineCooperated(const std::string &machineId) {
    QString machineID = QString::fromStdString(machineId);
    if (m_cooperatedMachines.contains(machineID)) {
        return;
    }

    if (m_cooperatedMachines.count() == 5) {
        m_cooperatedMachines.removeFirst();
    }
    m_cooperatedMachines.append(machineID);

    m_dbusAdaptor->updateCooperatedMachines(m_cooperatedMachines);

    m_dConfig->setValue("cooperatedMachineIds", m_cooperatedMachines);
}

void Manager::scan() noexcept {
    Message base;
    ScanRequest *request = base.mutable_scanrequest();
    request->set_key(SCAN_KEY);
    request->mutable_deviceinfo()->set_uuid(m_uuid);
    request->mutable_deviceinfo()->set_name(Net::getHostname());
    request->mutable_deviceinfo()->set_os(DEVICE_OS_LINUX);
    request->set_port(m_port);

    m_socketScan->writeDatagram(MessageHelper::genMessageQ(base),
                                QHostAddress::Broadcast,
                                m_scanPort);
}

void Manager::removeInputGrabber(const std::filesystem::path &path) {
    m_inputGrabbers.erase(path.string());
}

bool Manager::tryFlowOut(uint16_t direction, uint16_t x, uint16_t y, bool evFromPeer) {
    for (const auto &v : m_machines) {
        const std::shared_ptr<Machine> &machine = v.second;
        if (machine->m_deviceSharing && machine->m_direction == direction) {
            if (evFromPeer) {
                machine->flowTo(direction, x, y);
            } else if (isSharedDevices()) {
                machine->flowTo(direction, x, y);
                onFlowOut(machine);
            }
            return true;
        }
    }

    return false;
}

void Manager::cooperationStatusChanged(bool enable) {
    if (enable) {
        scan();
    } else {
        for (const auto &v : m_machines) {
            const std::shared_ptr<Machine> &machine = v.second;
            if (machine->m_connected) {
                machine->m_conn->close();
                sendServiceStoppedNotification();
            }
        }
        m_machines.clear();
        m_dbusAdaptor->updateMachines(getMachinePaths());
    }
}

void Manager::updateMachine(const std::string &ip, uint16_t port, const DeviceInfo &devInfo) {
    auto iter = m_machines.find(devInfo.uuid());
    if (iter == m_machines.end()) {
        addMachine(ip, port, devInfo);
    } else {
        iter->second->receivedPing();

        // existed need update machine info in case of the request pc restarted
        iter->second->updateMachineInfo(ip, port, devInfo);
    }
}

void Manager::addMachine(const std::string &ip, uint16_t port, const DeviceInfo &devInfo) {
    auto shortUUID = devInfo.uuid();
    shortUUID.resize(8);
    fs::path dataPath = m_dataDir / shortUUID;

    if (devInfo.os() == DEVICE_OS_ANDROID) {
        auto m = std::make_shared<AndroidMachine>(this,
                                                  m_clipboard.get(),
                                                  m_bus,
                                                  m_lastMachineIndex,
                                                  dataPath,
                                                  ip,
                                                  port,
                                                  devInfo);
        m_machines.insert(std::pair(devInfo.uuid(), m));
    } else {
        auto m = std::make_shared<PCMachine>(this,
                                             m_clipboard.get(),
                                             m_bus,
                                             m_lastMachineIndex,
                                             dataPath,
                                             ip,
                                             port,
                                             devInfo);
        m_machines.insert(std::pair(devInfo.uuid(), m));
    }
    m_lastMachineIndex++;
    m_dbusAdaptor->updateMachines(getMachinePaths());
}

void Manager::handleSocketError(const std::string &title, const std::string &msg) {
    spdlog::error("failed to send message: {} {}", title, msg);
}

void Manager::handleReceivedSocketScan() noexcept {

    if (!m_deviceSharingSwitch) {
        return;
    }

    while (m_socketScan->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(m_socketScan->pendingDatagramSize());

        QHostAddress addr;
        m_socketScan->readDatagram(datagram.data(), datagram.size(), &addr);

        const char *buff = datagram.data();
        size_t size = datagram.size();

        auto &header = MessageHelper::parseMessageHeader(buff);
        if (!header.legal()) {
            qWarning() << "illegal message from " << addr.toString();
            return;
        }

        buff += header_size;
        size -= header_size;

        auto base = MessageHelper::parseMessageBody<Message>(buff, size);
        spdlog::info("received packet, type: {}", base.payload_case());

        switch (base.payload_case()) {
        case Message::PayloadCase::kScanRequest: {
            const auto &request = base.scanrequest();
            if (request.key() != SCAN_KEY) {
                spdlog::error("key mismatch: {}", request.key());
                return;
            }

            // 自己扫描到自己，忽略
            auto uuid = request.deviceinfo().uuid();
            if (uuid == m_uuid) {
                return;
            }

            if (!isValidUUID(uuid)) {
                return;
            }

            updateMachine(addrToString(addr).toStdString(), request.port(), request.deviceinfo());

            Message msg;
            ScanResponse *response = msg.mutable_scanresponse();
            response->set_key(SCAN_KEY);
            response->mutable_deviceinfo()->set_uuid(m_uuid);
            response->mutable_deviceinfo()->set_name(Net::getHostname());
            response->mutable_deviceinfo()->set_os(DEVICE_OS_LINUX);
            response->set_port(m_port);
            m_socketScan->writeDatagram(MessageHelper::genMessageQ(msg), addr, m_scanPort);

            break;
        }
        case Message::PayloadCase::kScanResponse: {
            const auto &resp = base.scanresponse();
            if (resp.key() != SCAN_KEY) {
                spdlog::error("key mismatch: {}", SCAN_KEY);
                return;
            }

            auto uuid = resp.deviceinfo().uuid();
            if (!isValidUUID(uuid)) {
                return;
            }

            updateMachine(addrToString(addr).toStdString(), resp.port(), resp.deviceinfo());

            spdlog::info("{} responded", resp.deviceinfo().name());
            break;
        }
        case Message::PayloadCase::kServiceStoppedNotification: {
            const auto &notification = base.servicestoppednotification();
            const auto &uuid = notification.deviceuuid();

            auto iter = m_machines.find(uuid);
            if (iter != m_machines.end()) {
                if (iter->second->m_connected) {
                    iter->second->m_conn->close();
                }

                m_machines.erase(uuid);
                m_dbusAdaptor->updateMachines(getMachinePaths());
            }
            break;
        }
        default: {
            spdlog::error("unknown data type");
            break;
        }
        }
    }
}

void Manager::handleNewConnection() noexcept {
    while (m_listenPair->hasPendingConnections()) {
        spdlog::debug("new connection received");

        QTcpSocket *socket = m_listenPair->nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, [this, socket] {
            QByteArray buffer = socket->peek(header_size);
            auto header = MessageHelper::parseMessageHeader(buffer);
            if (!header.legal()) {
                qWarning() << "illegal message from " << socket->peerAddress().toString();
                return;
            }

            if (socket->size() < static_cast<qint64>(header_size + header.size())) {
                qDebug() << "partial packet:" << socket->size();
                return;
            }

            socket->read(header_size);
            auto size = header.size();
            buffer = socket->read(size);

            Message msg = MessageHelper::parseMessageBody<Message>(buffer.data(), buffer.size());

            const auto &request = msg.pairrequest();
            if (request.key() != SCAN_KEY) {
                spdlog::error("key mismatch {}", SCAN_KEY);
                socket->close();
                return;
            }

            auto i = m_machines.find(request.deviceinfo().uuid());
            if (i == m_machines.end()) {
                // TODO: return failed
                spdlog::error("cannot found device with uuid {}", request.deviceinfo().uuid());
                socket->close();
                return;
            }

            auto machine = i->second;
            if ((machine->isPcMachine() && hasPcMachinePaired()) ||
                (machine->isAndroid() && hasAndroidPaired())) {
                // TODO tips
                spdlog::error("cannot pair this device, this machine is paired with other machine");
                socket->close();
                return;
            }

            auto remote = socket->peerAddress();
            qInfo() << fmt::format("connected by {}@{}",
                                   request.deviceinfo().name(),
                                   remote.toString().toStdString())
                           .data();

            socket->disconnect();
            machine->onPair(socket);
        });
    }
}

void Manager::handleNewConnectionFirstPacket() noexcept {
}

void Manager::ping(const std::string &ip, uint16_t port) {
    Message msg;
    ScanRequest *request = msg.mutable_scanrequest();
    request->set_key(SCAN_KEY);
    request->mutable_deviceinfo()->set_uuid(m_uuid);
    request->mutable_deviceinfo()->set_name(Net::getHostname());
    request->mutable_deviceinfo()->set_os(DEVICE_OS_LINUX);
    request->set_port(m_port);

    m_socketScan->writeDatagram(MessageHelper::genMessageQ(msg),
                                QHostAddress(QString::fromStdString(ip)),
                                port);
}

void Manager::onMachineOffline(const std::string &uuid) {
    m_machines.erase(m_machines.find(uuid));

    m_dbusAdaptor->updateMachines(getMachinePaths());
}

void Manager::onStartDeviceSharing(const std::weak_ptr<Machine> &machine, bool proactively) {
    if (proactively) {
        m_displayServer->startEdgeDetection();
    } else {
        m_displayServer->hideMouse(true);
        onFlowOut(machine);
    }

    m_deviceSharingCnt++;
    inhibitScreensaver();
}

void Manager::onStopDeviceSharing() {
    for (auto &inputGrabber : m_inputGrabbers) {
        inputGrabber.second->stop();
    }

    m_displayServer->stopEdgeDetection();
    m_displayServer->hideMouse(false);

    m_deviceSharingCnt--;
    unInhibitScreensaver();
}

void Manager::onFlowBack(uint16_t direction, uint16_t x, uint16_t y) {
    for (auto &inputGrabber : m_inputGrabbers) {
        inputGrabber.second->stop();
    }

    m_displayServer->flowBack(direction, x, y);
}

void Manager::onFlowOut(const std::weak_ptr<Machine> &machine) {
    for (auto &inputGrabbers : m_inputGrabbers) {
        inputGrabbers.second->setMachine(machine);
        inputGrabbers.second->start();
    }
}

void Manager::onClipboardTargetsChanged(const std::vector<std::string> &targets) {
    for (auto &[uuid, machine] : m_machines) {
        if (!machine->m_connected) {
            continue;
        }

        machine->onClipboardTargetsChanged(targets);
    }
}

bool Manager::onReadClipboardContent(const std::string &target) {
    auto machine = m_clipboardOwner.lock();
    if (machine) {
        machine->readTarget(target);
        return true;
    }

    return false;
}

void Manager::onMachineOwnClipboard(const std::weak_ptr<Machine> &machine,
                                    const std::vector<std::string> &targets) {
    m_clipboardOwner = machine;
    m_clipboard->newClipboardOwnerTargets(targets);
}

static const QString applicationName = "DDE Cooperation";
static const QString inhibitReason = "cooperating";

void Manager::inhibitScreensaver() {
    if (m_deviceSharingCnt == 0) {
        return;
    }

    if (m_inhibitCookie) {
        return;
    }

    QDBusReply<uint32_t> reply = m_powersaverProxy.call("Inhibit", applicationName, inhibitReason);
    if (!reply.isValid()) {
        qWarning() << "Inhibit:" << reply.error();
        return;
    }
    m_inhibitCookie = reply.value();
}

void Manager::unInhibitScreensaver() {
    if (m_deviceSharingCnt != 0) {
        return;
    }

    if (!m_inhibitCookie) {
        return;
    }

    QDBusReply<void> reply = m_powersaverProxy.call("UnInhibit", m_inhibitCookie);
    if (!reply.isValid()) {
        qWarning() << "UnInhibit:" << reply.error();
    }
}

void Manager::sendServiceStoppedNotification() const {
    for (const auto &v : m_machines) {
        const std::shared_ptr<Machine> &machine = v.second;

        Message base;
        ServiceStoppedNotification *notification = base.mutable_servicestoppednotification();
        notification->set_deviceuuid(machine->m_uuid);

        m_socketScan->writeDatagram(MessageHelper::genMessageQ(base),
                                    QHostAddress(QString::fromStdString(machine->ip())),
                                    m_scanPort);
    }
}

void Manager::serviceStatusChanged() {
    for (const auto &v : m_machines) {
        const std::shared_ptr<Machine> &machine = v.second;
        if (machine->m_connected) {
            machine->sendServiceStatusNotification();
        }
    }
}