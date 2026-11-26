#include "chronos/gateway/postgres_server.hpp"
#include "chronos/gateway/python_control.hpp"

#include <nlohmann/json.hpp>

#include <boost/asio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <functional>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace chronos::gateway {
namespace {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using chronos::native::IntervalQueryResult;
using chronos::native::IntervalValue;

std::uint16_t read_u16(const unsigned char *data) {
    return static_cast<std::uint16_t>((data[0] << 8) | data[1]);
}

std::uint32_t read_u32(const unsigned char *data) {
    return (static_cast<std::uint32_t>(data[0]) << 24) |
        (static_cast<std::uint32_t>(data[1]) << 16) |
        (static_cast<std::uint32_t>(data[2]) << 8) |
        static_cast<std::uint32_t>(data[3]);
}

void append_u16(std::string &out, std::uint16_t value) {
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value));
}

void append_u32(std::string &out, std::uint32_t value) {
    out.push_back(static_cast<char>(value >> 24));
    out.push_back(static_cast<char>(value >> 16));
    out.push_back(static_cast<char>(value >> 8));
    out.push_back(static_cast<char>(value));
}

void append_cstring(std::string &out, const std::string &value) {
    out.append(value);
    out.push_back('\0');
}

std::string take_cstring(const std::string &payload, std::size_t &offset) {
    if (offset >= payload.size()) throw GatewayError("malformed PostgreSQL message");
    const auto end = payload.find('\0', offset);
    if (end == std::string::npos) throw GatewayError("unterminated PostgreSQL string");
    std::string result = payload.substr(offset, end - offset);
    offset = end + 1;
    return result;
}

void read_exact(tcp::socket &socket, void *data, std::size_t size) {
    asio::read(socket, asio::buffer(data, size));
}

std::string read_sized_payload(tcp::socket &socket) {
    std::array<unsigned char, 4> length{};
    read_exact(socket, length.data(), length.size());
    const std::uint32_t total = read_u32(length.data());
    if (total < 4 || total > 64U * 1024U * 1024U) {
        throw GatewayError("invalid PostgreSQL message length");
    }
    std::string payload(total - 4, '\0');
    if (!payload.empty()) read_exact(socket, payload.data(), payload.size());
    return payload;
}

void send_message(tcp::socket &socket, char type, const std::string &payload) {
    std::string frame;
    frame.reserve(payload.size() + 5);
    frame.push_back(type);
    append_u32(frame, static_cast<std::uint32_t>(payload.size() + 4));
    frame.append(payload);
    asio::write(socket, asio::buffer(frame));
}

void send_auth(tcp::socket &socket, std::uint32_t method) {
    std::string payload;
    append_u32(payload, method);
    send_message(socket, 'R', payload);
}

void send_parameter(tcp::socket &socket, const std::string &name, const std::string &value) {
    std::string payload;
    append_cstring(payload, name);
    append_cstring(payload, value);
    send_message(socket, 'S', payload);
}

void send_ready(tcp::socket &socket, char state) {
    send_message(socket, 'Z', std::string(1, state));
}

void send_error(tcp::socket &socket, const std::string &message, const std::string &code = "XX000") {
    std::string payload;
    payload.push_back('S'); append_cstring(payload, "ERROR");
    payload.push_back('V'); append_cstring(payload, "ERROR");
    payload.push_back('C'); append_cstring(payload, code);
    payload.push_back('M'); append_cstring(payload, message);
    payload.push_back('\0');
    send_message(socket, 'E', payload);
}

// The engine reports a UNIQUE constraint broken within a branch as
// "chronos_unique_violation: duplicate key value violates unique constraint
// "name": Key (col)=(value) ...". Clients get it as PostgreSQL sends a
// unique_violation: SQLSTATE 23505, the message, the key as DETAIL, and the
// constraint name. Returns false for any other error.
bool send_unique_violation(tcp::socket &socket, const std::string &what) {
    static const std::string marker = "chronos_unique_violation: ";
    const auto at = what.find(marker);
    if (at == std::string::npos) return false;
    std::string message = what.substr(at + marker.size());
    std::string detail;
    std::string constraint;
    const auto open = message.find('"');
    const auto close = open == std::string::npos ? std::string::npos : message.find("\": ", open + 1);
    if (close != std::string::npos) {
        constraint = message.substr(open + 1, close - open - 1);
        detail = message.substr(close + 3);
        message.resize(close + 1);
    }
    std::string payload;
    payload.push_back('S'); append_cstring(payload, "ERROR");
    payload.push_back('V'); append_cstring(payload, "ERROR");
    payload.push_back('C'); append_cstring(payload, "23505");
    payload.push_back('M'); append_cstring(payload, message);
    if (!detail.empty()) { payload.push_back('D'); append_cstring(payload, detail + "."); }
    if (!constraint.empty()) { payload.push_back('n'); append_cstring(payload, constraint); }
    payload.push_back('\0');
    send_message(socket, 'E', payload);
    return true;
}

std::string lower_keyword(const std::string &sql) {
    std::size_t offset = 0;
    while (offset < sql.size()) {
        while (offset < sql.size() &&
               (std::isspace(static_cast<unsigned char>(sql[offset])) || sql[offset] == ';')) {
            ++offset;
        }
        if (offset + 1 < sql.size() && sql[offset] == '-' && sql[offset + 1] == '-') {
            const auto newline = sql.find('\n', offset + 2);
            offset = newline == std::string::npos ? sql.size() : newline + 1;
            continue;
        }
        if (offset + 1 < sql.size() && sql[offset] == '/' && sql[offset + 1] == '*') {
            const auto end = sql.find("*/", offset + 2);
            if (end == std::string::npos) return {};
            offset = end + 2;
            continue;
        }
        break;
    }
    auto begin = sql.begin() + static_cast<std::ptrdiff_t>(offset);
    std::string word;
    while (begin != sql.end() && (std::isalpha(static_cast<unsigned char>(*begin)) || *begin == '_')) {
        word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*begin++))));
    }
    return word;
}

