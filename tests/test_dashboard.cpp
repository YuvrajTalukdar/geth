// Phase 4, Part C — the Dashboard component driven by synthetic FTXUI events:
// focus (Tab wraps over the visible panels, x hides + advances, the last panel
// cannot be hidden), the panel-manager state machine (open/select/toggle/
// reorder/rename/series submenu/reset, absent-hardware rows), the settings
// cycles (+/- interval, t window, c columns), and the no-background guarantee
// with the manager and the help overlay open.

#include <doctest.h>

#include <memory>
#include <string>
#include <vector>

#include "core/collector.hpp"
#include "sources/demo.hpp"
#include "ui/app.hpp"
#include "ui/config.hpp"
#include "ui/frame.hpp"
#include "ui/panel_spec.hpp"
#include "ui/theme.hpp"

#include "ui_test_util.hpp"

using namespace geth;

namespace {

// A no-save config (the Dashboard state tests never touch disk).
ui::Config no_save_cfg() {
  ui::Config c;
  c.no_save = true;
  return c;
}

// A demo collector with a populated window, plus the Dashboard on top of it.
struct DashFixture {
  Collector c;
  ui::Hardware hw;
  std::vector<ui::PanelSpec> page;  // the default page, for reference
  ui::FrameStats stats;
  // A NAMED theme: the Dashboard borrows it (const Theme&), so it must outlive
  // every Dashboard this fixture makes (a temporary would dangle).
  ui::Theme theme = ui::Theme::cyberpunk_blue();

  DashFixture() {
    c.add(std::make_unique<DemoSource>());
    c.probe_all(/*warm_up=*/true);
    for (int i = 0; i < 70; ++i) c.sample();  // enough points for a full line
    hw = ui::hardware_from(c.latest(), "AMD Ryzen 5 5600H with Radeon Graphics");
    page = ui::build_default_page(hw);
  }

  std::unique_ptr<ui::Dashboard> make(ui::Config cfg, long long interval_ms = 1000,
                                      ui::ColorMode mode = ui::ColorMode::Color256) {
    return std::make_unique<ui::Dashboard>(
        c, hw, std::move(cfg), theme, mode, interval_ms,
        /*count=*/0, /*window_idx=*/1, /*columns=*/-1, stats);
  }

  // Opens the manager and moves the selection to `target` (bounded).
  void select_row(ui::Dashboard& d, int target) {
    for (int i = 0; i < (int)d.manager_rows().size(); ++i) {
      if (d.manager_selected() == target) return;
      d.OnEvent(ftxui::Event::ArrowDown);
    }
  }
};

}  // namespace

TEST_SUITE("dashboard") {

TEST_CASE("focus: Tab walks the visible panels and wraps both ways") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());
  const int n = (int)d->page().size();
  REQUIRE(n >= 2);
  CHECK_EQ(d->focus_index(), -1);  // nothing focused yet
  d->OnEvent(ftxui::Event::Tab);
  CHECK_EQ(d->focus_index(), 0);   // first Tab selects the first panel
  for (int i = 1; i < n; ++i) {
    d->OnEvent(ftxui::Event::Tab);
    CHECK_EQ(d->focus_index(), i);
  }
  d->OnEvent(ftxui::Event::Tab);  // one more wraps to the front
  CHECK_EQ(d->focus_index(), 0);
  d->OnEvent(ftxui::Event::TabReverse);  // Shift-Tab from the front -> last
  CHECK_EQ(d->focus_index(), n - 1);
}

TEST_CASE("focus: Tab walks only the visible panels (skips hidden)") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());
  const int n = (int)d->page().size();
  REQUIRE(n >= 3);
  d->OnEvent(ftxui::Event::Tab);             // focus 0
  d->OnEvent(ftxui::Event::Character('x'));  // hide 0 -> focus 1
  d->OnEvent(ftxui::Event::Character('x'));  // hide 1 -> focus 2
  // Panels 0 and 1 are now hidden; Tab must never land on them.
  for (int i = 0; i < n; ++i) {
    d->OnEvent(ftxui::Event::Tab);
    CHECK(d->focus_index() >= 2);
  }
}

