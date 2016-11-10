#ifndef PASANE_PARSE_H
#define PASANE_PARSE_H

#include <map>
#include <string>
#include <vector>

struct ChannelMapping {
    float percentage;
    std::string name;
};

typedef std::map<std::string, std::vector<ChannelMapping>> mappings_t;

mappings_t parse();


#endif //PASANE_PARSE_H
