// test_users.cpp - the tests of nshtestusers, without a test framework
//
// Every test has a name and one result line: [ OK ], [ FAIL ] or [ -- ] (skipped). A test that
// checks many values in a loop is still one test; a failing check is shown under its line.

#include "../src/csv.h"
#include "../tools/nshtestusers/gen.h"
#include "../tools/nshtestusers/names.h"
#include "../tools/nshtestusers/random.h"

#include <cctype>
#include <cstdio>
#include <set>
#include <string>

#ifndef _WIN32
#include <sys/stat.h>
#endif

static std::string g_detail;   // failed checks of the running test
static std::string g_skipped;  // reason, when the running test skips itself

#define CHECK(cond)                                                                           \
    do                                                                                        \
    {                                                                                         \
        if (!(cond))                                                                          \
            g_detail += std::string("         ") + __FILE__ + ":" + std::to_string(__LINE__) + ": " + #cond + "\n"; \
    } while (0)

static std::string Lower(std::string s)
{
    for (size_t i = 0; i < s.size(); i++)
        s[i] = (char)std::tolower((unsigned char)s[i]);

    return s;
}

static bool OnlyLetters(const std::string &s)
{
    for (size_t i = 0; i < s.size(); i++)
    {
        if (!std::isalpha((unsigned char)s[i]))
            return false;
    }

    return !s.empty();
}

static bool Same(const UserRecord &a, const UserRecord &b)
{
    return a.firstName == b.firstName && a.lastName == b.lastName && a.password == b.password && a.shortName == b.shortName &&
           a.internetAddress == b.internetAddress;
}

static std::vector<UserRecord> Three()
{
    std::vector<UserRecord> users = NumberedUsers(NumberedOptions());
    users.resize(3);
    return users;
}

// ---- name lists ---------------------------------------------------------------

static void NameListsOnlyLetters()
{
    for (size_t i = 0; i < FIRST_NAME_COUNT; i++)
        CHECK(OnlyLetters(FIRST_NAMES[i]));

    for (size_t i = 0; i < LAST_NAME_COUNT; i++)
        CHECK(OnlyLetters(LAST_NAMES[i]));
}

static void NameListsUnique()
{
    std::set<std::string> first, last;

    for (size_t i = 0; i < FIRST_NAME_COUNT; i++)
        CHECK(first.insert(Lower(FIRST_NAMES[i])).second);

    for (size_t i = 0; i < LAST_NAME_COUNT; i++)
        CHECK(last.insert(Lower(LAST_NAMES[i])).second);
}

static void NameListsEnoughCombinations()
{
    CHECK(NameCombinations() == FIRST_NAME_COUNT * LAST_NAME_COUNT);
    CHECK(FIRST_NAME_COUNT >= 1000 && LAST_NAME_COUNT >= 1000);
    CHECK(NameCombinations() >= 1000000); // --generate takes up to 1000000 users
}

// ---- numbered users -----------------------------------------------------------

static void NumberedLikeNshtestherd()
{
    NumberedOptions o;
    o.count = 25;

    std::vector<UserRecord> mine   = NumberedUsers(o);
    std::vector<UserRecord> theirs = GenerateUsers(25, "load", "TestPassword", "example.com");

    CHECK(25 == mine.size() && 25 == theirs.size());

    for (size_t i = 0; i < mine.size() && i < theirs.size(); i++)
        CHECK(Same(mine[i], theirs[i]));
}

static void NumberedShape()
{
    std::vector<UserRecord> users = NumberedUsers(NumberedOptions());

    CHECK(100 == users.size());
    CHECK("Load" == users[0].firstName && "000001" == users[0].lastName && "load000001" == users[0].shortName);
    CHECK("load000001@example.com" == users[0].internetAddress && "TestPassword" == users[0].password);
    CHECK("load000100" == users[99].shortName);
}

static void NumberedSevenDigitsForAMillion()
{
    NumberedOptions o;
    o.count = 1000000;

    std::vector<UserRecord> users = NumberedUsers(o);

    CHECK(1000000 == users.size());
    CHECK("0000001" == users[0].lastName && "1000000" == users[999999].lastName);
}

