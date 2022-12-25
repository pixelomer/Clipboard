#include <vector>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <utility>
#include <string_view>
#include <locale>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <csignal>
#include <thread>
#include <condition_variable>
#include <mutex>

#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#define NOMINMAX
#include <Windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

#if defined(X11_AVAILABLE)
#include <X11/Xlib.h>
#endif

#if defined(WAYLAND_AVAILABLE)
#include <wayland-client-core.h>
#endif

namespace fs = std::filesystem;

fs::path filepath;
fs::path original_files_path;
fs::path home_directory;
fs::copy_options opts = fs::copy_options::overwrite_existing | fs::copy_options::recursive | fs::copy_options::copy_symlinks;
std::vector<fs::path> items;
std::vector<std::pair<std::string, std::error_code>> failedItems;
std::string clipboard_name = "0";

std::condition_variable cv;
std::mutex m;
std::jthread indicator;

unsigned int output_length = 0;
unsigned long files_success = 0;
unsigned long directories_success = 0;
unsigned long long bytes_success = 0;

bool stdin_is_tty = true;
bool stdout_is_tty = true;
bool stderr_is_tty = true;

constexpr std::string_view clipboard_version = "0.1.3";

std::array<std::pair<std::string_view, std::string_view>, 8> colors = {{
    {"{red}", "\033[38;5;196m"},
    {"{green}", "\033[38;5;40m"},
    {"{yellow}", "\033[38;5;214m"},
    {"{blue}", "\033[38;5;51m"},
    {"{orange}", "\033[38;5;208m"},
    {"{pink}", "\033[38;5;219m"},
    {"{bold}", "\033[1m"},
    {"{blank}", "\033[0m"}
}};

enum class Action : unsigned int { Cut, Copy, Paste, PipeIn, PipeOut, Clear, Show };
Action action;

template <typename T, size_t N>
class ActionArray : public std::array<T, N> {
public:
    T& operator[](Action index) {
        return std::array<T, N>::operator[](static_cast<unsigned int>(index)); //switch to std::to_underlying when available
    }
};

ActionArray<std::string_view, 7> actions = {{
    "cut",
    "copy",
    "paste",
    "pipe in",
    "pipe out",
    "clear",
    "show"
}};

ActionArray<std::string_view, 7> action_shortcuts = {{
    "ct",
    "cp",
    "p",
    "pin",
    "pout",
    "clr",
    "sh"
}};

ActionArray<std::string_view, 7> doing_action = {{
    "Cutting",
    "Copying",
    "Pasting",
    "Piping in",
    "Piping out",
    "Clearing"
}};

ActionArray<std::string_view, 7> did_action = {{
    "Cut",
    "Copied",
    "Pasted",
    "Piped in",
    "Piped out",
    "Cleared"
}};

std::string_view help_message = "{blue}▏This is Clipboard %s, the cut, copy, and paste system for the command line.{blank}\n"
                                "{blue}{bold}▏How To Use{blank}\n"
                                "{orange}▏clipboard cut (item) [items]{blank} {pink}(This cuts an item or items.){blank}\n"
                                "{orange}▏clipboard copy (item) [items]{blank} {pink}(This copies an item or items.){blank}\n"
                                "{orange}▏clipboard paste{blank} {pink}(This pastes a clipboard.){blank}\n"
                                "{orange}▏clipboard show{blank} {pink}(This shows what's in a clipboard.){blank}\n"
                                "{orange}▏clipboard clear{blank} {pink}(This clears a clipboard's contents.){blank}\n"
                                "{blue}▏You can substitute \"cb\" for \"clipboard\" and use various shorthands for the actions to save time.{blank}\n"
                                "{blue}▏You can also choose which of the 10 clipboards that you have available you want to use by adding a number to the end.{blank}\n"
                                "{blue}{bold}▏Examples{blank}\n"
                                "{orange}▏cb ct Nuclear_Launch_Codes.txt contactsfolder{blank} {pink}(This cuts the following items into the default clipboard, 0.){blank}\n"
                                "{orange}▏clipboard cp1 dogfood.conf{blank} {pink}(This copies the following items into clipboard 1.){blank}\n"
                                "{orange}▏cb p1{blank} {pink}(This pastes clipboard 1.){blank}\n"
                                "{orange}▏cb sh4{blank} {pink}(This shows the contents of clipboard 4.){blank}\n"
                                "{orange}▏cb clr{blank} {pink}(This clears the contents of the default clipboard.){blank}\n"
                                "{blue}▏You can show this help screen anytime with {bold}clipboard -h{blank}{blue}, {bold}clipboard --help{blank}{blue}, or{bold} clipboard help{blank}{blue}.\n"
                                "{blue}▏You can also get more help in our Discord server at {bold}https://discord.gg/J6asnc3pEG{blank}\n"
                                "{blue}▏Copyright (C) 2022 Jackson Huff. Licensed under the GPLv3.{blank}\n"
                                "{blue}▏This program comes with ABSOLUTELY NO WARRANTY. This is free software, and you are welcome to redistribute it under certain conditions.{blank}\n";