void enforce_branch_sql_boundary(const std::string &sql) {
    std::string lowered;
    lowered.reserve(sql.size());
    std::transform(sql.begin(), sql.end(), std::back_inserter(lowered), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    // The core engine exposes its physical interval and metadata tables to
    // trusted administrative callers. A sandbox connection is not trusted:
    // naming that reserved namespace would bypass logical-table visibility
    // rewriting, so fail closed before parsing or execution.
    if (lowered.find("_chronos_") != std::string::npos) {
        throw GatewayError("the _chronos_ namespace is not accessible through the sandbox gateway");
    }
}

std::string value_text(const IntervalValue &value) {
    if (std::holds_alternative<std::monostate>(value)) return {};
    if (const auto *integer = std::get_if<std::int64_t>(&value)) return std::to_string(*integer);
    if (const auto *number = std::get_if<double>(&value)) {
        std::ostringstream out;
        out << std::setprecision(17) << *number;
        return out.str();
    }
    if (const auto *text = std::get_if<std::string>(&value)) return *text;
    if (const auto *blob = std::get_if<chronos::native::IntervalBlob>(&value)) {
        std::ostringstream out;
        out << "\\x" << std::hex << std::setfill('0');
        for (auto byte : *blob) out << std::setw(2) << static_cast<unsigned int>(byte);
        return out.str();
    }
    if (const auto *decimal = std::get_if<chronos::native::IntervalDecimal>(&value)) {
        return decimal->text;
    }
    const auto &date = std::get<chronos::native::IntervalDate>(value);
    std::ostringstream out;
    out << std::setfill('0') << std::setw(4) << date.year << '-'
        << std::setw(2) << date.month << '-' << std::setw(2) << date.day;
    return out.str();
}

constexpr std::uint32_t kTextOid = 25;
constexpr std::uint32_t kBoolOid = 16;

// PostgreSQL type OID for a value whose column type the store did not report.
std::uint32_t inferred_type_oid(const IntervalValue &value) {
    if (std::holds_alternative<std::int64_t>(value)) return 20;                       // int8
    if (std::holds_alternative<double>(value)) return 701;                            // float8
    if (std::holds_alternative<chronos::native::IntervalBlob>(value)) return 17;      // bytea
    if (std::holds_alternative<chronos::native::IntervalDecimal>(value)) return 1700; // numeric
    if (std::holds_alternative<chronos::native::IntervalDate>(value)) return 1082;    // date
    return kTextOid;
}

// One type OID per result column: the store's type when known, otherwise the
// type of the column's first non-null value, otherwise text.
std::vector<std::uint32_t> result_type_oids(const IntervalQueryResult &result) {
    std::vector<std::uint32_t> oids(result.columns.size(), 0);
    for (std::size_t column = 0; column < oids.size(); ++column) {
        if (column < result.column_types.size()) oids[column] = result.column_types[column];
        if (oids[column] != 0) continue;
        oids[column] = kTextOid;
        for (const auto &row : result.rows) {
            if (column < row.size() && !std::holds_alternative<std::monostate>(row[column])) {
                oids[column] = inferred_type_oid(row[column]);
                break;
            }
        }
    }
    return oids;
}

// Text-format value for its column type; booleans travel as t/f.
std::string value_text(const IntervalValue &value, std::uint32_t oid) {
    if (oid == kBoolOid) {
        if (const auto *integer = std::get_if<std::int64_t>(&value)) return *integer ? "t" : "f";
    }
    return value_text(value);
}

void send_row_description(tcp::socket &socket, const IntervalQueryResult &result) {
    const auto oids = result_type_oids(result);
    std::string payload;
    append_u16(payload, static_cast<std::uint16_t>(result.columns.size()));
    for (std::size_t column = 0; column < result.columns.size(); ++column) {
        append_cstring(payload, result.columns[column]);
        append_u32(payload, 0);              // table oid
        append_u16(payload, 0);              // attribute number
        append_u32(payload, oids[column]);   // type oid
        append_u16(payload, 0xffff);         // variable length
        append_u32(payload, 0xffffffff);     // no type modifier
        append_u16(payload, 0);              // text format
    }
    send_message(socket, 'T', payload);
}

void send_data_rows(tcp::socket &socket, const IntervalQueryResult &result) {
    const auto oids = result_type_oids(result);
    for (const auto &row : result.rows) {
        std::string payload;
        append_u16(payload, static_cast<std::uint16_t>(row.size()));
        for (std::size_t column = 0; column < row.size(); ++column) {
            const auto &value = row[column];
            if (std::holds_alternative<std::monostate>(value)) {
                append_u32(payload, 0xffffffff);
            } else {
                const auto text = value_text(value, column < oids.size() ? oids[column] : kTextOid);
                append_u32(payload, static_cast<std::uint32_t>(text.size()));
                payload.append(text);
            }
        }
        send_message(socket, 'D', payload);
    }
    send_message(socket, 'C', "SELECT " + std::to_string(result.rows.size()) + '\0');
}

void send_result(tcp::socket &socket, const IntervalQueryResult &result) {
    send_row_description(socket, result);
    send_data_rows(socket, result);
}

// --- search_path -------------------------------------------------------------
// The gateway keeps each connection's search_path itself (the store's own
// connection always uses public) and gives it to the branch session, which
// resolves unqualified table names along it.

// Schema names from a search_path value: double-quoted names are kept as
// written, unquoted ones are lowercased, and "$user" is dropped.
std::vector<std::string> parse_search_path(const std::string &text) {
    std::vector<std::string> schemas;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',')) ++i;
        if (i >= text.size()) break;
        std::string name;
        if (text[i] == '"' || text[i] == '\'') {
            const char quote = text[i++];
            while (i < text.size()) {
                if (text[i] == quote && i + 1 < text.size() && text[i + 1] == quote) { name += quote; i += 2; continue; }
                if (text[i] == quote) { ++i; break; }
                name += text[i++];
            }
            if (quote == '\'') {  // 'a, b' is one value holding a list
                for (auto &inner : parse_search_path(name)) schemas.push_back(std::move(inner));
                continue;
            }
        } else {
            while (i < text.size() && text[i] != ',' && !std::isspace(static_cast<unsigned char>(text[i]))) {
                name += static_cast<char>(std::tolower(static_cast<unsigned char>(text[i++])));
            }
        }
        if (!name.empty() && name != "$user") schemas.push_back(std::move(name));
    }
    return schemas;
}

