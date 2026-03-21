#pragma once
#include "model/query_db.h"
#include <sstream>
#include <string>

// Export query results as delimited text (CSV with ',' or TSV with '\t').
// CSV quoting follows RFC 4180: fields containing the delimiter, quotes, or
// newlines are wrapped in double-quotes with internal quotes escaped.
inline std::string export_result(const QueryDb::QueryResult& result, char delimiter) {
    auto needs_quoting = [&](const std::string& s) {
        if (delimiter != ',') return false;
        return s.find_first_of(",\"\n") != std::string::npos;
    };
    auto write_field = [&](std::ostringstream& out, const std::string& s) {
        if (needs_quoting(s)) {
            out << '"';
            for (char ch : s) {
                if (ch == '"') out << '"';
                out << ch;
            }
            out << '"';
        } else {
            out << s;
        }
    };

    std::ostringstream out;
    for (size_t c = 0; c < result.columns.size(); c++) {
        if (c > 0) out << delimiter;
        write_field(out, result.columns[c]);
    }
    out << '\n';
    for (const auto& row : result.rows) {
        for (size_t c = 0; c < row.size(); c++) {
            if (c > 0) out << delimiter;
            write_field(out, row[c]);
        }
        out << '\n';
    }
    return out.str();
}
