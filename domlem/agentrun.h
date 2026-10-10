/* agentrun.h - run an agent of a Domino database (implemented in agentrun.cpp)
 *
 * AgentRunner opens the database and the agent, runs the agent (a run context per run) as the current identity, and
 * Close() releases the agent first and then the database (CloseDb). domlem opens, runs and closes it once per
 * operation, so nothing stays open between two runs. Settings (time limit, ...) are set before Open().
 *
 *   AgentRunner Runner;
 *   std::string Err;
 *
 *   Runner.SetTimeLimit (30);
 *
 *   if (NOERROR == Runner.Open ("server!!db.nsf", "TestAgent", Err))
 *       Runner.Run (Err);
 *
 *   Runner.Close();                                         // also done by the destructor
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

    /* Opens the database (a full path, server!!path) and the agent. On an error Err has the failing call with its error
     * text and nothing stays open. */
    STATUS Open (const char *pszDbPath, const char *pszAgentName, std::string &Err);

    /* Runs the agent once, with a new run context (and the time limit) every time. Err has the error text when it fails. */
    STATUS Run (std::string &Err);

    /* Releases the agent and then the database. Safe to call more than once. */
    void Close();

    BOOL IsOpen() const { return NULL != m_hAgent; }

private:

    AgentRunner (const AgentRunner &);              /* not copyable: owns Notes handles */
    AgentRunner &operator= (const AgentRunner &);

    STATUS OpenAgent (const char *pszAgentName, std::string &Err);   /* agent of the open database */

    std::string m_AgentName;
    DWORD       m_dwTimeLimit;
    DBHANDLE    m_hDb;
    HAGENT      m_hAgent;
};

#endif /* DOMLEM_AGENTRUN_H */
