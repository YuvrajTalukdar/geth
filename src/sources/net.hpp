// Network rx/tx rates from /proc/net/dev (byte counters per interface).
//
// Only real, physical interfaces are sampled: an interface qualifies if it
// has a backing PCI device (/sys/class/net/<if>/device exists), is not a
// bridge, and is Ethernet or wireless. That drops loopback, veth pairs,
// docker*, bridges, wireguard, tun/tap — none of which represents traffic
// leaving the machine. The rule is pure over a sysfs root, so it is
// unit-testable against a fake tree.
#pragma once

#include "core/source.hpp"
#include "util/sysfs_file.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace geth {

struct NetRow {
  std::string name;
  unsigned long long rx_bytes = 0;
  unsigned long long tx_bytes = 0;
};

// Pure over string_view for testing. Layout after the ':' per interface:
//   0 RX_BYTES  1 packets  2 errs  3 drop  4 fifo  5 frame  6 compressed
//   7 multicast  8 TX_BYTES  9 packets ...
std::vector<NetRow> parse_netdev(std::string_view text);

struct NetFilterResult {
  bool included = false;
  std::string reason;  // "" when included; why it was excluded otherwise
};

// Decides whether to sample `name`. Every rule is checked against
// `root`/name in sysfs (default /sys/class/net) so tests can point it at a
// fixture tree:
//   1. "lo"                          -> loopback, excluded
//   2. veth*/docker*/br-*/virbr*/tun*/tap*/wg* -> virtual, excluded
//   3. no <root>/<if>/device         -> no backing PCI device, excluded
//   4. <root>/<if>/bridge exists     -> bridge, excluded
//   5. type != 1 (Ethernet) and
//      type != 801 (IEEE 802.11)     -> excluded
NetFilterResult net_iface_included(const std::string& root, const std::string& name);

class NetSource : public Source {
 public:
  NetSource() = default;
  explicit NetSource(std::string sysfs_root) : _sysfs_root(std::move(sysfs_root)) {}

  const char* name() const override { return "Network"; }
  SourceStatus probe() override;
  void sample(Snapshot& out) override;

 private:
  struct NetIf {
    std::string name;
    unsigned long long prev_rx = 0;
    unsigned long long prev_tx = 0;
    bool have_prev = false;
  };

  SysfsFile _netdev;                 // /proc/net/dev
  std::string _sysfs_root = "/sys/class/net";
  std::vector<NetIf> _ifs;           // included interfaces, fixed at probe
  TimePoint _prev_tp;
  bool _have_prev = false;
  bool _sized = false;  // snapshot's interface slots sized once
};

}  // namespace geth