std::string format_search_path(const std::vector<std::string> &schemas) {
    if (schemas.empty()) return "\"$user\", public";
    std::string text;
    for (const auto &schema : schemas) {
        if (!text.empty()) text += ", ";
        text += schema;
    }
    return text;
}

// The value of a setting in a StartupMessage's options ("-c name=x" or
// "--name=x"), if any. Several settings may appear, as
// "-c search_path=x -c chronos.branch=y".
std::optional<std::string> startup_option(const std::string &options,
                                          const std::vector<std::string> &keys) {
    for (const std::string &key : keys) {
        const auto at = options.find(key);
        if (at == std::string::npos) continue;
        std::size_t end = at + key.size();
        while (end < options.size() && !std::isspace(static_cast<unsigned char>(options[end]))) {
            if (options[end] == '\\' && end + 1 < options.size()) ++end;  // libpq escapes spaces
            ++end;
        }
        std::string value;
        for (std::size_t i = at + key.size(); i < end; ++i) {
            if (options[i] == '\\' && i + 1 < end) ++i;
            value += options[i];
        }
        return value;
    }
    return std::nullopt;
}

std::optional<std::string> startup_search_path(const std::string &options) {
    return startup_option(options, {"search_path=", "search-path="});
}

std::optional<std::string> startup_branch(const std::string &options) {
    return startup_option(options, {"chronos.branch="});
}

std::string lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// The value of "SET [SESSION | LOCAL] parameter {TO | =} value", if sql is
// one; DEFAULT gives an empty value.
std::optional<std::string> set_parameter_value(const std::string &sql, const std::string &parameter) {
    std::istringstream words(sql);
    std::string word;
    words >> word;
    if (lowercase(word) != "set") return std::nullopt;
    words >> word;
    if (lowercase(word) == "session" || lowercase(word) == "local") words >> word;
    std::string target = lowercase(word);
    std::string rest;
    std::getline(words, rest);
    // "parameter=x" may arrive as one word.
    if (target.rfind(parameter + "=", 0) == 0) {
        if (parameter == "search_path") return target.substr(parameter.size() + 1) + rest;
        rest = word.substr(parameter.size() + 1) + rest;  // keeps the value's case
        while (!rest.empty() && (rest.back() == ';' || std::isspace(static_cast<unsigned char>(rest.back())))) {
            rest.pop_back();
        }
        return lowercase(rest) == "default" ? std::string() : rest;
    }
    if (target != parameter) return std::nullopt;
    auto start = rest.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return std::string();
    if (rest[start] == '=') ++start;
    else if (lowercase(rest.substr(start, 2)) == "to") start += 2;
    std::string value = rest.substr(start);
    while (!value.empty() && (value.back() == ';' || std::isspace(static_cast<unsigned char>(value.back())))) {
        value.pop_back();
    }
    if (lowercase(value.substr(value.find_first_not_of(" \t") == std::string::npos ? 0 :
                               value.find_first_not_of(" \t"))) == "default") {
        return std::string();
    }
    return value;
}

std::optional<std::string> set_search_path_value(const std::string &sql) {
    return set_parameter_value(sql, "search_path");
}

// --- chronos.branch ------------------------------------------------------------
// A principal's connection chooses its branch at login (startup option
// "-c chronos.branch=NAME", else the principal's default branch) and may change
// it with SET chronos.branch outside a transaction.

constexpr const char *kBranchParameter = "chronos.branch";

// A branch name from a SET value: quoted names are unquoted, others are used
// as written (branch names are case-sensitive).
std::string branch_value(std::string value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    value = value.substr(start);
    if (!value.empty() && (value.front() == '\'' || value.front() == '"')) {
        const char quote = value.front();
        std::string name;
        for (std::size_t i = 1; i < value.size(); ++i) {
            if (value[i] == quote && i + 1 < value.size() && value[i + 1] == quote) { name += quote; ++i; continue; }
            if (value[i] == quote) return name;
            name += value[i];
        }
        throw GatewayError("unterminated quoted branch name");
    }
    return value;
}

bool second_word_is(const std::string &sql, const std::string &expected) {
    std::istringstream words(sql);
    std::string first, second;
    words >> first >> second;
    return lowercase(second) == expected;
}

