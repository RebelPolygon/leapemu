#include "gui/romwindow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "core/assets.h"
#include "core/celp.h"
#include "core/soundtrack.h"
#include "imgui.h"

namespace fs = std::filesystem;

namespace leap {

namespace {

std::string size_text(u64 n) {
  char b[32];
  if (n >= (1u << 20)) std::snprintf(b, sizeof(b), "%.1f MiB", n / 1048576.0);
  else if (n >= 1024) std::snprintf(b, sizeof(b), "%.1f KiB", n / 1024.0);
  else std::snprintf(b, sizeof(b), "%llu B", static_cast<unsigned long long>(n));
  return b;
}

std::string hex32(u32 v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%08X", v);
  return b;
}

bool parse_hex_bytes(const std::string& s, std::vector<u8>* out) {
  out->clear();
  std::string digits;
  for (char c : s)
    if (std::isxdigit(u8(c))) digits.push_back(c);
    else if (c != ' ' && c != ',') return false;
  if (digits.empty() || digits.size() % 2) return false;
  for (size_t i = 0; i < digits.size(); i += 2) out->push_back(u8(std::stoul(digits.substr(i, 2), nullptr, 16)));
  return true;
}

const ImVec4 kActiveEdit(1.00f, 0.62f, 0.25f, 1), kOtherEdit(0.45f, 0.75f, 1.00f, 1), kDim(0.55f, 0.55f, 0.55f, 1);

}  // namespace

fs::path patch_file_path(const fs::path& dir, u32 cart_crc) {
  char b[32];
  std::snprintf(b, sizeof(b), "%08x.txt", cart_crc);
  return dir / b;
}

RomWindow::RomWindow(GameEntry entry, RomWindowHost* host, Tab tab) : e_(std::move(entry)), host_(host), want_tab_(tab) {
  title_ = e_.name + "##rom" + e_.path;
  load_patches();
}

RomWindow::~RomWindow() {
  if (playing_ != SIZE_MAX && host_->stop_audio) host_->stop_audio();
  if (preview_tex_ && host_->free_texture) host_->free_texture(preview_tex_);
  export_stop_ = true;
  if (export_thread_.joinable()) export_thread_.join();
}

bool RomWindow::load_image() {
  if (image_tried_) return !img_.empty();
  image_tried_ = true;
  if (!load_rom_file(e_.path, &img_, &image_err_)) { img_.clear(); return false; }
  blocks_joined_ = join_rom_blocks(&img_);
  header_ = parse_rom_header(img_);
  contents_ = list_rom_contents(img_);
  by_offset_ = contents_.assets;
  std::sort(by_offset_.begin(), by_offset_.end(), [](const RomAsset& a, const RomAsset& b) { return a.offset < b.offset; });
  load_patches();  // (now that the original bytes are known)
  return true;
}

// Patches are kept per byte while editing; the file keeps runs.
void RomWindow::load_patches() {
  patches_.clear();
  patch_err_.clear();
  const fs::path file = patch_file_path(host_->patch_dir, e_.crc);
  std::error_code ec;
  if (!fs::exists(file, ec)) return;
  PatchFile pf;
  if (!pf.load(file.string(), &patch_err_)) return;
  for (const RomPatch& p : pf.patches) {
    UiPatch u;
    u.name = p.name;
    u.enabled = p.enabled;
    for (const RomEdit& e : p.edits) {
      const bool fits = !img_.empty() && size_t(e.offset) + e.from.size() <= img_.size() &&
                        std::memcmp(&img_[e.offset], e.from.data(), e.from.size()) == 0;
      if (!fits) { u.foreign.push_back(e); continue; }
      for (size_t i = 0; i < e.to.size(); i++)
        if (e.to[i] != e.from[i]) u.bytes[u32(e.offset + i)] = e.to[i];
    }
    patches_.push_back(std::move(u));
  }
  if (active_ >= int(patches_.size())) active_ = -1;
  if (active_ < 0 && !patches_.empty()) active_ = 0;
  rebuild_overlay();
}

void RomWindow::save_patches() {
  PatchFile pf;
  pf.cart_crc = e_.crc;
  pf.cart_title = e_.name;
  for (const UiPatch& u : patches_) {
    RomPatch p;
    p.name = u.name;
    p.enabled = u.enabled;
    for (auto it = u.bytes.begin(); it != u.bytes.end();) {  // runs of consecutive bytes
      RomEdit e;
      e.offset = it->first;
      u32 next = it->first;
      for (; it != u.bytes.end() && it->first == next; ++it, ++next) {
        e.from.push_back(img_[it->first]);
        e.to.push_back(it->second);
      }
      p.edits.push_back(std::move(e));
    }
    p.edits.insert(p.edits.end(), u.foreign.begin(), u.foreign.end());
    pf.patches.push_back(std::move(p));
  }
  std::error_code ec;
  fs::create_directories(host_->patch_dir, ec);
  const fs::path file = patch_file_path(host_->patch_dir, e_.crc);
  if (pf.patches.empty()) fs::remove(file, ec);
  else if (!pf.save(file.string(), &patch_err_)) return;
  patch_err_.clear();
  if (host_->running(e_.crc)) dirty_since_start_ = true;
}

// What the hex view shows: the enabled patches, then the one being edited.
void RomWindow::rebuild_overlay() {
  overlay_.clear();
  for (int i = 0; i < int(patches_.size()); i++)
    if (patches_[size_t(i)].enabled && i != active_)
      for (const auto& [off, v] : patches_[size_t(i)].bytes) overlay_[off] = {v, i};
  if (active_ >= 0)
    for (const auto& [off, v] : patches_[size_t(active_)].bytes) overlay_[off] = {v, active_};
}

u8 RomWindow::shown(u32 off) const {
  auto it = overlay_.find(off);
  return it != overlay_.end() ? it->second.first : img_[off];
}

const RomAsset* RomWindow::asset_at(u32 off) const {
  auto it = std::upper_bound(by_offset_.begin(), by_offset_.end(), off, [](u32 o, const RomAsset& a) { return o < a.offset; });
  if (it == by_offset_.begin()) return nullptr;
  --it;
  return off < it->offset + it->size ? &*it : nullptr;
}

void RomWindow::set_byte(u32 off, u8 v) {
  if (active_ < 0) {
    patches_.push_back({"Patch " + std::to_string(patches_.size() + 1), true, {}, {}});
    active_ = int(patches_.size()) - 1;
  }
  UiPatch& p = patches_[size_t(active_)];
  if (v == img_[off]) p.bytes.erase(off);
  else p.bytes[off] = v;
  rebuild_overlay();
  save_patches();
}

void RomWindow::revert_byte(u32 off) {
  if (active_ < 0) return;
  if (patches_[size_t(active_)].bytes.erase(off)) {
    rebuild_overlay();
    save_patches();
  }
}

void RomWindow::goto_offset(u32 off) {
  if (img_.empty()) return;
  cursor_ = std::min<u32>(off, u32(img_.size() - 1));
  low_nibble_ = false;
  scroll_to_cursor_ = true;
}

// Finds hex bytes ("4a 26") or, in quotes, text ("\"Sonic") in what the view
// shows, after the cursor.
void RomWindow::search(bool from_start) {
  std::string q = search_buf_;
  std::vector<u8> needle;
  if (q.size() >= 2 && q.front() == '"') {
    q = q.substr(1, q.back() == '"' ? q.size() - 2 : std::string::npos);
    needle.assign(q.begin(), q.end());
  } else if (!parse_hex_bytes(q, &needle)) {
    search_msg_ = "Hex bytes, or text in quotes";
    return;
  }
  if (needle.empty()) return;
  std::vector<u8> view = img_;
  for (const auto& [off, v] : overlay_) view[off] = v.first;
  const size_t start = from_start ? 0 : size_t(cursor_) + 1;
  auto it = std::search(view.begin() + std::ptrdiff_t(std::min(start, view.size())), view.end(), needle.begin(), needle.end());
  if (it == view.end() && !from_start) it = std::search(view.begin(), view.end(), needle.begin(), needle.end());
  if (it == view.end()) { search_msg_ = "Not found"; return; }
  search_msg_.clear();
  goto_offset(u32(it - view.begin()));
}

bool RomWindow::draw() {
  if (focus_) {
    ImGui::SetNextWindowFocus();
    focus_ = false;
  }
  // (Sized to fit the main window: at 4x it is 640 pixels wide.)
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8, vp->WorkPos.y + 28), ImGuiCond_Appearing);
  ImGui::SetNextWindowSize(ImVec2(std::min(800.0f, vp->WorkSize.x - 16), std::min(600.0f, vp->WorkSize.y - 56)), ImGuiCond_Appearing);
  ImGui::SetNextWindowBgAlpha(1.0f);  // (opaque: the game runs behind it)
  if (!ImGui::Begin(title_.c_str(), &open_, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
    focused_ = false;
    ImGui::End();
    return open_;
  }
  focused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
  const Tab want = want_tab_;  // (a tab asked for while drawing opens next frame)
  want_tab_ = Tab::None;
  auto flags = [&](Tab t) { return want == t ? ImGuiTabItemFlags_SetSelected : 0; };
  if (ImGui::BeginTabBar("##tabs")) {
    if (ImGui::BeginTabItem("Info", nullptr, flags(Tab::Info))) { draw_info(); ImGui::EndTabItem(); }
    bool contents = false;
    if (ImGui::BeginTabItem("Contents", nullptr, flags(Tab::Contents))) { contents = true; draw_contents(); ImGui::EndTabItem(); }
    if (!contents && playing_ != SIZE_MAX) {  // (listening belongs to the Contents tab)
      host_->stop_audio();
      playing_ = SIZE_MAX;
    }
    if (ImGui::BeginTabItem("Hex & Patches", nullptr, flags(Tab::Hex))) { draw_hex(); ImGui::EndTabItem(); }
    ImGui::EndTabBar();
  }
  ImGui::End();
  if (previewing_ != SIZE_MAX) draw_preview();
  return open_;
}

