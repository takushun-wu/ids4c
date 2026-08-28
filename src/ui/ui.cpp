#include "ids4c/ui.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "utf8.h"

#include "ids4c/ids4c.h"
#include "ids4c/ui.h"

namespace fs = boost::filesystem;
namespace {
    std::string PreprocessRulesText(const IDSMatchDetail& detail) {
        std::string out;
        for(const IDSPreprocessRule rule: detail.preprocessRules) {
            if(!out.empty()) out += ",";
            out += IDSPreprocessRuleName(rule);
        }
        return out;
    }

    std::string MatchPathsText(const std::vector<IDSMatchPath>& paths) {
        std::string out;
        for(const IDSMatchPath& path: paths) {
            if(!out.empty()) out += ";";
            out += std::string(IDSMatchPathKindName(path.kind)) + ":";
            out += path.queryExpressionIndex == std::numeric_limits<size_t>::max()
                ? "input@"
                : "q[" + std::to_string(path.queryExpressionIndex) + "]@";
            out += path.queryPath + "=>" + path.path;
        }
        return out;
    }
} // namespace

IDSui::IDSui():
    mainBox(Gtk::ORIENTATION_VERTICAL, 0),
    queryButton("Query"),
    r1Box(Gtk::ORIENTATION_HORIZONTAL, 0),
    r2Box(Gtk::ORIENTATION_HORIZONTAL, 0),
    r3Box(Gtk::ORIENTATION_HORIZONTAL, 0),
    r4Box(Gtk::ORIENTATION_HORIZONTAL, 0),
    r5Box(Gtk::ORIENTATION_HORIZONTAL, 0),
    r1BoxEquivalent(Gtk::ORIENTATION_HORIZONTAL, 0),
    equivalentUILabel("Equivalent Syntax:"),
    menuItemFile("_File", true),
    menuItemHelp("_Help", true),
    menuItemFileDatabase("_Database...", true),
    menuItemFileSettings("_Settings", true),
    menuItemFileExit("E_xit", true),
    menuItemHelpAbout("_About", true),
    rbFilterAll("All", true),
    rbFilterLcSuffix("Ignore LC suffix", true),
    rbFilterLocale("Ignore other locales", true),
    cbIgnoreOverlay(u8"Ignore \u2FFB overlay structures"),
    cbShowDetails("Show match details"),
    cbTrackMatchPaths("Track match paths"),
    labelGlyphDomain("Glyphs:"),
    labelUnicodeBlock("Blocks:"),
    unicodeBlockButton("All blocks"),
    labelUnification("IWDS") {
    set_title("Han Ideograph Finder");
    set_default_size(1024, 768);
    // Init
    ReadConfigFile();
    // Font configuration
    Pango::FontDescription queryFontCfg, resultFontCfg;
    queryFontCfg.set_family(fontCfg);
    queryFontCfg.set_size(queryFontSize * PANGO_SCALE);
    resultFontCfg.set_family(fontCfg);
    resultFontCfg.set_size(resultFontSize * PANGO_SCALE);
    // GUI Box
    add(mainBox);
    CreateMenuBar();
    mainBox.pack_start(menuBar, Gtk::PACK_SHRINK);
    mainBox.pack_start(r1Box, Gtk::PACK_SHRINK);
    equivalentQueryLabel.set_xalign(0.0);
    equivalentQueryLabel.set_line_wrap(true);
    equivalentQueryLabel.set_selectable(true);
    equivalentQueryLabel.override_font(queryFontCfg);
    r1BoxEquivalent.pack_start(equivalentUILabel, Gtk::PACK_SHRINK);
    r1BoxEquivalent.pack_start(equivalentQueryLabel, Gtk::PACK_SHRINK);
    mainBox.pack_start(r1BoxEquivalent, Gtk::PACK_SHRINK);
    CreateInputBar();
    mainBox.pack_start(r2Box, Gtk::PACK_SHRINK);
    mainBox.pack_start(r3Box, Gtk::PACK_SHRINK);
    CreateRBFilters();
    mainBox.pack_start(r4Box, Gtk::PACK_SHRINK);
    mainBox.pack_start(r5Box, Gtk::PACK_SHRINK);
    mainBox.pack_start(resultScrollBox);
    mainBox.pack_start(statusBar, Gtk::PACK_SHRINK);
    // r1Box
    entry.override_font(queryFontCfg);
    r1Box.pack_start(entry);
    r1Box.pack_start(queryButton, Gtk::PACK_SHRINK);
    // TextView
    resultScrollBox.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_ALWAYS);
    resultScrollBox.add(resultBox);
    resultBox.set_editable(false);
    resultBox.set_wrap_mode(Gtk::WRAP_WORD);
    resultBox.override_font(resultFontCfg);
    resultBuf = resultBox.get_buffer();
    // Event
    entry.signal_activate().connect(sigc::mem_fun(*this, &IDSui::onQuery));
    queryButton.signal_clicked().connect(sigc::mem_fun(*this, &IDSui::onQuery));
    dispatcher_.connect(sigc::mem_fun(*this, &IDSui::onQueryFinished));
    // Other initialization
    CreateDBTreeView();
    idsdb.reset(new IDSdatabase(selectedDB));
    idsdb->config.fuzzyMatch.unificationLevel = unificationLevel;
    idsdb->config.fuzzyMatch.defaultRegion    = defaultRegion;
    ParseLocaleSuffixFallbackOrder(localeSuffixFallbackOrder, idsdb->config.fuzzyMatch.localeSuffixFallbackOrder);
    // Show all children items
    show_all_children();
}

