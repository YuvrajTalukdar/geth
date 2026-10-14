#include "sources/net.hpp"

#include "util/parse.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace geth {

std::vector<NetRow> parse_netdev(std::string_view text) {
  std::vector<NetRow> out;
  for (const std::string_view line : parse::split_lines(text)) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) continue;  // the two header lines
    NetRow r;
    r.name.assign(parse::trim(line.substr(0, colon)));
    const auto toks = parse::tokens(line.substr(colon + 1));
    if (toks.size() < 9) continue;
    const auto rx = parse::to_u64(toks[0]);
    const auto tx = parse::to_u64(toks[8]);
    if (!rx || !tx) continue;
    r.rx_bytes = *rx;
    r.tx_bytes = *tx;
    out.push_back(std::move(r));
  }
  return out;
}

NetFilterResult net_iface_included(const std::string& root, const std::string& name) {
  NetFilterResult r;

  if (name == "lo") {
    r.reason = "loopback";
    return r;
  }
  // Name-based virtual interfaces. (A docker/bridge device *could* have a
  // device link in exotic setups; the name rule keeps the output honest.)
  static const std::string_view kVirtualPrefixes[] = {
      "veth", "docker", "br-", "virbr", "tun", "tap", "wg"};
  for (const auto p : kVirtualPrefixes) {
    if (name.starts_with(p)) {
      r.reason = "virtual interface";
      return r;
    }
  }

  const std::string dir = root + "/" + name;
  std::error_code ec;
  // No backing PCI device: the interface is virtual (veth, bridge, ...).
  // exists() follows the symlink, so a broken link also reads as absent.
  if (!std::filesystem::exists(dir + "/device", ec) || ec) {
    r.reason = "no backing device";
    return r;
  }
  // Bridges carry a `bridge` attribute file.
  if (std::filesystem::exists(dir + "/bridge", ec) && !ec) {
    r.reason = "bridge";
    return r;
  }
  // type holds the ARPHRD_* value: 1 = Ethernet, 801 = IEEE 802.11.
  // (Modern wifi drivers also report 1, so 1 is the common case.)
  std::ifstream tf(dir + "/type");
  std::string tv;
  std::getline(tf, tv);
  const auto t = parse::to_u64(parse::trim(tv));
  if (!t) {
    r.reason = "no type file";
    return r;
  }
  if (*t != 1 && *t != 801) {
    r.reason = "type " + std::to_string(*t) + " is not Ethernet/wireless";
    return r;
  }
  r.included = true;
  return r;
}

SourceStatus NetSource::probe() {
  SourceStatus st;
  st.description = name();
  int err = 0;
  if (!SysfsFile::open("/proc/net/dev", _netdev, err)) {
    st.detail = "cannot open /proc/net/dev (errno " + std::to_string(err) + ")";
    return st;
  }
  st.paths.push_back("/proc/net/dev");

  // Report the filter decision for every interface the kernel knows.
  std::string inc, exc;
  for (const auto& row : parse_netdev(_netdev.read())) {
    const auto res = net_iface_included(_sysfs_root, row.name);
    if (res.included) {
      if (!inc.empty()) inc += ", ";
      inc += row.name;
    } else {
      if (!exc.empty()) exc += "; ";
      exc += row.name + " (" + res.reason + ")";
    }
  }
  st.detail = "per-interface rx/tx byte rates; sampled: " +
             (inc.empty() ? "(none)" : inc);
  if (!exc.empty()) st.detail += "\nexcluded: " + exc;
  st.available = true;
  return st;
}

void NetSource::sample(Snapshot& out) {
  const auto rows = parse_netdev(_netdev.read());

  if (!_have_prev) {
    _prev = rows;
    _have_prev = true;
    _prev_tp = Clock::now();
    return;
  }

  const double dt = std::chrono::duration<double>(Clock::now() - _prev_tp).count();
  const bool usable = dt > 0.0 && dt < 604800.0;

  double tot_rx = 0.0, tot_tx = 0.0;
  bool any = false;
  for (const auto& r : rows) {
    // Re-checked each tick: interfaces can appear/disappear at runtime.
    if (!net_iface_included(_sysfs_root, r.name).included) continue;
    NetIfSample n;
    n.name = r.name;
    if (usable) {
      const auto prev = std::find_if(_prev.begin(), _prev.end(),
                                     [&](const NetRow& p) { return p.name == r.name; });
      if (prev != _prev.end()) {
        // Unsigned: byte counters are monotonic (they only reset on ifdown).
        const unsigned long long drx = r.rx_bytes - prev->rx_bytes;
        const unsigned long long dtx = r.tx_bytes - prev->tx_bytes;
        n.rx_bytes_per_s = static_cast<double>(drx) / dt;
        n.tx_bytes_per_s = static_cast<double>(dtx) / dt;
        tot_rx += *n.rx_bytes_per_s;
        tot_tx += *n.tx_bytes_per_s;
        any = true;
      }
    }
    out.net.interfaces.push_back(std::move(n));
  }
  if (any) {
    out.net.total_rx_bytes_per_s = tot_rx;
    out.net.total_tx_bytes_per_s = tot_tx;
  }

  _prev = rows;
  _prev_tp = Clock::now();
}

}  // namespace geth
