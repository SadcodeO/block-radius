#define Py_LIMITED_API 0x03090000
#include <Python.h>

#include "json.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;
using Lines = std::vector<std::string>;
using Workers = std::vector<std::vector<int>>;

struct Test {
    int number, milliseconds, coverage_index;
};

Lines read_lines(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open " + path.string());
    Lines lines;
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    if (in.bad()) throw std::runtime_error("Read failed: " + path.string());
    return lines;
}

Workers schedule(const std::vector<Test>& sorted_tests) {
    Workers best;
    long long best_cost = std::numeric_limits<long long>::max();
    for (size_t count = 1; count <= 8 && count <= sorted_tests.size(); ++count) {
        Workers candidate(count);
        std::vector<long long> loads(count);
        for (const auto& test : sorted_tests) {
            auto slot = std::min_element(loads.begin(), loads.end());
            candidate[slot - loads.begin()].push_back(test.number);
            *slot += test.milliseconds;
        }
        std::erase_if(candidate, [](const auto& worker) { return worker.empty(); });
        long long cost = *std::max_element(loads.begin(), loads.end()) + 2000LL * candidate.size();
        if (cost < best_cost) {
            best_cost = cost;
            best = std::move(candidate);
        }
    }
    return best;
}

bool compiles(const std::string& source, const std::string& name) {
    if (source.find('\0') != std::string::npos) return false;
    if (!Py_IsInitialized()) Py_Initialize();
    PyObject* code = Py_CompileString(source.c_str(), name.c_str(), Py_file_input);
    if (!code) {
        PyErr_Clear();
        return false;
    }
    Py_DECREF(code);
    return true;
}

using Cache = std::map<std::string, Lines>;
struct Selection {
    std::set<int> indices;
    bool include_unmeasured = false;
    bool include_new = false;
};

int hex_value(char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; }

void add_coverage(const json& encoded, std::set<int>& tests) {
    if (encoded.is_array()) {
        for (const auto& index : encoded) tests.insert(index.get<int>());
        return;
    }
    const auto& hex = encoded.get_ref<const std::string&>();
    int base = 0;
    for (auto it = hex.rbegin(); it != hex.rend(); ++it, base += 4) {
        int bits = hex_value(*it);
        for (int bit = 0; bit < 4; ++bit)
            if (bits & (1 << bit)) tests.insert(base + bit);
    }
}

// A '#' inside a multiline string is data. Compare parsed trees before
// skipping a patch that appears to consist entirely of comments and blanks.
bool same_syntax_tree(const Lines& original, const std::string& patched) {
    std::string source;
    for (const auto& line : original) source.append(line).push_back('\n');
    PyObject* ast = PyImport_ImportModule("ast");
    if (!ast) { PyErr_Clear(); return false; }
    PyObject* before = PyObject_CallMethod(ast, "parse", "s", source.c_str());
    PyObject* after = before ? PyObject_CallMethod(ast, "parse", "s", patched.c_str()) : nullptr;
    bool equal = false;
    if (before && after) {
        if (PyObject_HasAttrString(ast, "compare")) {
            PyObject* result = PyObject_CallMethod(ast, "compare", "OO", before, after);
            equal = result && PyObject_IsTrue(result) == 1;
            Py_XDECREF(result);
        } else {
            PyObject* left = PyObject_CallMethod(ast, "dump", "O", before);
            PyObject* right = left ? PyObject_CallMethod(ast, "dump", "O", after) : nullptr;
            equal = left && right && PyObject_RichCompareBool(left, right, Py_EQ) == 1;
            Py_XDECREF(left);
            Py_XDECREF(right);
        }
    }
    Py_XDECREF(before);
    Py_XDECREF(after);
    Py_DECREF(ast);
    PyErr_Clear();
    return equal;
}