void RomWindow::draw_info() {
  GameEntry& g = e_;
  auto section = [](const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("", 0, 1.0f);
    ImGui::TableSetupColumn("", 0, 2.6f);
    return true;
  };
  auto label = [](const char* k) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", k);
    ImGui::TableSetColumnIndex(1);
  };
  auto row = [&](const char* k, const std::string& v) {
    if (v.empty()) return;
    label(k);
    ImGui::TextWrapped("%s", v.c_str());
  };
  ImGui::SeparatorText("Game");
  if (section("##game")) {
    row("Title", g.name);
    row("Region", g.region);
    row("Type", g.type + (g.download ? " (a Leapster 2 downloadable, runs from RAM)" : ""));
    label("Compatibility");
    {
      // Yours, or leapemu's (from its game database).
      static const char* kTiers[] = {"In-game", "Menus", "Intro", "Broken", "Untested"};
      const std::string leapemu = (g.in_database ? "leapemu's: " : "leapemu's (not in its database): ") + g.database_compatibility;
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16);
      if (ImGui::BeginCombo("##rating", g.own_compatibility ? (g.compatibility + "  (your rating)").c_str() : leapemu.c_str())) {
        if (ImGui::Selectable(leapemu.c_str(), !g.own_compatibility)) host_->set_rating(g, "");
        ImGui::Separator();
        for (const char* t : kTiers)
          if (ImGui::Selectable(t, g.own_compatibility && g.compatibility == t)) host_->set_rating(g, t);
        ImGui::EndCombo();
      }
    }
    row("Notes", g.notes);
    // What leapemu does by itself for this game (docs/compatibility.md, "Fixes
    // leapemu applies by itself").
    std::string fixes;
    auto fix = [&](const std::string& text) { fixes += (fixes.empty() ? "" : "\n") + text; };
    if (g.needs_unsigned) fix("Skips the BaseROM's content checks (the dump has damaged bytes).");
    if (load_image() && blocks_joined_) fix("A development upload: its block header is removed on loading.");
    if (g.download) fix("Loaded into RAM as on a Leapster 2, without the cartridge checks.");
    if (g.stub_services) fix("Leapster 2 services (files, the current program), supplied by leapemu; its save file is kept in its .sav.");
    row("Compatibility fixes", fixes);
    ImGui::EndTable();
  }
  ImGui::SeparatorText("File");
  if (section("##file")) {
    row("File", g.path);
    row("Size", size_text(g.size) + " (" + std::to_string(g.size) + " bytes, " + g.format + ")");
    row("CRC-32", hex32(g.crc));
    if (host_->save_path) {
      if (ImGui::GetTime() - save_path_at_ > 1.0) { save_path_ = host_->save_path(g.path); save_path_at_ = ImGui::GetTime(); }
      const std::string& sp = save_path_;
      std::error_code ec;
      const auto n = fs::file_size(sp, ec);
      row("Save file", sp + (ec ? "  (none yet)" : "  (" + size_text(n) + ")"));
    }
    unsigned on = 0;
    for (const UiPatch& p : patches_) on += p.enabled;
    if (!patches_.empty())
      row("Patches", std::to_string(patches_.size()) + (patches_.size() == 1 ? " patch, " : " patches, ") + std::to_string(on) + " on");
    ImGui::EndTable();
  }
  ImGui::SeparatorText("ROM header");
  if (section("##header")) {
    row("Internal title", g.internal_title);
    row("Part number", g.part_number);
    row("Version", g.version);
    row("Built", g.build_date);
    ImGui::EndTable();
  }
  ImGui::Spacing();
  const bool running = host_->running(g.crc);
  if (ImGui::Button(running ? "Restart" : "Play")) {
    if (running) host_->restart();
    else host_->play(g.path, g.needs_unsigned);
  }
  ImGui::SameLine();
  if (ImGui::Button("Open Containing Folder")) host_->open_folder(fs::path(g.path).parent_path().string());
}

