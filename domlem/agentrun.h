/* agentrun.h - run an agent of a Domino database (implemented in agentrun.cpp)
 *
 * AgentRunner opens the database and the agent once and can then run the agent as often as needed (a run context per run), as the current
 * identity. Settings (time limit, ...) are set before Open(). Everything is released by Close() or the destructor.
 *
 *   AgentRunner Runner;
 *   std::string Err;
 *
 *   Runner.SetTimeLimit (30);
 *
 *   if (Runner.Open ("server!!db.nsf", "TestAgent", Err))   // NOERROR when ok
 *       ...
 *
 *   Runner.Run (Err);                                       // as often as needed
 */

#ifndef DOMLEM_AGENTRUN_H
#define DOMLEM_AGENTRUN_H

#include <string>

#include <global.h>
#include <nsfdb.h>
#include <agents.h>

class AgentRunner
{
public:

    AgentRunner();
    ~AgentRunner();

    /* Settings: take effect with the next Run() */
    void SetTimeLimit (DWORD dwSeconds);       /* execution limit of one run; 0: no limit (default) */

    /* Opens the database (a full path, server!!path) and the agent, and creates the run context. The runner owns
     * the database handle and closes it. On an error Err has the failing call with its error text and nothing stays open. */
    STATUS Open (const char *pszDbPath, const char *pszAgentName, std::string &Err);

    /* The same with a database that is already open. The handle stays with the caller: it must stay open as long as
     * the runner is open, and Close() does not close it. */
    STATUS Open (DBHANDLE hDb, const char *pszAgentName, std::string &Err);

    /* Runs the agent once, with a new run context (and the time limit) every time. Err has the error text when it fails. */
    STATUS Run (std::string &Err);

    /* Releases everything. Safe to call more than once. */
    void Close();

    BOOL IsOpen() const { return NULL != m_hAgent; }

private:

    AgentRunner (const AgentRunner &);              /* not copyable: owns Notes handles */
    AgentRunner &operator= (const AgentRunner &);

    STATUS OpenAgent (const char *pszAgentName, std::string &Err);   /* agent of the open database */

    std::string m_AgentName;
    DWORD       m_dwTimeLimit;
    BOOL        m_bOwnsDb;               /* Close() closes the database only when it was opened here */
    DBHANDLE    m_hDb;
    HAGENT      m_hAgent;
};

#endif /* DOMLEM_AGENTRUN_H */
