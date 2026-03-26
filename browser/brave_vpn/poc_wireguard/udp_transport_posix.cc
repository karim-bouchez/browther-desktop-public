#include "brave/browser/brave_vpn/poc_wireguard/udp_transport_posix.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/logging.h"
#include "base/posix/eintr_wrapper.h"

namespace wireguard {

PosixUdpTransport::PosixUdpTransport() = default;

PosixUdpTransport::~PosixUdpTransport() {
  Close();
}

bool PosixUdpTransport::Open(const std::string& peer_ip,
                             uint16_t peer_port,
                             uint16_t local_port) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd_ < 0) {
    PLOG(ERROR) << "UDP: socket() failed";
    return false;
  }

  if (local_port > 0) {
    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(local_port);
    if (::bind(fd_, reinterpret_cast<struct sockaddr*>(&local), sizeof(local)) <
        0) {
      PLOG(ERROR) << "UDP: bind() to port " << local_port << " failed";
      Close();
      return false;
    }
  }

  peer_addr_.sin_family = AF_INET;
  peer_addr_.sin_port = htons(peer_port);
  if (::inet_pton(AF_INET, peer_ip.c_str(), &peer_addr_.sin_addr) != 1) {
    LOG(ERROR) << "UDP: invalid peer IP: " << peer_ip;
    Close();
    return false;
  }

  LOG(INFO) << "UDP transport open -> " << peer_ip << ":" << peer_port
            << " (fd=" << fd_ << ")";
  return true;
}

void PosixUdpTransport::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

int PosixUdpTransport::Send(const uint8_t* buf, size_t size) {
  ssize_t n = HANDLE_EINTR(::sendto(
      fd_, buf, size, 0, reinterpret_cast<const struct sockaddr*>(&peer_addr_),
      sizeof(peer_addr_)));
  if (n < 0) {
    PLOG(ERROR) << "UDP sendto error";
  }
  return static_cast<int>(n);
}

int PosixUdpTransport::Recv(uint8_t* buf, size_t buf_size) {
  ssize_t n = HANDLE_EINTR(::recvfrom(fd_, buf, buf_size, 0, nullptr, nullptr));
  if (n < 0) {
    PLOG(ERROR) << "UDP recvfrom error";
  }
  return static_cast<int>(n);
}

void PosixUdpTransport::WatchReadable(base::RepeatingClosure on_readable) {
  watcher_ =
      base::FileDescriptorWatcher::WatchReadable(fd_, std::move(on_readable));
}

void PosixUdpTransport::StopWatching() {
  watcher_.reset();
}

std::unique_ptr<UdpTransport> CreateUdpTransport() {
  return std::make_unique<PosixUdpTransport>();
}

}  // namespace wireguard
