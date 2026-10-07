#ifndef _UI_H
#define _UI_H

#include <atomic>
#include <chrono>
#include <gtkmm.h>
#include <memory>
#include <mutex>
#include <thread>

#include "toml.hpp"

#include "ids4c/idsdb.h"

#define FONTCFG_DEFAULT         "WenJin Mincho Plane 0,WenJin Mincho Plane 2,WenJin Mincho Plane 3"
#define QUERY_FONTSIZE_DEFAULT  16
#define RESULT_FONTSIZE_DEFAULT 20

class DBTreeViewColomns: public Gtk::TreeModel::ColumnRecord {
public:
    Gtk::TreeModelColumn<Glib::ustring> dbName;
    // Gtk::TreeModelColumn<Glib::ustring> dbType;

    DBTreeViewColomns() {
        add(dbName);
        // add(dbType);
    }
};

class IDSui: public Gtk::Window {
public:
    IDSui();
    virtual ~IDSui() = default;

protected:
    Gtk::Box                      mainBox;
    Gtk::Entry                    entry;
    Gtk::Button                   queryButton;
    Gtk::Box                      r1Box, r2Box, r3Box, r4Box, r5Box, r6Box, r1BoxEquivalent;
    std::vector<Gtk::Button*>     inputButton, inputButton2;
    Gtk::ScrolledWindow           resultScrollBox;
    Gtk::TextView                 resultBox;
    Glib::RefPtr<Gtk::TextBuffer> resultBuf;

    Gtk::MenuBar                 menuBar;
    Gtk::MenuItem                menuItemFile, menuItemHelp;
    Gtk::Menu                    menuFile, menuHelp;
    Gtk::MenuItem                menuItemFileDatabase, menuItemFileSettings, menuItemFileExit;
    Gtk::MenuItem                menuItemHelpAbout;
    Gtk::Statusbar               statusBar;
    Gtk::RadioButton::Group      rgFilter;
    Gtk::RadioButton             rbFilterAll, rbFilterLcSuffix, rbFilterLocale, rbFilterLocaleKeepIVS;
    Gtk::CheckButton             cbIgnoreOverlay, cbShowDetails, cbTrackMatchPaths;
    Gtk::CheckButton             cbStrictEnclosureMatch;
    Gtk::Label                   labelOverlapMatchMode;
    Gtk::ComboBoxText            cbOverlapMatchMode;
    Gtk::Label                   labelGlyphDomain, labelUnicodeBlock;
    Gtk::ComboBoxText            cbGlyphDomain;
    Gtk::Button                  unicodeBlockButton;
    Gtk::Label                   labelUnification;
    Gtk::Label                   equivalentQueryLabel;
    Gtk::Label                   equivalentUILabel;
    Gtk::ComboBoxText            cbUnification;
    IWDSUnificationLevel         unificationLevel = IWDS_UNIFICATION_NONE;
    std::vector<IDSunicodeBlock> selectedUnicodeBlocks_;

    Gtk::TreeView                dbList;
    Glib::RefPtr<Gtk::ListStore> dbListStore;
    DBTreeViewColomns            dbListColumn;
    Gtk::Label                   fileNameLabel;
    Glib::ustring                filename;

    std::vector<std::string> databaseList;
    std::string              selectedDB;

    std::string fontCfg;
    std::string defaultRegion;
    std::string localeSuffixFallbackOrder;
    bool        excludeNonEquivalentSameIDS = true;
    unsigned    queryFontSize, resultFontSize;
    toml::value tomlCfg;

    std::unique_ptr<IDSdatabase> idsdb;
    std::vector<Ideograph>       result;
    std::vector<IDSMatchDetail>  resultDetails_;
    std::chrono::milliseconds    queryTime;
    bool                         pendingShowDetails_ = false;
    std::string                  pendingQueryText_;
    IDSqueryOptions              pendingQueryOptions_;

    Glib::Dispatcher  dispatcher_;
    std::thread       worker_;
    std::atomic<bool> running_{false};
    std::mutex        result_mutex_;
    std::string       result_message_;
    std::string       equivalentQueryMessage_;

    void            CreateMenuBar();
    void            CreateInputBar();
    void            CreateDBTreeView();
    void            CreateRBFilters();
    void            ReadDatabase();
    void            ReadConfigFile();
    void            WriteConfigFile();
    IDSqueryOptions GetQueryOptions() const;

    void onQuery();
    void onInput(std::string input);
    void RunQueryTask();
    void onQueryFinished();
    void onQueryOptionsChanged();
    void onSelectUnicodeBlocks();

    void onMenuDatabase();
    void onMenuSettings();
    void onMenuAbout();
    void onExit();

    void onDBTreeViewChanged();
    void onSelectDBfile();
    void onSelectPrivateDBfile(bool replaceExisting);
    void onSelectDBfileDialog();
    void onSelectIWDSFile();
    bool onDBdialogDeleted(GdkEventAny* event);
};

class IDSapp: public Gtk::Application {
public:
    static Glib::RefPtr<IDSapp> create() { return Glib::RefPtr<IDSapp>(new IDSapp()); }

protected:
    IDSapp(): Gtk::Application("com.takushun.HanIdeoFinder", Gio::APPLICATION_FLAGS_NONE) {}

    void on_activate() override {
        if(!m_window) {
            m_window = new IDSui();
            add_window(*m_window);

            m_window->signal_hide().connect([this]() {
                delete m_window;
                m_window = nullptr;
            });
        }
        m_window->present();
    }

private:
    IDSui* m_window = nullptr;
};

#endif