bool resets_branch(const std::string &sql) {
    std::istringstream words(sql);
    std::string first, second;
    words >> first >> second;
    while (!second.empty() && second.back() == ';') second.pop_back();
    return lowercase(first) == "reset" && lowercase(second) == kBranchParameter;
}

// Statements a read-only branch accepts. Everything else (INSERT, UPDATE,
// DELETE, MERGE, DDL, COPY, ...) is a write.
bool writes_data(const std::string &sql, const std::string &keyword) {
    static const std::vector<std::string> reads = {
        "select", "with", "show", "set", "reset", "discard", "begin", "start",
        "commit", "end", "rollback", "savepoint", "release", "deallocate"};
    if (std::find(reads.begin(), reads.end(), keyword) == reads.end()) return true;
    if (keyword != "select" && keyword != "with") return false;
    // A query may still write: WITH ... DELETE, SELECT ... INTO. Look for
    // such words outside quotes and comments; FOR UPDATE is only a lock.
    std::string previous;
    for (std::size_t i = 0; i < sql.size();) {
        const char ch = sql[i];
        if (ch == '\'' || ch == '"') {
            const auto end = sql.find(ch, i + 1);
            i = end == std::string::npos ? sql.size() : end + 1;
            continue;
        }
        if (ch == '-' && i + 1 < sql.size() && sql[i + 1] == '-') {
            const auto end = sql.find('\n', i);
            i = end == std::string::npos ? sql.size() : end + 1;
            continue;
        }
        if (ch == '/' && i + 1 < sql.size() && sql[i + 1] == '*') {
            const auto end = sql.find("*/", i + 2);
            i = end == std::string::npos ? sql.size() : end + 2;
            continue;
        }
        if (ch == '$' && i + 1 < sql.size() && sql[i + 1] == '$') {
            const auto end = sql.find("$$", i + 2);
            i = end == std::string::npos ? sql.size() : end + 2;
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(ch)) || ch == '_') {
            std::string word;
            while (i < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) {
                word += static_cast<char>(std::tolower(static_cast<unsigned char>(sql[i++])));
            }
            if (word == "insert" || word == "delete" || word == "merge" || word == "truncate" ||
                word == "into" || (word == "update" && previous != "for" && previous != "key")) {
                return true;
            }
            previous = std::move(word);
            continue;
        }
        ++i;
    }
    return false;
}

bool resets_search_path(const std::string &sql) {
    std::istringstream words(sql);
    std::string first, second;
    words >> first >> second;
    first = lowercase(first);
    second = lowercase(second);
    while (!second.empty() && second.back() == ';') second.pop_back();
    return (first == "reset" && (second == "search_path" || second == "all")) ||
           (first == "discard" && second == "all");
}

// current_schema() (or the bare current_schema) outside quotes becomes the
// connection's schema as a literal, since the store's own connection would
// answer public.
std::string with_current_schema(const std::string &sql, const std::string &schema) {
    static const std::string name = "current_schema";
    std::string literal = "'";
    for (char ch : schema) literal += ch == '\'' ? std::string("''") : std::string(1, ch);
    literal += "'";
    std::string out;
    bool single = false, dbl = false;
    for (std::size_t i = 0; i < sql.size();) {
        const char ch = sql[i];
        if (!dbl && ch == '\'') single = !single;
        else if (!single && ch == '"') dbl = !dbl;
        if (!single && !dbl && (i == 0 || !(std::isalnum(static_cast<unsigned char>(sql[i - 1])) ||
                                            sql[i - 1] == '_' || sql[i - 1] == '.')) &&
            lowercase(sql.substr(i, name.size())) == name) {
            std::size_t end = i + name.size();
            if (end < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[end])) || sql[end] == '_')) {
                out += sql[i++];
                continue;
            }
            std::size_t after = end;
            while (after < sql.size() && std::isspace(static_cast<unsigned char>(sql[after]))) ++after;
            if (after + 1 < sql.size() && sql[after] == '(') {
                std::size_t close = after + 1;
                while (close < sql.size() && std::isspace(static_cast<unsigned char>(sql[close]))) ++close;
                if (close < sql.size() && sql[close] == ')') end = close + 1;
            }
            out += literal;
            i = end;
            continue;
        }
        out += ch;
        ++i;
    }
    return out;
}

// The statement as the session runs it: current_schema() resolved.
std::string for_connection(const SqlBranchSession &branch_session, const std::string &sql) {
    const auto &path = branch_session.search_path();
    return path.empty() ? sql : with_current_schema(sql, path.front());
}

// An error carrying its PostgreSQL SQLSTATE.
class SqlStateError : public std::runtime_error {
  public:
    SqlStateError(const std::string &message, std::string code)
        : std::runtime_error(message), code(std::move(code)) {}
    std::string code;
};

struct PreparedStatement {
    std::string sql;
    // Parameter types the client declared in Parse; 0 where unspecified.
    std::vector<std::uint32_t> parameter_types;
};