static void NumberedPrefixDomainPasswordStart()
{
    NumberedOptions p;
    p.count    = 3;
    p.start    = 501;
    p.prefix   = "perf";
    p.domain   = "lab.example.com";
    p.password = "x,y";

    std::vector<UserRecord> users = NumberedUsers(p);

    CHECK(3 == users.size());
    CHECK("Perf" == users[0].firstName && "000501" == users[0].lastName && "perf000503" == users[2].shortName);
    CHECK("perf000501@lab.example.com" == users[0].internetAddress && "x,y" == users[0].password);
}

static void NumberedGrowPastSixDigits()
{
    NumberedOptions p;
    p.start = 999999;
    p.count = 3;

    std::vector<UserRecord> users = NumberedUsers(p);

    CHECK("0999999" == users[0].lastName && "1000001" == users[2].lastName);
}

// ---- random unique names ------------------------------------------------------

static void NamesNoCombinationTwice()
{
    std::vector<UserRecord> users;
    std::string             err;

    CHECK(RandomNameUsers(3000, "pw", "example.com", 42, users, err));
    CHECK(3000 == users.size());
    CHECK(CheckUnique(users, err));

    std::set<std::string> pairs;

    for (size_t i = 0; i < users.size(); i++)
        CHECK(pairs.insert(users[i].firstName + " " + users[i].lastName).second);
}

static void NamesShortNameAddressPassword()
{
    std::vector<UserRecord> users;
    std::string             err;

    CHECK(RandomNameUsers(10, "pw", "lab.example.com", 1, users, err));

    for (size_t i = 0; i < users.size(); i++)
    {
        CHECK(Lower(users[i].firstName) + "." + Lower(users[i].lastName) == users[i].shortName);
        CHECK(users[i].shortName + "@lab.example.com" == users[i].internetAddress);
        CHECK("pw" == users[i].password);
    }
}

static void NamesSameSeedSameList()
{
    std::vector<UserRecord> a, b;
    std::string             err;

    CHECK(RandomNameUsers(3000, "pw", "example.com", 42, a, err));
    CHECK(RandomNameUsers(3000, "pw", "example.com", 42, b, err));
    CHECK(a.size() == b.size());

    for (size_t i = 0; i < a.size() && i < b.size(); i++)
        CHECK(Same(a[i], b[i]));
}

static void NamesOtherSeedOtherList()
{
    std::vector<UserRecord> a, c;
    std::string             err;

    CHECK(RandomNameUsers(3000, "pw", "example.com", 42, a, err));
    CHECK(RandomNameUsers(3000, "pw", "example.com", 43, c, err));

    bool other = false;

    for (size_t i = 0; i < a.size() && i < c.size(); i++)
        other = other || !Same(a[i], c[i]);

    CHECK(other);
}

// Small lists of the test: 4 first and 5 last names give 20 users, which can be used up completely.
static const char *const SMALL_FIRST[] = { "Ann", "Bob", "Cy", "Di" };
static const char *const SMALL_LAST[]  = { "Lee", "Fox", "Ray", "Poe", "Kay" };

static void NamesTheWholePool()
{
    std::vector<UserRecord> all;
    std::string             err;

    CHECK(RandomNameUsersFrom(SMALL_FIRST, 4, SMALL_LAST, 5, 20, "pw", "example.com", 7, all, err));
    CHECK(20 == all.size());
    CHECK(CheckUnique(all, err));

    std::set<std::string> pairs;

    for (size_t i = 0; i < all.size(); i++)
        CHECK(pairs.insert(all[i].shortName).second);

    CHECK(20 == pairs.size());
}

static void NamesOneMoreThanThePoolIsRefused()
{
    std::vector<UserRecord> all;
    std::string             err;

    CHECK(!RandomNameUsersFrom(SMALL_FIRST, 4, SMALL_LAST, 5, 21, "pw", "example.com", 7, all, err));
    CHECK(!err.empty() && all.empty());
    CHECK(std::string::npos != err.find("20"));
}

static std::set<std::string> ExcludeOf(const std::vector<UserRecord> &users)
{
    std::set<std::string> exclude;

    for (size_t i = 0; i < users.size(); i++)
    {
        exclude.insert(Lower(users[i].shortName));
        exclude.insert(Lower(users[i].internetAddress));
    }

    return exclude;
}

