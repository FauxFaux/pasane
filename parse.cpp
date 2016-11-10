#include <iostream>
#include <string>

#include <yaml-cpp/yaml.h>
#include "parse.h"

static std::string ltrim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
    return s;
}

static std::string rtrim(std::string s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
    return s;
}

static std::string trim(std::string s) {
    return ltrim(rtrim(s));
}

mappings_t parse() {
    const YAML::Node &config = YAML::LoadFile("sample.yml");
    if (!config.IsMap()) {
        throw std::range_error("the root must be a map");
    }
    const YAML::Node &profiles = config["balance_profiles"];
    if (!profiles.IsSequence()) {
        throw std::range_error("balance_profiles must be a list");
    }

    std::map<std::string, std::vector<ChannelMapping>> ret;

    for (size_t i = 0; i < profiles.size(); ++i) {
        const YAML::Node &profile = profiles[i];
        if (1 != profile.size() || !profile.IsMap()) {
            throw std::range_error("profile " + std::to_string(i) + " must be a dict from name to list");
        }

        const std::string &name = profile.begin()->first.as<std::string>();
        const YAML::Node &items = profile.begin()->second;
        for (const YAML::Node &item : items) {
            const std::string &row = item.as<std::string>();
            const unsigned long sign = row.find('%');
            if (std::string::npos == sign) {
                throw std::range_error(name + " -> '" + row + "' is invalid; it must contain a %");
            };
            ret[name].push_back((ChannelMapping) {
                    .percentage = std::stoi(row.substr(0, sign)) / 100.f,
                    .name = trim(row.substr(sign + 1))
            });
        }
    }

    return ret;
}