void RomWindow::draw_contents() {
  if (!load_image()) { ImGui::TextDisabled("Cannot read the image: %s", image_err_.c_str()); return; }
  const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
  if (ImGui::CollapsingHeader("Header", ImGuiTreeNodeFlags_DefaultOpen) && ImGui::BeginTable("##hdr", 2, tf)) {
    auto row = [](const char* k, const std::string& v) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextDisabled("%s", k);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(v.c_str());
    };
    row("Mapped at", hex32(header_.device_start) + "-" + hex32(header_.device_end));
    row("Resource index", hex32(header_.rib_table));
    row("Full checksum at", hex32(contents_.full_checksum));
    row("Sparse checksum at", hex32(contents_.sparse_checksum));
    if (contents_.product_id) row("Product id", hex32(contents_.product_id));
    if (contents_.rom_version) row("ROM version", std::to_string(contents_.rom_version));
    if (!contents_.build_tool.empty()) row("Build tool", contents_.build_tool);
    if (blocks_joined_)
      row("Image", "a development upload: " + std::to_string(blocks_joined_) + " block header" + (blocks_joined_ == 1 ? "" : "s") +
                       " removed (offsets are without " + (blocks_joined_ == 1 ? "it" : "them") + ")");
    ImGui::EndTable();
  }
  if (ImGui::CollapsingHeader("Resource groups") && ImGui::BeginTable("##groups", 4, tf)) {
    ImGui::TableSetupColumn("Id", 0, 0.6f);
    ImGui::TableSetupColumn("Group", 0, 1.6f);
    ImGui::TableSetupColumn("Entries", 0, 0.7f);
    ImGui::TableSetupColumn("At", 0, 1.0f);
    ImGui::TableHeadersRow();
    for (const RomGroup& g : contents_.groups) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%04X", g.id);
      ImGui::TableSetColumnIndex(1);
      const char* n = rom_group_name(g.id);
      if (n) ImGui::TextUnformatted(n); else ImGui::TextDisabled("(unnamed)");
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%u", g.count);
      ImGui::TableSetColumnIndex(3);
      ImGui::Text("%08X", g.addr);
    }
    ImGui::EndTable();
  }

  // Assets, by type.
  std::map<u16, std::vector<size_t>> by_type;
  for (size_t i = 0; i < contents_.assets.size(); i++) by_type[contents_.assets[i].type].push_back(i);
  ImGui::SeparatorText("Assets");
  if (exporting_) {
    ImGui::ProgressBar(export_total_ ? float(export_done_) / float(export_total_) : 0.0f, ImVec2(-80, 0));
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) export_stop_ = true;
  } else {
    ImGui::BeginDisabled(contents_.assets.empty());
    if (ImGui::Button("Export All...")) {
      std::weak_ptr<int> alive = alive_;
      std::vector<RomAsset> all = contents_.assets;
      const bool raw = export_raw_;
      host_->pick_folder([this, alive, all, raw](const std::string& dir) { if (!alive.expired()) export_assets(all, dir, raw); });
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Writes every asset into a folder per type: Flash movies as .swf, speech and\n"
                          "A-law sounds as .wav, SYN music as MIDI (.mid), the rest as stored (.bin).");
    ImGui::SameLine();
    ImGui::Checkbox("Raw", &export_raw_);
    ImGui::SetItemTooltip("Export every asset exactly as stored in the ROM, without converting it.");
  }
  // What is playing (or the last result), on the same line: nothing below moves.
  if (playing_ != SIZE_MAX && (!host_->audio_left || host_->audio_left() <= 0)) playing_ = SIZE_MAX;
  if (playing_ != SIZE_MAX) {
    const RomAsset& a = contents_.assets[playing_];
    const char* tn = rom_asset_type_name(a.type);
    const double length = play_length_, at = std::max(0.0, length - host_->audio_left());
    ImGui::SameLine();
    ImGui::TextDisabled("Playing %s %04X  %d:%02d / %d:%02d", tn ? tn : "asset", a.handle, int(at) / 60, int(at) % 60,
                        int(length) / 60, int(length) % 60);
    if (a.type == 0x6) ImGui::SetItemTooltip("Music is played with stand-in instruments, not the game's own.");
  } else if (!exporting_) {
    std::lock_guard<std::mutex> lock(export_mu_);
    const std::string& msg = !play_err_.empty() ? play_err_ : export_result_;
    if (!msg.empty()) {
      ImGui::SameLine();
      ImGui::TextDisabled("%s", msg.c_str());
      ImGui::SetItemTooltip("%s", msg.c_str());
    }
  }
  if (contents_.assets.empty()) ImGui::TextDisabled("No asset tables.");
  for (const auto& [type, idx] : by_type) {
    const char* tn = rom_asset_type_name(type);
    char label[96];
    std::snprintf(label, sizeof(label), "%s (%zu)##type%x", tn ? tn : ("Type " + std::to_string(type)).c_str(), idx.size(), type);
    if (!ImGui::TreeNode(label)) continue;
    const bool audio = asset_is_audio(RomAsset{type, 0, 0, 0, false});
    const int cols = audio ? 6 : 5;
    const float row_h = std::max(ImGui::GetTextLineHeightWithSpacing(), ImGui::GetFrameHeight());
    if (ImGui::BeginTable("##assets", cols, tf | ImGuiTableFlags_ScrollY, ImVec2(0, float(std::min(idx.size(), size_t(12)) + 1) * row_h + 4))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      if (audio) ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
      ImGui::TableSetupColumn("Handle", 0, 0.7f);
      ImGui::TableSetupColumn("Offset", 0, 0.9f);
      ImGui::TableSetupColumn("Size", 0, 0.9f);
      ImGui::TableSetupColumn("Starts with", 0, 2.0f);
      ImGui::TableSetupColumn("", 0, 0.6f);
      ImGui::TableHeadersRow();
      ImGuiListClipper clip;
      clip.Begin(int(idx.size()));
      while (clip.Step())
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
          const RomAsset& a = contents_.assets[idx[size_t(r)]];
          ImGui::TableNextRow();
          int col = 0;
          if (audio) {
            ImGui::TableSetColumnIndex(col++);
            ImGui::PushID(int(idx[size_t(r)]));
            if (playing_ == idx[size_t(r)]) {
              // (a square button the size of the play arrow, with a square in it)
              const float sz = ImGui::GetFrameHeight();
              if (ImGui::Button("##stop", ImVec2(sz, sz))) { host_->stop_audio(); playing_ = SIZE_MAX; }
              const ImVec2 lo = ImGui::GetItemRectMin(), hi = ImGui::GetItemRectMax(), pad(sz * 0.3f, sz * 0.3f);
              ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(lo.x + pad.x, lo.y + pad.y), ImVec2(hi.x - pad.x, hi.y - pad.y),
                                                        ImGui::GetColorU32(ImGuiCol_Text));
              ImGui::SetItemTooltip("Stop");
            } else {
              if (ImGui::ArrowButton("##play", ImGuiDir_Right)) play(idx[size_t(r)]);
              ImGui::SetItemTooltip("Listen");
            }
            ImGui::PopID();
          }
          ImGui::TableSetColumnIndex(col++);
          char h[16];
          std::snprintf(h, sizeof(h), "%04X##a%zu", a.handle, idx[size_t(r)]);
          if (ImGui::Selectable(h, selected_asset_ == idx[size_t(r)], ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick)) {
            selected_asset_ = u32(idx[size_t(r)]);
            if (asset_is_image(a) || asset_is_font(a)) preview(idx[size_t(r)]);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { goto_offset(a.offset); want_tab_ = Tab::Hex; }
          }
          if (ImGui::BeginPopupContextItem()) {
            if (audio && ImGui::MenuItem("Listen")) play(idx[size_t(r)]);
            if ((asset_is_image(a) || asset_is_font(a)) && ImGui::MenuItem("View")) preview(idx[size_t(r)]);
            if (ImGui::MenuItem("Show in Hex")) { goto_offset(a.offset); want_tab_ = Tab::Hex; }
            ImGui::Separator();
            const std::string conv = asset_file_name(img_, a, false), raw = asset_file_name(img_, a, true);
            if (ImGui::MenuItem(("Save As " + conv.substr(conv.rfind('.')) + "...").c_str())) save_asset_as(a, false);
            if (conv != raw && ImGui::MenuItem("Save Raw As...")) save_asset_as(a, true);
            if (conv != raw) ImGui::SetItemTooltip("Exactly as stored in the ROM (%s).", raw.substr(raw.rfind('.')).c_str());
            ImGui::EndPopup();
          }
          ImGui::TableSetColumnIndex(col++);
          ImGui::Text("%08X", a.offset);
          ImGui::TableSetColumnIndex(col++);
          if (a.exact) ImGui::TextUnformatted(size_text(a.size).c_str());
          else ImGui::TextDisabled("<= %s", size_text(a.size).c_str());
          if (!a.exact) ImGui::SetItemTooltip("Up to where the next asset starts: the exact size is not stored.");
          ImGui::TableSetColumnIndex(col++);
          char bytes[64];
          int p = 0;
          for (u32 k = 0; k < 8 && a.offset + k < img_.size(); k++) p += std::snprintf(bytes + p, sizeof(bytes) - size_t(p), "%02X ", img_[a.offset + k]);
          ImGui::TextUnformatted(bytes);
          ImGui::TableSetColumnIndex(col++);
          if (a.type == 0x7 && a.offset + 4 <= img_.size()) ImGui::TextDisabled("v%u", img_[a.offset + 3]);  // SWF version
        }
      ImGui::EndTable();
    }
    ImGui::TreePop();
  }
}