void IDSui::CreateMenuBar() {
    menuBar.append(menuItemFile);
    menuBar.append(menuItemHelp);
    menuItemFile.set_submenu(menuFile);
    menuItemHelp.set_submenu(menuHelp);
    menuFile.append(menuItemFileDatabase);
    menuFile.append(menuItemFileSettings);
    menuFile.append(menuItemFileExit);
    menuHelp.append(menuItemHelpAbout);
    menuItemFileDatabase.signal_activate().connect(sigc::mem_fun(*this, &IDSui::onMenuDatabase));
    menuItemFileSettings.signal_activate().connect(sigc::mem_fun(*this, &IDSui::onMenuSettings));
    menuItemFileExit.signal_activate().connect(sigc::mem_fun(*this, &IDSui::onExit));
    menuItemHelpAbout.signal_activate().connect(sigc::mem_fun(*this, &IDSui::onMenuAbout));
}

void IDSui::CreateInputBar() {
    const std::vector<std::string> quickInput = {"⿰", "⿱", "⿲", "⿳", "⿴", "⿵", "⿶", "⿷", "⿼", "⿸", "⿹", "⿺",
        "⿽", "⿻", "⿾", "⿿", "㇯", "〾", "⬚", "🔄"};
    for(auto i: quickInput) {
        auto                   button = new Gtk::Button(i);
        Pango::FontDescription buttonFontCfg;
        buttonFontCfg.set_family(fontCfg);
        button->override_font(buttonFontCfg);
        inputButton.push_back(button);
    }
    for(auto i: inputButton) {
        r2Box.pack_start(*i);
        i->signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &IDSui::onInput), i->get_label()));
    }
    const std::vector<std::string> quickInput2 = {
        "<search=>", "<stroke=>", "<residue=>", "<var=>", "<any=>", "<except=>"};
    for(auto i: quickInput2)
        inputButton2.push_back(new Gtk::Button(i));
    for(auto i: inputButton2) {
        r3Box.pack_start(*i);
        i->signal_clicked().connect(sigc::bind(sigc::mem_fun(*this, &IDSui::onInput), i->get_label()));
    }
}

void IDSui::CreateDBTreeView() {
    dbListStore = Gtk::ListStore::create(dbListColumn);
    dbList.set_model(dbListStore);

    dbList.append_column("DB Name", dbListColumn.dbName);
    // dbList.append_column("DB Format", dbListColumn.dbType);

    dbList.get_selection()->set_mode(Gtk::SELECTION_SINGLE);
    dbList.get_selection()->signal_changed().connect(sigc::mem_fun(*this, &IDSui::onDBTreeViewChanged));
}

void IDSui::CreateRBFilters() {
    rgFilter = Gtk::RadioButton::Group();
    rbFilterAll.set_group(rgFilter);
    rbFilterLcSuffix.set_group(rgFilter);
    rbFilterLocale.set_group(rgFilter);
    r4Box.pack_start(rbFilterAll, Gtk::PACK_SHRINK);
    r4Box.pack_start(rbFilterLcSuffix, Gtk::PACK_SHRINK);
    r4Box.pack_start(rbFilterLocale, Gtk::PACK_SHRINK);
    r4Box.pack_start(cbIgnoreOverlay, Gtk::PACK_SHRINK);
    r4Box.pack_start(labelUnification, Gtk::PACK_SHRINK);
    r4Box.pack_start(cbUnification, Gtk::PACK_SHRINK);
    rbFilterAll.set_active(true);
    cbTrackMatchPaths.set_active(true);
    cbUnification.append("none", "None");
    cbUnification.append("srcseparation", "Source code separation");
    cbUnification.append("lv1", "Lv1");
    cbUnification.append("lv2", "Lv2");
    cbUnification.set_active_id(IWDSUnificationLevelName(unificationLevel));
    if(cbUnification.get_active_row_number() < 0) cbUnification.set_active_id("none");
    r5Box.pack_start(cbShowDetails, Gtk::PACK_SHRINK);
    r5Box.pack_start(cbTrackMatchPaths, Gtk::PACK_SHRINK);
    r5Box.pack_start(labelGlyphDomain, Gtk::PACK_SHRINK);
    r5Box.pack_start(cbGlyphDomain, Gtk::PACK_SHRINK);
    r5Box.pack_start(labelUnicodeBlock, Gtk::PACK_SHRINK);
    r5Box.pack_start(unicodeBlockButton, Gtk::PACK_SHRINK);
    cbGlyphDomain.append("all", "All glyphs");
    cbGlyphDomain.append("unicode", "Unicode");
    cbGlyphDomain.append("private", "Private use");
    cbGlyphDomain.append("abstract", "Abstract");
    cbGlyphDomain.set_active_id("all");
    rbFilterAll.signal_toggled().connect([this]() {
        if(rbFilterAll.get_active()) onQueryOptionsChanged();
    });
    rbFilterLcSuffix.signal_toggled().connect([this]() {
        if(rbFilterLcSuffix.get_active()) onQueryOptionsChanged();
    });
    rbFilterLocale.signal_toggled().connect([this]() {
        if(rbFilterLocale.get_active()) onQueryOptionsChanged();
    });
    cbIgnoreOverlay.signal_toggled().connect(sigc::mem_fun(*this, &IDSui::onQueryOptionsChanged));
    cbShowDetails.signal_toggled().connect(sigc::mem_fun(*this, &IDSui::onQueryOptionsChanged));
    cbTrackMatchPaths.signal_toggled().connect(sigc::mem_fun(*this, &IDSui::onQueryOptionsChanged));
    cbGlyphDomain.signal_changed().connect(sigc::mem_fun(*this, &IDSui::onQueryOptionsChanged));
    unicodeBlockButton.signal_clicked().connect(sigc::mem_fun(*this, &IDSui::onSelectUnicodeBlocks));
    cbUnification.signal_changed().connect([this]() {
        IWDSUnificationLevel selectedLevel = IWDS_UNIFICATION_NONE;
        if(!ParseIWDSUnificationLevel(std::string(cbUnification.get_active_id()), selectedLevel)) return;
        unificationLevel                          = selectedLevel;
        idsdb->config.fuzzyMatch.unificationLevel = selectedLevel;
        WriteConfigFile();
        onQueryOptionsChanged();
    });
}
void IDSui::ReadDatabase() {
    fs::path                         dbPath("./db");
    fs::recursive_directory_iterator end_iter;
    databaseList.clear();
    for(fs::recursive_directory_iterator iter(dbPath); iter != end_iter; iter++) {
        try {
            if(!fs::is_directory(*iter)) {
                if(iter->path().extension().string() == ".sqlite" && iter->path().stem().string() != "unifiable")
                    databaseList.push_back(iter->path().stem().string());
            }
        } catch(const std::exception& ex) {
            std::cerr << ex.what() << std::endl;
            continue;
        }
    }
    dbListStore->clear();
    for(auto i: databaseList) {
        Gtk::TreeModel::Row row  = *(dbListStore->append());
        row[dbListColumn.dbName] = i;
        // row[dbListColumn.dbType] = "";
    }
    selectedDB     = idsdb->name;
    auto selection = dbList.get_selection();
    selection->unselect_all();
    for(size_t i = 0; i < databaseList.size(); i++) {
        if(selectedDB == databaseList[i]) selection->select(Gtk::TreeModel::Path(std::to_string(i)));
    }
}