std::string_view check_clipboard_status_message = "{blue}• There are items in these clipboards: ";
std::string_view clipboard_contents_message = "{blue}• Here are the first {bold}%i{blank}{blue} items in clipboard {bold}%s{blank}{blue}: {blank}\n";
std::string_view no_clipboard_contents_message = "{blue}• There is currently nothing in the clipboard.{blank}\n";
std::string_view clipboard_action_prompt = "{pink}Add {bold}%s, %s, {blank}{pink}or{bold} %s{blank}{pink} to the end, like {bold}clipboard %s{blank}{pink} to get started, or if you need help, try {bold}clipboard -h{blank}{pink} to show the help screen.{blank}\n";
std::string_view no_valid_action_message = "{red}╳ You did not specify a valid action (\"%s\"), or you forgot to include one. {pink}Try using or adding {bold}cut, copy, {blank}{pink}or {bold}paste{blank}{pink} instead, like {bold}clipboard copy.{blank}\n";
std::string_view choose_action_items_message = "{red}╳ You need to choose something to %s.{pink} Try adding the items you want to %s to the end, like {bold}clipboard %s contacts.txt myprogram.cpp{blank}\n";
std::string_view fix_redirection_action_message = "{red}╳ You can't use the {bold}%s{blank}{red} action with redirection here. {pink}Try removing {bold}%s{blank}{pink} or use {bold}%s{blank}{pink} instead, like {bold}clipboard %s{blank}{pink}.\n";
std::string_view redirection_no_items_message = "{red}╳ You can't specify items when you use redirection. {pink}Try removing the items that come after {bold}clipboard [action].\n";
std::string_view paste_success_message = "{green}✓ Pasted successfully{blank}\n";
std::string_view clear_success_message = "{green}✓ Cleared the clipboard{blank}\n";
std::string_view clear_fail_message = "{red}╳ Failed to clear the clipboard{blank}\n";
std::string_view clipboard_failed_message = "{red}╳ Clipboard couldn't %s these items:{blank}\n";
std::string_view and_more_fails_message = "{red}▏ ...and {bold}%i{blank}{red} more.{blank}\n";
std::string_view and_more_items_message = "{blue}▏ ...and {bold}%i{blank}{blue} more.{blank}\n";
std::string_view fix_problem_message = "{pink}▏ See if you have the needed permissions, or\n"
                                       "▏ try double-checking the spelling of the files or what directory you're in.{blank}\n";
std::string_view not_enough_storage_message = "{red}╳ There won't be enough storage available to paste all your items (%gkB to paste, %gkB available).{blank}{pink} Try double-checking what items you've selected or delete some files to free up space.{blank}\n";
std::string_view working_message = "{yellow}• %s... %i%s %s{blank}\r";
std::string_view pipe_success_message = "{green}✓ %s %i bytes{blank}\n";
std::string_view one_item_success_message = "{green}✓ %s %s{blank}\n";
std::string_view multiple_files_success_message = "{green}✓ %s %i files{blank}\n";
std::string_view multiple_directories_success_message = "{green}✓ %s %i directories{blank}\n";
std::string_view multiple_files_directories_success_message = "{green}✓ %s %i files and %i directories{blank}\n";
std::string_view internal_error_message = "{red}╳ Internal error: %s\n▏ This is probably a bug, or you might be lacking permissions on this system.{blank}\n";

#include "langs.hpp"

std::string replaceColors(const std::string_view& str) {
    std::string temp(str); //a string to do scratch work on
    for (const auto& key : colors) { //iterate over all the possible colors to replace
        for (int i = 0; (i = temp.find(key.first, i)) != std::string::npos; i += key.second.length()) { //
            temp.replace(i, key.first.length(), key.second);
        }
    }
    return temp;
}

