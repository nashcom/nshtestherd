/* dominoload.h - the operations domlem performs against a Domino server (implemented in dominoload.cpp)
 *
 * Every test is a method, Op<Name>(). It does one operation and returns the Notes STATUS; Message has the result for
 * the coordinator (or the error text when it fails). New tests are added one after another as new methods.
 *
 * No database stays open between two operations, as a user closes a database when done with it: every operation opens
 * what it needs, closes its notes (and other objects) before their database, and closes the database (CloseDb: with the
 * session when -closesession is on). What is read from the directory for a job (the recipient list) is read once, with
 * the directory closed again before the job opens the databases it works in. Init() only remembers the settings and
 * builds the paths; it opens nothing.
 *
 * The operations run with the identity the process has: with an identity switch (-switch), call Init() after the switch.
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

    /* Prepares a job: remembers the server and the database of the job (pszServer may be empty: local; pszDbFile may be
     * empty: no database, like the mail jobs), builds its path, resets the counters. Opens nothing. On an error Err has
     * the failing call with its error text. */
    STATUS Init (const char *pszServer, const char *pszDbFile, std::string &Err);

    /* Forgets the job. No handle is open between operations, so there is nothing to close. Safe to call more than once. */
    void Term();

    BOOL IsInit() const { return m_bInit; }

    /* The Domino logical name of the job's database: opens it, takes the file part of its canonical path (NSFDbPathGet,
     * split by OSPathNetParse), relative to the data directory ("names.nsf", "mtdata/mtstore.nsf"), and closes it again.
     * Empty when there is no database or it cannot be opened or named; Err then has the reason. */
    std::string GetDbLogicalPath (std::string &Err) const;

    /* ---- the tests: each one opens what it needs and closes it again ---- */

    /* Opens the database as a new connection, reads the access level and closes it again (the test of a connect) */
    STATUS OpDbOpen (std::string &Message);

    /* Runs the agent once: opens the database and the agent, runs it, closes the agent and then the database */
    STATUS OpRunAgent (std::string &Message);

    /* Crafts one message: a sent copy into the sender's mail file when the settings ask for it, then the message into the
     * mail.box of the server. Each database is opened for its note and closed again. */
    STATUS OpSendMail (std::string &Message);

private:

    STATUS LoadRandomRecipients (std::string &Err);
    STATUS FindSentCopyPath (std::string &Err);
    STATUS PrepareMail (std::string &Err);

    DominoLoad (const DominoLoad &);                /* not copyable */
    DominoLoad &operator= (const DominoLoad &);

    BOOL        m_bInit;                    /* Init() succeeded */
    char        m_szFullPath[MAXPATH+1];    /* server!!path of the job's database, empty: none */
    std::string m_DbFile;                   /* the database name as given, for messages */

    std::string m_AgentName;
    DWORD       m_dwAgentTimeLimit;

    std::string m_Server;                   /* server of the job's database and of the mail.box */
    std::string m_UserName;                 /* the identity of the process, the sender of the mail */
    std::string m_MailTo;
    MailSettings m_MailSettings;
    std::vector<std::string> m_RandomNames;     /* full names of the people matching the filter, read with the first mail */
    BOOL         m_bRandomLoaded;
    BOOL         m_bMailPrepared;           /* the mail client has the settings, the paths and the sent copy decision of this job */
    MailClient  m_Mail;
    std::string m_TestId;                   /* the worker, for subjects and attachment names */
    std::string m_ShortName;

    DWORD       m_dwDbOpens;
    DWORD       m_dwAgentRuns;
    DWORD       m_dwMailsSent;
};

#endif /* DOMLEM_DOMINOLOAD_H */