TEST_CASE("focus: x hides the focused panel and advances to the next") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());
  const int n = (int)d->page().size();
  REQUIRE(n >= 3);
  d->OnEvent(ftxui::Event::Tab);
  CHECK_EQ(d->focus_index(), 0);
  const std::string id0 = d->page()[0].id;
  d->OnEvent(ftxui::Event::Character('x'));  // hide it
  CHECK_FALSE(d->panel_visible(id0));
  CHECK_FALSE(d->page()[0].visible);
  CHECK_EQ(d->focus_index(), 1);  // advanced to the next visible panel
}

TEST_CASE("focus: the last visible panel cannot be hidden (guard + toast)") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());
  const int n = (int)d->page().size();
  REQUIRE(n >= 2);
  d->OnEvent(ftxui::Event::Tab);  // focus 0
  for (int i = 0; i < n - 1; ++i)
    d->OnEvent(ftxui::Event::Character('x'));  // hide down to one
  int vis = 0, vis_idx = -1;
  for (int i = 0; i < n; ++i)
    if (d->page()[i].visible) {
      ++vis;
      vis_idx = i;
    }
  CHECK_EQ(vis, 1);
  CHECK_EQ(d->focus_index(), vis_idx);
  // The final x is refused.
  d->OnEvent(ftxui::Event::Character('x'));
  CHECK(d->page()[vis_idx].visible);
  CHECK(d->toast().find("at least one panel") != std::string::npos);
}

TEST_CASE("manager: e opens, rows mirror the page, space toggles visibility") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());
  CHECK_FALSE(d->manager_open());
  d->OnEvent(ftxui::Event::Character('e'));
  CHECK(d->manager_open());
  const auto& rows = d->manager_rows();
  REQUIRE_EQ((int)rows.size(), (int)d->page().size());
  for (int i = 0; i < (int)rows.size(); ++i) {
    CHECK_EQ(rows[i].id, d->page()[i].id);
    CHECK(rows[i].present);
    CHECK(rows[i].visible);
  }
  CHECK_EQ(d->manager_selected(), 0);  // no focus -> row 0
  // Toggle row 0 off, then back on.
  d->OnEvent(ftxui::Event::Character(' '));
  CHECK_FALSE(d->manager_rows()[0].visible);
  CHECK_FALSE(d->panel_visible(d->page()[0].id));
  d->OnEvent(ftxui::Event::Character(' '));
  CHECK(d->manager_rows()[0].visible);
  CHECK(d->panel_visible(d->page()[0].id));
  // Close with Esc.
  d->OnEvent(ftxui::Event::Escape);
  CHECK_FALSE(d->manager_open());
}

TEST_CASE("manager: down/kj move the selection; J/K reorder the page + order") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());
  d->OnEvent(ftxui::Event::Character('e'));
  const auto before = d->order();
  REQUIRE(before.size() >= 2);
  // Selection moves down and up (k).
  d->OnEvent(ftxui::Event::ArrowDown);
  CHECK_EQ(d->manager_selected(), 1);
  d->OnEvent(ftxui::Event::Character('k'));
  CHECK_EQ(d->manager_selected(), 0);
  // J moves row 0 down to 1: the order and the page both follow.
  d->OnEvent(ftxui::Event::Character('J'));
  CHECK_EQ(d->order()[0], before[1]);
  CHECK_EQ(d->order()[1], before[0]);
  CHECK_EQ(d->page()[0].id, before[1]);
  CHECK_EQ(d->page()[1].id, before[0]);
  CHECK_EQ(d->manager_selected(), 1);  // the selection follows the moved row
  // K moves it back.
  d->OnEvent(ftxui::Event::Character('K'));
  CHECK_EQ(d->order()[0], before[0]);
  CHECK_EQ(d->order()[1], before[1]);
  CHECK_EQ(d->manager_selected(), 0);
}

