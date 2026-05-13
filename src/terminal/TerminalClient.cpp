#include "TerminalClient.hpp"

#include <sys/stat.h>

#include "LocalClipboardImage.hpp"
#include "TelemetryService.hpp"
#include "TunnelUtils.hpp"

namespace et {

#ifdef __APPLE__
namespace {

bool isImageExtension(const string& ext) {
  return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" ||
         ext == "tiff" || ext == "bmp" || ext == "webp";
}

optional<ClipboardImagePayload> readLocalFileAsImage(const string& path) {
  struct stat st;
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return nullopt;
  }
  if (static_cast<uint64_t>(st.st_size) > kMaxClipboardImageBytes ||
      st.st_size <= 0) {
    return nullopt;
  }

  size_t dot = path.rfind('.');
  if (dot == string::npos || dot + 1 >= path.size()) {
    return nullopt;
  }
  string ext = path.substr(dot + 1);
  for (auto& c : ext) c = tolower(c);
  if (!isImageExtension(ext)) {
    return nullopt;
  }

  ifstream input(path, ios::binary);
  if (!input) {
    return nullopt;
  }
  string bytes((istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
  if (bytes.empty()) {
    return nullopt;
  }
  if (ext == "jpeg") ext = "jpg";
  return ClipboardImagePayload{ext, bytes};
}

string extractBracketedPasteContent(const string& input, size_t startPos,
                                    size_t* endPos) {
  static const string kPasteEnd = "\x1b[201~";
  size_t end = input.find(kPasteEnd, startPos);
  if (end == string::npos) {
    *endPos = input.size();
    return input.substr(startPos);
  }
  *endPos = end + kPasteEnd.size();
  return input.substr(startPos, end - startPos);
}

string trimWhitespace(const string& s) {
  size_t start = s.find_first_not_of(" \t\n\r");
  if (start == string::npos) return "";
  size_t end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

}  // namespace
#endif

TerminalClient::TerminalClient(
    shared_ptr<SocketHandler> _socketHandler,
    shared_ptr<SocketHandler> _pipeSocketHandler,
    const SocketEndpoint& _socketEndpoint, const string& id,
    const string& passkey, shared_ptr<Console> _console, bool jumphost,
    const string& tunnels, const string& reverseTunnels, bool forwardSshAgent,
    const string& identityAgent, int _keepaliveDuration,
    const vector<pair<string, string>>& envVars,
    bool _clipboardImagePasteSupported)
    : console(_console),
      shuttingDown(false),
      keepaliveDuration(_keepaliveDuration),
      clipboardImagePasteSupported(_clipboardImagePasteSupported) {
  portForwardHandler = shared_ptr<PortForwardHandler>(
      new PortForwardHandler(_socketHandler, _pipeSocketHandler));
  InitialPayload payload;
  payload.set_jumphost(jumphost);

  for (const auto& envVar : envVars) {
    (*payload.mutable_environmentvariables())[envVar.first] = envVar.second;
  }

  try {
    if (tunnels.length()) {
      auto pfsrs = parseRangesToRequests(tunnels);
      for (auto& pfsr : pfsrs) {
        auto pfsresponse =
            portForwardHandler->createSource(pfsr, nullptr, -1, -1);
        if (pfsresponse.has_error()) {
          LOG(WARNING) << "Failed to establish port forward "
                       << pfsr.source().port() << ":"
                       << pfsr.destination().port() << " - "
                       << pfsresponse.error();
          continue;
        }
      }
    }
    if (reverseTunnels.length()) {
      auto pfsrs = parseRangesToRequests(reverseTunnels);
      for (auto& pfsr : pfsrs) {
        *(payload.add_reversetunnels()) = pfsr;
      }
    }
    if (forwardSshAgent) {
      PortForwardSourceRequest pfsr;
      string authSock = "";
      if (identityAgent.length()) {
        authSock.assign(identityAgent);
      } else {
        auto authSockEnv = getenv("SSH_AUTH_SOCK");
        if (!authSockEnv) {
          CLOG(INFO, "stdout")
              << "Missing environment variable SSH_AUTH_SOCK.  Are you sure "
                 "you "
                 "ran ssh-agent first?"
              << endl;
          exit(1);
        }
        authSock.assign(authSockEnv);
      }
      if (authSock.length()) {
        pfsr.mutable_destination()->set_name(authSock);
        pfsr.set_environmentvariable("SSH_AUTH_SOCK");
        *(payload.add_reversetunnels()) = pfsr;
      }
    }
  } catch (const std::runtime_error& ex) {
    CLOG(INFO, "stdout") << "Error establishing port forward: " << ex.what()
                         << endl;
    exit(1);
  }

  connection = shared_ptr<ClientConnection>(
      new ClientConnection(_socketHandler, _socketEndpoint, id, passkey));

  int connectFailCount = 0;
  while (true) {
    try {
      bool fail = true;
      if (connection->connect()) {
        connection->writePacket(
            Packet(EtPacketType::INITIAL_PAYLOAD, protoToString(payload)));
        fd_set rfd;
        timeval tv;
        for (int a = 0; a < 3; a++) {
          FD_ZERO(&rfd);
          int clientFd = connection->getSocketFd();
          if (clientFd < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
          }
          FD_SET(clientFd, &rfd);
          tv.tv_sec = 1;
          tv.tv_usec = 0;
          select(clientFd + 1, &rfd, NULL, NULL, &tv);
          if (FD_ISSET(clientFd, &rfd)) {
            Packet initialResponsePacket;
            if (connection->readPacket(&initialResponsePacket)) {
              if (initialResponsePacket.getHeader() !=
                  EtPacketType::INITIAL_RESPONSE) {
                CLOG(INFO, "stdout") << "Error: Missing initial response\n";
                STFATAL << "Missing initial response!";
              }
              auto initialResponse = stringToProto<InitialResponse>(
                  initialResponsePacket.getPayload());
              if (initialResponse.has_error()) {
                CLOG(INFO, "stdout") << "Error initializing connection: "
                                     << initialResponse.error() << endl;
                exit(1);
              }
              fail = false;
              break;
            }
          }
        }
      }
      if (fail) {
        LOG(WARNING) << "Connecting to server failed: Connect timeout";
        connectFailCount++;
        if (connectFailCount == 3) {
          throw std::runtime_error("Connect Timeout");
        }
      }
    } catch (const runtime_error& err) {
      LOG(INFO) << "Could not make initial connection to server";
      CLOG(INFO, "stdout") << "Could not make initial connection to "
                           << _socketEndpoint << ": " << err.what() << endl;
      exit(1);
    }

    TelemetryService::get()->logToDatadog("Connection Established",
                                          el::Level::Info, __FILE__, __LINE__);
    break;
  }
  VLOG(1) << "Client created with id: " << connection->getId();
};

TerminalClient::~TerminalClient() {
  connection->shutdown();
  console.reset();
  portForwardHandler.reset();
  connection.reset();
}

void TerminalClient::run(const string& command, const bool noexit) {
  if (console) {
    console->setup();
  }

// TE sends/receives data to/from the shell one char at a time.
#define BUF_SIZE (16 * 1024)
  char b[BUF_SIZE];

  time_t keepaliveTime = time(NULL) + keepaliveDuration;
  bool waitingOnKeepalive = false;

  auto sendTerminalBuffer = [&](const string& buffer) {
    if (buffer.empty()) {
      return;
    }
    et::TerminalBuffer tb;
    tb.set_buffer(buffer);
    connection->writePacket(
        Packet(TerminalPacketType::TERMINAL_BUFFER, protoToString(tb)));
    keepaliveTime = time(NULL) + keepaliveDuration;
  };

  // Bracketed paste: accumulate content between \e[200~ and \e[201~
  // across multiple read() chunks.  When complete, check if the pasted
  // text is a local image file path — if so, send as image frame.
  string pasteBuf;
  bool inPaste = false;

  auto flushPasteBuf = [&]() {
    if (pasteBuf.empty()) {
      inPaste = false;
      return;
    }
#ifdef __APPLE__
    const bool imagePasteEnabled =
        clipboardImagePasteSupported &&
        getenv("ET_DISABLE_CLIPBOARD_IMAGE_PASTE") == nullptr;
    if (imagePasteEnabled) {
      string trimmed = trimWhitespace(pasteBuf);
      if (!trimmed.empty()) {
        optional<ClipboardImagePayload> fileImage =
            readLocalFileAsImage(trimmed);
        if (fileImage) {
          sendTerminalBuffer(encodeClipboardImageFrame(*fileImage));
          pasteBuf.clear();
          inPaste = false;
          return;
        }
      }
    }
#endif
    // Not an image — re-emit with bracketed paste markers
    static const string kPS = "\x1b[200~";
    static const string kPE = "\x1b[201~";
    sendTerminalBuffer(kPS + pasteBuf + kPE);
    pasteBuf.clear();
    inPaste = false;
  };

  auto sendUserInput = [&](const string& input) {
    static const string kPS = "\x1b[200~";
    static const string kPE = "\x1b[201~";

    // Debug: log raw input to ~/et-plus-debug.log (max 50KB, rotates)
    if (getenv("ET_PLUS_DEBUG") != nullptr) {
      const char* home = getenv("HOME");
      if (home) {
        string logPath = string(home) + "/et-plus-debug.log";
        struct stat logSt;
        if (::stat(logPath.c_str(), &logSt) == 0 && logSt.st_size > 50000) {
          ::rename(logPath.c_str(), (logPath + ".old").c_str());
        }
        FILE* dbg = fopen(logPath.c_str(), "a");
        if (dbg) {
          fprintf(dbg, "[%d bytes] ", (int)input.size());
          for (size_t j = 0; j < input.size() && j < 200; j++) {
            unsigned char ch = input[j];
            if (ch >= 32 && ch < 127)
              fprintf(dbg, "%c", ch);
            else
              fprintf(dbg, "\\x%02x", ch);
          }
          if (input.size() > 200) fprintf(dbg, "...");
          fprintf(dbg, "\n");
          fclose(dbg);
        }
      }
    }

#ifdef __APPLE__
    const bool imagePasteEnabled =
        clipboardImagePasteSupported &&
        getenv("ET_DISABLE_CLIPBOARD_IMAGE_PASTE") == nullptr;
#endif

    size_t i = 0;
    while (i < input.size()) {
      if (inPaste) {
        // Look for paste-end marker in current data
        size_t pePos = input.find(kPE, i);
        if (pePos != string::npos) {
          pasteBuf.append(input, i, pePos - i);
          flushPasteBuf();
          i = pePos + kPE.size();
        } else {
          // End marker not in this chunk — keep buffering
          pasteBuf.append(input, i, input.size() - i);
          return;
        }
        continue;
      }

      // Not in paste — look for paste-start marker
      size_t psPos = input.find(kPS, i);

      if (psPos == string::npos) {
        // No paste start — process rest as normal input
        string rest = input.substr(i);
#ifdef __APPLE__
        if (imagePasteEnabled) {
          string passthrough;
          for (char c : rest) {
            if (c == 0x16) {
              sendTerminalBuffer(passthrough);
              passthrough.clear();
              optional<ClipboardImagePayload> image =
                  readLocalClipboardImage();
              if (image) {
                sendTerminalBuffer(encodeClipboardImageFrame(*image));
              } else {
                passthrough.push_back(c);
              }
            } else {
              passthrough.push_back(c);
            }
          }
          sendTerminalBuffer(passthrough);
        } else
#endif
        {
          sendTerminalBuffer(rest);
        }
        return;
      }

      // Send everything before paste start
      if (psPos > i) {
        sendTerminalBuffer(input.substr(i, psPos - i));
      }

      inPaste = true;
      pasteBuf.clear();
      i = psPos + kPS.size();
    }
  };

  if (command.length()) {
    LOG(INFO) << "Got command: " << command;
    if (noexit)
      sendTerminalBuffer(command + "\n");
    else
      sendTerminalBuffer(command + "; exit\n");
  }

  TerminalInfo lastTerminalInfo;

  if (!console.get()) {
    // NOTE: ../../scripts/ssh-et relies on the wording of this message, so if
    // you change it please update it as well.
    CLOG(INFO, "stdout") << "ET running, feel free to background..." << endl;
  }

  while (!connection->isShuttingDown()) {
    {
      lock_guard<recursive_mutex> guard(shutdownMutex);
      if (shuttingDown) {
        break;
      }
    }
    // Data structures needed for select() and
    // non-blocking I/O.
    fd_set rfd;
    timeval tv;

    FD_ZERO(&rfd);
    int maxfd = -1;
    int consoleFd = -1;
    if (console) {
      consoleFd = console->getFd();
      maxfd = consoleFd;
      FD_SET(consoleFd, &rfd);
    }
    int clientFd = connection->getSocketFd();
    if (clientFd > 0) {
      FD_SET(clientFd, &rfd);
      maxfd = max(maxfd, clientFd);
    }
    // Include port forward sockets in select for low-latency forwarding.
    set<int> pfFds;
    portForwardHandler->getForwardFds(&pfFds);
    for (int fd : pfFds) {
      FD_SET(fd, &rfd);
      maxfd = max(maxfd, fd);
    }
    tv.tv_sec = 0;
    tv.tv_usec = 10000;
    select(maxfd + 1, &rfd, NULL, NULL, &tv);


    try {
      if (console) {
        // Check for data to send.
        if (FD_ISSET(consoleFd, &rfd)) {
          // Read from stdin and write to our client that will then send it to
          // the server.
          VLOG(4) << "Got data from stdin";
#ifdef WIN32
          DWORD events;
          INPUT_RECORD buffer[128];
          HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
          PeekConsoleInput(handle, buffer, 128, &events);
          if (events > 0) {
            ReadConsoleInput(handle, buffer, 128, &events);
            string s;
            for (int keyEvent = 0; keyEvent < events; keyEvent++) {
              if (buffer[keyEvent].EventType == KEY_EVENT &&
                  buffer[keyEvent].Event.KeyEvent.bKeyDown) {
                char charPressed =
                    ((char)buffer[keyEvent].Event.KeyEvent.uChar.AsciiChar);
                if (charPressed) {
                  s += charPressed;
                }
              }
            }
            if (s.length()) {
              sendUserInput(s);
            }
          }
#else
          if (console) {
            int rc = ::read(consoleFd, b, BUF_SIZE);
            int savedErrno = errno;  // Save errno before any logging
            if (rc > 0) {
              // VLOG(1) << "Sending byte: " << int(b) << " " << char(b) << " "
              // << connection->getWriter()->getSequenceNumber();
              string s(b, rc);
              sendUserInput(s);
            } else if (rc == 0) {
              LOG(INFO) << "Console EOF";
              break;
            } else {
              if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
                // Transient error, retry
              } else {
                LOG(INFO) << "Console read error: (" << savedErrno
                          << "): " << strerror(savedErrno);
                break;
              }
            }
          }
#endif
        }
      }

      if (clientFd > 0 && FD_ISSET(clientFd, &rfd)) {
        VLOG(4) << "Clientfd is selected";
        while (connection->hasData()) {
          VLOG(4) << "connection has data";
          Packet packet;
          if (!connection->read(&packet)) {
            break;
          }
          uint8_t packetType = packet.getHeader();
          if (packetType == et::TerminalPacketType::PORT_FORWARD_DATA ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST ||
              packetType ==
                  et::TerminalPacketType::PORT_FORWARD_DESTINATION_RESPONSE) {
            keepaliveTime = time(NULL) + keepaliveDuration;
            VLOG(4) << "Got PF packet type " << packetType;
            portForwardHandler->handlePacket(packet, connection);
            continue;
          }
          switch (packetType) {
            case et::TerminalPacketType::TERMINAL_BUFFER: {
              if (console) {
                VLOG(3) << "Got terminal buffer";
                // Read from the server and write to our fake terminal
                et::TerminalBuffer tb =
                    stringToProto<et::TerminalBuffer>(packet.getPayload());
                const string& s = tb.buffer();
                // VLOG(5) << "Got message: " << s;
                // VLOG(1) << "Got byte: " << int(b) << " " << char(b) << " " <<
                // connection->getReader()->getSequenceNumber();
                keepaliveTime = time(NULL) + keepaliveDuration;
                console->write(s);
              }
              break;
            }
            case et::TerminalPacketType::KEEP_ALIVE:
              waitingOnKeepalive = false;
              // This will fill up log file quickly but is helpful for debugging
              // latency issues.
              LOG(INFO) << "Got a keepalive";
              break;
            default:
              STFATAL << "Unknown packet type: " << int(packetType);
          }
        }
      }

      if (clientFd > 0 && keepaliveTime < time(NULL)) {
        keepaliveTime = time(NULL) + keepaliveDuration;
        if (waitingOnKeepalive) {
          LOG(INFO) << "Missed a keepalive, killing connection.";
          connection->closeSocketAndMaybeReconnect();
          waitingOnKeepalive = false;
        } else {
          LOG(INFO) << "Writing keepalive packet";
          connection->writePacket(Packet(TerminalPacketType::KEEP_ALIVE, ""));
          waitingOnKeepalive = true;
        }
      }
      if (clientFd < 0) {
        // We are disconnected, so stop waiting for keepalive.
        waitingOnKeepalive = false;
      }

      if (console) {
        TerminalInfo ti = console->getTerminalInfo();

        if (ti != lastTerminalInfo) {
          LOG(INFO) << "Window size changed: row: " << ti.row()
                    << " column: " << ti.column() << " width: " << ti.width()
                    << " height: " << ti.height();
          lastTerminalInfo = ti;
          connection->writePacket(
              Packet(TerminalPacketType::TERMINAL_INFO, protoToString(ti)));
        }
      }

      vector<PortForwardDestinationRequest> requests;
      vector<PortForwardData> dataToSend;
      portForwardHandler->update(&requests, &dataToSend);
      for (auto& pfr : requests) {
        connection->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DESTINATION_REQUEST,
                   protoToString(pfr)));
        VLOG(4) << "send PF request";
        keepaliveTime = time(NULL) + keepaliveDuration;
      }
      for (auto& pwd : dataToSend) {
        connection->writePacket(
            Packet(TerminalPacketType::PORT_FORWARD_DATA, protoToString(pwd)));
        VLOG(4) << "send PF data";
        keepaliveTime = time(NULL) + keepaliveDuration;
      }
    } catch (const runtime_error& re) {
      STERROR << "Error: " << re.what();
      CLOG(INFO, "stdout") << "Connection closing because of error: "
                           << re.what() << endl;
      lock_guard<recursive_mutex> guard(shutdownMutex);
      shuttingDown = true;
    }
  }
  if (console) {
    console->teardown();
  }
  CLOG(INFO, "stdout") << "Session terminated" << endl;
}
}  // namespace et
