#include "ui/config.hpp"

#include <map>
#include <set>
#include <string_view>

#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>

#include <toml++/toml.hpp>

namespace geth::ui {

namespace {

std::string key_str(const toml::key& k) {
  return std::string(std::string_view(k));
}
int key_line(const toml::key& k) { return k.source().begin.line; }

// Prints the warning to stderr and returns it for Config::warnings.
std::string warn(const Config& c, int line, const std::string& what) {
  std::fprintf(stderr, "geth: config%s%s: %s\n", c.path.empty() ? "" : ":",
               line > 0 ? (" line " + std::to_string(line) + ":").c_str()
                        : "",
               what.c_str());
  return what;
}

// TOML basic-string escaping (backslash, quote, control characters).
std::string escape_toml(const std::string& s) {
  std::string out;
  for (const char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\f': out += "\\f"; break;
      case '\r': out += "\\r"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04X", static_cast<unsigned>(c));
          out += buf;
        } else {
          out += ch;
        }
    }
  }
  return out;
}

}  // namespace

bool valid_interval_ms(long long ms) {
  return ms == 250 || ms == 500 || ms == 1000 || ms == 2000 || ms == 5000;
}

bool valid_window_ns(int64_t ns) {
  return ns == 30000000000LL || ns == 60000000000LL || ns == 120000000000LL ||
         ns == 300000000000LL || ns == 600000000000LL;
}

bool valid_columns(int columns) {
  return columns == -1 || (columns >= 1 && columns <= 8);
}

std::string window_to_string(int64_t ns) {
  if (ns == 30000000000LL) return "30s";
  if (ns == 60000000000LL) return "1m";
  if (ns == 120000000000LL) return "2m";
  if (ns == 300000000000LL) return "5m";
  if (ns == 600000000000LL) return "10m";
  return "";
}

bool window_from_string(const std::string& s, int64_t& out) {
  if (s == "30s") out = 30000000000LL;
  else if (s == "1m") out = 60000000000LL;
  else if (s == "2m") out = 120000000000LL;
  else if (s == "5m") out = 300000000000LL;
  else if (s == "10m") out = 600000000000LL;
  else return false;
  return true;
}

std::string Config::default_path() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  std::string base;
  if (xdg && *xdg)
    base = xdg;
  else if (const char* home = std::getenv("HOME"))
    base = std::string(home) + "/.config";
  else
    base = ".config";
  return base + "/geth/config.toml";
}