// Answers Describe for a statement before it is bound, as clients such as
// postgres.js require: they read the result columns from this answer and
// reuse them for every later execution.
void send_statement_description(tcp::socket &socket, SqlBranchSession &branch_session,
                                const PreparedStatement &statement) {
    const auto keyword = lower_keyword(statement.sql);
    std::vector<std::uint32_t> parameter_types = statement.parameter_types;
    std::optional<IntervalQueryResult> columns;
    if (keyword == "select" || keyword == "with") {
        enforce_branch_sql_boundary(statement.sql);
        auto description = branch_session.session().describe(for_connection(branch_session, statement.sql));
        if (parameter_types.size() < description.parameter_types.size()) {
            parameter_types.resize(description.parameter_types.size(), 0);
        }
        for (std::size_t index = 0; index < description.parameter_types.size(); ++index) {
            if (parameter_types[index] == 0) parameter_types[index] = description.parameter_types[index];
        }
        columns.emplace();
        columns->columns = std::move(description.columns);
        columns->column_types = std::move(description.column_types);
    } else if (keyword == "show") {
        columns.emplace();
        columns->columns = {"setting"};
        columns->column_types = {kTextOid};
    }
    std::string payload;
    append_u16(payload, static_cast<std::uint16_t>(parameter_types.size()));
    for (const auto type : parameter_types) append_u32(payload, type);
    send_message(socket, 't', payload);
    if (columns) send_row_description(socket, *columns);
    else send_message(socket, 'n', {});
}

struct Portal {
    std::string sql;
    std::vector<IntervalValue> params;
    std::optional<IntervalQueryResult> described_result;
};

struct Execution {
    bool returns_rows = false;
    IntervalQueryResult rows;
    std::string tag;
};

// Runs CREATE TABLE / CREATE INDEX for every branch of a shared-structure
// workspace and returns the command tag.
using SharedDdl = std::function<std::string(const std::string &sql)>;

// Replaces the connection's branch session with one on another branch;
// nullopt returns to the branch chosen at login. The current session is gone
// when it returns.
using BranchSwitch = std::function<void(const std::optional<std::string> &branch)>;

Execution execute_sql(SqlBranchSession &branch_session, const std::string &sql,
                      const std::vector<IntervalValue> &params,
                      const SharedDdl &shared_ddl = {},
                      const BranchSwitch &switch_branch = {}) {
    enforce_branch_sql_boundary(sql);
    const std::string keyword = lower_keyword(sql);
    if (keyword.empty()) return {false, {}, ""};
    if (keyword == "show" && lowercase(sql).find(kBranchParameter) != std::string::npos) {
        IntervalQueryResult result;
        result.columns = {kBranchParameter};
        result.rows = {{branch_session.branch()}};
        return {true, std::move(result), {}};
    }
    if (keyword == "set" || keyword == "reset") {
        std::optional<std::string> target;
        bool changes_branch = false;
        if (auto value = set_parameter_value(sql, kBranchParameter)) {
            if (second_word_is(sql, "local")) {
                throw SqlStateError("SET LOCAL chronos.branch is not supported; use SET chronos.branch", "0A000");
            }
            changes_branch = true;
            if (!value->empty()) target = branch_value(*value);
            if (target && target->empty()) throw GatewayError("branch name is required");
        } else if (resets_branch(sql)) {
            changes_branch = true;
        }
        if (changes_branch) {
            if (branch_session.session().in_transaction()) {
                throw SqlStateError("chronos.branch cannot be changed inside a transaction block", "25001");
            }
            if (!switch_branch) throw GatewayError("this connection cannot change branches");
            // Replaces branch_session: nothing below may touch it.
            switch_branch(target);
            return {false, {}, keyword == "reset" ? "RESET" : "SET"};
        }
    }
    if (branch_session.read_only() && writes_data(sql, keyword)) {
        throw PermissionDenied(
            "branch \"" + branch_session.branch() + "\" is read-only for this login");
    }
    auto &session = branch_session.session();
    if (!branch_session.schema_branching() &&
        (keyword == "create" || keyword == "alter" || keyword == "drop")) {
        // A branch-local definition would give this branch its own table
        // structure, which a shared-structure workspace cannot merge. CREATE
        // TABLE and CREATE INDEX instead apply to every branch, outside the
        // client's transaction (see chronos_core.control.ddl).
        if (keyword == "create" && shared_ddl) return {false, {}, shared_ddl(sql)};
        throw GatewayError(
            "table structure is shared by every branch in this workspace; "
            "only CREATE TABLE and CREATE INDEX are accepted, for every branch");
    }
    if (keyword == "begin" || keyword == "start") {
        session.begin();
        return {false, {}, "BEGIN"};
    }
    if (keyword == "commit" || keyword == "end") {
        session.commit();
        return {false, {}, "COMMIT"};
    }
    if (keyword == "rollback") {
        session.rollback();
        return {false, {}, "ROLLBACK"};
    }
    if (keyword == "set" || keyword == "reset" || keyword == "discard") {
        if (auto value = set_search_path_value(sql)) {
            branch_session.set_search_path(parse_search_path(*value));
        } else if (resets_search_path(sql)) {
            branch_session.set_search_path({});
        }
        return {false, {}, keyword == "discard" ? "DISCARD ALL" : (keyword == "reset" ? "RESET" : "SET")};
    }
    if (keyword == "show") {
        IntervalQueryResult result;
        const auto shown = lowercase(sql);
        if (shown.find("search_path") != std::string::npos) {
            result.columns = {"search_path"};
            result.rows = {{format_search_path(branch_session.search_path())}};
        } else {
            result.columns = {"setting"};
            result.rows = {{std::string("on")}};
        }
        return {true, std::move(result), {}};
    }
    if (keyword == "select" || keyword == "with") {
        return {true, session.query(for_connection(branch_session, sql), params), {}};
    }
    const auto affected = session.execute(for_connection(branch_session, sql), params);
    std::string command = keyword;
    std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    if (command == "INSERT") return {false, {}, "INSERT 0 " + std::to_string(affected)};
    return {false, {}, command + " " + std::to_string(affected)};
}

