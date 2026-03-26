#include "brave/browser/brave_vpn/poc_wireguard/wireguard_config_parser.h"

#include <fstream>
#include <sstream>
#include <string>

#include "base/logging.h"
#include "base/strings/string_util.h"

namespace wireguard {

namespace {

// Trims leading and trailing ASCII whitespace in-place.
std::string Trim(const std::string& s) {
  const auto start = s.find_first_not_of(" \t\r\n");
  if (start == std::string::npos) {
    return {};
  }
  const auto end = s.find_last_not_of(" \t\r\n");
  return s.substr(start, end - start + 1);
}

// Splits "key = value" into key and value, returns false if malformed.
bool SplitKeyValue(const std::string& line,
                   std::string& key,
                   std::string& value) {
  const auto eq = line.find('=');
  if (eq == std::string::npos) {
    return false;
  }
  key = Trim(line.substr(0, eq));
  value = Trim(line.substr(eq + 1));
  return !key.empty();
}

enum class Section { kNone, kInterface, kPeer };

}  // namespace

bool ParseConfigFile(const std::string& path, WireGuardConfig& config) {
  std::ifstream f(path);
  if (!f.is_open()) {
    LOG(ERROR) << "ParseConfigFile: cannot open: " << path;
    return false;
  }

  Section section = Section::kNone;
  std::string line;
  int line_num = 0;

  while (std::getline(f, line)) {
    ++line_num;
    line = Trim(line);

    // Skip blank lines and comments.
    if (line.empty() || line[0] == '#' || line[0] == ';') {
      continue;
    }

    // Section header.
    if (line[0] == '[') {
      const auto close = line.find(']');
      if (close == std::string::npos) {
        LOG(ERROR) << "ParseConfigFile:" << line_num << ": malformed section";
        return false;
      }
      const std::string name = Trim(line.substr(1, close - 1));
      if (base::EqualsCaseInsensitiveASCII(name, "Interface")) {
        section = Section::kInterface;
      } else if (base::EqualsCaseInsensitiveASCII(name, "Peer")) {
        section = Section::kPeer;
      } else {
        section = Section::kNone;
      }
      continue;
    }

    std::string key, value;
    if (!SplitKeyValue(line, key, value)) {
      LOG(WARNING) << "ParseConfigFile:" << line_num
                   << ": skipping malformed line: " << line;
      continue;
    }

    if (section == Section::kInterface) {
      if (base::EqualsCaseInsensitiveASCII(key, "PrivateKey")) {
        config.local_private_key = value;
      } else if (base::EqualsCaseInsensitiveASCII(key, "Address")) {
        // Strip CIDR prefix if present (e.g. "10.10.2.4/24" -> "10.10.2.4").
        const auto slash = value.find('/');
        config.local_tun_ip =
            slash != std::string::npos ? value.substr(0, slash) : value;
      } else if (base::EqualsCaseInsensitiveASCII(key, "DNS")) {
        // DNS may be a comma-separated list.
        std::istringstream ss(value);
        std::string server;
        while (std::getline(ss, server, ',')) {
          const std::string trimmed = Trim(server);
          if (!trimmed.empty()) {
            config.dns_servers.push_back(trimmed);
          }
        }
      } else if (base::EqualsCaseInsensitiveASCII(key, "MTU")) {
        config.mtu = std::stoi(value);
      }
      // ListenPort intentionally ignored — we use ephemeral port.
    } else if (section == Section::kPeer) {
      if (base::EqualsCaseInsensitiveASCII(key, "PublicKey")) {
        config.peer_public_key = value;
      } else if (base::EqualsCaseInsensitiveASCII(key, "PresharedKey")) {
        config.preshared_key = value;
      } else if (base::EqualsCaseInsensitiveASCII(key, "Endpoint")) {
        // "host:port" — handle IPv6 "[::1]:port" too.
        const auto colon = value.rfind(':');
        if (colon == std::string::npos) {
          LOG(ERROR) << "ParseConfigFile:" << line_num
                     << ": malformed Endpoint: " << value;
          return false;
        }
        config.peer_ip = value.substr(0, colon);
        config.peer_port =
            static_cast<uint16_t>(std::stoi(value.substr(colon + 1)));
        // Strip brackets from IPv6 addresses.
        if (!config.peer_ip.empty() && config.peer_ip.front() == '[') {
          config.peer_ip = config.peer_ip.substr(1, config.peer_ip.size() - 2);
        }
      } else if (base::EqualsCaseInsensitiveASCII(key, "PersistentKeepalive")) {
        config.keepalive_seconds = static_cast<uint16_t>(std::stoi(value));
      }
      // AllowedIPs ignored for now.
    }
  }

  // Validate required fields.
  bool ok = true;
  if (config.local_private_key.empty()) {
    LOG(ERROR) << "ParseConfigFile: missing PrivateKey";
    ok = false;
  }
  if (config.peer_public_key.empty()) {
    LOG(ERROR) << "ParseConfigFile: missing PublicKey";
    ok = false;
  }
  if (config.peer_ip.empty()) {
    LOG(ERROR) << "ParseConfigFile: missing Endpoint";
    ok = false;
  }
  if (config.local_tun_ip.empty()) {
    LOG(ERROR) << "ParseConfigFile: missing Address";
    ok = false;
  }
  return ok;
}

void LogConfig(const WireGuardConfig& config) {
  // Mask keys — show only the first 4 characters so the log is useful for
  // debugging (confirms the right key was loaded) without leaking secrets.
  auto mask = [](const std::string& key) -> std::string {
    if (key.empty()) {
      return "(not set)";
    }
    return key.substr(0, 4) + "****";
  };

  LOG(INFO) << "[Interface]";
  LOG(INFO) << "  Address    = " << config.local_tun_ip;
  LOG(INFO) << "  PrivateKey = " << mask(config.local_private_key);
  LOG(INFO) << "  MTU        = " << config.mtu;
  if (!config.dns_servers.empty()) {
    std::string dns;
    for (const auto& s : config.dns_servers) {
      if (!dns.empty()) {
        dns += ", ";
      }
      dns += s;
    }
    LOG(INFO) << "  DNS        = " << dns;
  }
  LOG(INFO) << "[Peer]";
  LOG(INFO) << "  PublicKey  = " << mask(config.peer_public_key);
  LOG(INFO) << "  Endpoint   = " << config.peer_ip << ":" << config.peer_port;
  if (!config.preshared_key.empty()) {
    LOG(INFO) << "  PresharedKey = " << mask(config.preshared_key);
  }
  if (config.keepalive_seconds > 0) {
    LOG(INFO) << "  PersistentKeepalive = " << config.keepalive_seconds;
  }
}

}  // namespace wireguard