void IDSui::ReadConfigFile() {
    fs::path dbPath("./db"), cfgFile("./db/config.toml");
    if(!fs::exists(dbPath)) fs::create_directory(dbPath);
    if(!fs::exists(cfgFile)) {
        fontCfg       = FONTCFG_DEFAULT;
        queryFontSize = QUERY_FONTSIZE_DEFAULT, resultFontSize = RESULT_FONTSIZE_DEFAULT;
        selectedDB                = "";
        defaultRegion             = "";
        localeSuffixFallbackOrder = "";
        unificationLevel          = IWDS_UNIFICATION_NONE;
        tomlCfg                   = {
            {                  "fontCfg",         FONTCFG_DEFAULT},
            {            "queryFontSize",  QUERY_FONTSIZE_DEFAULT},
            {           "resultFontSize", RESULT_FONTSIZE_DEFAULT},
            {               "selectedDB",                      ""},
            {            "defaultRegion",                      ""},
            {"localeSuffixFallbackOrder",                      ""},
            {         "unificationLevel",                  "none"},
        };
        std::ofstream outputCfg("./db/config.toml");
        outputCfg << tomlCfg;
        outputCfg.close();
    }
    tomlCfg                                 = toml::parse("./db/config.toml");
    fontCfg                                 = toml::find_or<std::string>(tomlCfg, "fontCfg", FONTCFG_DEFAULT);
    queryFontSize                           = toml::find_or<int>(tomlCfg, "queryFontSize", QUERY_FONTSIZE_DEFAULT);
    resultFontSize                          = toml::find_or<int>(tomlCfg, "resultFontSize", RESULT_FONTSIZE_DEFAULT);
    selectedDB                              = toml::find_or<std::string>(tomlCfg, "selectedDB", "");
    defaultRegion                           = toml::find_or<std::string>(tomlCfg, "defaultRegion", "");
    localeSuffixFallbackOrder               = toml::find_or<std::string>(tomlCfg, "localeSuffixFallbackOrder", "");
    const std::string configuredUnification = toml::find_or<std::string>(tomlCfg, "unificationLevel", "none");
    if(!ParseIWDSUnificationLevel(configuredUnification, unificationLevel)) unificationLevel = IWDS_UNIFICATION_NONE;
}

void IDSui::WriteConfigFile() {
    tomlCfg["fontCfg"]       = fontCfg;
    tomlCfg["queryFontSize"] = queryFontSize, tomlCfg["resultFontSize"] = resultFontSize;
    tomlCfg["selectedDB"]                = selectedDB;
    tomlCfg["defaultRegion"]             = defaultRegion;
    tomlCfg["localeSuffixFallbackOrder"] = localeSuffixFallbackOrder;
    tomlCfg["unificationLevel"]          = IWDSUnificationLevelName(unificationLevel);
    std::ofstream outputCfg("./db/config.toml");
    outputCfg << tomlCfg;
    outputCfg.close();
}

void IDSui::onQuery() {
    if(idsdb->isEmpty()) {
        const std::string& error = idsdb->GetLastError();
        statusBar.push(error.empty() ? "Empty database. Please import a database before querying." : error);
        resultBuf->set_text("");
        equivalentQueryLabel.set_text("");
        return;
    }
    std::string queryStr = entry.get_text();
    if(queryStr.empty()) {
        statusBar.push("Empty query. Please input a query expression.");
        resultBuf->set_text("");
        equivalentQueryLabel.set_text("");
        return;
    }

    if(running_) return;
    pendingQueryText_    = queryStr;
    pendingQueryOptions_ = GetQueryOptions();
    pendingShowDetails_  = cbShowDetails.get_active();
    running_             = true;
    queryButton.set_sensitive(false);
    rbFilterAll.set_sensitive(false);
    rbFilterLcSuffix.set_sensitive(false);
    rbFilterLocale.set_sensitive(false);
    cbIgnoreOverlay.set_sensitive(false);
    cbUnification.set_sensitive(false);
    cbShowDetails.set_sensitive(false);
    cbTrackMatchPaths.set_sensitive(false);
    cbGlyphDomain.set_sensitive(false);
    unicodeBlockButton.set_sensitive(false);
    equivalentQueryLabel.set_text("");
    statusBar.push("Matching... Please wait.");

    worker_ = std::thread(&IDSui::RunQueryTask, this);
}

