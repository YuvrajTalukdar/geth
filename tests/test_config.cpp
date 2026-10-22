// Phase 4, Part B — the config file: load/serialize round-trip, unknown-key
// warnings (never errors), a broken file that is NEVER overwritten, the
// atomic save (tmp + rename, stale .tmp dropped on load), the merge rules
// (file order, hidden stays hidden, hidden_series, new hardware inserted
// visible, absent hardware dropped from the page), sync_panels, and the
// per-GPU overrides.

#include <doctest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "ui/config.hpp"
#include "ui/panel_spec.hpp"

using namespace geth;

namespace {

std::string tmp_dir() {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "geth_cfg_tests";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  return dir.string();
}
std::string tmp_path(const std::string& name) {
  const std::string p = tmp_dir() + "/" + name;
  // The temp dir persists between runs: start each test from a clean slate.
  // remove_all (not remove): tests may leave directory trees behind.
  std::error_code ec;
  std::filesystem::remove_all(p, ec);
  std::filesystem::remove(p + ".tmp", ec);
  return p;
}

void write_file(const std::string& path, const std::string& contents) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  f << contents;
}
std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
bool warned(const std::vector<std::string>& w, const std::string& sub) {
  for (const auto& s : w)
    if (s.find(sub) != std::string::npos) return true;
  return false;
}

// A minimal graph panel with the given series (default title "T_<id>").
ui::PanelSpec mk_panel(const std::string& id,
                       const std::vector<std::string>& series_ids) {
  ui::PanelSpec p;
  p.id = id;
  p.title = "T_" + id;
  for (const auto& s : series_ids) {
    ui::SeriesSpec sr;
    sr.id = s;
    sr.label = s;
    p.series.push_back(std::move(sr));
  }
  return p;
}
bool series_hidden(const ui::PanelSpec& p, const std::string& sid) {
  for (const auto& sr : p.series)
    if (sr.id == sid) return sr.hidden;
  return false;
}

}  // namespace

