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

namespace {

// Reads field 0 (RX bytes) and field 8 (TX bytes) of the text after the
// ':' of a /proc/net/dev line. Hand-tokenized: the per-tick hot path must
// not build intermediate token vectors.
bool netdev_counters(std::string_view s, unsigned long long& rx,
                     unsigned long long& tx) {
  unsigned long long f[9];
  int nf = 0;
  while (nf < 9) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s = s.substr(1);
    std::size_t i = 0;
    while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
    const std::string_view tok = s.substr(0, i);
    const auto v = parse::to_u64(tok);
    if (!v) return false;
    f[nf++] = *v;
    if (i >= s.size()) break;
    s = s.substr(i);
  }
  if (nf != 9) return false;
  rx = f[0];
  tx = f[8];
  return true;
}

}  // namespace

SourceStatus NetSource::probe() {
  SourceStatus st;
  st.description = name();
  if (!_test_netdev_on) {
    int err = 0;
    if (!SysfsFile::open("/proc/net/dev", _netdev, err)) {
      st.detail = "cannot open /proc/net/dev (errno " + std::to_string(err) + ")";
      return st;
    }
    st.paths.push_back("/proc/net/dev");
  }

  // Report the filter decision for every interface the kernel knows, and
  // fix the sampled set: the per-tick path only fills values for these.
  // (An interface hot-plugged after probe shows up on the next geth run —
  // the same is true of the test seam, which is fixed at probe time.)
  std::string inc, exc;
  _ifs.clear();
  for (const auto& row : parse_netdev(netdev_text())) {
    const auto res = net_iface_included(_sysfs_root, row.name);
    if (res.included) {
      if (!inc.empty()) inc += ", ";
      inc += row.name;
      NetIf if_;
      if_.name = row.name;
      _ifs.push_back(std::move(if_));
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
  const TimePoint now = Clock::now();
  const double dt = std::chrono::duration<double>(now - _prev_tp).count();
  const bool usable = _have_prev && dt > 0.0 && dt < 604800.0;

  if (!_sized) {
    // One-time: size the reused Snapshot's interface slots + names.
    out.net.interfaces.assign(_ifs.size(), NetIfSample{});
    for (std::size_t i = 0; i < _ifs.size(); ++i)
      out.net.interfaces[i].name = _ifs[i].name;
    _sized = true;
  }

  // Interfaces missing from this tick's /proc/net/dev get a gap (nullopt),
  // never a stale or fake 0.
  for (auto& n : out.net.interfaces) {
    n.rx_bytes_per_s.reset();
    n.tx_bytes_per_s.reset();
  }
  out.net.total_rx_bytes_per_s.reset();
  out.net.total_tx_bytes_per_s.reset();

  double tot_rx = 0.0, tot_tx = 0.0;
  bool any = false;
  const std::string& text = netdev_text();
  parse::for_each_line(text, [&](std::string_view line) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return true;  // the two header lines
    const std::string_view name = parse::trim(line.substr(0, colon));
    int idx = -1;
    for (std::size_t i = 0; i < _ifs.size(); ++i)
      if (name == std::string_view(_ifs[i].name)) {
        idx = static_cast<int>(i);
        break;
      }
    if (idx < 0) return true;  // not in the probed set: ignore
    unsigned long long rx = 0, tx = 0;
    if (!netdev_counters(line.substr(colon + 1), rx, tx)) return true;
    NetIf& if_ = _ifs[static_cast<std::size_t>(idx)];
    if (usable && if_.have_prev) {
      // Unsigned: byte counters are monotonic (they only reset on ifdown).
      const unsigned long long drx = rx - if_.prev_rx;
      const unsigned long long dtx = tx - if_.prev_tx;
      out.net.interfaces[static_cast<std::size_t>(idx)].rx_bytes_per_s =
          static_cast<double>(drx) / dt;
      out.net.interfaces[static_cast<std::size_t>(idx)].tx_bytes_per_s =
          static_cast<double>(dtx) / dt;
      tot_rx += static_cast<double>(drx) / dt;
      tot_tx += static_cast<double>(dtx) / dt;
      any = true;
    }
    if_.prev_rx = rx;
    if_.prev_tx = tx;
    if_.have_prev = true;
    return true;
  });
  if (any) {
    out.net.total_rx_bytes_per_s = tot_rx;
    out.net.total_tx_bytes_per_s = tot_tx;
  }

  _prev_tp = now;
  _have_prev = true;
}

}  // namespace geth