// Writes assets to <dir>/<game>/<type>/<handle><ext>, in the background.
void RomWindow::export_assets(std::vector<RomAsset> which, const std::string& dir, bool raw) {
  if (exporting_) return;
  if (export_thread_.joinable()) export_thread_.join();
  const std::vector<u8>* bios = host_->bios ? host_->bios() : nullptr;
  std::vector<u8> codebook;
  if (bios && bios->size() >= CelpDecoder::kCodebookPageOffset + CelpDecoder::kCodebookSize)
    codebook.assign(bios->begin() + CelpDecoder::kCodebookPageOffset,
                    bios->begin() + CelpDecoder::kCodebookPageOffset + CelpDecoder::kCodebookSize);
  export_total_ = unsigned(which.size());
  export_done_ = 0;
  export_stop_ = false;
  exporting_ = true;
  {
    std::lock_guard<std::mutex> lock(export_mu_);
    export_result_.clear();
  }
  const fs::path root = fs::path(dir) / safe_file_name(e_.name);
  // Soundtracks are decoded by a machine of the export's own (not the window's: another thread).
  std::vector<u8> bios_copy;
  if (bios && !raw && std::any_of(which.begin(), which.end(), [](const RomAsset& a) { return a.type == 0xf; })) bios_copy = *bios;
  export_thread_ = std::thread([this, which = std::move(which), codebook, root, raw, bios_copy = std::move(bios_copy)] {
    CelpDecoder dec;
    if (!codebook.empty()) dec.set_codebook(codebook.data(), codebook.size());
    SoundtrackDecoder tracks;
    std::string err;
    const bool have_tracks = !bios_copy.empty() && tracks.open(bios_copy, img_, &err);
    unsigned written = 0, failed = 0;
    for (const RomAsset& a : which) {
      if (export_stop_) break;
      (export_asset(img_, a, &dec, raw, e_.name, asset_folder(root, a.type) / asset_file_name(img_, a, raw), have_tracks ? &tracks : nullptr)
           ? written
           : failed)++;
      export_done_++;
    }
    {
      std::lock_guard<std::mutex> lock(export_mu_);
      export_result_ = std::to_string(written) + " files written to " + root.string() +
                       (failed ? " (" + std::to_string(failed) + " failed)" : "");
    }
    exporting_ = false;
  });
}