TEST_CASE("manager: r renames, Enter commits, Esc cancels (manager stays open)") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());
  d->OnEvent(ftxui::Event::Character('e'));  // sel 0
  const std::string orig = d->page()[0].title;
  // Clear the pre-filled buffer, type a new title, commit.
  d->OnEvent(ftxui::Event::Character('r'));
  const std::size_t len = d->manager_rows()[0].label.size();
  for (std::size_t i = 0; i < len + 4; ++i) d->OnEvent(ftxui::Event::Backspace);
  for (char ch : "My CPU") d->OnEvent(ftxui::Event::Character(ch));
  d->OnEvent(ftxui::Event::Return);
  CHECK_EQ(d->page()[0].title, "My CPU");
  CHECK_EQ(d->manager_rows()[0].label, "My CPU");
  CHECK(d->manager_open());
  // Esc cancels a rename without changing the title or closing the manager.
  d->OnEvent(ftxui::Event::Character('r'));
  for (char ch : "XXX") d->OnEvent(ftxui::Event::Character(ch));
  d->OnEvent(ftxui::Event::Escape);
  CHECK_EQ(d->page()[0].title, "My CPU");  // unchanged
  CHECK(d->manager_open());
  CHECK_NE(orig, "My CPU");  // (sanity: the default was not "My CPU")
}

TEST_CASE("manager: Enter opens the series submenu; space toggles; Esc closes only it") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());
  d->OnEvent(ftxui::Event::Character('e'));  // sel 0
  const int pi = 0;
  const auto& spec = d->page()[pi];
  REQUIRE(spec.series.size() >= 1);
  const bool was_hidden = spec.series[0].hidden;
  d->OnEvent(ftxui::Event::Return);  // open the submenu
  d->OnEvent(ftxui::Event::Character(' '));  // toggle series 0
  CHECK_EQ(d->page()[pi].series[0].hidden, !was_hidden);
  CHECK(d->page()[pi].visible);  // the PANEL was not toggled (submenu, not row)
  // Esc closes the submenu; the manager stays open.
  d->OnEvent(ftxui::Event::Escape);
  CHECK(d->manager_open());
  // A second Esc closes the manager.
  d->OnEvent(ftxui::Event::Escape);
  CHECK_FALSE(d->manager_open());
}

TEST_CASE("manager: d confirms a reset that restores the default page") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());
  d->OnEvent(ftxui::Event::Character('e'));  // sel 0
  // Rename panel 0 and hide it, so there is something to reset.
  d->OnEvent(ftxui::Event::Character('r'));
  const std::size_t len = d->manager_rows()[0].label.size();
  for (std::size_t i = 0; i < len + 4; ++i) d->OnEvent(ftxui::Event::Backspace);
  d->OnEvent(ftxui::Event::Character('Z'));
  d->OnEvent(ftxui::Event::Return);
  d->OnEvent(ftxui::Event::Character(' '));  // toggle panel 0 off
  CHECK_FALSE(d->page()[0].visible);
  CHECK_EQ(d->page()[0].title, "Z");
  // Request a reset; decline (n cancels).
  d->OnEvent(ftxui::Event::Character('d'));
  d->OnEvent(ftxui::Event::Character('n'));
  CHECK_FALSE(d->page()[0].visible);  // unchanged
  // Request again; accept (y).
  d->OnEvent(ftxui::Event::Character('d'));
  d->OnEvent(ftxui::Event::Character('y'));
  for (int i = 0; i < (int)d->page().size(); ++i)
    CHECK(d->page()[i].visible);
  CHECK_EQ(d->page()[0].title, fx.page[0].title);  // the default title again
}

TEST_CASE("manager: absent-hardware panels show, are not on the page, toggle saved") {
  DashFixture fx;
  ui::Config cfg = no_save_cfg();
  ui::PanelEntry gone;
  gone.id = "gpu.9999:99:99.9.usage";
  gone.title = "Gone GPU";
  cfg.panels.push_back(gone);
  auto d = fx.make(cfg);
  d->OnEvent(ftxui::Event::Character('e'));
  CHECK(d->manager_open());
  // The absent panel is in the rows, marked not-present, with its title.
  int idx = -1;
  for (int i = 0; i < (int)d->manager_rows().size(); ++i)
    if (d->manager_rows()[i].id == "gpu.9999:99:99.9.usage") idx = i;
  REQUIRE(idx >= 0);
  CHECK_FALSE(d->manager_rows()[idx].present);
  CHECK_EQ(d->manager_rows()[idx].label, "Gone GPU");
  // It is NOT on the live page (the hardware is absent).
  CHECK_EQ(d->page_index("gpu.9999:99:99.9.usage"), -1);
  // Selecting + toggling it flips the SAVED setting, not the page.
  fx.select_row(*d, idx);
  CHECK_EQ(d->manager_selected(), idx);
  CHECK(d->manager_rows()[idx].visible);
  d->OnEvent(ftxui::Event::Character(' '));
  CHECK_FALSE(d->manager_rows()[idx].visible);
  d->OnEvent(ftxui::Event::Character(' '));
  CHECK(d->manager_rows()[idx].visible);
}