void IDSui::RunQueryTask() {
    std::string       message;
    std::string       equivalentMessage;
    const std::string queryStr = pendingQueryText_;
    resultDetails_.clear();
    try {
        IDSParseError parseError;
        IDSOwner      queryIDS = ParseIDSOwned(queryStr, &parseError);
        if(queryIDS == nullptr) {
            message = "Illegal query expression. Please check.";
            if(!parseError.message.empty()) {
                message = "Illegal query expression";
                if(parseError.hasPosition) message += " at character #" + std::to_string(parseError.position + 1);
                message += ": " + parseError.message;
            }
            result.clear();
            resultDetails_.clear();
            queryTime = std::chrono::milliseconds(0);
        } else {
            const std::vector<std::string> equivalentQueries = idsdb->GetEquivalentQueries(queryIDS.get());
            equivalentMessage                                = u8"";
            for(const std::string& equivalentQuery: equivalentQueries)
                equivalentMessage += equivalentQuery + (equivalentQuery == equivalentQueries.back() ? "" : ", ");
            message        = "Querying...";
            const auto tic = std::chrono::system_clock::now();
            if(pendingShowDetails_) {
                resultDetails_ = idsdb->MatchDetailed(queryIDS.get(), pendingQueryOptions_);
                result.clear();
                result.reserve(resultDetails_.size());
                for(const IDSMatchDetail& detail: resultDetails_)
                    result.push_back(detail.glyph);
            } else {
                result = idsdb->MatchQuery(queryIDS.get(), pendingQueryOptions_);
            }
            const auto toc = std::chrono::system_clock::now();
            queryTime      = std::chrono::duration_cast<std::chrono::milliseconds>(toc - tic);

            message = std::to_string(result.size()) + " match" + (result.size() > 1 ? "es" : "") +
                " found. Matching time: " + std::to_string(queryTime.count()) + "ms.";
            std::sort(result.begin(), result.end(), IdeographCmp);
        }
    } catch(const std::exception& e) {
        result.clear();
        resultDetails_.clear();
        message = std::string("Query failed: ") + e.what();
    } catch(...) {
        result.clear();
        resultDetails_.clear();
        message = "Query failed: unknown error.";
    }
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        result_message_         = message;
        equivalentQueryMessage_ = equivalentMessage;
    }
    dispatcher_.emit();
}

void IDSui::onQueryFinished() {
    if(worker_.joinable()) worker_.join();
    std::string message;
    std::string equivalentMessage;
    std::string resultStr;
    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        message           = result_message_;
        equivalentMessage = equivalentQueryMessage_;
        if(pendingShowDetails_) {
            for(const IDSMatchDetail& detail: resultDetails_) {
                resultStr += detail.glyph.toString() + "\tkind=" + IDSMatchKindName(detail.matchKind) +
                    "\tsource=" + IDSMatchSourceName(detail.matchSource) +
                    "\tpreprocess=" + PreprocessRulesText(detail) + "\n";
                resultStr += "  paths: " + MatchPathsText(detail.matchPaths) + "\n";
                resultStr += "  IDS: " + detail.matchedIDS + "\n";
                resultStr += "  raw: " + detail.rawIDS + "\n";
            }
        } else {
            for(const auto& ideograph: result)
                resultStr += ideograph.toString() + " ";
        }
    }
    statusBar.push(message);
    equivalentQueryLabel.set_text(equivalentMessage);
    if(!pendingShowDetails_ && !resultStr.empty()) resultStr.pop_back();
    resultBuf->set_text(resultStr);
    queryButton.set_sensitive(true);
    rbFilterAll.set_sensitive(true);
    rbFilterLcSuffix.set_sensitive(true);
    rbFilterLocale.set_sensitive(true);
    cbIgnoreOverlay.set_sensitive(true);
    cbUnification.set_sensitive(true);
    cbShowDetails.set_sensitive(true);
    cbTrackMatchPaths.set_sensitive(true);
    cbGlyphDomain.set_sensitive(true);
    unicodeBlockButton.set_sensitive(true);
    running_ = false;
}

IDSqueryOptions IDSui::GetQueryOptions() const {
    IDSqueryOptions options;
    options.filter.ignoreOverlayStructure = cbIgnoreOverlay.get_active();
    options.trackMatchPaths               = cbTrackMatchPaths.get_active();
    if(rbFilterLcSuffix.get_active())
        options.filter.resultFilter = IDS_RESULT_IGNORE_LC_SUFFIX;
    else if(rbFilterLocale.get_active())
        options.filter.resultFilter = IDS_RESULT_IGNORE_OTHER_LOCALES;
    ParseLocaleSuffixFallbackOrder(localeSuffixFallbackOrder, options.filter.localeSuffixFallbackOrder);
    ParseIDSglyphDomain(std::string(cbGlyphDomain.get_active_id()), options.filter.glyphDomain);
    options.filter.unicodeBlocks = selectedUnicodeBlocks_;
    return options;
}

