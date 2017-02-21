#ifndef PASANE_PARSE_H
#define PASANE_PARSE_H

#include <map>
#include <string>
#include <vector>

struct ChannelMapping {
    float percentage;
    std::string name;
};

typedef std::vector<ChannelMapping> mapping_t;
typedef std::map<std::string, mapping_t> mappings_t;

mappings_t parse(const char *path);


#endif //PASANE_PARSE_H