static void NamesExcludeSkipsAnEarlierList()
{
    std::vector<UserRecord> a, b;
    std::string             err;

    CHECK(RandomNameUsersFrom(SMALL_FIRST, 4, SMALL_LAST, 5, 10, "pw", "example.com", 1, a, err));

    std::set<std::string> exclude = ExcludeOf(a);

    CHECK(RandomNameUsersFrom(SMALL_FIRST, 4, SMALL_LAST, 5, 10, "pw", "example.com", 2, b, err, &exclude));
    CHECK(10 == b.size());

    // no name of the second list is in the first, and together they use the whole pool of 20
    std::set<std::string> all;

    for (size_t i = 0; i < a.size(); i++)
        CHECK(all.insert(a[i].shortName).second);

    for (size_t i = 0; i < b.size(); i++)
        CHECK(all.insert(b[i].shortName).second);

    CHECK(20 == all.size());
}

static void NamesExcludeRefusesWhenTooFewAreLeft()
{
    std::vector<UserRecord> a, b;
    std::string             err;

    CHECK(RandomNameUsersFrom(SMALL_FIRST, 4, SMALL_LAST, 5, 10, "pw", "example.com", 1, a, err));

    std::set<std::string> exclude = ExcludeOf(a);

    CHECK(!RandomNameUsersFrom(SMALL_FIRST, 4, SMALL_LAST, 5, 11, "pw", "example.com", 2, b, err, &exclude));
    CHECK(b.empty());
    CHECK(std::string::npos != err.find("only 10"));
}

// ---- passwords and random generators ------------------------------------------

static void PasswordLengthLimits()
{
    std::string pw, err;

    CHECK(!RandomPassword(7, pw, err));
    CHECK(!RandomPassword(129, pw, err));
    CHECK(RandomPassword(8, pw, err) && 8 == pw.size());
    CHECK(RandomPassword(128, pw, err) && 128 == pw.size());
}

static void PasswordKindsOfCharacters()
{
    std::string pw, err;

    for (int i = 0; i < 500; i++)
    {
        size_t length = (0 == i % 3) ? 8 : ((1 == i % 3) ? 16 : 128);

        CHECK(RandomPassword(length, pw, err));
        CHECK(length == pw.size());

        bool upper = false, lower = false, digit = false, bad = false;

        for (size_t k = 0; k < pw.size(); k++)
        {
            unsigned char c = (unsigned char)pw[k];

            upper = upper || std::isupper(c);
            lower = lower || std::islower(c);
            digit = digit || std::isdigit(c);
            bad   = bad || !std::isalnum(c) || '0' == c || 'O' == c || '1' == c || 'l' == c || 'I' == c;
        }

        CHECK(upper && lower && digit && !bad);
    }
}

static void PasswordsAreNotRepeated()
{
    std::set<std::string> seen;
    std::string           pw, err;

    for (int i = 0; i < 500; i++)
    {
        CHECK(RandomPassword(16, pw, err));
        CHECK(seen.insert(pw).second);
    }
}

static void PasswordPerUser()
{
    std::vector<UserRecord> users = NumberedUsers(NumberedOptions());
    std::string             err;

    CHECK(SetRandomPasswords(users, 12, err));

    std::set<std::string> distinct;

    for (size_t i = 0; i < users.size(); i++)
    {
        CHECK(12 == users[i].password.size());
        distinct.insert(users[i].password);
    }

    CHECK(distinct.size() == users.size());
}

static void SeededGeneratorKnownValues()
{
    // splitmix64 with seed 0: the published test values
    SeededRandom rnd(0);

    CHECK(0xE220A8397B1DCDAFULL == rnd.Next());
    CHECK(0x6E789E6AA1B965F4ULL == rnd.Next());
}

static void SeededGeneratorStaysInRange()
{
    SeededRandom rnd(1);

    for (int i = 0; i < 1000; i++)
        CHECK(rnd.Below(7) < 7);
}

static void SystemGeneratorStaysInRange()
{
    std::string err;
    size_t      n = 0;

    for (int i = 0; i < 200; i++)
    {
        CHECK(SecureBelow(10, n, err));
        CHECK(n < 10);
    }
}

static void SystemGeneratorIsReadInBlocks()
{
    // 5000 passwords need about a million random bytes, which is a few blocks of 64 KB. One read
    // of the operating system per random number (about 125000 reads) took minutes for 100000 users.
    const size_t before = RandomOsReads();
    std::string  pw, err;

    for (int i = 0; i < 5000; i++)
        CHECK(RandomPassword(16, pw, err));

    CHECK(RandomOsReads() - before < 100);
}

