// main.cpp - nshtestusers: makes the user CSV for nshtestherd (and nshreg)
//
//   nshtestusers --generate 500 --output users.csv
//   nshtestusers --names --generate 200 --random-passwords --domain lab.example.com --output users.csv
//   nshtestusers --check users.csv

#include "../../src/csv.h"
#include "gen.h"
#include "random.h"
#include "../../src/version.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace
{

const size_t MAX_USERS = 1000000;

void Usage()
{
    std::printf(
        "nshtestusers %s - makes the user CSV for nshtestherd\n"
        "\n"
        "Usage: nshtestusers [options]\n"
        "\n"
        "Numbered users (the same options as nshtestherd --generate):\n"
        "  --generate <n>          number of users (1-%zu, default 100)\n"
        "  --prefix <name>         name prefix (default load): load000001\n"
        "  --start <n>             first number (default 1), to continue an earlier list\n"
        "  --password <pw>         the password of every user (default TestPassword)\n"
        "  --domain <name>         mail domain (default example.com)\n"
        "\n"
        "Random unique names (anna.meyer@example.com; every first/last name combination at most once):\n"
        "  --names                 names instead of numbers; --generate gives the count (up to %zu)\n"
        "  --seed <n>              the same seed gives the same list (it is printed when not given)\n"
        "  --exclude <file>        skip the names of an earlier list (a user CSV), so that a second batch does not collide\n"
        "\n"
        "Passwords:\n"
        "  --random-passwords      a different random password for every user (not with --password)\n"
        "  --password-length <n>   length of the random passwords (8-128, default 16)\n"
        "\n"
        "Output:\n"
        "  --output <file>         write the file (default: standard output); owner-only access\n"
        "  --force                 replace an existing output file\n"
        "  --header                write the header row FirstName,LastName,Password,Shortname,InternetAddress\n"
        "\n"
        "Check:\n"
        "  --check <file>          validate a user CSV (format, duplicate short names and addresses)\n"
        "\n"
        "  --version, --help\n"
        "\n"
        "Exit codes: 0 ok, 1 failure (cannot write, check failed), 2 usage error.\n",
        NSHTESTHERD_VERSION, MAX_USERS, NameCombinations());
}

bool ParseNumber(const std::string &text, size_t &value)
{
    if (text.empty() || text.size() > 12)
        return false;

    for (size_t i = 0; i < text.size(); i++)
    {
        if (!std::isdigit((unsigned char)text[i]))
            return false;
    }

    value = (size_t)std::strtoull(text.c_str(), NULL, 10);
    return true;
}

bool ValidName(const std::string &s, bool allowDot)
{
    if (s.empty())
        return false;

    for (size_t i = 0; i < s.size(); i++)
    {
        unsigned char c = (unsigned char)s[i];

        if (!(std::isalnum(c) || '-' == c || '_' == c || (allowDot && '.' == c)))
            return false;
    }

    return true;
}

int Fail(const std::string &message, int code)
{
    std::fprintf(stderr, "nshtestusers: %s\n", message.c_str());
    return code;
}

int CheckFile(const std::string &path)
{
    std::ifstream in(path.c_str(), std::ios::binary);

    if (!in)
        return Fail("cannot read " + path, 1);

    std::stringstream buf;
    buf << in.rdbuf();

    std::vector<UserRecord> users;
    std::string             err;

    if (!ParseUsersCsv(buf.str(), users, err))
        return Fail(path + ": " + err, 1);

    if (!CheckUnique(users, err))
        return Fail(path + ": " + err, 1);

    std::printf("[ OK ]   %s: %zu users, no duplicate short names or addresses\n", path.c_str(), users.size());
    return 0;
}

// Reads an earlier user CSV: its short names and addresses (lower case) are the names that a new list must not use.
bool LoadExclude(const std::string &path, std::set<std::string> &exclude, std::string &err)
{
    std::ifstream in(path.c_str(), std::ios::binary);

    if (!in)
    {
        err = "cannot read " + path;
        return false;
    }

    std::stringstream buf;
    buf << in.rdbuf();

    std::vector<UserRecord> users;
    std::string             parseErr;

    if (!ParseUsersCsv(buf.str(), users, parseErr))
    {
        err = path + ": " + parseErr;
        return false;
    }

    for (size_t i = 0; i < users.size(); i++)
    {
        std::string s = users[i].shortName, a = users[i].internetAddress;

        for (size_t k = 0; k < s.size(); k++)
            s[k] = (char)std::tolower((unsigned char)s[k]);

        for (size_t k = 0; k < a.size(); k++)
            a[k] = (char)std::tolower((unsigned char)a[k]);

        exclude.insert(s);
        exclude.insert(a);
    }

    return true;
}

} // namespace

