#include <chrono>
#include <cstdio>
#include <iostream>

#ifdef _WIN32
#    include <windows.h>
#endif

#include "ids4c/ids4c.h"
#include "ids4c/idsdb.h"

#include "ids4c/ui.h"

int main(int argc, char** argv) {
#ifdef _WIN32
    setlocale(LC_ALL, ".utf-8");
    SetConsoleOutputCP(CP_UTF8);
#endif
    Gtk::Main kit(argc, argv);
    auto      app = IDSapp::create();
    return app->run(argc, argv);
}
