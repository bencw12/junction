extern "C" {
#include <netinet/tcp.h>
}

#include "junction/net/socket.h"

#include "junction/bindings/log.h"

namespace junction {

namespace {

// Options that tune behaviour this stack does not have, or has already: there
// is no Nagle delay to turn off, no keepalive probing, no kernel buffer to
// size, no ICMP error queue, no TOS marking. Accepting and ignoring them is
// invisible to the caller; refusing them is not. glibc's resolver abandons a
// nameserver whose socket rejects IP_RECVERR, and Python's http.client raises
// when TCP_NODELAY fails -- so no DNS, no pip, no urllib.
bool IsAdvisorySockOpt(int level, int optname) {
  switch (level) {
    case IPPROTO_TCP:
      return optname == TCP_NODELAY || optname == TCP_KEEPIDLE ||
             optname == TCP_KEEPINTVL || optname == TCP_KEEPCNT ||
             optname == TCP_QUICKACK || optname == TCP_USER_TIMEOUT;
    case IPPROTO_IP:
      return optname == IP_RECVERR || optname == IP_TOS;
    case SOL_SOCKET:
      return optname == SO_KEEPALIVE || optname == SO_SNDBUF ||
             optname == SO_RCVBUF || optname == SO_BROADCAST ||
             optname == SO_PRIORITY || optname == SO_OOBINLINE;
    default:
      return false;
  }
}

}  // namespace

// What an advisory option reads back as when it was never set: Linux's
// defaults, except TCP_NODELAY, which is simply true of this stack.
int AdvisoryDefault(int level, int optname) {
  if (level == IPPROTO_TCP) {
    switch (optname) {
      case TCP_NODELAY: return 1;
      case TCP_KEEPIDLE: return 7200;
      case TCP_KEEPINTVL: return 75;
      case TCP_KEEPCNT: return 9;
    }
  }
  if (level == SOL_SOCKET && (optname == SO_SNDBUF || optname == SO_RCVBUF))
    return 212992;
  return 0;
}

Status<size_t> Socket::GetSockOpt(int level, int optname,
                                  std::span<std::byte> value) const {
  if (IsAdvisorySockOpt(level, optname)) {
    if (value.size() < sizeof(int)) return MakeError(EINVAL);
    int v = AdvisoryDefault(level, optname);
    for (const auto &o : advisory_opts_)
      if (o.level == level && o.optname == optname) v = o.value;
    *reinterpret_cast<int *>(value.data()) = v;
    return sizeof(int);
  }
  if (level == IPPROTO_TCP || level == IPPROTO_UDP)
    return GetSockOptImpl(level, optname, value);
  if (level == IPPROTO_IP) return GetIPSocketOptions(optname, value);
  if (level != SOL_SOCKET) return MakeError(EINVAL);
  switch (optname) {
    case SO_REUSEADDR:  // stored together with SO_REUSEPORT
    case SO_REUSEPORT:
      if (value.size() < sizeof(int)) return MakeError(EINVAL);
      *reinterpret_cast<int *>(value.data()) = reuse_port() ? 1 : 0;
      return sizeof(int);
    case SO_RCVTIMEO:
      if (value.size() < sizeof(timeval)) return MakeError(EINVAL);
      *reinterpret_cast<timeval *>(value.data()) = read_timeout_.Timeval();
      return sizeof(timeval);
    case SO_SNDTIMEO:
      if (value.size() < sizeof(timeval)) return MakeError(EINVAL);
      *reinterpret_cast<timeval *>(value.data()) = write_timeout_.Timeval();
      return sizeof(timeval);
    default:
      return GetSockOptImpl(level, optname, value);
  }
}


Status<void> Socket::SetSockOpt(int level, int optname,
                                std::span<const std::byte> value) {
  if (IsAdvisorySockOpt(level, optname)) {
    // Remembered so that it reads back as set: gRPC sets TCP_NODELAY and then
    // checks it, and gives up on the connection if it cannot.
    int v = value.size() >= sizeof(int)
                ? *reinterpret_cast<const int *>(value.data())
                : 0;
    for (auto &o : advisory_opts_)
      if (o.level == level && o.optname == optname) {
        o.value = v;
        return {};
      }
    advisory_opts_.push_back({level, optname, v});
    return {};
  }
  if (level == IPPROTO_TCP || level == IPPROTO_UDP)
    return SetSockOptImpl(level, optname, value);
  if (level == IPPROTO_IP) return SetIPSocketOptions(optname, value);
  if (level != SOL_SOCKET) return MakeError(EINVAL);
  switch (optname) {
    case SO_REUSEADDR:
    case SO_REUSEPORT:
      reuse_port_ = *reinterpret_cast<const int *>(value.data());
      return {};
    case SO_RCVTIMEO:
      if (value.size() < sizeof(timeval)) return MakeError(EINVAL);
      read_timeout_ =
          Duration(*reinterpret_cast<const timeval *>(value.data()));
      return {};
    case SO_SNDTIMEO:
      if (value.size() < sizeof(timeval)) return MakeError(EINVAL);
      write_timeout_ =
          Duration(*reinterpret_cast<const timeval *>(value.data()));
      return {};
    default:
      return SetSockOptImpl(level, optname, value);
  }
}

Status<void> IPSocket::SetIPSocketOptions(int optname,
                                          std::span<const std::byte> optval) {
  if (optval.size() < sizeof(int)) return MakeError(EINVAL);
  const int val = *reinterpret_cast<const int *>(optval.data());
  if (optname == IP_RECVTOS) {
    if (!val)
      remove_socket_option(kSockOptRecvTos);
    else
      add_socket_option(kSockOptRecvTos);
  } else if (optname == IP_PKTINFO) {
    if (!val)
      remove_socket_option(kSockOptPktInfo);
    else
      add_socket_option(kSockOptPktInfo);
  } else if (optname == IP_MTU_DISCOVER) {
    LOG_ONCE(WARN) << "Setsockopt: ignoring IP_MTU_DISCOVER directive";
  } else {
    return MakeError(EINVAL);
  }
  return {};
}

Status<size_t> IPSocket::GetIPSocketOptions(int optname,
                                            std::span<std::byte> value) const {
  if (value.size() < sizeof(int)) return MakeError(EINVAL);
  int *optval = reinterpret_cast<int *>(value.data());
  if (optname == IP_RECVTOS) {
    *optval = socket_options() & kSockOptRecvTos ? 1 : 0;
  } else if (optname == IP_PKTINFO) {
    *optval = socket_options() & kSockOptPktInfo ? 1 : 0;
  } else if (optname == IP_MTU) {
    LOG_ONCE(WARN) << "Getsockopt: reporting default (non-path specific) MTU";
    *optval = udp_get_payload_size();
  } else {
    return MakeError(EINVAL);
  }
  return sizeof(int);
}

}  // namespace junction