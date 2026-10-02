// csv.cpp - RFC 4180 style parser for the five-column nshreg user format

#include "csv.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace
{

const size_t USER_COLUMNS = 5;

const char *HEADER_NAMES[USER_COLUMNS] = { "firstname", "lastname", "password", "shortname", "internetaddress" };

bool IsLineEnd(const std::string &t, size_t pos)
{
    if ('\n' == t[pos])
        return true;

    return ('\r' == t[pos]) && (pos + 1 < t.size()) && ('\n' == t[pos + 1]);
}

// Parses one record starting at pos and leaves pos after its line end.
bool ParseRecord(const std::string &t, size_t &pos, std::vector<std::string> &fields, std::string &err)
{
    const size_t n = t.size();
    std::string  cur;

    fields.clear();

    for (;;)
    {
        cur.clear();

        if (pos < n && '"' == t[pos])
        {
            ++pos;

            for (;;)
            {
                if (pos >= n)
                {
                    err = "unterminated quoted field";
                    return false;
                }

                char c = t[pos++];

                if ('"' != c)
                {
                    cur += c;
                    continue;
                }

                if (pos < n && '"' == t[pos])
                {
                    cur += '"';
                    ++pos;
                    continue;
                }

                break;
            }

            if (pos < n && ',' != t[pos] && !IsLineEnd(t, pos))
            {
                err = "unexpected character after closing quote";
                return false;
            }
        }
        else
        {
            while (pos < n && ',' != t[pos] && !IsLineEnd(t, pos))
            {
                if ('"' == t[pos])
                {
                    err = "quote inside unquoted field";
                    return false;
                }

                cur += t[pos++];
            }
        }

        fields.push_back(cur);

        if (pos >= n)
            return true;

        if (',' == t[pos])
        {
            ++pos;
            continue;
        }

        pos += ('\r' == t[pos]) ? 2 : 1;
        return true;
    }
}

bool IsHeaderRow(const std::vector<std::string> &fields)
{
    if (USER_COLUMNS != fields.size())
        return false;

    for (size_t i = 0; i < USER_COLUMNS; i++)
    {
        const std::string &f = fields[i];
        const char        *h = HEADER_NAMES[i];
        size_t             k = 0;

        for (; k < f.size() && h[k]; k++)
        {
            if (std::tolower((unsigned char)f[k]) != h[k])
                return false;
        }

        if (k != f.size() || h[k])
            return false;
    }

    return true;
}

} // namespace

bool ParseUsersCsv(const std::string &text, std::vector<UserRecord> &users, std::string &err)
{
    size_t                   pos      = 0;
    size_t                   recordNo = 0;
    bool                     first    = true;
    std::set<std::string>    seen;
    std::vector<std::string> fields;
    std::vector<UserRecord>  result;

    users.clear();

    if (text.size() >= 3 && "\xEF\xBB\xBF" == text.substr(0, 3))
        pos = 3;

    while (pos < text.size())
    {
        ++recordNo;

        if (IsLineEnd(text, pos))
        {
            pos += ('\r' == text[pos]) ? 2 : 1;
            continue;
        }

        std::string perr;

        if (!ParseRecord(text, pos, fields, perr))
        {
            err = "record " + std::to_string(recordNo) + ": " + perr;
            return false;
        }

        if (first)
        {
            first = false;

            if (IsHeaderRow(fields))
                continue;
        }

        if (USER_COLUMNS != fields.size())
        {
            err = "record " + std::to_string(recordNo) + ": expected 5 columns, found " + std::to_string(fields.size());
            return false;
        }

        if (fields[3].empty())
        {
            err = "record " + std::to_string(recordNo) + ": empty Shortname";
            return false;
        }

        if (!seen.insert(fields[3]).second)
        {
            err = "record " + std::to_string(recordNo) + ": duplicate Shortname";
            return false;
        }

        UserRecord u;
        u.firstName       = fields[0];
        u.lastName        = fields[1];
        u.password        = fields[2];
        u.shortName       = fields[3];
        u.internetAddress = fields[4];
        result.push_back(u);
    }

    if (result.empty())
    {
        err = "no user records found";
        return false;
    }

    users.swap(result);
    return true;
}

std::vector<UserRecord> GenerateUsers(size_t count, const std::string &prefix, const std::string &password, const std::string &domain)
{
    std::vector<UserRecord> users;
    size_t                  width = std::max<size_t>(6, std::to_string(count).size());
    std::string             first = prefix;

    if (!first.empty())
        first[0] = (char)std::toupper((unsigned char)first[0]);

    users.reserve(count);

    for (size_t i = 1; i <= count; i++)
    {
        std::string number = std::to_string(i);
        number.insert(0, width - number.size(), '0');

        UserRecord u;
        u.firstName       = first;
        u.lastName        = number;
        u.password        = password;
        u.shortName       = prefix + number;
        u.internetAddress = u.shortName + "@" + domain;
        users.push_back(u);
    }

    return users;
}
