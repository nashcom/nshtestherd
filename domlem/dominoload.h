/* dominoload.h - the operations domlem performs against a Domino server (implemented in dominoload.cpp)
 *
 * DominoLoad owns the Notes handles: Init() opens the test database, Term() (or the destructor) releases everything.
 * Every test is a method, Op<Name>(). It does one operation and returns the Notes STATUS; Message has the result for
 * the coordinator (or the error text when it fails). New tests are added one after another as new methods.
 *
 * The handles belong to the identity the process has when Init() runs: with an identity switch (-switch), call
 * Init() after the switch.
 *
 *   DominoLoad Load;
 *   std::string Message;
 *
 *   Load.SetAgent ("TestAgent", 600);
 *   Load.Init ("server", "nshtestherd.nsf", Message);
 *   Load.OpRunAgent (Message);          // as often as needed
 *   Load.Term();
 */

#ifndef DOMLEM_DOMINOLOAD_H
#define DOMLEM_DOMINOLOAD_H

#include <string>
#include <vector>

#include <global.h>
#include <nsfdb.h>

#include "agentrun.h"
#include "mailclient.h"

/* Subject of the mails: "DEFAULT" means the job decides (mail: fixed, mailtest: random) */
enum MailSubject
{
    SUBJECT_DEFAULT,
    SUBJECT_FIXED,               /* "domlem load test" */
    SUBJECT_RANDOM               /* a few words or a sentence of the body text style */
};

/* Sent copy in the sender's mail file: "DEFAULT" means the job decides (mail: off, mailtest: auto) */
enum SentCopyMode
{
    SENTCOPY_DEFAULT,
    SENTCOPY_OFF,
    SENTCOPY_AUTO,               /* when the sender has a mail file in the directory; else none, logged once */
    SENTCOPY_ON                  /* required: no mail file is an error */
};

/* What a mail of the mail jobs contains */
struct MailSettings
{
    MailSettings() : BodyKB (1, 20), BodyStyle (TEXT_LOREM), AttachCount (0, 2), AttachKB (10, 200), bAttachBinary (TRUE), RandomCount (0), bRandomCountSet (FALSE), DirectoryFile ("names.nsf"), Subject (SUBJECT_DEFAULT), SentCopy (SENTCOPY_DEFAULT) {}

    ValueRange BodyKB;           /* size of the generated body text in KB, fixed or a range, 0: no body */
    TextStyle BodyStyle;         /* kind of the body text (and of a random subject) */
    ValueRange AttachCount;      /* attachments per mail, fixed or a range */
    ValueRange AttachKB;         /* size of one attachment in KB, fixed or a range */
    BOOL      bAttachBinary;     /* random bytes (TRUE) or generated text (FALSE) */
    ValueRange RandomCount;      /* random recipients per mail, fixed or a range, picked from the directory; 0: none */
    BOOL      bRandomCountSet;   /* RandomCount was given as an option (else the job decides) */
    std::string RandomFilter;    /* selection formula of the people to pick from; empty: the test pool of the account */
    std::string DirectoryFile;   /* the Domino Directory to search */
    MailSubject  Subject;
    SentCopyMode SentCopy;
};


class DominoLoad
{
public:

    DominoLoad();
    ~DominoLoad();

    /* Settings: take effect with the next Init() */
    void SetAgent (const char *pszAgentName, DWORD dwTimeLimitSeconds);

    /* Mail: recipients of the messages, comma separated (empty: the current user, mail to self), and the content.
     * Subject and SentCopy must be decided (not DEFAULT); DEFAULT counts as fixed subject, no sent copy. */
    void SetMail (const char *pszRecipients, const MailSettings &Settings);

    /* The worker, for subjects and attachment names: "[domlem t<test id> <short name> #<n>]" */
    void SetWorker (const char *pszTestId, const char *pszShortName);

    /* Opens the test database (pszServer may be empty: local; pszDbFile may be empty: no database, for jobs that need
     * none, like the mail job). On an error Err has the failing call with its error text
     * and nothing stays open. Calling it again closes what was open before and resets the counters. */
    STATUS Init (const char *pszServer, const char *pszDbFile, std::string &Err);

    /* Releases everything. Safe to call more than once. */
    void Term();

    BOOL IsInit() const { return m_bInit; }

    /* The Domino logical name of the open test database: the file part of its canonical path (NSFDbPathGet, split by
     * OSPathNetParse), relative to the data directory, for example "names.nsf" or "mtdata/mtstore.nsf". Empty when no
     * database is open or the path cannot be read. */
    std::string GetDbLogicalPath() const;

    /* ---- the tests ---- */

    /* Opens the database as a new connection, reads the access level and closes it again (the test of a connect) */
    STATUS OpDbOpen (std::string &Message);

    /* Runs the agent once. The agent is opened with the first run and stays open until Term() */
    STATUS OpRunAgent (std::string &Message);

    /* Crafts one message and writes it into the mail.box of the server (and a sent copy into the sender's mail file when
     * the settings ask for it). The mail.box is opened with the first send. */
    STATUS OpSendMail (std::string &Message);

private:

    STATUS LoadRandomRecipients (std::string &Err);
    STATUS OpenSentCopy (std::string &Err);

    DominoLoad (const DominoLoad &);                /* not copyable: owns Notes handles */
    DominoLoad &operator= (const DominoLoad &);

    BOOL        m_bInit;                    /* Init() succeeded */
    DBHANDLE    m_hDb;                      /* the test database, NULLHANDLE when Init() got no database name */
    char        m_szFullPath[MAXPATH+1];    /* server!!path of the test database */
    std::string m_DbFile;                   /* the database name as given, for messages */

    std::string m_AgentName;
    DWORD       m_dwAgentTimeLimit;
    AgentRunner m_Agent;

    std::string m_Server;                   /* server of the test database and of the mail.box */
    std::string m_UserName;                 /* the identity of the process, the sender of the mail */
    std::string m_MailTo;
    MailSettings m_MailSettings;
    std::vector<std::string> m_RandomNames;     /* full names of the people matching the filter, loaded with the first mail */
    BOOL         m_bRandomLoaded;
    MailClient  m_Mail;
    std::string m_TestId;                   /* the worker, for subjects and attachment names */
    std::string m_ShortName;

    DWORD       m_dwDbOpens;
    DWORD       m_dwAgentRuns;
    DWORD       m_dwMailsSent;
};

#endif /* DOMLEM_DOMINOLOAD_H */
