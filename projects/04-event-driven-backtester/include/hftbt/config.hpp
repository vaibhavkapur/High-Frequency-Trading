// INI-style run configuration: `[section]` + `key = value`, flattened to
// "section.key". Unknown keys are an error so typos cannot silently change
// an experiment.
#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "hftbt/engine.hpp"

namespace hftbt {

class Config {
public:
    bool parse(std::string_view text, std::string& err);
    bool load(const std::string& path, std::string& err);
    // "section.key=value"
    bool set(std::string_view assignment, std::string& err);

    bool has(const std::string& key) const { return values_.contains(key); }
    std::string get_str(const std::string& key, const std::string& def = "");
    std::int64_t get_int(const std::string& key, std::int64_t def);
    double get_double(const std::string& key, double def);
    bool get_bool(const std::string& key, bool def);
    std::vector<std::int64_t> get_int_list(const std::string& key);

    // Conversion failures and keys nobody asked for.
    std::vector<std::string> problems() const;

private:
    std::map<std::string, std::string> values_;
    std::set<std::string> used_;
    std::vector<std::string> errors_;
};

EngineConfig make_engine_config(Config& cfg);
// Reads `strategy.name` and its parameters. Null (with err) if unknown.
std::unique_ptr<Strategy> make_strategy(Config& cfg, std::string& err);

} // namespace hftbt
