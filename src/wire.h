// wire.h - flat field list, text/JSON rendering and form decoding

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Longest status message (bytes) the coordinator accepts. The client core cuts its messages to it: one constant for
// both sides. Normal messages are much shorter; this leaves room for full error texts.
const size_t MAX_STATUS_MESSAGE_BYTES = 2048;

// Ordered flat list of scalar fields. Numbers render unquoted in JSON.
class Fields
{
public:
    void Add(const std::string &key, const std::string &value);
    void Add(const std::string &key, const char *value);
    void Add(const std::string &key, long long value);

    struct Item
    {
        std::string key;
        std::string value;
        bool        isNumber;
    };

    const std::vector<Item> &Items() const { return items_; }

private:
    std::vector<Item> items_;
};

// Escapes backslash, LF, CR and TAB as \\ \n \r \t (the text protocol rule).
std::string EscapeText(const std::string &value);

// "key=value\n" per field, values escaped with EscapeText.
std::string RenderText(const Fields &fields);

// JSON string content escaping (no surrounding quotes). Invalid UTF-8 bytes
// are replaced by U+FFFD so the output is always valid JSON.
std::string EscapeJson(const std::string &value);

// One flat JSON object.
std::string RenderJson(const Fields &fields);

typedef std::map<std::string, std::string> Form;

// Parses application/x-www-form-urlencoded (also used for query strings).
// Rejects malformed percent escapes, NUL bytes, empty/valueless pairs and
// duplicate keys. An empty input yields an empty form.
bool ParseForm(const std::string &encoded, Form &form, std::string &err);

// Percent-encodes a form value (unreserved characters kept, everything else %XX).
std::string UrlEncode(const std::string &value);

// Parses a text-format response body: one key=value per line, split at the
// first '=', values unescaped (\ \n \r \t). Lines without '=' are ignored.
void ParseTextFields(const std::string &body, std::map<std::string, std::string> &out);