void send_execution(tcp::socket &socket, const Execution &execution, bool include_description) {
    if (execution.returns_rows) {
        if (include_description) send_result(socket, execution.rows);
        else send_data_rows(socket, execution.rows);
    } else if (execution.tag.empty()) {
        send_message(socket, 'I', {});
    } else {
        send_message(socket, 'C', execution.tag + '\0');
    }
}

} // namespace

class PostgresServer::Impl {
  public:
    Impl(GatewayRuntime &runtime, ListenAddress listen)
        : runtime(runtime), listen(std::move(listen)), acceptor(io) {}

    ~Impl() { stop(); join_clients(); }

    // Set by PostgresServer::set_python_control before run().
    PythonControl *python = nullptr;

    void run() {
        const auto address = asio::ip::make_address(listen.host);
        tcp::endpoint endpoint(address, listen.port);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::socket_base::reuse_address(true));
        acceptor.bind(endpoint);
        acceptor.listen(asio::socket_base::max_listen_connections);
        acceptor.non_blocking(true);
        while (!stopping.load()) {
            boost::system::error_code error;
            auto socket = std::make_shared<tcp::socket>(io);
            acceptor.accept(*socket, error);
            if (error) {
                if (error == asio::error::would_block || error == asio::error::try_again) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    continue;
                }
                if (stopping.load() || error == asio::error::operation_aborted ||
                    error == asio::error::bad_descriptor) break;
                throw boost::system::system_error(error);
            }
            // A reply is several small writes (RowDescription, DataRow,
            // CommandComplete, ReadyForQuery). With Nagle's algorithm each
            // later write waits for the client's delayed ACK (~40 ms), so
            // every statement paid that. PostgreSQL disables it too.
            boost::system::error_code no_delay_error;
            socket->set_option(tcp::no_delay(true), no_delay_error);
            std::lock_guard lock(clients_mutex);
            reap_finished_clients();
            sockets.push_back(socket);
            clients.emplace_back([this, socket] { serve(socket); finish_client(socket); });
        }
        join_clients();
    }

    void stop() {
        if (stopping.exchange(true)) return;
        boost::system::error_code ignored;
        acceptor.cancel(ignored);
        acceptor.close(ignored);
        std::lock_guard lock(clients_mutex);
        for (const auto &socket : sockets) {
            socket->cancel(ignored);
            socket->close(ignored);
        }
    }

  private:
    // Closes a finished connection so the client sees its end (a pg client
    // waits for the close after Terminate), and lets its thread be reaped.
    void finish_client(const std::shared_ptr<tcp::socket> &socket) {
        std::lock_guard lock(clients_mutex);
        boost::system::error_code ignored;
        socket->shutdown(tcp::socket::shutdown_both, ignored);
        socket->close(ignored);
        sockets.erase(std::remove(sockets.begin(), sockets.end(), socket), sockets.end());
        finished.push_back(std::this_thread::get_id());
    }

    // Joins threads whose connections have finished; called under clients_mutex.
    void reap_finished_clients() {
        if (finished.empty()) return;
        for (auto client = clients.begin(); client != clients.end();) {
            const auto done = std::find(finished.begin(), finished.end(), client->get_id());
            if (done == finished.end()) { ++client; continue; }
            finished.erase(done);
            if (client->joinable()) client->join();
            client = clients.erase(client);
        }
    }

    void join_clients() {
        std::vector<std::thread> pending;
        {
            std::lock_guard lock(clients_mutex);
            pending.swap(clients);
            finished.clear();
            sockets.clear();
        }
        for (auto &thread : pending) if (thread.joinable()) thread.join();
    }

    void serve(const std::shared_ptr<tcp::socket> &socket_ptr) noexcept {
        try {
            auto &socket = *socket_ptr;
            std::unordered_map<std::string, std::string> startup;
            while (true) {
                const auto payload = read_sized_payload(socket);
                if (payload.size() < 4) throw GatewayError("invalid PostgreSQL startup packet");
                const auto protocol = read_u32(reinterpret_cast<const unsigned char *>(payload.data()));
                if (protocol == 80877103U) {
                    asio::write(socket, asio::buffer("N", 1));
                    continue;
                }
                if (protocol != 196608U) throw GatewayError("unsupported PostgreSQL protocol version");
                std::size_t offset = 4;
                while (offset < payload.size() && payload[offset] != '\0') {
                    auto name = take_cstring(payload, offset);
                    auto value = take_cstring(payload, offset);
                    startup.emplace(std::move(name), std::move(value));
                }
                break;
            }
            const auto user = startup.find("user");
            const auto database = startup.find("database");
            if (user == startup.end() || database == startup.end()) {
                throw GatewayError("startup packet requires user and database");
            }
            send_auth(socket, 3);
            char type = 0;
            read_exact(socket, &type, 1);
            if (type != 'p') throw GatewayError("expected PostgreSQL password message");
            auto password_payload = read_sized_payload(socket);
            std::size_t password_offset = 0;
            const auto password = take_cstring(password_payload, password_offset);
            // libpq and node-postgres send "-c search_path=... -c chronos.branch=..."
            // in options; without chronos.branch a principal gets its default branch.
            std::optional<std::string> requested_branch;
            std::optional<std::string> search_path;
            if (const auto options = startup.find("options"); options != startup.end()) {
                requested_branch = startup_branch(options->second);
                search_path = startup_search_path(options->second);
            }
            auto branch_session = runtime.open_postgres(
                user->second, password, database->second, requested_branch);
            if (search_path) branch_session->set_search_path(parse_search_path(*search_path));
            const std::string workspace_name = branch_session->workspace();
            const std::string store_alias = branch_session->store_alias();
            const SharedDdl shared_ddl = [this, &workspace_name, &store_alias, &branch_session](
                                             const std::string &sql) {
                if (!python) throw GatewayError("table creation needs the gateway's Python control");
                const nlohmann::json arguments = {
                    {"store", store_alias}, {"sql", sql}, {"search_path", branch_session->search_path()}};
                const auto result = python->call(workspace_name, "apply_ddl", arguments.dump());
                const auto body = nlohmann::json::parse(result.body, nullptr, false);
                const bool object = body.is_object();
                const auto message = object ? body.value("error", std::string("table creation failed"))
                                            : std::string("table creation failed");
                if (result.status != 200) {
                    throw SqlStateError(message, object ? body.value("sqlstate", std::string("XX000"))
                                                        : std::string("XX000"));
                }
                return body.at("tag").get<std::string>();
            };
            const BranchSwitch switch_branch = [this, &branch_session](const std::optional<std::string> &branch) {
                branch_session = runtime.switch_postgres_branch(
                    *branch_session, branch ? *branch : branch_session->startup_branch());
            };
            // Before each statement: if this gateway moved the branch's head
            // (branch reset, keep-branch merge) since the session opened, the
            // session still reads the earlier state. Outside a transaction it
            // reopens at the current state; inside one the transaction must
            // end first, so other statements fail with a retryable error.
            const auto follow_branch_moves = [this, &branch_session](const std::string &keyword) {
                if (runtime.branch_generation(branch_session->workspace(), branch_session->branch()) ==
                    branch_session->branch_generation()) {
                    return;
                }
                if (branch_session->session().in_transaction()) {
                    if (keyword == "rollback" || keyword == "commit" || keyword == "end") return;
                    throw SqlStateError(
                        "branch \"" + branch_session->branch() +
                            "\" was reset or continued while this transaction was open; roll back and retry",
                        "40001");
                }
                branch_session = runtime.refresh_postgres(*branch_session);
            };

            send_auth(socket, 0);
            send_parameter(socket, "server_version", "17.0-chronos");
            send_parameter(socket, "server_encoding", "UTF8");
            send_parameter(socket, "client_encoding", "UTF8");
            send_parameter(socket, "DateStyle", "ISO, MDY");
            send_parameter(socket, "integer_datetimes", "on");
            send_parameter(socket, "standard_conforming_strings", "on");
            send_parameter(socket, "TimeZone", "UTC");
            std::string key;
            append_u32(key, 0); append_u32(key, 0);
            send_message(socket, 'K', key);
            send_ready(socket, 'I');

            std::unordered_map<std::string, PreparedStatement> statements;
            std::unordered_map<std::string, Portal> portals;
            bool failed_transaction = false;
            bool ignore_until_sync = false;
            // PostgreSQL accepts only the end of a failed transaction block.
            // Returns the reply for COMMIT/END, which roll it back.
            auto failed_transaction_reply = [&](const std::string &keyword) -> std::optional<Execution> {
                if (!failed_transaction || keyword == "rollback") return std::nullopt;
                if (keyword == "commit" || keyword == "end") {
                    branch_session->session().rollback();
                    failed_transaction = false;
                    return Execution{false, {}, "ROLLBACK"};
                }
                throw SqlStateError(
                    "current transaction is aborted, commands ignored until end of transaction block",
                    "25P02");
            };
            while (!stopping.load()) {
                read_exact(socket, &type, 1);
                auto payload = read_sized_payload(socket);
                if (type == 'X') break;
                try {
                    // A live connection must be invalidated immediately when
                    // its workspace expires or is revoked.
                    runtime.recheck_postgres(*branch_session, user->second, password, database->second);
                    if (ignore_until_sync && type != 'S') continue;
                    if (type == 'Q') {
                        std::size_t offset = 0;
                        const auto sql = take_cstring(payload, offset);
                        follow_branch_moves(lower_keyword(sql));
                        auto execution = failed_transaction_reply(lower_keyword(sql));
                        if (!execution) execution = execute_sql(*branch_session, sql, {}, shared_ddl, switch_branch);
                        send_execution(socket, *execution, true);
                        failed_transaction = false;
                        send_ready(socket, branch_session->session().in_transaction() ? 'T' : 'I');
                    } else if (type == 'P') {
                        std::size_t offset = 0;
                        auto name = take_cstring(payload, offset);
                        PreparedStatement statement{take_cstring(payload, offset), {}};
                        if (offset + 2 <= payload.size()) {
                            const auto count = read_u16(reinterpret_cast<const unsigned char *>(payload.data() + offset));
                            offset += 2;
                            for (std::uint16_t index = 0; index < count && offset + 4 <= payload.size(); ++index) {
                                statement.parameter_types.push_back(
                                    read_u32(reinterpret_cast<const unsigned char *>(payload.data() + offset)));
                                offset += 4;
                            }
                        }
                        statements[std::move(name)] = std::move(statement);
                        send_message(socket, '1', {});
                    } else if (type == 'B') {
                        std::size_t offset = 0;
                        auto portal_name = take_cstring(payload, offset);
                        auto statement_name = take_cstring(payload, offset);
                        auto found = statements.find(statement_name);
                        if (found == statements.end()) throw GatewayError("unknown prepared statement");
                        if (offset + 2 > payload.size()) throw GatewayError("malformed bind message");
                        const auto format_count = read_u16(reinterpret_cast<const unsigned char *>(payload.data() + offset));
                        offset += 2 + static_cast<std::size_t>(format_count) * 2;
                        if (offset + 2 > payload.size()) throw GatewayError("malformed bind formats");
                        const auto count = read_u16(reinterpret_cast<const unsigned char *>(payload.data() + offset));
                        offset += 2;
                        Portal portal;
                        portal.sql = found->second.sql;
                        for (std::uint16_t index = 0; index < count; ++index) {
                            if (offset + 4 > payload.size()) throw GatewayError("malformed bind parameter");
                            const auto length = read_u32(reinterpret_cast<const unsigned char *>(payload.data() + offset));
                            offset += 4;
                            if (length == 0xffffffffU) portal.params.emplace_back(std::monostate{});
                            else {
                                if (offset + length > payload.size()) throw GatewayError("truncated bind parameter");
                                portal.params.emplace_back(payload.substr(offset, length));
                                offset += length;
                            }
                        }
                        portals[std::move(portal_name)] = std::move(portal);
                        send_message(socket, '2', {});
                    } else if (type == 'D') {
                        if (payload.empty()) throw GatewayError("malformed describe message");
                        std::size_t offset = 1;
                        const auto name = take_cstring(payload, offset);
                        if (payload[0] == 'S') {
                            auto found = statements.find(name);
                            if (found == statements.end()) throw GatewayError("unknown prepared statement");
                            follow_branch_moves(lower_keyword(found->second.sql));
                            send_statement_description(socket, *branch_session, found->second);
                        } else if (payload[0] == 'P') {
                            auto found = portals.find(name);
                            if (found == portals.end()) throw GatewayError("unknown portal");
                            const auto keyword = lower_keyword(found->second.sql);
                            follow_branch_moves(keyword);
                            // A failed transaction runs nothing; Execute reports the error.
                            if (!failed_transaction &&
                                (keyword == "select" || keyword == "with" || keyword == "show")) {
                                auto execution = execute_sql(*branch_session, found->second.sql, found->second.params, shared_ddl, switch_branch);
                                found->second.described_result = std::move(execution.rows);
                                send_row_description(socket, *found->second.described_result);
                            } else send_message(socket, 'n', {});
                        } else throw GatewayError("invalid describe target");
                    } else if (type == 'E') {
                        std::size_t offset = 0;
                        const auto portal_name = take_cstring(payload, offset);
                        auto found = portals.find(portal_name);
                        if (found == portals.end()) throw GatewayError("unknown portal");
                        follow_branch_moves(lower_keyword(found->second.sql));
                        if (auto reply = failed_transaction_reply(lower_keyword(found->second.sql))) {
                            send_execution(socket, *reply, false);
                        } else if (found->second.described_result) {
                            Execution execution{true, std::move(*found->second.described_result), {}};
                            found->second.described_result.reset();
                            send_execution(socket, execution, false);
                        } else {
                            send_execution(socket, execute_sql(
                                *branch_session, found->second.sql, found->second.params, shared_ddl, switch_branch), false);
                        }
                        // Only ROLLBACK can succeed in a failed transaction.
                        failed_transaction = false;
                    } else if (type == 'C') {
                        if (payload.empty()) throw GatewayError("malformed close message");
                        std::size_t offset = 1;
                        const auto name = take_cstring(payload, offset);
                        if (payload[0] == 'S') statements.erase(name);
                        else if (payload[0] == 'P') portals.erase(name);
                        send_message(socket, '3', {});
                    } else if (type == 'S') {
                        ignore_until_sync = false;
                        send_ready(socket, failed_transaction ? 'E' :
                            (branch_session->session().in_transaction() ? 'T' : 'I'));
                    } else if (type == 'H') {
                        // Flush requires no response; writes are synchronous.
                    } else {
                        throw GatewayError("unsupported PostgreSQL frontend message");
                    }
                } catch (const std::exception &error) {
                    failed_transaction = branch_session->session().in_transaction();
                    const auto *coded = dynamic_cast<const SqlStateError *>(&error);
                    const bool denied = dynamic_cast<const PermissionDenied *>(&error) != nullptr;
                    if (!send_unique_violation(socket, error.what())) {
                        send_error(socket, error.what(), coded ? coded->code : (denied ? "42501" : "0A000"));
                    }
                    if (type == 'Q') send_ready(socket, failed_transaction ? 'E' : 'I');
                    else ignore_until_sync = true;
                }
            }
        } catch (const boost::system::system_error &) {
            // Disconnects and stop() are ordinary session termination.
        } catch (const std::exception &error) {
            const bool denied = dynamic_cast<const PermissionDenied *>(&error) != nullptr;
            try { send_error(*socket_ptr, error.what(), denied ? "42501" : "28000"); } catch (...) {}
        }
    }

    GatewayRuntime &runtime;
    ListenAddress listen;
    asio::io_context io;
    tcp::acceptor acceptor;
    std::atomic<bool> stopping{false};
    std::mutex clients_mutex;
    std::vector<std::shared_ptr<tcp::socket>> sockets;
    std::vector<std::thread> clients;
    std::vector<std::thread::id> finished;
};

PostgresServer::PostgresServer(GatewayRuntime &runtime, ListenAddress listen)
    : impl_(std::make_unique<Impl>(runtime, std::move(listen))) {}
PostgresServer::~PostgresServer() = default;
void PostgresServer::run() { impl_->run(); }
void PostgresServer::stop() { impl_->stop(); }
void PostgresServer::set_python_control(PythonControl *python) { impl_->python = python; }

} // namespace chronos::gateway