TEST_SUITE("config") {

TEST_CASE("validators: the allowed [general] values") {
  for (long long ms : {250, 500, 1000, 2000, 5000}) CHECK(ui::valid_interval_ms(ms));
  for (long long ms : {0, 1, 249, 251, 750, 1001, 3000, 5001})
    CHECK_FALSE(ui::valid_interval_ms(ms));

  for (int64_t ns : {30000000000LL, 60000000000LL, 120000000000LL,
                     300000000000LL, 600000000000LL})
    CHECK(ui::valid_window_ns(ns));
  for (int64_t ns : {0LL, 1LL, 31000000000LL, 90000000000LL, 700000000000LL})
    CHECK_FALSE(ui::valid_window_ns(ns));

  CHECK(ui::valid_columns(-1));
  for (int c : {1, 2, 3, 4, 5, 6, 7, 8}) CHECK(ui::valid_columns(c));
  for (int c : {0, 9, -2, 100}) CHECK_FALSE(ui::valid_columns(c));
}

TEST_CASE("window string <-> ns round-trip") {
  const std::pair<std::string, int64_t> cases[] = {
      {"30s", 30000000000LL},  {"1m", 60000000000LL},
      {"2m", 120000000000LL},  {"5m", 300000000000LL},
      {"10m", 600000000000LL},
  };
  for (const auto& [s, ns] : cases) {
    CHECK_EQ(ui::window_to_string(ns), s);
    int64_t out = 0;
    CHECK(ui::window_from_string(s, out));
    CHECK_EQ(out, ns);
  }
  int64_t out = 0;
  CHECK_FALSE(ui::window_from_string("99s", out));
  CHECK_FALSE(ui::window_from_string("2min", out));
  CHECK_FALSE(ui::window_from_string("", out));
  CHECK_EQ(ui::window_to_string(12345), "");  // not a preset
}

TEST_CASE("round-trip: serialize -> load preserves every field") {
  ui::Config c;
  c.interval_ms = 250;
  c.window_ns = 120000000000LL;  // 2m
  c.columns = 3;
  c.title = "My \"GETH\" \\ title";
  c.theme = "matrix-green";
  c.background = "solid";
  ui::PanelEntry p1;
  p1.id = "cpu";
  p1.visible = false;
  p1.title = "MyCPU";
  p1.hidden_series = {"cpu.usage"};
  ui::PanelEntry p2;
  p2.id = "mem";  // default title, visible, no hidden series
  c.panels = {p1, p2};
  ui::GpuEntry g;
  g.name = "RTX 3050";
  g.integrated = false;
  c.gpus["0000:01:00.0"] = g;

  const std::string path = tmp_path("roundtrip.toml");
  write_file(path, c.serialize());
  const ui::Config r = ui::Config::load(path, /*no_save=*/false);
  CHECK(r.loaded);
  CHECK(r.warnings.empty());
  CHECK_EQ(r.interval_ms, 250);
  CHECK_EQ(r.window_ns, 120000000000LL);
  CHECK_EQ(r.columns, 3);
  CHECK_EQ(r.title, "My \"GETH\" \\ title");
  CHECK_EQ(r.theme, "matrix-green");
  CHECK_EQ(r.background, "solid");
  REQUIRE_EQ((int)r.panels.size(), 2);
  CHECK_EQ(r.panels[0].id, "cpu");
  CHECK_FALSE(r.panels[0].visible);
  CHECK_EQ(r.panels[0].title, "MyCPU");
  REQUIRE_EQ((int)r.panels[0].hidden_series.size(), 1);
  CHECK_EQ(r.panels[0].hidden_series[0], "cpu.usage");
  CHECK_EQ(r.panels[1].id, "mem");
  CHECK(r.panels[1].visible);
  CHECK_EQ(r.panels[1].title, "");
  REQUIRE(r.gpus.count("0000:01:00.0"));
  CHECK_EQ(r.gpus.at("0000:01:00.0").name, "RTX 3050");
  REQUIRE(r.gpus.at("0000:01:00.0").integrated.has_value());
  CHECK_FALSE(*r.gpus.at("0000:01:00.0").integrated);
}

TEST_CASE("round-trip: [logging] dir and autostart") {
  ui::Config c;
  c.log_dir = "/var/tmp/geth-logs";
  c.log_autostart = true;

  const std::string path = tmp_path("logging.toml");
  write_file(path, c.serialize());
  const ui::Config r = ui::Config::load(path, /*no_save=*/false);
  CHECK(r.loaded);
  CHECK(r.warnings.empty());
  CHECK_EQ(r.log_dir, "/var/tmp/geth-logs");
  CHECK(r.log_autostart);

  // The defaults serialize too (an empty dir stays empty).
  ui::Config d;
  const std::string s = d.serialize();
  CHECK(s.find("[logging]") != std::string::npos);
  CHECK(s.find("dir = \"\"") != std::string::npos);
  CHECK(s.find("autostart = false") != std::string::npos);
}

TEST_CASE("logging: unknown / invalid keys warn (never error) and are ignored") {
  const std::string path = tmp_path("logging_unknown.toml");
  write_file(path,
             "[logging]\n"
             "dir = \"/tmp/ok\"\n"
             "foo = 1\n");  // unknown logging key -> warning
  const ui::Config c = ui::Config::load(path, false);
  CHECK(c.loaded);
  CHECK_EQ(c.log_dir, "/tmp/ok");
  CHECK_FALSE(c.log_autostart);
  CHECK(warned(c.warnings, "logging.foo"));

  const std::string path2 = tmp_path("logging_broken.toml");
  write_file(path2,
             "logging = 1\n");  // not a table -> warning, section ignored
  const ui::Config b = ui::Config::load(path2, false);
  CHECK(b.loaded);
  CHECK(b.log_dir.empty());
  CHECK(warned(b.warnings, "must be a table"));
}

TEST_CASE("round-trip: an empty title serializes to \"\" and a gpus auto is kept") {
  ui::Config c;
  ui::PanelEntry p;
  p.id = "cpu";
  p.title = "";
  c.panels = {p};
  ui::GpuEntry g;
  g.name = "Ryzen iGPU";
  g.integrated = std::nullopt;  // "auto"
  c.gpus["0000:07:00.0"] = g;

  const std::string path = tmp_path("roundtrip2.toml");
  write_file(path, c.serialize());
  const ui::Config r = ui::Config::load(path, false);
  CHECK(r.loaded);
  REQUIRE_EQ((int)r.panels.size(), 1);
  CHECK_EQ(r.panels[0].title, "");
  REQUIRE(r.gpus.count("0000:07:00.0"));
  CHECK_EQ(r.gpus.at("0000:07:00.0").name, "Ryzen iGPU");
  CHECK_FALSE(r.gpus.at("0000:07:00.0").integrated.has_value());  // back to auto
}

TEST_CASE("graph_renderer: auto/pixels/text/steps round-trip; invalid values warn") {
  for (const char* r : {"auto", "pixels", "text", "steps"}) {
    ui::Config c;
    c.graph_renderer = r;
    const std::string path = tmp_path(std::string("renderer_") + r + ".toml");
    write_file(path, c.serialize());
    const ui::Config l = ui::Config::load(path, /*no_save=*/false);
    CHECK(l.loaded);
    CHECK(l.warnings.empty());
    CHECK_EQ(l.graph_renderer, r);
  }
  // An invalid value keeps the current setting (the default "auto") + warns.
  const std::string path2 = tmp_path("renderer_bad.toml");
  write_file(path2,
             "[general]\n"
             "interval_ms = 500\n"
             "graph_renderer = \"dots\"\n");
  const ui::Config b = ui::Config::load(path2, false);
  CHECK(b.loaded);
  CHECK_EQ(b.interval_ms, 500);
  CHECK_EQ(b.graph_renderer, "auto");
  CHECK(warned(b.warnings, "general.graph_renderer"));
  // A non-string value warns too.
  const std::string path3 = tmp_path("renderer_nons.toml");
  write_file(path3, "[general]\ngraph_renderer = 42\n");
  const ui::Config n = ui::Config::load(path3, false);
  CHECK(n.loaded);
  CHECK_EQ(n.graph_renderer, "auto");
  CHECK(warned(n.warnings, "general.graph_renderer"));
}

TEST_CASE("graph_renderer: a legacy \"bars\" value migrates to \"steps\" (no warning)") {
  // A v1.2.0 file with graph_renderer = "bars" loads clean (the UI shows one
  // toast instead, via had_bars) and the value is migrated to "steps".
  const std::string path = tmp_path("migrate_bars.toml");
  write_file(path,
             "[general]\n"
             "interval_ms = 500\n"
             "graph_renderer = \"bars\"\n");
  const ui::Config c = ui::Config::load(path, false);
  CHECK(c.loaded);
  CHECK(c.warnings.empty());
  CHECK(c.had_bars);
  CHECK_EQ(c.interval_ms, 500);
  CHECK_EQ(c.graph_renderer, "steps");
  // The next save writes "steps" (never "bars").
  const std::string out = c.serialize();
  CHECK(out.find("graph_renderer = \"steps\"") != std::string::npos);
  CHECK(out.find("\"bars\"") == std::string::npos);
  // And a file written with "steps" loads back clean, without the flag.
  const std::string path2 = tmp_path("migrate_bars_saved.toml");
  write_file(path2, out);
  const ui::Config r = ui::Config::load(path2, false);
  CHECK(r.loaded);
  CHECK(r.warnings.empty());
  CHECK_FALSE(r.had_bars);
  CHECK_EQ(r.graph_renderer, "steps");
}

TEST_CASE("graph_style: the retired key loads clean, is ignored, dropped on save") {
  // A v1.0.3 file with graph_style = "braille" loads WITHOUT warnings (the
  // UI shows one toast instead, via had_graph_style) and keeps its other
  // settings; graph_renderer stays at its own value (the default "auto").
  const std::string path = tmp_path("migrate_braille.toml");
  write_file(path,
             "[general]\n"
             "interval_ms = 500\n"
             "window = \"2m\"\n"
             "graph_style = \"braille\"\n");
  ui::Config c = ui::Config::load(path, false);
  CHECK(c.loaded);
  CHECK(c.warnings.empty());
  CHECK(c.had_graph_style);
  CHECK_EQ(c.interval_ms, 500);
  CHECK_EQ(c.window_ns, 120000000000LL);
  CHECK_EQ(c.graph_renderer, "auto");
  // The next save DROPS the key (graph_renderer is written instead).
  c.sync_panels({}, {});
  CHECK(c.save());
  const std::string out = read_file(path);
  CHECK(out.find("graph_style") == std::string::npos);
  CHECK(out.find("graph_renderer") != std::string::npos);
  // And the rewritten file loads back clean, without the flag.
  const ui::Config r = ui::Config::load(path, false);
  CHECK(r.loaded);
  CHECK(r.warnings.empty());
  CHECK_FALSE(r.had_graph_style);
  CHECK_EQ(r.interval_ms, 500);

  // "blocks" is the other recognized retired value...
  const std::string p2 = tmp_path("migrate_blocks.toml");
  write_file(p2, "[general]\ngraph_style = \"blocks\"\n");
  const ui::Config cb = ui::Config::load(p2, false);
  CHECK(cb.loaded);
  CHECK(cb.had_graph_style);
  CHECK(cb.warnings.empty());
  // ...while an unrecognized value warns (a typo is not a known style).
  const std::string p3 = tmp_path("migrate_bad.toml");
  write_file(p3, "[general]\ngraph_style = \"dots\"\n");
  const ui::Config cd = ui::Config::load(p3, false);
  CHECK(cd.loaded);
  CHECK_FALSE(cd.had_graph_style);
  CHECK(warned(cd.warnings, "general.graph_style"));
}

TEST_CASE("unknown keys warn (never error) and are ignored") {
  const std::string path = tmp_path("unknown.toml");
  write_file(path,
             "bogus_root = 1\n"
             "[general]\n"
             "interval_ms = 500\n"
             "not_a_key = true\n"
             "window = \"99s\"\n"  // invalid value -> warning + default
             "[[panels]]\n"
             "id = \"cpu\"\n"
             "extra = 42\n"        // unknown panel key -> warning
             "visible = true\n"
             "[gpus.\"0000:01:00.0\"]\n"
             "name = \"x\"\n"
             "wat = 1\n");         // unknown gpus key -> warning
  const ui::Config c = ui::Config::load(path, false);
  CHECK(c.loaded);  // still parses
  CHECK_EQ(c.interval_ms, 500);             // the valid bit was applied
  CHECK_EQ(c.window_ns, 60000000000LL);     // the invalid window fell back
  REQUIRE_EQ((int)c.panels.size(), 1);
  CHECK_EQ(c.panels[0].id, "cpu");
  CHECK(c.panels[0].visible);
  // Every unknown / invalid key produced a warning.
  CHECK(warned(c.warnings, "bogus_root"));
  CHECK(warned(c.warnings, "general.not_a_key"));
  CHECK(warned(c.warnings, "general.window"));
  CHECK(warned(c.warnings, "panels[0].extra"));
  CHECK(warned(c.warnings, ".wat"));
}

TEST_CASE("a broken file loads as defaults and is NEVER overwritten") {
  const std::string path = tmp_path("broken.toml");
  const std::string broken = "[general]\ninterval_ms = \n";  // no value
  write_file(path, broken);
  const ui::Config c = ui::Config::load(path, false);
  CHECK_FALSE(c.loaded);
  CHECK(!c.error.empty());
  CHECK(c.exists);
  // Defaults are in force.
  CHECK_EQ(c.interval_ms, 1000);
  // save() refuses to touch a file it could not parse.
  CHECK_FALSE(c.save());
  // The file is byte-identical.
  CHECK_EQ(read_file(path), broken);
}

TEST_CASE("a missing file is a first run (defaults; save still allowed)") {
  const std::string path = tmp_path("does_not_exist.toml");
  ui::Config c = ui::Config::load(path, false);
  CHECK(c.loaded);
  CHECK_FALSE(c.exists);
  CHECK(c.warnings.empty());
  // At the Config level a first run can be written (the app only writes on a
  // change).
  c.interval_ms = 500;
  CHECK(c.save());
  CHECK(std::filesystem::exists(path));
  const ui::Config r = ui::Config::load(path, false);
  CHECK(r.loaded);
  CHECK_EQ(r.interval_ms, 500);
}

TEST_CASE("save is atomic (tmp + rename) and leaves no .tmp behind") {
  const std::string path = tmp_path("atomic.toml");
  ui::Config c;
  c.path = path;
  c.interval_ms = 2000;
  // A stale .tmp left by a "crashed" save must not survive.
  write_file(path + ".tmp", "garbage");
  CHECK(c.save());
  CHECK(std::filesystem::exists(path));
  CHECK_FALSE(std::filesystem::exists(path + ".tmp"));
  const ui::Config r = ui::Config::load(path, false);
  CHECK(r.loaded);
  CHECK_EQ(r.interval_ms, 2000);
}

TEST_CASE("GETH_TEST=1 refuses saves under the real ~/.config") {
  // Restore HOME / XDG_CONFIG_HOME / GETH_TEST exactly as found on scope exit,
  // so a mid-test failure still leaves the environment untouched.
  struct EnvGuard {
    std::string saved_home, saved_xdg, saved_test;
    bool had_home, had_xdg, had_test;
    EnvGuard() {
      const char* h = std::getenv("HOME");
      const char* x = std::getenv("XDG_CONFIG_HOME");
      const char* t = std::getenv("GETH_TEST");
      had_home = h != nullptr;
      had_xdg = x != nullptr;
      had_test = t != nullptr;
      saved_home = had_home ? h : "";
      saved_xdg = had_xdg ? x : "";
      saved_test = had_test ? t : "";
    }
    ~EnvGuard() {
      if (had_home) setenv("HOME", saved_home.c_str(), 1);
      else unsetenv("HOME");
      if (had_xdg) setenv("XDG_CONFIG_HOME", saved_xdg.c_str(), 1);
      else unsetenv("XDG_CONFIG_HOME");
      if (had_test) setenv("GETH_TEST", saved_test.c_str(), 1);
      else unsetenv("GETH_TEST");
    }
  } guard;

  // A fake HOME that looks like the user's real one: ~/.config exists.
  const std::string home = tmp_path("home");
  std::error_code ec;
  std::filesystem::create_directories(home + "/.config", ec);
  const std::string xdg = tmp_path("xdg");
  const std::string real = home + "/.config/geth/config.toml";

  // 1) GETH_TEST=1, no XDG_CONFIG_HOME -> default path IS under the real
  //    ~/.config -> save must be refused, nothing written.
  setenv("HOME", home.c_str(), 1);
  unsetenv("XDG_CONFIG_HOME");
  setenv("GETH_TEST", "1", 1);
  {
    ui::Config c;
    c.path = ui::Config::default_path();
    CHECK_EQ(c.path, real);
    CHECK_FALSE(c.save());
    CHECK_FALSE(std::filesystem::exists(c.path));
  }

  // 2) GETH_TEST=1 but XDG_CONFIG_HOME points at a temp dir -> allowed.
  setenv("XDG_CONFIG_HOME", xdg.c_str(), 1);
  {
    ui::Config c;
    c.path = ui::Config::default_path();
    CHECK_EQ(c.path, xdg + "/geth/config.toml");
    CHECK(c.save());
    CHECK(std::filesystem::exists(c.path));
  }

  // 3) GETH_TEST unset -> the guard is off; saving under the (fake) real
  //    ~/.config is allowed again.
  unsetenv("GETH_TEST");
  unsetenv("XDG_CONFIG_HOME");
  {
    ui::Config c;
    c.path = ui::Config::default_path();
    CHECK_EQ(c.path, real);
    CHECK(c.save());
    CHECK(std::filesystem::exists(c.path));
  }
}

TEST_CASE("load drops a stale .tmp and reads the main file") {
  const std::string path = tmp_path("stale_tmp.toml");
  ui::Config main;
  main.path = path;
  main.interval_ms = 2000;
  CHECK(main.save());
  // Simulate a crashed save that left a different .tmp behind.
  write_file(path + ".tmp", "interval = 250\n");
  const ui::Config r = ui::Config::load(path, false);
  CHECK(r.loaded);
  CHECK_EQ(r.interval_ms, 2000);  // the main file wins
  CHECK_FALSE(std::filesystem::exists(path + ".tmp"));  // the stale tmp dropped
}

TEST_CASE("merge: file order, hidden stays hidden, title, hidden_series") {
  std::vector<ui::PanelSpec> def = {
      mk_panel("a", {"a.s1", "a.s2"}),
      mk_panel("b", {"b.s1"}),
      mk_panel("c", {"c.s1"}),
  };
  ui::Config cfg;
  ui::PanelEntry ec;
  ec.id = "c";
  ec.visible = false;
  ec.title = "C*";
  ec.hidden_series = {"c.s1"};
  ui::PanelEntry ea;
  ea.id = "a";
  ui::PanelEntry eb;
  eb.id = "b";
  cfg.panels = {ec, ea, eb};  // file order: c, a, b

  const std::vector<ui::PanelSpec> page = ui::merge_page(def, cfg);
  REQUIRE_EQ((int)page.size(), 3);
  CHECK_EQ(page[0].id, "c");
  CHECK_FALSE(page[0].visible);
  CHECK_EQ(page[0].title, "C*");
  CHECK(series_hidden(page[0], "c.s1"));
  CHECK_EQ(page[1].id, "a");
  CHECK(page[1].visible);
  CHECK_EQ(page[1].title, "T_a");  // empty file title -> the default
  CHECK_FALSE(series_hidden(page[1], "a.s1"));
  CHECK_EQ(page[2].id, "b");
  CHECK(page[2].visible);
}

TEST_CASE("merge: absent-hardware entries are dropped from the page") {
  std::vector<ui::PanelSpec> def = {
      mk_panel("a", {}),
      mk_panel("b", {}),
  };
  ui::Config cfg;
  ui::PanelEntry ea;
  ea.id = "a";
  ui::PanelEntry ex;
  ex.id = "x";  // hardware that is gone
  ex.visible = false;
  ui::PanelEntry eb;
  eb.id = "b";
  cfg.panels = {ea, ex, eb};

  const std::vector<ui::PanelSpec> page = ui::merge_page(def, cfg);
  REQUIRE_EQ((int)page.size(), 2);  // x dropped
  CHECK_EQ(page[0].id, "a");
  CHECK_EQ(page[1].id, "b");
  for (const auto& p : page) CHECK(p.id != "x");
}

TEST_CASE("merge: new hardware is inserted VISIBLE at its default position") {
  std::vector<ui::PanelSpec> def = {
      mk_panel("a", {}),
      mk_panel("b", {}),  // not in the file -> new hardware
      mk_panel("c", {}),
  };
  ui::Config cfg;
  ui::PanelEntry ea;
  ea.id = "a";
  ui::PanelEntry ec;
  ec.id = "c";
  cfg.panels = {ea, ec};

  const std::vector<ui::PanelSpec> page = ui::merge_page(def, cfg);
  REQUIRE_EQ((int)page.size(), 3);
  // b is inserted right after a (its default predecessor).
  CHECK_EQ(page[0].id, "a");
  CHECK_EQ(page[1].id, "b");
  CHECK(page[1].visible);  // new hardware is visible
  CHECK_EQ(page[2].id, "c");
}

TEST_CASE("merge: an empty config keeps the default page order (no reversal)") {
  // Regression: before the fix, an all-new page (first run / empty file) was
  // built reversed.
  std::vector<ui::PanelSpec> def = {
      mk_panel("a", {}),
      mk_panel("b", {}),
      mk_panel("c", {}),
      mk_panel("d", {}),
  };
  const ui::Config cfg;  // no panels
  const std::vector<ui::PanelSpec> page = ui::merge_page(def, cfg);
  REQUIRE_EQ((int)page.size(), 4);
  CHECK_EQ(page[0].id, "a");
  CHECK_EQ(page[1].id, "b");
  CHECK_EQ(page[2].id, "c");
  CHECK_EQ(page[3].id, "d");
  for (const auto& p : page) CHECK(p.visible);
}

TEST_CASE("sync_panels: present panels take live settings in `order`, absent keep saved") {
  ui::Config cfg;
  ui::PanelEntry ea;
  ea.id = "a";
  ui::PanelEntry eb;
  eb.id = "b";
  ui::PanelEntry ex;
  ex.id = "x";
  ex.visible = false;
  ex.title = "X kept";
  cfg.panels = {ea, eb, ex};

  ui::PanelSpec pa = mk_panel("a", {"a.s1", "a.s2"});
  pa.visible = false;
  pa.title = "A2";
  pa.series[0].hidden = true;  // a.s1 hidden
  ui::PanelSpec pb = mk_panel("b", {"b.s1"});  // visible, default title
  std::vector<ui::PanelSpec> page = {pa, pb};
  const std::vector<std::string> order = {"b", "a", "x"};

  cfg.sync_panels(page, order);
  REQUIRE_EQ((int)cfg.panels.size(), 3);
  // b: from the live page, in its `order` position.
  CHECK_EQ(cfg.panels[0].id, "b");
  CHECK(cfg.panels[0].visible);
  CHECK_EQ(cfg.panels[0].title, "T_b");
  CHECK(cfg.panels[0].hidden_series.empty());
  // a: live settings (hidden, renamed, a.s1 hidden).
  CHECK_EQ(cfg.panels[1].id, "a");
  CHECK_FALSE(cfg.panels[1].visible);
  CHECK_EQ(cfg.panels[1].title, "A2");
  REQUIRE_EQ((int)cfg.panels[1].hidden_series.size(), 1);
  CHECK_EQ(cfg.panels[1].hidden_series[0], "a.s1");
  // x: absent hardware, the saved settings are kept verbatim.
  CHECK_EQ(cfg.panels[2].id, "x");
  CHECK_FALSE(cfg.panels[2].visible);
  CHECK_EQ(cfg.panels[2].title, "X kept");
}

TEST_CASE("apply_gpu_overrides: name and integrated are applied by bus id") {
  ui::Hardware hw;
  ui::GpuInfo g;
  g.bus_id = "0000:01:00.0";
  g.name = "Detected Name";
  g.is_integrated = true;
  hw.gpus.push_back(g);
  ui::GpuInfo g2;  // no override for this bus
  g2.bus_id = "0000:07:00.0";
  g2.name = "Other";
  g2.is_integrated = true;
  hw.gpus.push_back(g2);

  ui::Config cfg;
  ui::GpuEntry ge;
  ge.name = "Custom Name";
  ge.integrated = false;
  cfg.gpus["0000:01:00.0"] = ge;

  ui::apply_gpu_overrides(hw, cfg);
  CHECK_EQ(hw.gpus[0].name, "Custom Name");
  CHECK_FALSE(hw.gpus[0].is_integrated);
  // The second GPU is untouched.
  CHECK_EQ(hw.gpus[1].name, "Other");
  CHECK(hw.gpus[1].is_integrated);
}

}  // TEST_SUITE