void setupSignals() {
    signal(SIGINT, [](int dummy) {
        indicator.request_stop();
        fprintf(stderr, "\r%*s\r", output_length, "");
        fprintf(stderr, replaceColors("{green}✓ Cancelled %s{blank}\n").data(), actions[action].data());
        fflush(stderr);
        exit(1);
    });
}

void setupVariables(const int argc, char *argv[]) {
    #if defined(_WIN64) || defined (_WIN32)
	HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE); //Windows terminal color compatibility
	DWORD dwMode = 0;
	GetConsoleMode(hOut, &dwMode);
	if (!SetConsoleMode(hOut, (dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT))) {
        for (auto& key : colors) {
            key.second = "";
        }
	}
	SetConsoleOutputCP(CP_UTF8); //fix broken accents on Windows
    #endif

    stdin_is_tty = isatty(fileno(stdin));
    stdout_is_tty = isatty(fileno(stdout));
    stderr_is_tty = isatty(fileno(stderr));

    fs::path perma_directory;

    bool use_perma_clip = false;

    #if defined(_WIN64) || defined (_WIN32)
    home_directory = getenv("USERPROFILE");
    #else
    home_directory = getenv("HOME");
    #endif

    if (argc >= 2) {
        clipboard_name = argv[1];
        if (clipboard_name.find_first_of("-_:;|") != std::string::npos) {
            clipboard_name = clipboard_name.substr(clipboard_name.find_first_of("-_:;|") + 1);
            use_perma_clip = true;
        } else {
            clipboard_name = clipboard_name.substr(clipboard_name.find_last_not_of("0123456789") + 1);
        }
        
        if (clipboard_name.empty()) {
            clipboard_name = "0";
        } else {
            argv[1][strlen(argv[1]) - (clipboard_name.length() + use_perma_clip)] = '\0';
        }
    }

    if (use_perma_clip) {
        filepath = home_directory / ".clipboard" / clipboard_name / perma_directory;
    } else {
        if (getenv("TMPDIR") != nullptr) {
            filepath = fs::path(getenv("TMPDIR")) / "Clipboard" / clipboard_name;
        } else {
            filepath = fs::temp_directory_path() / "Clipboard" / clipboard_name;
        }
    }

    original_files_path = filepath.parent_path() / (clipboard_name + ".files");

    for (int i = 2; i < argc; i++) {
        items.push_back(argv[i]);
    }

    if (getenv("NO_COLOR") != nullptr && getenv("FORCE_COLOR") == nullptr) {
        for (auto& key : colors) {
            key.second = "";
        }
    }

    try {
        if (std::locale("").name().substr(0, 2) == "es") {
            setLanguageES();
        } else if (std::locale("").name().substr(0, 2) == "pt") {
            setLanguagePT();
        } else if (std::locale("").name().substr(0, 2) == "tr") {
            setLanguageTR();
        }
    } catch (...) {}
}

void checkFlags(const int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help") || (argc >= 2 && !strcmp(argv[1], "help"))) {
            printf(replaceColors(help_message).data(), clipboard_version.data());
            exit(0);
        }
    }
}

void createTempDirectory() {
    if (!fs::is_directory(filepath)) {
        fs::create_directories(filepath);
    }
}

void syncWithGUIClipboard() {
    #if defined(X11_AVAILABLE)
    Display* dpy;
    Window root;
    int screen;
    Atom selection;

    dpy = XOpenDisplay(NULL);
    if (dpy == NULL) {
        return;
    }
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);
    selection = XInternAtom(dpy, "CLIPBOARD", False);
    



    #endif

    #if defined(WAYLAND_AVAILABLE)

    #endif

    #if defined(_WIN32) || defined(_WIN64)
    
    #elif defined(__APPLE__)

    #endif
}

