// SPDX-License-Identifier: MIT
//
// Tiny "--key value" / "--flag" command-line parser shared by the apps and
// benchmarks. Intentionally minimal: no dependency on a CLI library.
#pragma once

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace mds {

class CliArgs {
public:
    CliArgs(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            if (a.rfind("--", 0) != 0) throw std::invalid_argument("unexpected argument: " + a);
            a = a.substr(2);
            const auto eq = a.find('=');
            if (eq != std::string::npos) {
                values_[a.substr(0, eq)] = a.substr(eq + 1);
            } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
                values_[a] = argv[++i];
            } else {
                flags_.insert(a);
            }
        }
    }

    [[nodiscard]] bool has(const std::string& k) const {
        return flags_.count(k) != 0 || values_.count(k) != 0;
    }
    [[nodiscard]] std::string str(const std::string& k, const std::string& def = "") const {
        auto it = values_.find(k);
        return it == values_.end() ? def : it->second;
    }
    [[nodiscard]] std::int64_t i64(const std::string& k, std::int64_t def) const {
        auto it = values_.find(k);
        return it == values_.end() ? def : std::stoll(it->second);
    }
    [[nodiscard]] double f64(const std::string& k, double def) const {
        auto it = values_.find(k);
        return it == values_.end() ? def : std::stod(it->second);
    }

private:
    std::map<std::string, std::string> values_;
    std::set<std::string> flags_;
};

}  // namespace mds