// ---- CSV text -----------------------------------------------------------------

static void CsvRoundTrip(bool header)
{
    std::vector<UserRecord> users = Three();
    users[0].password             = "with,comma";
    users[1].password             = "with\"quote";
    users[2].password             = "line\nbreak";

    std::vector<UserRecord> back;
    std::string             err;
    std::string             text = FormatUsersCsv(users, header);

    CHECK(header == (0 == text.find("FirstName,LastName,Password,Shortname,InternetAddress\n")));
    CHECK(ParseUsersCsv(text, back, err));
    CHECK(3 == back.size());

    for (size_t i = 0; i < back.size() && i < users.size(); i++)
        CHECK(Same(users[i], back[i]));
}

static void CsvRoundTripWithoutHeader()
{
    CsvRoundTrip(false);
}

static void CsvRoundTripWithHeader()
{
    CsvRoundTrip(true);
}

static void CsvFirstLineOfTheDefaultList()
{
    std::string text = FormatUsersCsv(NumberedUsers(NumberedOptions()), false);

    CHECK("Load,000001,TestPassword,load000001,load000001@example.com\n" == text.substr(0, 59));
}

// ---- duplicate check ----------------------------------------------------------

static void CheckAcceptsUniqueList()
{
    std::string err;

    CHECK(CheckUnique(Three(), err));
}

static void CheckFindsDuplicateShortName()
{
    std::vector<UserRecord> users = Three();
    std::string             err;

    users[2].shortName = "LOAD000001";

    CHECK(!CheckUnique(users, err));
    CHECK(std::string::npos != err.find("record 3"));
}

static void CheckFindsDuplicateAddress()
{
    std::vector<UserRecord> users = Three();
    std::string             err;

    users[1].internetAddress = "Load000001@Example.com";

    CHECK(!CheckUnique(users, err));
    CHECK(std::string::npos != err.find("record 2"));
}

// ---- output file --------------------------------------------------------------

static const char *OUTPUT_PATH = "test_users_output.csv";

static std::string ReadOutput()
{
    std::string out;
    FILE       *f = std::fopen(OUTPUT_PATH, "rb");

    if (NULL != f)
    {
        char   buf[64];
        size_t n;

        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            out.append(buf, n);

        std::fclose(f);
    }

    return out;
}

static void OutputFileIsCreated()
{
    std::string err;

    std::remove(OUTPUT_PATH);

    CHECK(WriteUsersFile(OUTPUT_PATH, "one\n", false, err));
    CHECK("one\n" == ReadOutput());

    std::remove(OUTPUT_PATH);
}

static void OutputFileIsNotReplaced()
{
    std::string err;

    std::remove(OUTPUT_PATH);

    CHECK(WriteUsersFile(OUTPUT_PATH, "one\n", false, err));
    CHECK(!WriteUsersFile(OUTPUT_PATH, "two\n", false, err));
    CHECK(!err.empty());
    CHECK("one\n" == ReadOutput());

    std::remove(OUTPUT_PATH);
}

static void OutputFileIsReplacedWithForce()
{
    std::string err;

    std::remove(OUTPUT_PATH);

    CHECK(WriteUsersFile(OUTPUT_PATH, "one\n", false, err));
    CHECK(WriteUsersFile(OUTPUT_PATH, "three\n", true, err));
    CHECK("three\n" == ReadOutput());

    std::remove(OUTPUT_PATH);
}

static void OutputFileIsForTheOwnerOnly()
{
#ifdef _WIN32
    g_skipped = "Windows has no file modes like this";
#else
    std::string err;

    std::remove(OUTPUT_PATH);
    CHECK(WriteUsersFile(OUTPUT_PATH, "one\n", false, err));

    struct stat st;
    CHECK(0 == stat(OUTPUT_PATH, &st));

    // Some file systems show every file as 0777 whatever mode it was created with (a Windows drive
    // in WSL, /mnt/d). The mode can only be checked where a chmod 0600 sticks.
    const std::string probe = std::string(OUTPUT_PATH) + ".probe";
    FILE             *p     = std::fopen(probe.c_str(), "wb");

    if (NULL != p)
        std::fclose(p);

    chmod(probe.c_str(), 0600);

    struct stat ps;
    bool        keepsModes = (0 == stat(probe.c_str(), &ps)) && (0 == (ps.st_mode & 077));
    std::remove(probe.c_str());

    if (keepsModes)
        CHECK(0 == (st.st_mode & 077));
    else
        g_skipped = "this file system does not keep modes (a Windows drive in WSL)";

    std::remove(OUTPUT_PATH);
#endif
}

