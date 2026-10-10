// gen.h - the user lists of nshtestusers: numbered users, random unique names, passwords, CSV text

#pragma once

#include "../../src/csv.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

struct NumberedOptions
{
    size_t      count    = 100;
    size_t      start    = 1;
    std::string prefix   = "load";
    std::string password = "TestPassword";
    std::string domain   = "example.com";
};

// Numbered users, the same shape as the --generate option of nshtestherd (GenerateUsers):
//   Load,000001,TestPassword,load000001,load000001@example.com
// With the default start the list is identical to GenerateUsers; --start lets a second batch continue
// where the first ended. The number is zero padded to at least 6 digits.
std::vector<UserRecord> NumberedUsers(const NumberedOptions &options);

// How many different users the built-in name lists give.
size_t NameCombinations();

// count users with random names, every first and last name combination at most once. The short name is
// <first>.<last> in lower case, the address <first>.<last>@<domain>. The same seed gives the same list.
// Fails when count is above NameCombinations().
// exclude (optional): lower case short names and addresses of an earlier list; users with them are skipped,
// so a second batch does not collide with the first. Fails when fewer than count names are left.
bool RandomNameUsers(size_t count, const std::string &password, const std::string &domain, uint64_t seed, std::vector<UserRecord> &users, std::string &err,
                     const std::set<std::string> *exclude = NULL);

// The same with name lists of your own (the tests use small ones: the built-in pool is far too big to use up in a test).
bool RandomNameUsersFrom(const char *const *firstNames, size_t firstCount, const char *const *lastNames, size_t lastCount, size_t count, const std::string &password, const std::string &domain, uint64_t seed, std::vector<UserRecord> &users, std::string &err,
                         const std::set<std::string> *exclude = NULL);

// A random password of the given length (8 to 128) from the operating system's generator: every character is
// equally likely among 55 letters and digits (letters that look alike, 0 O 1 l I, are not used), and a password
// without an upper case letter, a lower case letter and a digit is drawn again.
bool RandomPassword(size_t length, std::string &password, std::string &err);

// Gives every user its own random password.
bool SetRandomPasswords(std::vector<UserRecord> &users, size_t length, std::string &err);

// Fails when two users have the same short name or the same internet address (compared without regard to case).
bool CheckUnique(const std::vector<UserRecord> &users, std::string &err);

// The CSV text: five columns, fields in quotes where needed, LF line ends, an optional header row.
std::string FormatUsersCsv(const std::vector<UserRecord> &users, bool header);

// Writes the text to the file. The file is created readable for the owner only (it holds passwords). An
// existing file is only replaced with overwrite.
bool WriteUsersFile(const std::string &path, const std::string &text, bool overwrite, std::string &err);
