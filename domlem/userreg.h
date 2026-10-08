/* userreg.h - register one Domino user, with mail file (implemented in userreg.cpp)
 *
 * domlem calls RegEnsureUser() in "-switch" mode, once per worker, right after the coordinator has allocated a test
 * account: make sure the Domino user of that account exists, so that its ID can be downloaded from the ID vault
 * and the process can switch to it. The user is registered together with its mail file.
 *
 */

#ifndef DOMLEM_USERREG_H
#define DOMLEM_USERREG_H

#include <string>

#include <global.h>

/* What is the same for every user: where and with which certifier to register. Plain C++ strings; empty means
 * "not set" (the defaults below apply) */
struct DomRegSetup
{
    /* The certifier: a certifier ID file with its password is used directly, else the Domino CA. One of the two is required. */
    struct Certifier
    {
        std::string caName;                   /* name of the Domino CA */
        std::string file;                     /* certifier ID file */
        std::string password;                 /* password of the certifier ID file */
    };

    /* The mail file that is created together with the user */
    struct Mail
    {
        std::string templateFile;             /* mail template, for example mail14.ntf; empty: server default */
        std::string directory;                /* directory of the mail files; empty: g_szDefaultMailDir */
    };

    std::string server;                       /* registration, directory and mail server; empty: the server of the caller */
    std::string policy;                       /* explicit policy (for example the one that puts the ID into the ID vault); empty: none */
    WORD        expirationMonths = 0;         /* validity of the new certificates; 0: g_wDefaultMonths */

    Certifier   certifier;
    Mail        mail;
};

/* Registration defaults, defined in domlem.cpp (variables, so they can be changed later) */
extern WORD g_wMailSystem;                    /* mail system of new users: 0 is Notes mail */
extern char g_szDefaultMailDir[MAXPATH+1];    /* mail directory when DomRegSetup has none */
extern WORD g_wDefaultMonths;                 /* certificate validity when DomRegSetup has none */

/* Makes sure the Domino user of a test account exists and registers it when it does not (ID, directory entry and
 * mail file in one step).
 *
 * The existence check is a directory lookup by full name, short name and internet address: if any of them is found
 * the user is taken as existing and nothing is registered. Repeatable: calling it again for an existing user only
 * does the lookup.
 *
 *   pSetup              server and certifier (see DomRegSetup)
 *   pszFirstName ...    fields of the test account, as in the nshreg CSV (first or last name may be empty, not both)
 *   pszShortName        short name of the account
 *   pszInternetAddress  mail address of the account
 *   pszRetUserName      receives the hierarchical Notes name, the name SECidfGet() is called with
 *   wMaxUserName        size of pszRetUserName
 *   pbRetCreated        receives TRUE when the user was registered by this call (its ID may need a moment to reach the
 *                       ID vault), FALSE when it existed; may be NULL
 *
 * Returns NOERROR when the user exists or was created.
 */
STATUS RegEnsureUser (const DomRegSetup *pSetup,
                      const char         *pszFirstName,
                      const char         *pszLastName,
                      const char         *pszPassword,
                      const char         *pszShortName,
                      const char         *pszInternetAddress,
                      char               *pszRetUserName,
                      WORD                wMaxUserName,
                      BOOL               *pbRetCreated);

#endif /* DOMLEM_USERREG_H */
