#ifndef WIREGUARD_WIREGUARD_CONFIG_PARSER_H_
#define WIREGUARD_WIREGUARD_CONFIG_PARSER_H_

#include <string>

#include "brave/browser/brave_vpn/poc_wireguard/wireguard_tunnel.h"

namespace wireguard {

// Parses a WireGuard .conf file and populates |config|.
// Returns false and logs an error if the file cannot be read or is malformed.
// Unknown keys are silently ignored for forward compatibility.
bool ParseConfigFile(const std::string& path, WireGuardConfig& config);

// Logs the contents of |config| at INFO level. Private key is masked.
void LogConfig(const WireGuardConfig& config);

}  // namespace wireguard

#endif  // WIREGUARD_WIREGUARD_CONFIG_PARSER_H_