TEST_CASE("settings: +/- cycle the interval (faster/slower, with wrap)") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg(), /*interval_ms=*/1000);
  CHECK_EQ(d->interval_ms(), 1000);
  d->OnEvent(ftxui::Event::Character('+'));  // faster
  CHECK_EQ(d->interval_ms(), 500);
  d->OnEvent(ftxui::Event::Character('='));  // faster (alias)
  CHECK_EQ(d->interval_ms(), 250);
  d->OnEvent(ftxui::Event::Character('+'));  // wraps to the slowest
  CHECK_EQ(d->interval_ms(), 5000);
  d->OnEvent(ftxui::Event::Character('-'));  // slower, wraps to the fastest
  CHECK_EQ(d->interval_ms(), 250);
  d->OnEvent(ftxui::Event::Character('-'));  // slower
  CHECK_EQ(d->interval_ms(), 500);
  d->OnEvent(ftxui::Event::Character('_'));  // slower (alias)
  CHECK_EQ(d->interval_ms(), 1000);
}

TEST_CASE("settings: t cycles the graph window (30s .. 10m, with wrap)") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());  // window_idx 1 (1 m)
  CHECK_EQ(d->window_idx(), 1);
  d->OnEvent(ftxui::Event::Character('t'));
  CHECK_EQ(d->window_idx(), 2);
  d->OnEvent(ftxui::Event::Character('t'));
  CHECK_EQ(d->window_idx(), 3);
  d->OnEvent(ftxui::Event::Character('t'));
  CHECK_EQ(d->window_idx(), 4);
  d->OnEvent(ftxui::Event::Character('t'));  // wraps
  CHECK_EQ(d->window_idx(), 0);
  d->OnEvent(ftxui::Event::Character('t'));
  CHECK_EQ(d->window_idx(), 1);
}

TEST_CASE("settings: c cycles the columns (auto -> 1..8 -> auto)") {
  DashFixture fx;
  auto d = fx.make(no_save_cfg());  // columns auto (-1)
  CHECK_EQ(d->columns_setting(), -1);
  d->OnEvent(ftxui::Event::Character('c'));
  CHECK_EQ(d->columns_setting(), 1);
  for (int want = 2; want <= 8; ++want) {
    d->OnEvent(ftxui::Event::Character('c'));
    CHECK_EQ(d->columns_setting(), want);
  }
  d->OnEvent(ftxui::Event::Character('c'));  // wraps to auto
  CHECK_EQ(d->columns_setting(), -1);
}

TEST_CASE("no background color is ever emitted, manager + help open (160x48, all modes)") {
  DashFixture fx;
  const ui::ColorMode modes[] = {ui::ColorMode::TrueColor, ui::ColorMode::Color256,
                                 ui::ColorMode::Color16};
  for (auto mode : modes) {
    {  // the manager open
      auto d = fx.make(no_save_cfg(), 1000, mode);
      d->set_size(160, 48);
      d->OnEvent(ftxui::Event::Character('e'));
      CHECK(d->manager_open());
      ftxui::Screen screen(160, 48);
      screen.Clear();
      auto el = d->OnRender();
      ftxui::Render(screen, el);
      const std::string out = screen.ToString();
      CHECK(!out.empty());
      CHECK(out.find('\xe2') != std::string::npos);  // box/braille glyphs
      CHECK(!has_forbidden_style(out));
    }
    {  // the help open
      auto d = fx.make(no_save_cfg(), 1000, mode);
      d->set_size(160, 48);
      d->OnEvent(ftxui::Event::Character('?'));
      CHECK(d->help_open());
      ftxui::Screen screen(160, 48);
      screen.Clear();
      auto el = d->OnRender();
      ftxui::Render(screen, el);
      const std::string out = screen.ToString();
      CHECK(!out.empty());
      CHECK(!has_forbidden_style(out));
    }
  }
}

}  // TEST_SUITE