Config Config::load(const std::string& path, bool no_save) {
  Config c;
  c.path = path;
  c.no_save = no_save;

  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return c;  // first run: defaults
  c.exists = true;

  // A .tmp left by a crashed save: the main file wins, drop the tmp.
  {
    std::error_code ec2;
    std::filesystem::remove(path + ".tmp", ec2);
  }

  try {
    const toml::table root = toml::parse_file(path);

    // Unknown root keys.
    for (auto& [k, v] : root) {
      const std::string name = key_str(k);
      if (name != "general" && name != "panels" && name != "gpus")
        c.warnings.push_back(
            warn(c, key_line(k), "unknown key '" + name + "' — ignored"));
    }

    // [general] -----------------------------------------------------------
    if (root.contains("general")) {
      const auto& gv = root.at("general");
      if (!gv.is_table()) {
        c.warnings.push_back(warn(c, 0, "'general' must be a table — ignored"));
      } else {
        const toml::table* gt = gv.as<toml::table>();
        for (auto& [k, v] : *gt) {
          const std::string name = key_str(k);
          const int line = key_line(k);
          if (name == "interval_ms") {
            if (v.is_integer()) {
              const long long ms = v.as<int64_t>()->get();
              if (valid_interval_ms(ms)) c.interval_ms = ms;
              else c.warnings.push_back(
                  warn(c, line, "general.interval_ms must be one of 250/500/"
                                "1000/2000/5000 — ignored"));
            } else
              c.warnings.push_back(
                  warn(c, line, "general.interval_ms must be an integer — "
                                "ignored"));
          } else if (name == "window") {
            if (v.is_string()) {
              int64_t ns = 0;
              if (window_from_string(v.as<std::string>()->get(), ns))
                c.window_ns = ns;
              else
                c.warnings.push_back(warn(
                    c, line,
                    "general.window must be 30s/1m/2m/5m/10m — ignored"));
            } else
              c.warnings.push_back(
                  warn(c, line, "general.window must be a string — ignored"));
          } else if (name == "columns") {
            if (v.is_string() && v.as<std::string>()->get() == "auto")
              c.columns = -1;
            else if (v.is_integer()) {
              const int n = static_cast<int>(v.as<int64_t>()->get());
              if (valid_columns(n)) c.columns = n;
              else c.warnings.push_back(warn(c, line,
                                             "general.columns must be "
                                             "\"auto\" or 1..8 — ignored"));
            } else
              c.warnings.push_back(warn(
                  c, line, "general.columns must be \"auto\" or 1..8 — "
                           "ignored"));
          } else if (name == "title") {
            if (v.is_string()) c.title = v.as<std::string>()->get();
            else
              c.warnings.push_back(warn(c, line,
                                        "general.title must be a string — "
                                        "ignored"));
          } else if (name == "theme") {
            // Any non-empty string is accepted here; an unknown name is a
            // resolution-time warning (fallback to cyberpunk-blue), not a
            // config error — a typo shouldn't break the whole file.
            if (v.is_string() && !v.as<std::string>()->get().empty())
              c.theme = v.as<std::string>()->get();
            else
              c.warnings.push_back(warn(
                  c, line, "general.theme must be a non-empty string — "
                           "ignored"));
          } else if (name == "background") {
            if (v.is_string()) {
              const std::string s = v.as<std::string>()->get();
              if (s == "transparent" || s == "solid") c.background = s;
              else
                c.warnings.push_back(warn(
                    c, line, "general.background must be \"transparent\" or "
                             "\"solid\" — ignored"));
            } else
              c.warnings.push_back(warn(
                  c, line, "general.background must be a string — ignored"));
          } else if (name == "graph_style") {
            if (v.is_string()) {
              const std::string s = v.as<std::string>()->get();
              if (s == "braille" || s == "blocks") c.graph_style = s;
              else
                c.warnings.push_back(warn(
                    c, line, "general.graph_style must be \"braille\" or "
                             "\"blocks\" — ignored"));
            } else
              c.warnings.push_back(warn(
                  c, line, "general.graph_style must be a string — ignored"));
          } else {
            c.warnings.push_back(warn(c, line,
                                      "unknown key 'general." + name +
                                          "' — ignored"));
          }
        }
      }
    }

    // [[panels]] -----------------------------------------------------------
    if (root.contains("panels")) {
      const auto& pv = root.at("panels");
      if (!pv.is_array()) {
        c.warnings.push_back(warn(c, 0, "'panels' must be an array of "
                                        "tables — ignored"));
      } else {
        const toml::array* pa = pv.as<toml::array>();
        int idx = 0;
        for (auto& item : *pa) {
          if (!item.is_table()) {
            c.warnings.push_back(warn(c, 0, "panels[" + std::to_string(idx) +
                                                "] must be a table — ignored"));
            ++idx;
            continue;
          }
          const toml::table* pt = item.as<toml::table>();
          PanelEntry e;
          for (auto& [k, v] : *pt) {
            const std::string name = key_str(k);
            const int line = key_line(k);
            if (name == "id") {
              if (v.is_string()) e.id = v.as<std::string>()->get();
              else
                c.warnings.push_back(warn(
                    c, line, "panels[" + std::to_string(idx) +
                                 "].id must be a string — ignored"));
            } else if (name == "visible") {
              if (v.is_boolean()) e.visible = v.as<bool>()->get();
              else
                c.warnings.push_back(warn(
                    c, line, "panels[" + std::to_string(idx) +
                                 "].visible must be a boolean — ignored"));
            } else if (name == "title") {
              if (v.is_string()) e.title = v.as<std::string>()->get();
              else
                c.warnings.push_back(warn(
                    c, line, "panels[" + std::to_string(idx) +
                                 "].title must be a string — ignored"));
            } else if (name == "hidden_series") {
              if (v.is_array()) {
                for (auto& s : *v.as<toml::array>())
                  if (s.is_string())
                    e.hidden_series.push_back(s.as<std::string>()->get());
                  else
                    c.warnings.push_back(warn(
                        c, line, "panels[" + std::to_string(idx) +
                                     "].hidden_series must be an array of "
                                     "strings — entry ignored"));
              } else
                c.warnings.push_back(warn(
                    c, line, "panels[" + std::to_string(idx) +
                                 "].hidden_series must be an array — ignored"));
            } else {
              c.warnings.push_back(warn(
                  c, line, "unknown key 'panels[" + std::to_string(idx) + "]" +
                               "." + name + "' — ignored"));
            }
          }
          if (e.id.empty())
            c.warnings.push_back(warn(c, 0, "panels[" + std::to_string(idx) +
                                                "] has no id — ignored"));
          else
            c.panels.push_back(std::move(e));
          ++idx;
        }
      }
    }

    // [gpus."<bus>"] --------------------------------------------------------
    if (root.contains("gpus")) {
      const auto& gv = root.at("gpus");
      if (!gv.is_table()) {
        c.warnings.push_back(warn(c, 0, "'gpus' must be a table — ignored"));
      } else {
        const toml::table* gt = gv.as<toml::table>();
        for (auto& [bus, v] : *gt) {
          const std::string b = key_str(bus);
          if (!v.is_table()) {
            c.warnings.push_back(warn(c, key_line(bus),
                                      "gpus.\"" + b + "\" must be a table — "
                                       "ignored"));
            continue;
          }
          const toml::table* bt = v.as<toml::table>();
          GpuEntry ge;
          for (auto& [k, v2] : *bt) {
            const std::string name = key_str(k);
            const int line = key_line(k);
            if (name == "name") {
              if (v2.is_string()) ge.name = v2.as<std::string>()->get();
              else
                c.warnings.push_back(warn(
                    c, line, "gpus.\"" + b + "\".name must be a string — "
                                 "ignored"));
            } else if (name == "integrated") {
              if (v2.is_string() && v2.as<std::string>()->get() == "auto")
                ge.integrated = std::nullopt;
              else if (v2.is_boolean()) ge.integrated = v2.as<bool>()->get();
              else
                c.warnings.push_back(warn(
                    c, line, "gpus.\"" + b +
                                 "\".integrated must be \"auto\", true or "
                                 "false — ignored"));
            } else {
              c.warnings.push_back(warn(
                  c, line, "unknown key 'gpus.\"" + b + "\"." + name +
                               "' — ignored"));
            }
          }
          c.gpus[b] = ge;
        }
      }
    }
  } catch (const toml::parse_error& e) {
    c.loaded = false;
    const int line = e.source().begin.line;
    c.error = (line > 0 ? "line " + std::to_string(line) + ": " : "") +
              std::string(e.description());
  }
  return c;
}