// ---- the list of tests --------------------------------------------------------

struct TestCase
{
    const char *name;
    void      (*run)();
};

static const TestCase TESTS[] = {
    { "name lists: only letters",                                             NameListsOnlyLetters },
    { "name lists: every name once, ignoring case",                           NameListsUnique },
    { "name lists: at least 10000 combinations",                              NameListsEnoughCombinations },
    { "numbered users: identical to nshtestherd --generate",                  NumberedLikeNshtestherd },
    { "numbered users: shape of the records",                                 NumberedShape },
    { "numbered users: 7 digits for a list of 1000000",                       NumberedSevenDigitsForAMillion },
    { "numbered users: prefix, domain, password and --start",                 NumberedPrefixDomainPasswordStart },
    { "numbered users: the number grows past 6 digits",                       NumberedGrowPastSixDigits },
    { "random names: no combination twice",                                   NamesNoCombinationTwice },
    { "random names: short name, address and password",                       NamesShortNameAddressPassword },
    { "random names: the same seed gives the same list",                      NamesSameSeedSameList },
    { "random names: another seed gives another list",                        NamesOtherSeedOtherList },
    { "random names: a small pool can be used up completely",                             NamesTheWholePool },
    { "random names: one more than the pool is refused",                      NamesOneMoreThanThePoolIsRefused },
    { "random names: --exclude skips the names of an earlier list",            NamesExcludeSkipsAnEarlierList },
    { "random names: --exclude refuses when too few names are left",         NamesExcludeRefusesWhenTooFewAreLeft },
    { "passwords: length limits 8 to 128",                                    PasswordLengthLimits },
    { "passwords: upper case, lower case, digit, no look-alike letters",      PasswordKindsOfCharacters },
    { "passwords: not repeated",                                              PasswordsAreNotRepeated },
    { "passwords: a different one per user",                                  PasswordPerUser },
    { "seeded generator: the published splitmix64 values",                    SeededGeneratorKnownValues },
    { "seeded generator: numbers stay in range",                              SeededGeneratorStaysInRange },
    { "system generator: numbers stay in range",                              SystemGeneratorStaysInRange },
    { "system generator: the operating system is read in blocks",             SystemGeneratorIsReadInBlocks },
    { "CSV text: round trip through the nshtestherd parser",                  CsvRoundTripWithoutHeader },
    { "CSV text: round trip with the header row",                             CsvRoundTripWithHeader },
    { "CSV text: first line of the default list",                             CsvFirstLineOfTheDefaultList },
    { "check: a unique list passes",                                          CheckAcceptsUniqueList },
    { "check: a duplicate short name is found",                               CheckFindsDuplicateShortName },
    { "check: a duplicate address is found",                                  CheckFindsDuplicateAddress },
    { "output file: is created",                                              OutputFileIsCreated },
    { "output file: an existing file is not replaced",                        OutputFileIsNotReplaced },
    { "output file: --force replaces it",                                     OutputFileIsReplacedWithForce },
    { "output file: readable for the owner only",                             OutputFileIsForTheOwnerOnly },
};

int main()
{
    const size_t total   = sizeof(TESTS) / sizeof(TESTS[0]);
    size_t       failed  = 0;
    size_t       skipped = 0;

    for (size_t i = 0; i < total; i++)
    {
        g_detail.clear();
        g_skipped.clear();

        TESTS[i].run();

        if (!g_detail.empty())
        {
            failed++;
            std::printf("[ FAIL ] %s\n%s", TESTS[i].name, g_detail.c_str());
        }
        else if (!g_skipped.empty())
        {
            skipped++;
            std::printf("[ -- ]   %s: skipped, %s\n", TESTS[i].name, g_skipped.c_str());
        }
        else
            std::printf("[ OK ]   %s\n", TESTS[i].name);
    }

    std::printf("\n");

    if (0 == failed)
    {
        std::printf("[ OK ]   %zu tests, %zu passed, %zu skipped\n", total, total - skipped, skipped);
        return 0;
    }

    std::printf("[ FAIL ] %zu tests, %zu failed, %zu skipped\n", total, failed, skipped);
    return 1;
}