bool inspect_file(const Lines& diff, size_t begin, size_t end, const fs::path& root,
                  const json& coverage, Cache& cache, Selection& selected) {
    std::string old_path, new_path;
    size_t pos = begin;
    while (pos < end && !diff[pos].starts_with("@@ ")) {
        const auto& line = diff[pos++];
        if (line.starts_with("--- a/")) old_path = line.substr(6);
        else if (line.starts_with("+++ b/")) new_path = line.substr(6);
        else if (!line.starts_with("index ")) return false;
    }
    if (pos == end || old_path != new_path) return false;
    const auto& files = coverage.at("files");
    bool fixture = coverage.contains("fixtures") && coverage.at("fixtures").contains(old_path);
    if (!fixture && !files.contains(old_path)) return false;
    if (!cache.contains(old_path)) cache.emplace(old_path, read_lines(root / old_path));
    const auto& original = cache.at(old_path);
    size_t cursor = 0;
    std::ostringstream rebuilt;
    bool comments_only = !fixture;
    bool standalone_insertions = comments_only && coverage.contains("continued")
        && coverage.at("continued").contains(old_path);
    bool localized = true;
    std::set<int> file_tests;
    static const std::regex hunk(R"(^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@.*$)");
    while (pos < end) {
        std::smatch match;
        if (!std::regex_match(diff[pos++], match, hunk)) return false;
        int old_count = match[2].matched ? std::stoi(match[2]) : 1;
        int new_count = match[4].matched ? std::stoi(match[4]) : 1;
        int start = std::stoi(match[1]) - (old_count != 0);
        if (start < 0 || size_t(start) < cursor || size_t(start) > original.size()) return false;
        while (cursor < size_t(start)) rebuilt << original[cursor++] << '\n';
        int low = std::numeric_limits<int>::max(), high = -1;
        while (pos < end && !diff[pos].starts_with("@@ ")) {
            const auto& line = diff[pos++];
            if (line == "\\ No newline at end of file") continue;
            if (line.empty()) return false;
            char op = line.front();
            if (op != '+' && op != '-' && op != ' ') return false;
            std::string_view body(line.data() + 1, line.size() - 1);
            if (op != '+') {
                if (cursor >= original.size() || original[cursor++] != body) return false;
                --old_count;
            }
            if (op != '-') {
                rebuilt << body << '\n';
                --new_count;
            }
            if (op != ' ') {
                if (!fixture && cursor <= 2) {
                    if (body.find("coding") != body.npos) return false;
                    for (size_t i = 0; i < std::min<size_t>(2, original.size()); ++i)
                        if (original[i].find("coding") != std::string::npos) return false;
                }
                auto first = body.find_first_not_of(" \t");
                if (first != body.npos && (body[first] != '#' || body.find("coding") != body.npos))
                    comments_only = false;
                if (!comments_only || op != '+') standalone_insertions = false;
                if (standalone_insertions && std::any_of(body.begin(), body.end(), [](unsigned char c) {
                        return c != '\t' && (c < 32 || c > 126);
                    })) standalone_insertions = false;
                if (standalone_insertions) {
                    const auto& mask = coverage.at("continued").at(old_path).get_ref<const std::string&>();
                    if (cursor / 4 < mask.size())
                        standalone_insertions = !(hex_value(mask[mask.size() - 1 - cursor / 4])
                                                  & (1 << (cursor % 4)));
                }
                low = std::min(low, static_cast<int>(cursor));
                high = std::max(high, static_cast<int>(cursor) + (op == '+'));
            }
        }
        if (high < low || old_count != 0 || new_count != 0) return false;
        if (fixture) continue;
        bool enclosed = false;
        for (const auto& range : files.at(old_path)) {
            if (range[0].get<int>() <= low && high <= range[1].get<int>()) {
                enclosed = true;
                add_coverage(range[2], file_tests);
            }
        }
        localized &= enclosed;
    }
    if (fixture) {
        for (const auto& index : coverage.at("fixtures").at(old_path))
            selected.indices.insert(index.get<int>());
        selected.include_new = true;
        if (coverage.contains("optimizer_readers") && coverage.at("optimizer_readers").contains(old_path))
            add_coverage(coverage.at("optimizer_readers").at(old_path), selected.indices);
        else
            selected.include_unmeasured = true;
        return true;
    }
    if (standalone_insertions) return true;
    if (!localized && !comments_only) return false;
    while (cursor < original.size()) rebuilt << original[cursor++] << '\n';
    auto patched = rebuilt.str();
    if (!compiles(patched, old_path)) return false;
    if (comments_only && same_syntax_tree(original, patched)) return true;
    if (!localized) return false;
    selected.indices.insert(file_tests.begin(), file_tests.end());
    selected.include_unmeasured = true;
    return true;
}

std::optional<Selection> affected(const Lines& diff, const fs::path& root,
                                      const json& files, Cache& cache) {
    Selection selected;
    if (diff.empty() || !diff.front().starts_with("diff --git ")) return {};
    for (size_t pos = 0; pos < diff.size();) {
        size_t end = pos + 1;
        while (end < diff.size() && !diff[end].starts_with("diff --git ")) ++end;
        if (!inspect_file(diff, pos + 1, end, root, files, cache, selected)) return {};
        pos = end;
    }
    return selected;
}

int main(int argc, char** argv) try {
    if (argc != 1 && argc != 4) throw std::runtime_error("Usage: selector DATA SRC COVERAGE_JSON");
    fs::path data = argc == 4 ? argv[1] : "/data";
    fs::path root = argc == 4 ? argv[2] : "/src";
    auto coverage = json::parse(std::ifstream(argc == 4 ? argv[3] : "/app/coverage.json"));
    std::map<std::string, int> indices;
    for (const auto& name : coverage.at("ids")) indices.emplace(name.get<std::string>(), indices.size());
    auto unknown = coverage.at("unknown").get<std::set<int>>();
    std::vector<Test> tests;
    auto catalog = json::parse(std::ifstream(data / "tests.json"));
    for (const auto& entry : catalog.at("tests")) {
        auto found = indices.find(entry.at("id").get<std::string>());
        tests.push_back({entry.at("number").get<int>(),
                         int(std::lround(entry.at("seconds").get<double>() * 1000)),
                         found == indices.end() ? -1 : found->second});
    }
    std::stable_sort(tests.begin(), tests.end(), [](const Test& a, const Test& b) {
        return a.milliseconds > b.milliseconds;
    });
    auto all = schedule(tests);
    std::set<fs::path> patches;
    for (const auto& entry : fs::directory_iterator(data / "commits"))
        if (entry.path().extension() == ".patch") patches.insert(entry.path());
    Cache cache;
    for (const auto& patch : patches) {
        std::optional<Selection> selected;
        try {
            selected = affected(read_lines(patch), root, coverage, cache);
        } catch (const std::exception& error) {
            std::cerr << patch.filename() << ": " << error.what() << '\n';
        }
        Workers workers = all;
        if (selected) {
            std::vector<Test> chosen;
            for (const auto& t : tests)
                if ((selected->include_unmeasured &&
                     (t.coverage_index < 0 || unknown.contains(t.coverage_index)))
                    || (selected->include_new && t.coverage_index < 0)
                    || selected->indices.contains(t.coverage_index)) chosen.push_back(t);
            workers = schedule(chosen);
        }
        std::cout << json{{"commit", patch.stem().string()}, {"workers", workers}}.dump() << '\n';
    }
    if (Py_IsInitialized()) Py_FinalizeEx();
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
}