std::string Config::serialize() const {
  std::string s;
  s += "# GETH configuration\n";
  s += "# Written by geth v" + std::string(GETH_VERSION) + ".\n";
  s += "# Edit freely; unknown keys are ignored with a warning on load.\n\n";
  s += "[general]\n";
  s += "interval_ms = " + std::to_string(interval_ms) + "\n";
  s += "window = \"" + window_to_string(window_ns) + "\"\n";
  if (columns < 0)
    s += "columns = \"auto\"\n";
  else
    s += "columns = " + std::to_string(columns) + "\n";
  s += "title = \"" + escape_toml(title) + "\"\n";
  s += "theme = \"" + escape_toml(theme) + "\"\n";
  s += "background = \"" + escape_toml(background) + "\"\n";
  s += "graph_style = \"" + escape_toml(graph_style) + "\"\n\n";
  for (const auto& e : panels) {
    s += "[[panels]]\n";
    s += "id = \"" + escape_toml(e.id) + "\"\n";
    s += "visible = " + std::string(e.visible ? "true" : "false") + "\n";
    s += "title = \"" + escape_toml(e.title) + "\"\n";
    if (!e.hidden_series.empty()) {
      s += "hidden_series = [";
      for (std::size_t i = 0; i < e.hidden_series.size(); ++i)
        s += (i ? ", " : "") + std::string("\"") + escape_toml(e.hidden_series[i]) +
             "\"";
      s += "]\n";
    }
    s += "\n";
  }
  for (const auto& [bus, g] : gpus) {
    if (g.name.empty() && !g.integrated) continue;
    s += "[gpus.\"" + escape_toml(bus) + "\"]\n";
    if (!g.name.empty()) s += "name = \"" + escape_toml(g.name) + "\"\n";
    s += "integrated = " +
         (g.integrated ? (g.integrated.value() ? "true" : "false")
                       : std::string("\"auto\"")) +
         "\n\n";
  }
  return s;
}

