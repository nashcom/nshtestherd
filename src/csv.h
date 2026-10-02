// csv.h - nshreg-format user import (FirstName,LastName,Password,Shortname,InternetAddress)

#pragma once

#include <string>
#include <vector>

struct UserRecord
{
    std::string firstName;
    std::string lastName;
    std::string password;
    std::string shortName;
    std::string internetAddress;
    bool        allocated = false;
};

// Parses and validates the complete text. On failure users is left empty and
// err carries "record N: ..." (N counts every record in the input, 1-based,
// including a header row and skipped empty lines).
bool ParseUsersCsv(const std::string &text, std::vector<UserRecord> &users, std::string &err);

// Simulates a standard CSV: count users named <prefix><NNNNNN> (zero padded to at
// least 6 digits), same shape as examples/users.csv. FirstName is the prefix with
// its first letter upper-cased, LastName the number, e.g.
//   Load,000001,TestPassword,load000001,load000001@example.com
std::vector<UserRecord> GenerateUsers(size_t count, const std::string &prefix, const std::string &password, const std::string &domain);
