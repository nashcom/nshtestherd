// gen.cpp - see gen.h

#include "gen.h"

#include "names.h"
#include "random.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <set>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace
{

std::string Lower(std::string s)
{
    for (size_t i = 0; i < s.size(); i++)
        s[i] = (char)std::tolower((unsigned char)s[i]);

    return s;
}

std::string Quoted(const std::string &field)
{
    if (std::string::npos == field.find_first_of(",\"\r\n") && (field.empty() || (' ' != field[0] && ' ' != field[field.size() - 1])))
        return field;

    std::string out = "\"";

    for (size_t i = 0; i < field.size(); i++)
    {
        if ('"' == field[i])
            out += '"';

        out += field[i];
    }

    out += '"';
    return out;
}

} // namespace

std::vector<UserRecord> NumberedUsers(const NumberedOptions &o)
{
    std::vector<UserRecord> users;
    size_t                  last  = o.start + o.count - 1;
    size_t                  width = std::max<size_t>(6, std::to_string(last).size());
    std::string             first = o.prefix;

    if (!first.empty())
        first[0] = (char)std::toupper((unsigned char)first[0]);

    users.reserve(o.count);

    for (size_t i = 0; i < o.count; i++)
    {
        std::string number = std::to_string(o.start + i);
        number.insert(0, width - number.size(), '0');

        UserRecord u;
        u.firstName       = first;
        u.lastName        = number;
        u.password        = o.password;
        u.shortName       = o.prefix + number;
        u.internetAddress = u.shortName + "@" + o.domain;
        users.push_back(u);
    }

    return users;
}

size_t NameCombinations()
{
    return FIRST_NAME_COUNT * LAST_NAME_COUNT;
}

bool RandomNameUsers(size_t count, const std::string &password, const std::string &domain, uint64_t seed, std::vector<UserRecord> &users, std::string &err,
                     const std::set<std::string> *exclude)
{
    return RandomNameUsersFrom(FIRST_NAMES, FIRST_NAME_COUNT, LAST_NAMES, LAST_NAME_COUNT, count, password, domain, seed, users, err, exclude);
}

bool RandomNameUsersFrom(const char *const *firstNames, size_t firstCount, const char *const *lastNames, size_t lastCount, size_t count, const std::string &password,
                         const std::string &domain, uint64_t seed, std::vector<UserRecord> &users, std::string &err, const std::set<std::string> *exclude)
{
    users.clear();

    const size_t total = firstCount * lastCount;

    if (count > total)
    {
        err = "the name lists give " + std::to_string(total) + " different users, not " + std::to_string(count);
        return false;
    }

    // Every combination of first and last name is one number. The numbers are shuffled one step at a
    // time (Fisher-Yates) and taken in that order: no combination twice, whatever the count is. A
    // combination that is in the exclude list (an earlier list) is skipped.
    std::vector<uint32_t> pool(total);

    for (size_t i = 0; i < total; i++)
        pool[i] = (uint32_t)i;

    SeededRandom rnd(seed);

    users.reserve(count);

    for (size_t i = 0; i < total && users.size() < count; i++)
    {
        size_t j = i + rnd.Below(total - i);
        std::swap(pool[i], pool[j]);

        const char *first = firstNames[pool[i] / lastCount];
        const char *last  = lastNames[pool[i] % lastCount];

        UserRecord u;
        u.firstName       = first;
        u.lastName        = last;
        u.password        = password;
        u.shortName       = Lower(first) + "." + Lower(last);
        u.internetAddress = u.shortName + "@" + domain;

        if (NULL != exclude && (exclude->count(Lower(u.shortName)) || exclude->count(Lower(u.internetAddress))))
            continue;

        users.push_back(u);
    }

    if (users.size() < count)
    {
        err = "only " + std::to_string(users.size()) + " name combinations are left that are not in the exclude list, not " + std::to_string(count);
        users.clear();
        return false;
    }

    return true;
}

bool RandomPassword(size_t length, std::string &password, std::string &err)
{
    static const std::string ALL = "ABCDEFGHJKMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz23456789"; // 55 characters
    static const size_t      LIMIT = 4 * 55;                                                  // bytes below this are used: no bias

    if (length < 8 || length > 128)
    {
        err = "the password length must be 8 to 128";
        return false;
    }

    // Every character is one random byte of the operating system's generator (a byte at or above the
    // limit is thrown away, so every character is equally likely). A password without an upper case
    // letter, a lower case letter or a digit is thrown away as a whole and drawn again; that is about
    // one in four for 8 characters, and rare for longer ones.
    for (int attempt = 0; attempt < 1000; attempt++)
    {
        password.assign(length, ' ');

        for (size_t i = 0; i < length; i++)
        {
            unsigned char b = 0;

            do
            {
                if (!FillRandom(&b, 1, err))
                    return false;
            } while (b >= LIMIT);

            password[i] = ALL[b % ALL.size()];
        }

        bool upper = false, lower = false, digit = false;

        for (size_t i = 0; i < length; i++)
        {
            unsigned char c = (unsigned char)password[i];

            upper = upper || std::isupper(c);
            lower = lower || std::islower(c);
            digit = digit || std::isdigit(c);
        }

        if (upper && lower && digit)
            return true;
    }

    err = "cannot make a password with all kinds of characters";
    return false;
}

bool SetRandomPasswords(std::vector<UserRecord> &users, size_t length, std::string &err)
{
    for (size_t i = 0; i < users.size(); i++)
    {
        if (!RandomPassword(length, users[i].password, err))
            return false;
    }

    return true;
}

bool CheckUnique(const std::vector<UserRecord> &users, std::string &err)
{
    std::set<std::string> shorts;
    std::set<std::string> addresses;

    for (size_t i = 0; i < users.size(); i++)
    {
        if (!shorts.insert(Lower(users[i].shortName)).second)
        {
            err = "record " + std::to_string(i + 1) + ": duplicate short name " + users[i].shortName;
            return false;
        }

        if (!addresses.insert(Lower(users[i].internetAddress)).second)
        {
            err = "record " + std::to_string(i + 1) + ": duplicate internet address " + users[i].internetAddress;
            return false;
        }
    }

    return true;
}

std::string FormatUsersCsv(const std::vector<UserRecord> &users, bool header)
{
    std::string out;

    if (header)
        out += "FirstName,LastName,Password,Shortname,InternetAddress\n";

    for (size_t i = 0; i < users.size(); i++)
    {
        out += Quoted(users[i].firstName) + "," + Quoted(users[i].lastName) + "," + Quoted(users[i].password) + "," +
               Quoted(users[i].shortName) + "," + Quoted(users[i].internetAddress) + "\n";
    }

    return out;
}

bool WriteUsersFile(const std::string &path, const std::string &text, bool overwrite, std::string &err)
{
    FILE *f = NULL;

#ifdef _WIN32
    f = std::fopen(path.c_str(), overwrite ? "wb" : "wbx");
#else
    // Owner only, and without overwrite never replace an existing file.
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | (overwrite ? O_TRUNC : O_EXCL), 0600);

    if (fd >= 0)
        f = ::fdopen(fd, "wb");
#endif

    if (NULL == f)
    {
        err = overwrite ? "cannot write " + path : "cannot create " + path + " (does it exist? --force replaces it)";
        return false;
    }

    bool ok = (text.size() == std::fwrite(text.data(), 1, text.size(), f));
    ok      = (0 == std::fclose(f)) && ok;

    if (!ok)
        err = "cannot write " + path;

    return ok;
}