void showClipboardStatus() {
    std::vector<std::pair<std::string, bool>> clipboards_with_contents;
    for (const auto& entry : fs::directory_iterator(filepath.parent_path())) {
        if (fs::is_directory(entry) && !fs::is_empty(entry)) {
            clipboards_with_contents.push_back({entry.path().filename().string(), false});
        }
    }
    for (const auto& entry : fs::directory_iterator(home_directory / ".clipboard")) {
        if (fs::is_directory(entry) && !fs::is_empty(entry)) {
            clipboards_with_contents.push_back({entry.path().filename().string(), true});
        }
    }
    std::sort(clipboards_with_contents.begin(), clipboards_with_contents.end());
    if (clipboards_with_contents.empty()) {
        printf("%s", replaceColors(no_clipboard_contents_message).data());
        printf(replaceColors(clipboard_action_prompt).data(), actions[Action::Cut].data(), actions[Action::Copy].data(), actions[Action::Paste].data(), actions[Action::Copy].data());
    } else {
        printf("%s", replaceColors(check_clipboard_status_message).data());
        for (int clipboard = 0; clipboard < clipboards_with_contents.size(); clipboard++) {
            printf(replaceColors("{bold}%s{blank}{blue}").data(), (clipboards_with_contents.at(clipboard).first + (clipboards_with_contents.at(clipboard).second ? " (perma)" : "")).data());
            if (clipboard != clipboards_with_contents.size() - 1) {
                printf(", ");
            }
        }
        printf("\n");
        printf(replaceColors(clipboard_action_prompt).data(), actions[Action::Cut].data(), actions[Action::Copy].data(), actions[Action::Paste].data(), actions[Action::Copy].data());
    }
}

void showClipboardContents() {
    if (fs::is_directory(filepath) && !fs::is_empty(filepath)) {
        unsigned int total_items = 0;
        for (const auto& entry : fs::directory_iterator(filepath)) {
            total_items++;
        }
        printf(replaceColors(clipboard_contents_message).data(), std::min(static_cast<unsigned int>(20), total_items), clipboard_name.data());
        auto it = fs::directory_iterator(filepath);
        for (int i = 0; i < std::min(static_cast<unsigned int>(20), total_items); i++) {
            printf(replaceColors("{blue}▏ {bold}%s{blank}\n").data(), it->path().filename().string().data());
            if (i == 19 && total_items > 20) {
                printf(replaceColors(and_more_items_message).data(), total_items - 20);
            }
            it++;
        }
    } else {
        printf(replaceColors(no_clipboard_contents_message).data(), actions[Action::Cut].data(), actions[Action::Copy].data(), actions[Action::Paste].data(), actions[Action::Copy].data());
    }
}

void setupAction(const int argc, char *argv[]) {
    if (argc >= 2) {
        if (!strcmp(argv[1], actions[Action::Cut].data()) || !strcmp(argv[1], action_shortcuts[Action::Cut].data())) {
            action = Action::Cut;
            if (!stdin_is_tty || !stdout_is_tty) {
                fprintf(stderr, replaceColors(fix_redirection_action_message).data(), actions[action].data(), actions[action].data(), actions[Action::Copy].data(), actions[Action::Copy].data());
                exit(1);
            }
        } else if (!strcmp(argv[1], actions[Action::Copy].data()) || !strcmp(argv[1], action_shortcuts[Action::Copy].data())) {
            action = Action::Copy;
            if (!stdin_is_tty) {
                action = Action::PipeIn;
            } else if (!stdout_is_tty) {
                fprintf(stderr, replaceColors(fix_redirection_action_message).data(), actions[action].data(), actions[action].data(), actions[Action::Paste].data(), actions[Action::Paste].data());
                exit(1);
            }
        } else if (!strcmp(argv[1], actions[Action::Paste].data()) || !strcmp(argv[1], action_shortcuts[Action::Paste].data())) {
            action = Action::Paste;
            if (!stdout_is_tty) {
                action = Action::PipeOut;
            } else if (!stdin_is_tty) {
                fprintf(stderr, replaceColors(fix_redirection_action_message).data(), actions[action].data(), actions[action].data(), actions[Action::Copy].data(), actions[Action::Copy].data());
                exit(1);
            }
        } else if (!strcmp(argv[1], actions[Action::Show].data()) || !strcmp(argv[1], action_shortcuts[Action::Show].data())) {
            action = Action::Show;
        } else if (!strcmp(argv[1], actions[Action::Clear].data()) || !strcmp(argv[1], action_shortcuts[Action::Clear].data())) {
            action = Action::Clear;
            if (!stdin_is_tty) {
                fprintf(stderr, replaceColors(fix_redirection_action_message).data(), actions[action].data(), actions[action].data(), actions[Action::Cut].data(), actions[Action::Cut].data());
                exit(1);
            } else if (!stdout_is_tty) {
                fprintf(stderr, replaceColors(fix_redirection_action_message).data(), actions[action].data(), actions[action].data(), actions[Action::Paste].data(), actions[Action::Paste].data());
                exit(1);
            }
        } else if (!strcmp(argv[1], "ee")) {
            printf("%s", replaceColors("{bold}{blue}https://youtu.be/Lg_Pn45gyMs\n{blank}").data());
            exit(0);
        } else {
            printf(replaceColors(no_valid_action_message).data(), argv[1]);
            exit(1);
        }
    } else if (!stdin_is_tty) {
        action = Action::PipeIn;
    } else if (!stdout_is_tty) {
        action = Action::PipeOut;
    } else {
        showClipboardStatus();
        exit(0);
    }
    if (action == Action::PipeIn || action == Action::PipeOut) {
        if (argc >= 3) {
            fprintf(stderr, "%s", replaceColors(redirection_no_items_message).data());
            exit(1);
        }
    }
}