bool Config::save() const {
  if (no_save) return true;  // --no-save: read-only
  if (!loaded) return false;  // never overwrite a config we could not parse
  // Test-safety guard (v1.0.1): with GETH_TEST=1 (set by soaks, CI and the
  // dev harnesses) refuse to write anywhere under the user's REAL
  // ~/.config. A temporary XDG_CONFIG_HOME (mktemp -d) is always fine; the
  // default location (or an explicit path under ~/.config) is refused.
  if (const char* test = std::getenv("GETH_TEST"); test && *test) {
    if (const char* home = std::getenv("HOME"); home && *home) {
      std::error_code ec;
      const std::filesystem::path real = std::filesystem::weakly_canonical(
          std::string(home) + "/.config", ec);
      const std::filesystem::path here =
          std::filesystem::weakly_canonical(path, ec);
      const std::string real_str = real.string() + "/";
      if (!ec && !real_str.empty() &&
          here.string().compare(0, real_str.size(), real_str) == 0) {
        std::fprintf(stderr,
                     "geth: GETH_TEST=1: refusing to save config under your "
                     "real ~/.config (set XDG_CONFIG_HOME to a temp dir)\n");
        return false;
      }
    }
  }
  std::error_code ec;
  const std::filesystem::path p(path);
  if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << serialize();
    f.flush();
    if (!f) return false;
  }
  std::error_code ec2;
  std::filesystem::rename(tmp, path, ec2);
  if (ec2) {
    std::error_code ec3;
    std::filesystem::remove(tmp, ec3);
    return false;
  }
  return true;
}

void Config::sync_panels(const std::vector<PanelSpec>& page,
                         const std::vector<std::string>& order) {
  std::map<std::string, const PanelSpec*> by_id;
  for (const auto& p : page) by_id[p.id] = &p;
  std::map<std::string, const PanelEntry*> old;
  for (const auto& e : panels) old[e.id] = &e;

  std::vector<PanelEntry> out;
  out.reserve(order.size());
  for (const auto& id : order) {
    if (const auto it = by_id.find(id); it != by_id.end()) {
      PanelEntry e;
      e.id = id;
      e.visible = it->second->visible;
      e.title = it->second->title;
      for (const auto& sr : it->second->series)
        if (sr.hidden) e.hidden_series.push_back(sr.id);
      out.push_back(std::move(e));
    } else if (const auto it2 = old.find(id); it2 != old.end()) {
      out.push_back(*it2->second);  // absent hardware: saved settings kept
    }
  }
  // Safety net: old entries not covered by the order (unreachable via the
  // UI, which always syncs `order` with the file first).
  for (const auto& e : panels) {
    bool seen = false;
    for (const auto& o : out)
      if (o.id == e.id) {
        seen = true;
        break;
      }
    if (!seen) out.push_back(e);
  }
  panels = std::move(out);
}

std::vector<PanelSpec> merge_page(const std::vector<PanelSpec>& default_page,
                                  const Config& cfg) {
  std::map<std::string, std::size_t> def_idx;
  for (std::size_t i = 0; i < default_page.size(); ++i)
    def_idx[default_page[i].id] = i;

  // 1. Panels present in both, in the FILE's order, settings applied.
  std::vector<PanelSpec> out;
  std::set<std::string> used;
  for (const auto& e : cfg.panels) {
    auto it = def_idx.find(e.id);
    if (it == def_idx.end()) continue;  // absent hardware: file only
    PanelSpec p = default_page[it->second];
    p.visible = e.visible;
    if (!e.title.empty()) p.title = e.title;
    for (const auto& hs : e.hidden_series)
      for (auto& sr : p.series)
        if (sr.id == hs) sr.hidden = true;
    out.push_back(std::move(p));
    used.insert(e.id);
  }

  // 2. New hardware: inserted VISIBLE at its default position — right after
  //    the last default-predecessor that is a known panel (in the file
  //    order), or at the front.
  for (std::size_t d = 0; d < default_page.size(); ++d) {
    if (used.count(default_page[d].id)) continue;
    int insert_at = 0;
    for (std::size_t q = d; q-- > 0;) {
      if (!used.count(default_page[q].id)) continue;
      for (int o = 0; o < (int)out.size(); ++o)
        if (out[o].id == default_page[q].id) {
          insert_at = o + 1;
          break;
        }
      break;
    }
    out.insert(out.begin() + insert_at, default_page[d]);
    used.insert(default_page[d].id);
  }
  return out;
}

void apply_gpu_overrides(Hardware& hw, const Config& cfg) {
  for (auto& g : hw.gpus) {
    auto it = cfg.gpus.find(g.bus_id);
    if (it == cfg.gpus.end()) continue;
    if (!it->second.name.empty()) {
      g.name = it->second.name;
      g.renamed = true;  // the title logic uses it as-is, not shortened
    }
    if (it->second.integrated) g.is_integrated = *it->second.integrated;
  }
}

}  // namespace geth::ui