SoundtrackDecoder* RomWindow::soundtracks() {
  if (!soundtracks_tried_) {
    soundtracks_tried_ = true;
    const std::vector<u8>* bios = host_->bios ? host_->bios() : nullptr;
    auto d = std::make_unique<SoundtrackDecoder>();
    std::string err;
    if (bios && d->open(*bios, img_, &err)) soundtracks_ = std::move(d);
  }
  return soundtracks_.get();
}

CelpDecoder* RomWindow::speech() {
  if (!speech_) {
    speech_ = std::make_unique<CelpDecoder>();
    const std::vector<u8>* bios = host_->bios ? host_->bios() : nullptr;
    if (bios && bios->size() >= CelpDecoder::kCodebookPageOffset + CelpDecoder::kCodebookSize)
      speech_->set_codebook(bios->data() + CelpDecoder::kCodebookPageOffset, CelpDecoder::kCodebookSize);
  }
  return speech_.get();
}

void RomWindow::play(size_t i) {
  play_err_.clear();
  std::vector<s16> pcm;
  unsigned rate = 0;
  const RomAsset& a = contents_.assets[i];
  if (!asset_audio(img_, a, speech(), 32000, &pcm, &rate, a.type == 0xf ? soundtracks() : nullptr) || pcm.empty()) {
    play_err_ = a.type == 0x4 && !speech()->has_codebook() ? "Speech needs a BIOS (its codebook is in the BaseROM)."
                : a.type == 0xf && !soundtracks()           ? "Soundtracks need a BIOS (the game's own decoder runs on it)."
                                                            : "This asset could not be decoded.";
    return;
  }
  host_->play_audio(pcm, rate);
  playing_ = i;
  play_length_ = double(pcm.size()) / rate;
}