void IDSui::onSelectUnicodeBlocks() {
    Gtk::Dialog dialog("Unicode blocks", *this, true);
    dialog.set_default_size(360, 480);
    dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("OK", Gtk::RESPONSE_OK);

    Gtk::ScrolledWindow scroll;
    Gtk::Box            list(Gtk::ORIENTATION_VERTICAL);
    Gtk::CheckButton    allButton("All blocks");
    scroll.set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll.add(list);
    dialog.get_content_area()->pack_start(scroll);

    const std::vector<IDSunicodeBlock> availableBlocks = {
        IDS_UNICODE_BLOCK_CJK,
        IDS_UNICODE_BLOCK_CJK_BASIC,
        IDS_UNICODE_BLOCK_CJK_EXT_A,
        IDS_UNICODE_BLOCK_CJK_EXT_B,
        IDS_UNICODE_BLOCK_CJK_EXT_C,
        IDS_UNICODE_BLOCK_CJK_EXT_D,
        IDS_UNICODE_BLOCK_CJK_EXT_E,
        IDS_UNICODE_BLOCK_CJK_EXT_F,
        IDS_UNICODE_BLOCK_CJK_EXT_G,
        IDS_UNICODE_BLOCK_CJK_EXT_H,
        IDS_UNICODE_BLOCK_CJK_EXT_I,
        IDS_UNICODE_BLOCK_CJK_EXT_J,
        IDS_UNICODE_BLOCK_CJK_COMPATIBILITY,
        IDS_UNICODE_BLOCK_CJK_RADICALS,
        IDS_UNICODE_BLOCK_CJK_STROKES,
        IDS_UNICODE_BLOCK_PRIVATE_BMP,
        IDS_UNICODE_BLOCK_PRIVATE_PLANE15,
        IDS_UNICODE_BLOCK_PRIVATE_PLANE16,
        IDS_UNICODE_BLOCK_ABSTRACT,
        IDS_UNICODE_BLOCK_OTHER,
    };
    allButton.set_active(selectedUnicodeBlocks_.empty());
    list.pack_start(allButton, Gtk::PACK_SHRINK);

    std::vector<std::unique_ptr<Gtk::CheckButton>> blockButtons;
    blockButtons.reserve(availableBlocks.size());
    for(const IDSunicodeBlock block: availableBlocks) {
        auto button = std::unique_ptr<Gtk::CheckButton>(new Gtk::CheckButton(IDSunicodeBlockName(block)));
        button->set_active(std::find(selectedUnicodeBlocks_.begin(), selectedUnicodeBlocks_.end(), block) !=
            selectedUnicodeBlocks_.end());
        list.pack_start(*button, Gtk::PACK_SHRINK);
        blockButtons.push_back(std::move(button));
    }

    dialog.show_all();
    if(dialog.run() != Gtk::RESPONSE_OK) return;

    if(allButton.get_active()) {
        selectedUnicodeBlocks_.clear();
        unicodeBlockButton.set_label("All blocks");
    } else {
        selectedUnicodeBlocks_.clear();
        for(size_t index = 0; index < blockButtons.size(); index++)
            if(blockButtons[index]->get_active()) selectedUnicodeBlocks_.push_back(availableBlocks[index]);
        if(selectedUnicodeBlocks_.empty())
            unicodeBlockButton.set_label("All blocks");
        else
            unicodeBlockButton.set_label("Blocks: " + std::to_string(selectedUnicodeBlocks_.size()));
    }
    onQueryOptionsChanged();
}

void IDSui::onQueryOptionsChanged() {
    if(running_) return;
    onQuery();
}

void IDSui::onInput(std::string input) {
    auto insertPos = entry.get_position();
    int  startPos, endPos;
    if(entry.get_selection_bounds(startPos, endPos)) {
        insertPos = startPos;
        entry.delete_text(startPos, endPos);
    } else if(insertPos < 0 || insertPos > static_cast<int>(entry.get_text().size()))
        insertPos = entry.get_text().size();
    entry.insert_text(input, -1, insertPos);
    entry.grab_focus();
    if(input.substr(0, 1) == "<" && input.substr(input.length() - 1) == ">") insertPos -= 1;
    entry.set_position(insertPos);
}

void IDSui::onMenuDatabase() {
    auto dialog = new Gtk::Dialog("Select a database...", *this, true);
    dialog->set_default_size(512, 368);

    Gtk::Box dbBox(Gtk::ORIENTATION_VERTICAL);
    dialog->get_content_area()->pack_start(dbBox, Gtk::PACK_EXPAND_WIDGET);
    Gtk::ScrolledWindow dbListBox;
    Gtk::Button         importDBbtn("Import database"), importPrivateBtn("Import private extensions"),
        reimportPrivateBtn("Reimport private extensions"), importIWDSBtn("Import IWDS XML"), cancel("Cancel"), ok("OK");
    Gtk::ButtonBox buttonBox;
    dbBox.pack_start(dbListBox);
    dbBox.pack_start(importDBbtn, Gtk::PACK_SHRINK);
    dbBox.pack_start(importPrivateBtn, Gtk::PACK_SHRINK);
    dbBox.pack_start(reimportPrivateBtn, Gtk::PACK_SHRINK);
    dbBox.pack_start(importIWDSBtn, Gtk::PACK_SHRINK);
    dbListBox.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_ALWAYS);
    dbListBox.add(dbList);
    dbBox.pack_start(buttonBox, Gtk::PACK_SHRINK);
    buttonBox.add(cancel);
    buttonBox.add(ok);
    buttonBox.set_layout(Gtk::BUTTONBOX_END);
    dialog->show_all();
    importDBbtn.signal_clicked().connect(sigc::mem_fun(*this, &IDSui::onSelectDBfile));
    importPrivateBtn.signal_clicked().connect([this]() { onSelectPrivateDBfile(false); });
    reimportPrivateBtn.signal_clicked().connect([this]() { onSelectPrivateDBfile(true); });
    importIWDSBtn.signal_clicked().connect(sigc::mem_fun(*this, &IDSui::onSelectIWDSFile));
    cancel.signal_clicked().connect([dialog]() { dialog->response(Gtk::RESPONSE_CANCEL); });
    ok.signal_clicked().connect([dialog]() { dialog->response(Gtk::RESPONSE_OK); });
    // dialog->signal_delete_event().connect(
    //     sigc::mem_fun(this, &IDSui::onDBdialogDeleted));
    ReadDatabase();
    int result = dialog->run();
    if(result == Gtk::RESPONSE_OK) {
        std::unique_ptr<IDSdatabase> newDB(new IDSdatabase(selectedDB));
        newDB->config.fuzzyMatch.unificationLevel = unificationLevel;
        ParseLocaleSuffixFallbackOrder(localeSuffixFallbackOrder, newDB->config.fuzzyMatch.localeSuffixFallbackOrder);
        newDB->config.fuzzyMatch.defaultRegion    = defaultRegion;
        // std::cout << "Read OK" << std::endl;
        idsdb = std::move(newDB);
        WriteConfigFile();
        // std::cout << "Delete OK" << std::endl;
    }
    delete dialog;
}