int main(int argc, char **argv)
{
    NumberedOptions numbered;
    bool            names           = false;
    bool            randomPasswords = false;
    bool            passwordGiven   = false;
    bool            header          = false;
    bool            force           = false;
    bool            seedGiven       = false;
    size_t          passwordLength  = 16;
    size_t          seedValue       = 0;
    std::string     output;
    std::string     checkPath;
    std::string     excludePath;

    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        std::string v;

        // Options with a value: "--name value"
        const bool hasNext = (i + 1 < argc);

        if ("--help" == a || "-h" == a)
        {
            Usage();
            return 0;
        }
        else if ("--version" == a)
        {
            std::printf("nshtestusers %s\n", NSHTESTHERD_VERSION);
            return 0;
        }
        else if ("--names" == a)
            names = true;
        else if ("--random-passwords" == a)
            randomPasswords = true;
        else if ("--header" == a)
            header = true;
        else if ("--force" == a)
            force = true;
        else if (("--generate" == a || "--prefix" == a || "--start" == a || "--password" == a || "--domain" == a || "--seed" == a ||
                  "--password-length" == a || "--output" == a || "--check" == a || "--exclude" == a))
        {
            if (!hasNext)
                return Fail(a + " needs a value", 2);

            v = argv[++i];

            size_t n = 0;

            if ("--generate" == a)
            {
                if (!ParseNumber(v, n) || n < 1 || n > MAX_USERS)
                    return Fail("--generate must be 1 to " + std::to_string(MAX_USERS), 2);
                numbered.count = n;
            }
            else if ("--start" == a)
            {
                if (!ParseNumber(v, n) || n < 1)
                    return Fail("--start must be a number from 1", 2);
                numbered.start = n;
            }
            else if ("--seed" == a)
            {
                if (!ParseNumber(v, n))
                    return Fail("--seed must be a number", 2);
                seedValue = n;
                seedGiven = true;
            }
            else if ("--password-length" == a)
            {
                if (!ParseNumber(v, n) || n < 8 || n > 128)
                    return Fail("--password-length must be 8 to 128", 2);
                passwordLength = n;
            }
            else if ("--prefix" == a)
            {
                if (!ValidName(v, false))
                    return Fail("--prefix may only have letters, digits, - and _", 2);
                numbered.prefix = v;
            }
            else if ("--domain" == a)
            {
                if (!ValidName(v, true) || '.' == v[0] || '.' == v[v.size() - 1])
                    return Fail("--domain may only have letters, digits, - _ and dots, and not start or end with a dot", 2);
                numbered.domain = v;
            }
            else if ("--password" == a)
            {
                if (v.empty())
                    return Fail("--password must not be empty", 2);
                numbered.password = v;
                passwordGiven     = true;
            }
            else if ("--output" == a || "--check" == a || "--exclude" == a)
            {
                if (v.empty() || (v.size() > 1 && 0 == v.compare(0, 2, "--")))
                    return Fail(a + " needs a file name", 2);

                if ("--output" == a)
                    output = v;
                else if ("--check" == a)
                    checkPath = v;
                else
                    excludePath = v;
            }
        }
        else
            return Fail("unknown option " + a + " (see --help)", 2);
    }

    if (!checkPath.empty())
        return CheckFile(checkPath);

    if (randomPasswords && passwordGiven)
        return Fail("--random-passwords and --password exclude each other", 2);

    std::vector<UserRecord> users;
    std::string             err;

    if (!excludePath.empty() && !names)
        return Fail("--exclude is for --names (numbered lists continue with --start)", 2);

    std::set<std::string> exclude;

    if (names)
    {
        if (!excludePath.empty() && !LoadExclude(excludePath, exclude, err))
            return Fail(err, 1);

        if (!seedGiven)
        {
            unsigned char b[8];

            if (!FillRandom(b, sizeof(b), err))
                return Fail(err, 1);

            for (int k = 0; k < 8; k++)
                seedValue = (seedValue << 8) | b[k];

            seedValue &= 0xFFFFFFFFFFFFULL; // 48 bits: a number that is easy to copy
        }

        if (!RandomNameUsers(numbered.count, numbered.password, numbered.domain, (uint64_t)seedValue, users, err, excludePath.empty() ? NULL : &exclude))
            return Fail(err, 2);

        if (!seedGiven)
            std::fprintf(stderr, "nshtestusers: seed %llu (--seed gives the same list again)\n", (unsigned long long)seedValue);
    }
    else
        users = NumberedUsers(numbered);

    if (randomPasswords && !SetRandomPasswords(users, passwordLength, err))
        return Fail(err, 1);

    if (!CheckUnique(users, err))
        return Fail(err, 1);

    const std::string text = FormatUsersCsv(users, header);

    if (output.empty())
    {
        std::fwrite(text.data(), 1, text.size(), stdout);
        return 0;
    }

    if (!WriteUsersFile(output, text, force, err))
        return Fail(err, 1);

    std::fprintf(stderr, "nshtestusers: %zu users written to %s\n", users.size(), output.c_str());
    return 0;
}