void checkForNoItems() {
    if ((action == Action::Cut || action == Action::Copy) && items.size() < 1) {
        printf(replaceColors(choose_action_items_message).data(), actions[action].data(), actions[action].data(), actions[action].data());
        exit(1);
    }
    if (action == Action::Paste && fs::is_empty(filepath)) {
        showClipboardStatus();
        exit(0);
    }
}

void setupIndicator(std::stop_token st) {
    std::unique_lock<std::mutex> lock(m);
    const std::array<std::string_view, 10> spinner_steps{"━       ", "━━      ", " ━━     ", "  ━━    ", "   ━━   ", "    ━━  ", "     ━━ ", "      ━━", "       ━", "        "};
    static unsigned int percent_done = 0;
    if ((action == Action::Cut || action == Action::Copy) && stderr_is_tty) {
        static unsigned long items_size = items.size();
        for (int i = 0; !st.stop_requested(); i == 9 ? i = 0 : i++) {
            percent_done = ((files_success + directories_success + failedItems.size()) * 100) / items_size;
            output_length = fprintf(stderr, replaceColors(working_message).data(), doing_action[action].data(), percent_done, "%", spinner_steps.at(i).data());
            fflush(stderr);
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
    } else if ((action == Action::PipeIn || action == Action::PipeOut) && stderr_is_tty) {
        for (int i = 0; !st.stop_requested(); i == 9 ? i = 0 : i++) {
            output_length = fprintf(stderr, replaceColors(working_message).data(), doing_action[action].data(), bytes_success, "B", spinner_steps.at(i).data());
            fflush(stderr);
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
    } else if (action == Action::Paste && stderr_is_tty) {
        static unsigned long items_size = 0;
        if (items_size == 0) {
            for (const auto& f : fs::directory_iterator(filepath)) {
                items_size++;
            }
            if (items_size == 0) {
                items_size = 1;
            }
        }
        for (int i = 0; !st.stop_requested(); i == 9 ? i = 0 : i++) {
            percent_done = ((files_success + directories_success + failedItems.size()) * 100) / items_size;
            output_length = fprintf(stderr, replaceColors(working_message).data(), doing_action[action].data(), percent_done, "%", spinner_steps.at(i).data());
            fflush(stderr);
            cv.wait_for(lock, std::chrono::milliseconds(50));
        }
    } else if (stderr_is_tty) {
        output_length = fprintf(stderr, replaceColors(working_message).data(), doing_action[action].data(), 0, "%", "");
        fflush(stderr);
    }
}

unsigned long long calculateTotalItemSize() {
    unsigned long long total_item_size = 0;
    for (const auto& i : items) {
        try {
            if (fs::is_directory(i)) {
                for (const auto& entry : fs::recursive_directory_iterator(i)) {
                    if (fs::is_regular_file(entry)) {
                        total_item_size += fs::file_size(entry);
                    } else {
                        total_item_size += 16;
                    }
                }
            } else if (fs::is_regular_file(i)) {
                total_item_size += fs::file_size(i);
            } else {
                total_item_size += 16;
            }
        } catch (const fs::filesystem_error& e) {
            failedItems.emplace_back(i.string(), e.code());
        }
    }   
    return total_item_size;
}

void checkItemSize() {
    const unsigned long long space_available = fs::space(filepath).available;
    unsigned long long total_item_size = 0;
    if (action == Action::Cut || action == Action::Copy) {
        total_item_size = calculateTotalItemSize();
        if (total_item_size > (space_available / 2)) {
            printf(replaceColors(not_enough_storage_message).data(), total_item_size / 1024.0, space_available / 1024.0);
            indicator.request_stop();
            cv.notify_one();
            exit(1);
        }
    }
}

void clearTempDirectory() {
    if (action != Action::Paste) {
        fs::remove(original_files_path);
    }
    if (action == Action::Copy || action == Action::Cut || action == Action::PipeIn || action == Action::Clear) {
        for (const auto& entry : fs::directory_iterator(filepath)) {
            fs::remove_all(entry.path());
        }
    }
}

void copyFiles() {
    std::ofstream originalFiles;
    if (action == Action::Cut) {
        originalFiles.open(original_files_path);
    }
    for (const auto& f : items) {
        auto copyItem = [&](const bool use_regular_copy = false) {
            if (fs::is_directory(f)) {
                if (f.filename() == "") {
                    fs::create_directories(filepath / f.parent_path().filename());
                    fs::copy(f, filepath / f.parent_path().filename(), opts);
                } else {
                    fs::create_directories(filepath / f.filename());
                    fs::copy(f, filepath / f.filename(), opts);
                }
                directories_success++;
            } else {
                fs::copy(f, filepath / f.filename(), use_regular_copy ? opts : opts | fs::copy_options::create_hard_links);
                files_success++;
            }
            if (action == Action::Cut) {
                originalFiles << fs::absolute(f).string() << std::endl;
            }
        };
        try {
            copyItem();
        } catch (const fs::filesystem_error& e) {
            try {
                copyItem(true);
            } catch (const fs::filesystem_error& e) {
                failedItems.emplace_back(f.string(), e.code());
            }
        }
    }
}

void removeOldFiles() {
    if (fs::is_regular_file(original_files_path)) {
        std::ifstream files(original_files_path);
        std::string line;
        while (std::getline(files, line)) {
            try {
                fs::remove_all(line);
            } catch (const fs::filesystem_error& e) {
                failedItems.emplace_back(line, e.code());
            }
        }
        files.close();
        if (failedItems.size() == 0) {
            fs::remove(original_files_path);
        }
        action = Action::Cut;
    }
}

int getUserDecision(const std::string& item) {
    if (!stderr_is_tty || !stdin_is_tty || !stdout_is_tty) {
        return 2;
    }
    fprintf(stderr, replaceColors("{yellow}• The item {bold}%s{blank}{yellow} already exists here. Would you like to replace it? {pink}Add {bold}all {blank}{pink}or {bold}a{blank}{pink} to use this decision for all items. {bold}[(y)es/(n)o)] ").data(), item.data());
    std::string decision;
    while (true) {
        std::getline(std::cin, decision);
        fprintf(stderr, "%s", replaceColors("{blank}").data());
        if (decision == "y" || decision == "yes") {
            return 1;
        } else if (decision == "ya" || decision == "yesall") {
            return 2;
        } else if (decision == "n" || decision == "no") {
            return -1;
        } else if (decision == "na" || decision == "noall") {
            return -2;
        } else {
            fprintf(stderr, "%s", replaceColors("{red}╳ Sorry, that wasn't a valid choice. Try again: {blank}{pink}{bold}[(y)es/(n)o)] ").data());
        }
    }
}

void pasteFiles() {
    int user_decision = 0;
    for (const auto& f : fs::directory_iterator(filepath)) {
        auto pasteItem = [&](const bool use_regular_copy = false) {
            if (fs::is_directory(f)) {
                fs::copy(f, fs::current_path() / f.path().filename(), opts);
                directories_success++;
            } else {
                fs::copy(f, fs::current_path() / f.path().filename(), use_regular_copy ? opts : opts | fs::copy_options::create_hard_links);
                files_success++;
            }
        };
        try {
            if (fs::exists(fs::current_path() / f.path().filename())) {
                switch (user_decision) {
                    case -2:
                        break;
                    case -1:
                    case 0:
                    case 1:
                        indicator.request_stop();
                        user_decision = getUserDecision(f.path().filename().string());
                        indicator = std::jthread(setupIndicator);
                        break;
                    case 2:
                        pasteItem();
                        break;
                }
                switch (user_decision) {
                    case -1:
                        break;
                    case 1:
                        pasteItem();
                }
            } else {
                pasteItem();
            }
        } catch (const fs::filesystem_error& e) {
            try {
                pasteItem(true);
            } catch (const fs::filesystem_error& e) {
                failedItems.emplace_back(f.path().filename().string(), e.code());
            }
        }
    }
    removeOldFiles();
}

void pipeIn() {
    std::ofstream file(filepath / "clipboard.txt");
    std::string buffer;
    std::string line;
    for (int i = 0; std::getline(std::cin, line); i == 19 ? i = 0 : i++) {
        buffer += line + "\n";
        bytes_success += line.size() + 1;
        if (i == 19) {
            file << buffer;
            buffer = "";
        }
    }
    if (buffer != "") {
        file << buffer;
    }
    file.close();
}

void pipeOut() {
    std::string line;
    for (const auto& entry : fs::recursive_directory_iterator(filepath)) {
        std::ifstream file(entry.path());
        while (std::getline(file, line)) {
            printf("%s\n", line.data());
            bytes_success += line.size() + 1;
        }
        file.close();
    }
}

void clearClipboard() {
    if (fs::is_empty(filepath)) {
        printf("%s", replaceColors(clear_success_message).data());
    } else {
        printf("%s", replaceColors(clear_fail_message).data());
    }
}

void performAction() {
    switch (action) {
        case Action::Copy:
        case Action::Cut:
            copyFiles();
            break;
        case Action::Paste:
            pasteFiles();
            break;
        case Action::PipeIn:
            pipeIn();
            break;
        case Action::PipeOut:
            pipeOut();
            break;
        case Action::Clear:
            clearClipboard();
            break;
        case Action::Show:
            showClipboardContents();
            break;
    }
}

void showFailures() {
    if (failedItems.size() > 0) {
        printf(replaceColors(clipboard_failed_message).data(), actions[action].data());
        for (int i = 0; i < std::min(5, static_cast<int>(failedItems.size())); i++) {
            printf(replaceColors("{red}▏ {bold}%s{blank}{red}: %s{blank}\n").data(), failedItems.at(i).first.data(), failedItems.at(i).second.message().data());
            if (i == 4 && failedItems.size() > 5) {
                printf(replaceColors(and_more_fails_message).data(), int(failedItems.size() - 5));
            }
        }
        printf("%s", replaceColors(fix_problem_message).data());
    }
}

void showSuccesses() {
    if (action == Action::PipeIn || action == Action::PipeOut && stderr_is_tty) {
        fprintf(stderr, replaceColors(pipe_success_message).data(), did_action[action].data(), bytes_success);
        return;
    }
    if ((files_success == 1 && directories_success == 0) || (files_success == 0 && directories_success == 1)) {
        if (action == Action::Paste) {
            printf("%s", replaceColors(paste_success_message).data());
        } else {
            printf(replaceColors(one_item_success_message).data(), did_action[action].data(), items.at(0).string().data());
        }
    } else {
        if ((files_success > 1) && (directories_success == 0)) {
            printf(replaceColors(multiple_files_success_message).data(), did_action[action].data(), files_success);
        } else if ((files_success == 0) && (directories_success > 1)) {
            printf(replaceColors(multiple_directories_success_message).data(), did_action[action].data(), directories_success);
        } else if ((files_success >= 1) && (directories_success >= 1)) {
            printf(replaceColors(multiple_files_directories_success_message).data(), did_action[action].data(), files_success, directories_success);
        }
    }
}

int main(int argc, char *argv[]) {
    try {
        setupSignals();

        setupVariables(argc, argv);

        checkFlags(argc, argv);

        createTempDirectory();

        syncWithGUIClipboard();

        setupAction(argc, argv);

        checkForNoItems();

        indicator = std::jthread(setupIndicator);

        checkItemSize();

        clearTempDirectory();

        performAction();

        indicator.request_stop();
        cv.notify_one();

        if (stderr_is_tty) {
            fprintf(stderr, "\r%*s\r", output_length, "");
            fflush(stderr);
        }

        showFailures();

        showSuccesses();
    } catch (const std::exception& e) {
        fprintf(stderr, replaceColors(internal_error_message).data(), e.what());
        indicator.request_stop();
        cv.notify_one();
        exit(1);
    }
    return 0;
}