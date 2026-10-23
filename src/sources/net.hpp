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

  // Test seam (mirrors CpuSource::set_test_stat): make probe() and sample()
  // parse THIS /proc/net/dev text instead of reading the real file. A removal
  // test "unplugs" an interface mid-run by setting new text without it —
  // something that can't happen for real in a test. Call before probe_all()
  // so the warm-up baseline uses it too; it stays on for the source's life.
  void set_test_netdev(std::string text) {
    _test_netdev = std::move(text);
    _test_netdev_on = true;
  }

 private:
  struct NetIf {
    std::string name;
    unsigned long long prev_rx = 0;
    unsigned long long prev_tx = 0;
    bool have_prev = false;
  };

  // The /proc/net/dev text for this tick: the test seam when set, else the
  // live file. probe() fixes the sampled set from it and sample() computes
  // rates — so a seam change removes an interface mid-run. The live read
  // aliases SysfsFile's reused buffer; the seam is a stable member string.
  const std::string& netdev_text() {
    if (_test_netdev_on) return _test_netdev;
    return _netdev.read();
  }

  SysfsFile _netdev;                 // /proc/net/dev
  std::string _sysfs_root = "/sys/class/net";
  std::vector<NetIf> _ifs;           // included interfaces, fixed at probe
  TimePoint _prev_tp;
  bool _have_prev = false;
  bool _sized = false;  // snapshot's interface slots sized once
  std::string _test_netdev;          // test seam (see set_test_netdev)
  bool _test_netdev_on = false;
};

}  // namespace geth