void IDSui::onMenuSettings() {
    auto dialog = new Gtk::Dialog("Settings", *this, true);

    Gtk::Box dbBox(Gtk::ORIENTATION_VERTICAL), findFBox(Gtk::ORIENTATION_VERTICAL),
        displayFBox(Gtk::ORIENTATION_VERTICAL), defaultRegionBox(Gtk::ORIENTATION_HORIZONTAL),
        localeSuffixOrderBox(Gtk::ORIENTATION_HORIZONTAL), querySizeBox(Gtk::ORIENTATION_HORIZONTAL),
        resultSizeBox(Gtk::ORIENTATION_HORIZONTAL);
    dialog->set_default_size(512, -1);
    dialog->get_content_area()->pack_start(dbBox, Gtk::PACK_EXPAND_WIDGET);
    Gtk::Button    cancel("Cancel"), ok("OK");
    Gtk::ButtonBox buttonBox;
    dbBox.pack_end(buttonBox, Gtk::PACK_SHRINK);
    buttonBox.add(cancel);
    buttonBox.add(ok);
    buttonBox.set_layout(Gtk::BUTTONBOX_END);
    Gtk::Frame       findFrame("Querying Settings"), displayFrame("Display Settings");
    Gtk::CheckButton symFallback("Character fallback"), suffixRisAltForm("Suffix \"r\" is an alternate ideograph");
    Gtk::Entry       defaultRegion, localeSuffixOrder, fontCfg;
    Gtk::SpinButton  querySize, resultSize;
    Glib::RefPtr<Gtk::Adjustment> queryAdj, resultAdj;
    Gtk::Label      defaultRegionLabel("Default glyphs' region: "),
        localeSuffixOrderLabel("Locale suffix fallback order (>, = same level, . = no suffix): "),
        queryLabel("Font size in query box"),
        resultLabel("Font size in result box"),
        fontLabel("Fonts used for display Han ideographs:\n"
            "More than 1 font can be selected, "
            "making a fallback sequence separated by comma.");
    dbBox.pack_start(findFrame, Gtk::PACK_SHRINK);
    dbBox.pack_start(displayFrame);
    findFrame.add(findFBox);
    displayFrame.add(displayFBox);
    findFBox.pack_start(symFallback, Gtk::PACK_SHRINK);
    findFBox.pack_start(suffixRisAltForm, Gtk::PACK_SHRINK);
    findFBox.pack_start(defaultRegionBox, Gtk::PACK_SHRINK);
    defaultRegionBox.pack_start(defaultRegionLabel, Gtk::PACK_SHRINK);
    defaultRegionBox.pack_start(defaultRegion, Gtk::PACK_SHRINK);
    findFBox.pack_start(localeSuffixOrderBox, Gtk::PACK_SHRINK);
    localeSuffixOrderBox.pack_start(localeSuffixOrderLabel, Gtk::PACK_SHRINK);
    localeSuffixOrderBox.pack_start(localeSuffixOrder, Gtk::PACK_SHRINK);
    displayFBox.pack_start(querySizeBox, Gtk::PACK_SHRINK);
    displayFBox.pack_start(resultSizeBox, Gtk::PACK_SHRINK);
    displayFBox.pack_start(fontLabel, Gtk::PACK_SHRINK);
    displayFBox.pack_start(fontCfg);
    querySizeBox.pack_start(queryLabel, Gtk::PACK_SHRINK);
    querySizeBox.pack_start(querySize);
    resultSizeBox.pack_start(resultLabel, Gtk::PACK_SHRINK);
    resultSizeBox.pack_start(resultSize);
    queryAdj  = Gtk::Adjustment::create(16, 8, 512, 1, 12, 0);
    resultAdj = Gtk::Adjustment::create(20, 8, 512, 1, 12, 0);
    querySize.set_adjustment(queryAdj);
    resultSize.set_adjustment(resultAdj);
    symFallback.set_active(idsdb->config.misc.symFallback);
    suffixRisAltForm.set_active(idsdb->config.misc.suffixRisAltForm);
    localeSuffixOrder.set_text(this->localeSuffixFallbackOrder);
    defaultRegion.set_text(idsdb->config.fuzzyMatch.defaultRegion);
    fontLabel.set_line_wrap();
    fontLabel.set_justify(Gtk::JUSTIFY_LEFT);
    fontLabel.set_xalign(0.0);
    querySize.set_value(queryFontSize);
    resultSize.set_value(resultFontSize);
    fontCfg.set_text(this->fontCfg);
    fontCfg.set_vexpand();
    dialog->set_size_request(512);
    dialog->show_all();
    cancel.signal_clicked().connect([dialog]() { dialog->response(Gtk::RESPONSE_CANCEL); });
    ok.signal_clicked().connect([dialog]() { dialog->response(Gtk::RESPONSE_OK); });
    int result = dialog->run();
    if(result == Gtk::RESPONSE_OK) {
        std::vector<std::string> parsedLocaleOrder;
        if(!ParseLocaleSuffixFallbackOrder(localeSuffixOrder.get_text(), parsedLocaleOrder)) {
            statusBar.push("Invalid locale suffix fallback order.");
            delete dialog;
            return;
        }
        IDSdbConfig& activeConfig             = idsdb->config;
        activeConfig.misc.symFallback         = symFallback.get_active();
        activeConfig.misc.suffixRisAltForm    = suffixRisAltForm.get_active();
        this->defaultRegion                   = defaultRegion.get_text();
        this->localeSuffixFallbackOrder       = localeSuffixOrder.get_text();
        activeConfig.fuzzyMatch.defaultRegion = this->defaultRegion;
        activeConfig.fuzzyMatch.localeSuffixFallbackOrder = parsedLocaleOrder;
        queryFontSize                         = querySize.get_value();
        resultFontSize                        = resultSize.get_value();
        this->fontCfg                         = fontCfg.get_text();
        Pango::FontDescription queryFontCfg, resultFontCfg;
        queryFontCfg.set_family(this->fontCfg);
        queryFontCfg.set_size(queryFontSize * PANGO_SCALE);
        resultFontCfg.set_family(this->fontCfg);
        resultFontCfg.set_size(resultFontSize * PANGO_SCALE);
        entry.override_font(queryFontCfg);
        resultBox.override_font(resultFontCfg);
        WriteConfigFile();
    }
    delete dialog;
}
void IDSui::onMenuAbout() {
    auto dialog = new Gtk::Dialog("About", *this, true);
    dialog->set_default_size(512, -1);
    Gtk::Box dbBox(Gtk::ORIENTATION_VERTICAL);
    dialog->get_content_area()->pack_start(dbBox, Gtk::PACK_SHRINK);
    Gtk::Label  about;
    auto        now         = std::chrono::system_clock::now();
    std::time_t now_c       = std::chrono::system_clock::to_time_t(now);
    std::tm*    local_time  = std::localtime(&now_c);
    auto        currentYear = local_time->tm_year + 1900;
    about.set_markup("Han Ideograph Finder, Version " VERSION "\n"
        "Copyright 2026-" + std::to_string(currentYear) + " Takushun Wu. Licensed under the Apache License, Version 2.0.\n"
        "\n"
        "<a href=\"https://github.com/takushun-wu/\" title=\"GitHub: takushun-wu\">My GitHub Homepage</a>\n"
        "\n"
        "Some ideographs are displayed as a tofu? \nTry <a href=\"https://github.com/takushun-wu/WenJinMincho\" title=\"Click to open the GitHub repo of WenJin Mincho\">WenJin Mincho</a>!");
    // about.set_line_wrap();
    about.set_xalign(0.0);
    about.set_yalign(0.0);
    dbBox.pack_start(about, Gtk::PACK_SHRINK);
    Gtk::Button    ok("OK");
    Gtk::ButtonBox buttonBox;
    buttonBox.set_layout(Gtk::BUTTONBOX_END);
    buttonBox.add(ok);
    dbBox.pack_start(buttonBox, Gtk::PACK_SHRINK);
    dialog->show_all();
    // dialog->resize(512, -1);
    ok.signal_clicked().connect([dialog]() { dialog->response(Gtk::RESPONSE_OK); });
    dialog->run();
    delete dialog;
}