void RomWindow::preview(size_t i) {
  if (!host_->make_texture) return;
  if (preview_tex_) host_->free_texture(preview_tex_);
  preview_tex_ = 0;
  previewing_ = i;
  preview_err_.clear();
  AssetImage im;
  if (asset_is_font(contents_.assets[i])) {  // a font: its glyphs
    AssetFont f;
    if (!decode_asset_font(img_, contents_.assets[i], &f)) { preview_err_ = "This font could not be decoded."; return; }
    im = font_sheet(f);
  } else if (!decode_asset_image(img_, contents_.assets[i], &im)) {
    preview_err_ = contents_.assets[i].type == 0xd ? "This bitmap's format (8-bit, with a palette not found yet) is not decoded."
                                                   : "This bitmap could not be decoded.";
    return;
  }
  preview_tex_ = host_->make_texture(im.argb, im.w, im.h);
  preview_w_ = im.w;
  preview_h_ = im.h;
}

// The bitmap preview: at up to 4x, over a checkerboard (for transparency).
void RomWindow::draw_preview() {
  const RomAsset& a = contents_.assets[previewing_];
  const char* tn = rom_asset_type_name(a.type);
  char title[160];
  std::snprintf(title, sizeof(title), "%s %04X##preview%s", tn ? tn : "Bitmap", a.handle, e_.path.c_str());
  bool open = true;
  ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
  if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
    if (!preview_tex_) {
      ImGui::TextDisabled("%s", preview_err_.c_str());
    } else {
      const ImGuiViewport* vp = ImGui::GetMainViewport();
      float k = std::max(1.0f, std::floor(std::min({4.0f, (vp->WorkSize.x - 60) / preview_w_, (vp->WorkSize.y - 120) / preview_h_})));
      const ImVec2 size(preview_w_ * k, preview_h_ * k), at = ImGui::GetCursorScreenPos();
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const float cell = 8;
      for (float y = 0; y < size.y; y += cell)
        for (float x = 0; x < size.x; x += cell)
          dl->AddRectFilled(ImVec2(at.x + x, at.y + y), ImVec2(at.x + std::min(x + cell, size.x), at.y + std::min(y + cell, size.y)),
                            (int(x / cell) + int(y / cell)) % 2 ? IM_COL32(150, 150, 150, 255) : IM_COL32(100, 100, 100, 255));
      ImGui::Image(ImTextureRef(ImTextureID(preview_tex_)), size);
      ImGui::TextDisabled("%d x %d%s", preview_w_, preview_h_, k > 1 ? ("  (shown at " + std::to_string(int(k)) + "x)").c_str() : "");
      ImGui::SameLine();
      const std::string conv = asset_file_name(img_, a, false);
      if (ImGui::SmallButton(("Save As " + conv.substr(conv.rfind('.')) + "...").c_str())) save_asset_as(a, false);
    }
  }
  ImGui::End();
  if (!open) {
    if (preview_tex_) host_->free_texture(preview_tex_);
    preview_tex_ = 0;
    previewing_ = SIZE_MAX;
  }
}

// Right-click > Save As: one asset, converted (or as stored if `raw`).
void RomWindow::save_asset_as(const RomAsset& a, bool raw) {
  const char* tn = rom_asset_type_name(a.type);
  const std::string name = safe_file_name(e_.name + " - " + (tn ? tn : "asset") + " " + asset_file_name(img_, a, raw));
  std::weak_ptr<int> alive = alive_;
  const RomAsset one = a;
  host_->pick_save(name, [this, alive, one, raw](const std::string& path) {
    if (alive.expired()) return;
    std::lock_guard<std::mutex> lock(export_mu_);
    export_result_ = export_asset(img_, one, speech(), raw, e_.name, path, one.type == 0xf ? soundtracks() : nullptr)
                         ? "Saved " + path
                         : "Could not write " + path;
  });
}

