#include "paf_utils.h"

#include <spdlog/spdlog.h>

#include <iostream>
#include <sstream>
#include <string>

namespace dorado::utils {

PafEntry parse_paf(std::stringstream& ss) {
    PafEntry entry;
    // Read the fields from the stringstream
    ss >> entry.qname >> entry.qlen >> entry.qstart >> entry.qend >> entry.strand >> entry.tname >>
            entry.tlen >> entry.tstart >> entry.tend >> entry.num_residue_matches >>
            entry.alignment_block_length >> entry.mapq;

    // The rest of the line is auxiliary data
    std::getline(ss, entry.aux);

    // Remove the leading tab from aux if it exists
    if (!entry.aux.empty() && entry.aux[0] == '\t') {
        entry.aux.erase(0, 1);
    }

    return entry;
}

PafEntry parse_paf(const std::string& paf_row) {
    std::stringstream ss(paf_row);
    return parse_paf(ss);
}

std::string serialize_paf(const PafEntry& entry) {
    std::ostringstream oss;
    oss << entry.qname << '\t' << entry.qlen << '\t' << entry.qstart << '\t' << entry.qend << '\t'
        << entry.strand << '\t' << entry.tname << '\t' << entry.tlen << '\t' << entry.tstart << '\t'
        << entry.tend << '\t' << entry.num_residue_matches << '\t' << entry.alignment_block_length
        << '\t' << entry.mapq << '\t' << entry.aux;
    return oss.str();
}

std::string_view paf_aux_get(const PafEntry& paf_entry, const char tag[2], char type) {
    const std::string t = std::string(tag) + ":" + std::string(1, type) + ":";
    std::string_view aux(paf_entry.aux);
    auto pos = aux.find(t.c_str());
    if (pos == std::string::npos) {
        return std::string_view();
    }
    pos += 5;
    auto end = aux.find('\t', pos);
    if (end == std::string::npos) {
        return aux.substr(pos);
    } else {
        return aux.substr(pos, end - pos);
    }
}

}  // namespace dorado::utils