void IDSui::onExit() {
    hide();
    if(worker_.joinable()) worker_.join();
}

void IDSui::onDBTreeViewChanged() {
    auto selection = dbList.get_selection();
    if(auto iter = selection->get_selected()) {
        Glib::ustring name = (*iter)[dbListColumn.dbName];
        // Glib::ustring type = (*iter)[dbListColumn.dbType];
        selectedDB = name;
    }
}

void IDSui::onSelectDBfile() {
    auto dialog = new Gtk::Dialog("Import Database...", *this, true);

    Gtk::Box dbBox(Gtk::ORIENTATION_VERTICAL);
    dialog->get_content_area()->pack_start(dbBox, Gtk::PACK_EXPAND_WIDGET);
    Gtk::Label r1label("Step 1: Select a IDS database file");
    fileNameLabel.set_text("<unselected>"), filename = "";
    Gtk::Label        r3label("Step 2: Select a IDS database type");
    Gtk::Label        r4label("Step 3: Specify a database name");
    Gtk::Label        r5label("");
    Gtk::Button       selectBtn("Select a file..."), cancel("Cancel"), ok("OK");
    Gtk::ButtonBox    buttonBox;
    Gtk::Entry        entry;
    Gtk::ComboBoxText dbType;
    r1label.set_halign(Gtk::ALIGN_START);
    r3label.set_halign(Gtk::ALIGN_START);
    r4label.set_halign(Gtk::ALIGN_START);
    dbBox.pack_start(r1label, Gtk::PACK_SHRINK);
    dbBox.pack_start(fileNameLabel, Gtk::PACK_SHRINK);
    dbBox.pack_start(selectBtn, Gtk::PACK_SHRINK);
    dbBox.pack_start(r3label, Gtk::PACK_SHRINK);
    dbBox.pack_start(dbType, Gtk::PACK_SHRINK);
    dbBox.pack_start(r4label, Gtk::PACK_SHRINK);
    dbBox.pack_start(entry, Gtk::PACK_SHRINK);
    dbBox.pack_start(r5label, Gtk::PACK_SHRINK);
    dbBox.pack_start(buttonBox, Gtk::PACK_SHRINK);
    dbType.append("default", "Default");
    dbType.append("yibai", "Yi Bai flavored");

    buttonBox.add(cancel);
    buttonBox.add(ok);
    buttonBox.set_layout(Gtk::BUTTONBOX_END);
    dialog->show_all();
    selectBtn.signal_clicked().connect(sigc::mem_fun(*this, &IDSui::onSelectDBfileDialog));
    cancel.signal_clicked().connect([dialog]() { dialog->response(Gtk::RESPONSE_CANCEL); });
    ok.signal_clicked().connect([dialog]() { dialog->response(Gtk::RESPONSE_OK); });
ShowDialog:
    int result = dialog->run();
    if(result == Gtk::RESPONSE_OK && filename.size() != 0) {
        std::unique_ptr<IDSdatabase> parseDB(new IDSdatabase(entry.get_text()));
        IDSdbFormat                  selectedMode = IDSDB_DEFAULT;
        if(dbType.get_active_id() == "yibai") selectedMode = IDSDB_YIBAI;
        try {
            const int importResult = parseDB->ImportDB(filename, selectedMode);
            if(importResult != 0) {
                const std::string& error       = parseDB->GetLastError();
                std::string        importError = error.empty() ? "Unable to import the IDS database." : error;
                if(parseDB->GetLastImportReport().HasIssues())
                    importError += "\n" + parseDB->GetLastImportReport().Summary();
                r5label.set_text(importError);
                goto ShowDialog;
            }
        } catch(const std::exception& e) {
            r5label.set_text(e.what());
            goto ShowDialog;
        }
        if(parseDB->isEmpty()) {
            r5label.set_text("File not exist or empty database.");
            goto ShowDialog;
        }

        const IDSimportReport& report = parseDB->GetLastImportReport();
        if(report.HasIssues() || report.cacheTruncations != 0) {
            Gtk::MessageDialog warning(*dialog, "The database was imported with diagnostics.", false,
                Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
            warning.set_secondary_text(report.Summary());
            warning.run();
        }
        ReadDatabase();
    }
    delete dialog;
}

void IDSui::onSelectPrivateDBfile(bool replaceExisting) {
    Gtk::FileChooserDialog fileDialog(
        replaceExisting ? "Reimport private IDS extensions..." : "Import private IDS extensions...",
        Gtk::FILE_CHOOSER_ACTION_OPEN);
    fileDialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    fileDialog.add_button("_Open", Gtk::RESPONSE_OK);
    if(fileDialog.run() != Gtk::RESPONSE_OK) return;

    Gtk::Dialog       formatDialog("Private IDS format", *this, true);
    Gtk::Box          content(Gtk::ORIENTATION_VERTICAL);
    Gtk::Label        formatLabel("Source format");
    Gtk::ComboBoxText format;
    format.append("default", "Default");
    format.append("yibai", "Yi Bai flavored");
    format.set_active_id("default");
    content.pack_start(formatLabel, Gtk::PACK_SHRINK);
    content.pack_start(format, Gtk::PACK_SHRINK);
    formatDialog.get_content_area()->pack_start(content, Gtk::PACK_SHRINK);
    formatDialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    formatDialog.add_button("_Import", Gtk::RESPONSE_OK);
    formatDialog.show_all();
    if(formatDialog.run() != Gtk::RESPONSE_OK) return;

    if(idsdb == nullptr || idsdb->isEmpty()) {
        Gtk::MessageDialog error(*this, "No database is selected.", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        error.run();
        return;
    }

    const IDSdbFormat formatType   = format.get_active_id() == "yibai" ? IDSDB_YIBAI : IDSDB_DEFAULT;
    const int         importResult = replaceExisting ? idsdb->ReimportPrivateDB(fileDialog.get_filename(), formatType)
                                                     : idsdb->ImportPrivateDB(fileDialog.get_filename(), formatType);
    if(importResult != 0) {
        const std::string& errorText   = idsdb->GetLastError();
        std::string        importError = errorText.empty() ? "The private IDS source was rejected." : errorText;
        if(idsdb->GetLastImportReport().HasIssues()) importError += "\n" + idsdb->GetLastImportReport().Summary();
        Gtk::MessageDialog error(
            *this, "Unable to import private IDS extensions.", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        error.set_secondary_text(importError);
        error.run();
        return;
    }

    Gtk::MessageDialog resultDialog(
        *this, "Private IDS extensions imported.", false, Gtk::MESSAGE_INFO, Gtk::BUTTONS_OK, true);
    resultDialog.set_secondary_text(idsdb->GetLastImportReport().Summary());
    resultDialog.run();
    onQuery();
}
void IDSui::onSelectIWDSFile() {
    Gtk::FileChooserDialog dialog("Select an IWDS XML file...", Gtk::FILE_CHOOSER_ACTION_OPEN);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("_Import", Gtk::RESPONSE_OK);

    auto xmlFilter = Gtk::FileFilter::create();
    xmlFilter->set_name("IWDS XML files");
    xmlFilter->add_pattern("*.xml");
    dialog.add_filter(xmlFilter);

    if(dialog.run() != Gtk::RESPONSE_OK) return;
    if(idsdb->ImportIWDSXml(dialog.get_filename()))
        statusBar.push("Imported IWDS unification data.");
    else
        statusBar.push(idsdb->GetLastError());
}
void IDSui::onSelectDBfileDialog() {
    Gtk::FileChooserDialog dialog("Select an IDS database file...", Gtk::FILE_CHOOSER_ACTION_OPEN);
    dialog.add_button("_Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("_OK", Gtk::RESPONSE_OK);

    int result = dialog.run();

    if(result == Gtk::RESPONSE_OK) {
        filename = dialog.get_filename();
        fileNameLabel.set_text(filename);
    }
}

bool IDSui::onDBdialogDeleted(GdkEventAny* event) {
    return false;
}