// The patch being edited, and the others, in one line above the bytes.
void RomWindow::draw_patch_bar() {
  const std::string preview = active_ < 0 ? std::string("(none)")
                                          : patches_[size_t(active_)].name + " (" + std::to_string(patches_[size_t(active_)].bytes.size()) + ")";
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted("Patch");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(std::min(200.0f, ImGui::GetContentRegionAvail().x * 0.4f));
  if (ImGui::BeginCombo("##patch", preview.c_str())) {
    for (int i = 0; i < int(patches_.size()); i++) {
      UiPatch& p = patches_[size_t(i)];
      ImGui::PushID(i);
      if (ImGui::Checkbox("##on", &p.enabled)) { rebuild_overlay(); save_patches(); }
      ImGui::SameLine();
      if (ImGui::Selectable((p.name + " (" + std::to_string(p.bytes.size()) + ")").c_str(), active_ == i)) {
        active_ = i;
        rebuild_overlay();
      }
      if (!p.foreign.empty()) ImGui::SetItemTooltip("Some of its edits do not match this ROM (kept, not applied).");
      ImGui::PopID();
    }
    if (patches_.empty()) ImGui::TextDisabled("None yet: type over a byte to start one.");
    ImGui::EndCombo();
  }
  ImGui::SetItemTooltip("Edits go into this patch. Each patch can be switched on and off (in the list).");
  if (active_ >= 0) {
    ImGui::SameLine();
    if (ImGui::Checkbox("On", &patches_[size_t(active_)].enabled)) { rebuild_overlay(); save_patches(); }
    ImGui::SetItemTooltip("On: applied when the game starts.");
  }
  ImGui::SameLine();
  if (ImGui::Button("New")) {
    patches_.push_back({"Patch " + std::to_string(patches_.size() + 1), true, {}, {}});
    active_ = int(patches_.size()) - 1;
    rebuild_overlay();
    save_patches();
    renaming_ = active_;
    std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", patches_.back().name.c_str());
    ImGui::OpenPopup("Rename patch");
  }
  ImGui::BeginDisabled(active_ < 0);
  ImGui::SameLine();
  if (ImGui::Button("Rename") && active_ >= 0) {
    renaming_ = active_;
    std::snprintf(rename_buf_, sizeof(rename_buf_), "%s", patches_[size_t(active_)].name.c_str());
    ImGui::OpenPopup("Rename patch");
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete")) ImGui::OpenPopup("Delete patch?");
  ImGui::EndDisabled();
  if (ImGui::BeginPopup("Rename patch")) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(240);
    if (ImGui::InputText("##name", rename_buf_, sizeof(rename_buf_), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
      if (rename_buf_[0] && renaming_ >= 0 && renaming_ < int(patches_.size())) {
        patches_[size_t(renaming_)].name = rename_buf_;
        save_patches();
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::TextDisabled("Enter to rename");
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("Delete patch?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    if (active_ >= 0) ImGui::Text("Delete \"%s\" and its %zu changed bytes?", patches_[size_t(active_)].name.c_str(), patches_[size_t(active_)].bytes.size());
    if (ImGui::Button("Delete") && active_ >= 0) {
      patches_.erase(patches_.begin() + active_);
      active_ = patches_.empty() ? -1 : 0;
      rebuild_overlay();
      save_patches();
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (dirty_since_start_ && host_->running(e_.crc)) {
    ImGui::SameLine();
    if (ImGui::Button("Restart Game")) {
      dirty_since_start_ = false;
      host_->restart();
    }
    ImGui::SetItemTooltip("Patches apply when the game starts.");
  }
  if (!patch_err_.empty()) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", patch_err_.c_str());
}

void RomWindow::draw_hex() {
  if (!load_image()) { ImGui::TextDisabled("Cannot read the image: %s", image_err_.c_str()); return; }
  const u32 size = u32(img_.size());
  const u32 base = header_.device_start;

  // Tool bar: go to, search, address style.
  ImGui::SetNextItemWidth(ImGui::CalcTextSize("00000000").x + 16);
  if (ImGui::InputTextWithHint("##goto", "Go to", goto_buf_, sizeof(goto_buf_), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsHexadecimal)) {
    const u32 v = u32(std::strtoul(goto_buf_, nullptr, 16));
    goto_offset(v >= base && v - base < size ? v - base : v);  // an address or a file offset
  }
  ImGui::SetItemTooltip("An address (%08X...) or a file offset, in hex.", base);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(std::min(200.0f, ImGui::GetContentRegionAvail().x * 0.4f));
  if (ImGui::InputTextWithHint("##find", "Find: 4a 26 or \"text\"", search_buf_, sizeof(search_buf_), ImGuiInputTextFlags_EnterReturnsTrue)) search(false);
  ImGui::SameLine();
  if (ImGui::Button("Next")) search(false);
  if (!search_msg_.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", search_msg_.c_str()); }
  ImGui::SameLine();
  ImGui::Checkbox("Addresses", &show_addresses_);
  ImGui::SetItemTooltip("Show where each row is mapped (on) or its offset in the file (off).");

  draw_patch_bar();
  ImGui::BeginChild("##bytes", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_HorizontalScrollbar);
  const float cw = ImGui::CalcTextSize("0").x, row_h = ImGui::GetTextLineHeightWithSpacing();
  const u32 rows = (size + 15) / 16;
  const bool focused = ImGui::IsWindowFocused();

  // Typing: hex digits overwrite the byte at the cursor, a nibble at a time.
  // (Keys, not text input: SDL sends text only while a text field is active.)
  if (focused && !ImGui::GetIO().WantTextInput && !ImGui::GetIO().KeyCtrl) {
    for (int k = 0; k < 16 + 10; k++) {
      const ImGuiKey key = k < 10 ? ImGuiKey(ImGuiKey_0 + k) : k < 16 ? ImGuiKey(ImGuiKey_A + k - 10) : ImGuiKey(ImGuiKey_Keypad0 + k - 16);
      if (!ImGui::IsKeyPressed(key)) continue;
      const u8 nib = u8(k < 16 ? k : k - 16);
      const u8 old = shown(cursor_);
      set_byte(cursor_, low_nibble_ ? u8((old & 0xf0) | nib) : u8((old & 0x0f) | (nib << 4)));
      if (low_nibble_ && cursor_ + 1 < size) cursor_++;
      low_nibble_ = !low_nibble_;
      scroll_to_cursor_ = true;
    }
    auto move = [&](s64 d) {
      cursor_ = u32(std::clamp<s64>(s64(cursor_) + d, 0, s64(size) - 1));
      low_nibble_ = false;
      scroll_to_cursor_ = true;
    };
    const int page = std::max(1, int(ImGui::GetWindowHeight() / row_h) - 1) * 16;
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move(-1);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move(1);
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move(-16);
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move(16);
    if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) move(-page);
    if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) move(page);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
      revert_byte(cursor_);
      if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) move(-1);
    }
  }
  if (scroll_to_cursor_) {
    const float y = float(cursor_ / 16) * row_h, top = ImGui::GetScrollY(), h = ImGui::GetWindowHeight() - row_h * 2;
    if (y < top || y > top + h) ImGui::SetScrollY(std::max(0.0f, y - h / 2));
    scroll_to_cursor_ = false;
  }

  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(cw, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.5f, 0));
  ImGuiListClipper clip;
  clip.Begin(int(rows), row_h);
  while (clip.Step())
    for (int r = clip.DisplayStart; r < clip.DisplayEnd; r++) {
      const u32 row = u32(r) * 16;
      ImGui::TextDisabled("%08X", show_addresses_ ? base + row : row);
      char ascii[17] = {};
      for (u32 k = 0; k < 16; k++) {
        const u32 off = row + k;
        ImGui::SameLine(0, k == 0 ? cw * 2 : k == 8 ? cw * 2 : cw);
        if (off >= size) { ImGui::Dummy(ImVec2(cw * 2, row_h)); ascii[k] = ' '; continue; }
        const u8 v = shown(off);
        ascii[k] = v >= 32 && v < 127 ? char(v) : '.';
        auto ov = overlay_.find(off);
        const bool edited = ov != overlay_.end();
        if (edited) ImGui::PushStyleColor(ImGuiCol_Text, ov->second.second == active_ ? kActiveEdit : kOtherEdit);
        char cell[24];
        std::snprintf(cell, sizeof(cell), "%02X##%u", v, off);
        if (ImGui::Selectable(cell, off == cursor_, ImGuiSelectableFlags_None, ImVec2(cw * 2, 0))) {
          cursor_ = off;
          low_nibble_ = false;
        }
        if (edited) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
          ImGui::BeginTooltip();
          ImGui::Text("Offset %08X, address %08X", off, base + off);
          if (edited) ImGui::Text("Was %02X (patch \"%s\")", img_[off], patches_[size_t(ov->second.second)].name.c_str());
          if (const RomAsset* a = asset_at(off)) {
            const char* tn = rom_asset_type_name(a->type);
            ImGui::Text("In %s %04X, +%X", tn ? tn : "asset", a->handle, off - a->offset);
          }
          ImGui::EndTooltip();
        }
      }
      ImGui::SameLine(0, cw * 2);
      ImGui::TextUnformatted(ascii);
    }
  ImGui::PopStyleVar(2);
  ImGui::EndChild();

  // Status line.
  const u8 v = shown(cursor_);
  ImGui::TextDisabled("Offset %08X  Address %08X  Value %02X (%u)", cursor_, base + cursor_, v, v);
  if (const RomAsset* a = asset_at(cursor_)) {
    const char* tn = rom_asset_type_name(a->type);
    ImGui::SameLine();
    ImGui::TextDisabled(" In %s %04X", tn ? tn : "asset", a->handle);
  }
  if (active_ >= 0) {
    ImGui::SameLine();
    ImGui::TextColored(kActiveEdit, " Editing \"%s\"%s", patches_[size_t(active_)].name.c_str(), patches_[size_t(active_)].enabled ? "" : " (off)");
  }
}

}  // namespace leap
