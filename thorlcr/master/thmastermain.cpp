/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems®.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

// Entrypoint for ThorMaster.EXE

#include "platform.h"

#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h> 
#include <stdlib.h> 

#ifdef _WIN32
#include <direct.h> 
#endif

#include "jlib.hpp"
#include "jcontainerized.hpp"
#include "jdebug.hpp"
#include "jfile.hpp"
#include "jmisc.hpp"
#include "jmutex.hpp"
#include "jprop.hpp"
#include "jset.hpp"
#include "jsocket.hpp"
#include "jthread.hpp"
#include "jexcept.hpp"

#include "mpbase.hpp"
#include "mplog.hpp"

#include "daaudit.hpp"
#include "daclient.hpp"
#include "dadfs.hpp"
#include "dalienv.hpp"
#include "daqueue.hpp"
#include "dasds.hpp"
#include "dllserver.hpp"
#include "workunit.hpp"
#include "rmtfile.hpp"

#include "portlist.h"

#include "thor.hpp"
#include "thorport.hpp"
#include "thormisc.hpp"

#include "thgraph.hpp"
#include "thgraphmaster.hpp"
#include "thgraphmanager.hpp"
#include "thmastermain.hpp"
#include "mawatchdog.hpp"
#include "thexception.hpp"
#include "thmem.hpp"

#ifndef _CONTAINERIED
#define DEFAULT_QUERY_SO_DIR "sodir"
#endif
#define MAX_SLAVEREG_DELAY 60*1000*15 // 15 mins
#define SLAVEREG_VERIFY_DELAY 5*1000
#define SHUTDOWN_IN_PARALLEL 20



class CThorEndHandler : implements IThreaded
{
    CThreaded threaded;
    unsigned timeout = 30000;
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    Semaphore sem;
public:
    CThorEndHandler() : threaded("CThorEndHandler")
    {
        threaded.init(this, false); // starts thread
    }
    ~CThorEndHandler()
    {
        stop();
        threaded.join(timeout);
    }
    void start(unsigned timeoutSecs)
    {
        bool expected = false;
        if (started.compare_exchange_strong(expected, true))
        {
            timeout = timeoutSecs * 1000; // sem_post and sem_wait are mem_barriers
            sem.signal();
        }
    }
    void stop()
    {
        bool expected = false;
        if (stopped.compare_exchange_strong(expected, true))
            sem.signal();
    }
    virtual void threadmain() override
    {
        // wait to be signalled to start timer
        sem.wait();
        if (stopped)
            return;
        if (!sem.wait(timeout))
        {
            // if it wasn't set by now then it's -1 and Thor restarts ...
            int eCode = queryExitCode();
            _exit(eCode);
        }
    }
};

static CThorEndHandler *thorEndHandler = nullptr;
static StringBuffer cloudJobName;

MODULE_INIT(INIT_PRIORITY_STANDARD)
{
    /* NB: CThorEndHandler starts the thread now, although strictly it is not needed until later.
     * This is to avoid requiring the thread to be started in a unsafe context, e.g. a signal handler
     */
    thorEndHandler = new CThorEndHandler();
    return true;
}
MODULE_EXIT()
{
    if (thorEndHandler)
        delete thorEndHandler;
}


class CRegistryServer : public CSimpleInterface
{
    unsigned msgDelay, slavesRegistered;
    CriticalSection crit;
    bool stopped = false;
    static CriticalSection regCrit;
    static CRegistryServer *registryServer;

    class CDeregistrationWatch : implements IThreaded
    {
        CThreaded threaded;
        CRegistryServer &registry;
        std::atomic<bool> running;
    public:
        CDeregistrationWatch(CRegistryServer &_registry) : threaded("CDeregistrationWatch"), registry(_registry), running(false) { }
        ~CDeregistrationWatch()
        {
            stop();
        }
        void start() { threaded.init(this, false); }
        void stop()
        {
            if (running)
            {
                running = false;
                queryWorldCommunicator().cancel(NULL, MPTAG_THORREGISTRATION);
                threaded.join();
            }
        }
        virtual void threadmain() override
        {
            running = true;
            for (;;)
            {
                INode *senderNode;
                CMessageBuffer msg;
                if (!queryWorldCommunicator().recv(msg, NULL, MPTAG_THORREGISTRATION, &senderNode))
                    return;
                rank_t sender = queryNodeGroup().rank(senderNode);
                SocketEndpoint ep = senderNode->endpoint();
                StringBuffer url;
                ep.getEndpointHostText(url);
                if (RANK_NULL == sender)
                {
                    PROGLOG("Node %s trying to deregister is not part of this cluster", url.str());
                    continue;
                }
                RegistryCode code;
                readUnderlyingType<RegistryCode>(msg, code);
                if (rc_deregister != code)
                    throwUnexpected();
                Owned<IException> e = deserializeException(msg);
                if (e.get())
                    EXCLOG(e, "Slave unregistered with exception");
                registry.deregisterNode(sender-1);
            }
            running = false;
        }
    } deregistrationWatch;
public:
    Linked<CMasterWatchdog> watchdog;
    IBitSet *status;

    CRegistryServer() : deregistrationWatch(*this)
    {
        status = createThreadSafeBitSet();
        msgDelay = SLAVEREG_VERIFY_DELAY;
        slavesRegistered = 0;
        if (globals->getPropBool("@watchdogEnabled"))
            watchdog.setown(createMasterWatchdog());
        else
            globals->setPropBool("@watchdogProgressEnabled", false);
        CriticalBlock b(regCrit);
        registryServer = this;
    }
    ~CRegistryServer()
    {
        CriticalBlock b(regCrit);
        registryServer = NULL;
        stop();
        if (watchdog)
            watchdog->stop();
        if (clusterInitialized())
            shutdown();
        status->Release();
    }
    static CRegistryServer *getRegistryServer()
    {
        CriticalBlock b(regCrit);
        return LINK(registryServer);
    }
    void deregisterNode(unsigned slave)
    {
        const SocketEndpoint &ep = queryNodeGroup().queryNode(slave+1).endpoint();
        StringBuffer url;
        ep.getEndpointHostText(url);
        if (!status->test(slave))
        {
            PROGLOG("Slave %d (%s) trying to unregister, but not currently registered", slave+1, url.str());
            return;
        }
        PROGLOG("Slave %d (%s) unregistered", slave+1, url.str());
        status->set(slave, false);
        --slavesRegistered;
        if (watchdog)
            watchdog->removeSlave(ep);
        abortThor(MakeThorOperatorException(TE_AbortException, "The machine %s and/or the slave was shutdown. Aborting Thor", url.str()), TEC_SlaveInit);
    }
    void registerNode(unsigned slave)
    {
        SocketEndpoint ep = queryNodeGroup().queryNode(slave+1).endpoint();
        StringBuffer url;
        ep.getEndpointHostText(url);
        if (status->test(slave))
        {
            PROGLOG("Slave %d (%s) already registered, rejecting", slave+1, url.str());
            return;
        }
        PROGLOG("Slave %d (%s) registered", slave+1, url.str());
        status->set(slave);
        if (watchdog)
            watchdog->addSlave(ep);
        ++slavesRegistered;
    }
    void connect(unsigned slaves)
    {
        IPointerArrayOf<INode> connectedSlaves;
        connectedSlaves.ensureCapacity(slaves);
        unsigned remaining = slaves;
        INode *_sender = nullptr;
        CMessageBuffer msg;

        // Will wait for all workers to register within timelimit (default = 15 mins bare-metal, 60 mins containerized)
        constexpr unsigned defaultMaxRegistrationMins = isContainerized() ? 60 : 15;
        unsigned maxRegistrationMins = (unsigned)getExpertOptInt64("maxWorkerRegistrationMins", defaultMaxRegistrationMins);
        constexpr unsigned oneMinMs = 60000;

        PROGLOG("Waiting for %u workers to register - max registration time = %u minutes", slaves, maxRegistrationMins);
        CTimeMon registerTM(maxRegistrationMins * oneMinMs);
        while (remaining)
        {
            // on timeout, check for any failed k8s worker job
            if (!queryWorldCommunicator().recv(msg, nullptr, MPTAG_THORREGISTRATION, &_sender, oneMinMs))
            {
                ::Release(_sender);
                if (registerTM.timedout())
                    throw makeStringExceptionV(TE_AbortException, "Timeout waiting for all workers to register within timeout period (%u mins)", maxRegistrationMins);

                if (isContainerized())
                {
                    // NB: this is checking for error only, will throw an exception if any found.
                    k8s::waitJob("thorworker", "job", cloudJobName.str(), 0, 0, k8s::KeepJobs::all);
                }

                // NB: will not reach here if waitJob fails.
                PROGLOG("Waiting for %u remaining workers to register", remaining);
            }
            else
            {
                Owned<INode> sender = _sender;
                if (NotFound != connectedSlaves.find(sender))
                {
                    StringBuffer epStr;
                    throw makeStringExceptionV(TE_AbortException, "Same slave registered twice!! : %s", sender->endpoint().getEndpointHostText(epStr).str());
                }

                /* NB: in base metal setup, the slaves know which slave number they are in advance, and send their slavenum at registration.
                * In non attached storage setup, they do not send a slave by default and instead are given a # once all are registered
                */
                unsigned slaveNum;
                msg.read(slaveNum);
                StringBuffer workerPodName, workerContainerName;
                if (NotFound == slaveNum)
                {
                    connectedSlaves.append(sender.getLink());
                    slaveNum = connectedSlaves.ordinality();
                    if (isContainerized())
                    {
                        msg.read(workerPodName);
                        msg.read(workerContainerName);
                        addConnectedWorkerPod(workerPodName, workerContainerName); // NB: these are added in worker # order
                    }
                }
                else
                {
                    unsigned pos = slaveNum - 1; // NB: slaveNum is 1 based
                    while (connectedSlaves.ordinality() < pos)
                        connectedSlaves.append(nullptr);
                    if (connectedSlaves.ordinality() == pos)
                        connectedSlaves.append(sender.getLink());
                    else
                        connectedSlaves.replace(sender.getLink(), pos);
                }
                StringBuffer epStr;
                PROGLOG("Slave %u connected from %s", slaveNum, sender->endpoint().getEndpointHostText(epStr).str());
                --remaining;
            }
        }
        assertex(slaves == connectedSlaves.ordinality());

        if (isContainerized())
        {
            unsigned wfid = globals->getPropInt("@wfid");
            const char *wuid = globals->queryProp("@workunit");
            const char *graphName = globals->queryProp("@graphName");
            Owned<IWorkUnitFactory> factory = getWorkUnitFactory();
            Owned<IWorkUnit> workunit = factory->updateWorkUnit(wuid);
            addTimeStamp(workunit, wfid, graphName, StWhenK8sReady);
            publishPodNames(workunit, graphName);
        }

        unsigned localThorPortInc = globals->getPropInt("@localThorPortInc", DEFAULT_SLAVEPORTINC);
        unsigned slaveBasePort = globals->getPropInt("@slaveport", DEFAULT_THORSLAVEPORT);
        unsigned channelsPerWorker = globals->getPropInt("@channelsPerWorker", 1);

        Owned<IGroup> processGroup;

        // NB: in bare metal Thor is bound to a group and cluster/communicator have alreday been setup (see earlier setClusterGroup call)
        if (clusterInitialized())
            processGroup.set(&queryProcessGroup());
        else
        {
            /* sort by {port, ip}
             * So that workers are not bunched on same node, but striped across the pod ips
             */
            auto compareINodeOrder = [](IInterface * const *ll, IInterface * const *rr)
            {
                INode *l = (INode *) *ll;
                INode *r = (INode *) *rr;
                const SocketEndpoint &lep = l->endpoint();
                const SocketEndpoint &rep = r->endpoint();
                if (lep.port < rep.port)
                    return -1;
                else if (lep.port > rep.port)
                    return 1;
                return lep.ipcompare(rep);
            };
            connectedSlaves.sort(compareINodeOrder);
            processGroup.setown(createIGroup(connectedSlaves.ordinality(), connectedSlaves.getArray()));
            setupCluster(queryMyNode(), processGroup, channelsPerWorker, slaveBasePort, localThorPortInc);
        }

        PROGLOG("Slaves connected, initializing..");
        msg.clear();
        msg.append(THOR_VERSION_MAJOR).append(THOR_VERSION_MINOR);
        processGroup->serialize(msg);
        globals->serialize(msg);
        getGlobalConfigSP()->serialize(msg);
        msg.append(masterSlaveMpTag);
        msg.append(kjServiceMpTag);
        if (!queryNodeComm().send(msg, RANK_ALL_OTHER, MPTAG_THORREGISTRATION, MP_ASYNC_SEND))
            throw makeStringException(TE_AbortException, "Failed to initialize slaves");

        // Wait for confirmation from slaves
        PROGLOG("Initialization sent to slave group");
        Owned<IException> exception;
        try
        {
            while (slavesRegistered < slaves)
            {
                rank_t sender;
                CMessageBuffer msg;
                if (!queryNodeComm().recv(msg, RANK_ALL, MPTAG_THORREGISTRATION, &sender, MAX_SLAVEREG_DELAY))
                {
                    PROGLOG("Slaves not responding to cluster initialization: ");
                    unsigned s=0;
                    for (;;)
                    {
                        unsigned ns = status->scan(s, false);
                        if (ns<s || ns >= slaves)
                            break;
                        s = ns+1;
                        StringBuffer str;
                        PROGLOG("Slave %d (%s)", s, queryNodeGroup().queryNode(s).endpoint().getEndpointHostText(str.clear()).str());
                    }
                    throw MakeThorException(TE_AbortException, "Slaves failed to respond to cluster initialization");
                }
                StringBuffer str;
                PROGLOG("Registration confirmation from %s", queryNodeGroup().queryNode(sender).endpoint().getEndpointHostText(str).str());
                if (msg.length())
                {
                    Owned<IException> e = deserializeException(msg);
                    EXCLOG(e, "Registration error");
                    throw e.getClear();
                }
                registerNode(sender-1);
            }

            // this is like a barrier, let slaves know all slaves are now connected
            PROGLOG("Slaves initialized");
            unsigned s=0;
            for (; s<slaves; s++)
            {
                CMessageBuffer msg;
                if (!queryNodeComm().send(msg, s+1, MPTAG_THORREGISTRATION))
                    throw makeStringExceptionV(TE_AbortException, "Failed to acknowledge slave %d registration", s+1);
            }
            if (watchdog)
                watchdog->start();
            deregistrationWatch.start();
            return;
        }
        catch (IException *e)
        {
            EXCLOG(e, "Slave registration exception");
            exception.setown(e);
        }
        shutdown();
        if (exception)
            throw exception.getClear();
    }
    void stop()
    {
        if (stopped)
            return;
        stopped = true;
        deregistrationWatch.stop();
        queryWorldCommunicator().cancel(NULL, MPTAG_THORREGISTRATION);
    }
    void shutdown()
    {
        CriticalBlock block(crit);
        unsigned i=0;
        mptag_t shutdownTag = createReplyTag();
        for (; i<queryNodeClusterWidth(); i++)
        {
            if (status->test(i))
            {
                SocketEndpoint ep = queryNodeGroup().queryNode(i+1).endpoint();
                CMessageBuffer msg;
                msg.append((unsigned)Shutdown);
                serializeMPtag(msg, shutdownTag);
                try
                {
                    queryNodeComm().send(msg, i+1, masterSlaveMpTag, MP_ASYNC_SEND);
                }
                catch (IMP_Exception *e) { e->Release(); }
                catch (IException *e)
                {
                    EXCLOG(e, "Shutting down slave");
                    e->Release();
                }
                if (watchdog)
                    watchdog->removeSlave(ep);
            }
        }

        CTimeMon tm(20000);
        unsigned numReplied = 0;
        while (numReplied < slavesRegistered)
        {
            unsigned remaining;
            if (tm.timedout(&remaining))
            {
                PROGLOG("Timeout waiting for Shutdown reply from slave(s) (%u replied out of %u total)", numReplied, slavesRegistered);
                StringBuffer slaveList;
                for (i=0;i<slavesRegistered;i++)
                {
                    if (status->test(i))
                    {
                        if (slaveList.length())
                            slaveList.append(",");
                        slaveList.append(i+1);
                    }
                }
                if (slaveList.length())
                    PROGLOG("Slaves that have not replied: %s", slaveList.str());
                break;
            }
            try
            {
                rank_t sender;
                CMessageBuffer msg;
                if (queryNodeComm().recv(msg, RANK_ALL, shutdownTag, &sender, remaining))
                {
                    if (sender) // paranoid, sender should always be > 0
                        status->set(sender-1, false);
                    numReplied++;
                }
            }
            catch (IException *e)
            {
                // do not log MP link closed exceptions from ending slaves
                e->Release();
            }
        }
    }
};

CriticalSection CRegistryServer::regCrit;
CRegistryServer *CRegistryServer::registryServer = NULL;


//
//////////////////

bool checkClusterRelicateDAFS(IGroup &grp)
{
    // check the dafilesrv is running (and right version) 
    unsigned start = msTick();
    PROGLOG("Checking cluster replicate nodes");
    SocketEndpointArray epa;
    grp.getSocketEndpoints(epa);
    ForEachItemIn(i1,epa) {
        epa.element(i1).port = getDaliServixPort();
    }
    SocketEndpointArray failures;
    UnsignedArray failedcodes;
    StringArray failedmessages;
    validateNodes(epa,NULL,NULL,true,failures,failedcodes,failedmessages);
    ForEachItemIn(i,failures) {
        SocketEndpoint ep(failures.item(i));
        ep.port = 0;
        StringBuffer ips;
        ep.getHostText(ips);
        FLLOG(MCoperatorError, "VALIDATE FAILED(%d) %s : %s",failedcodes.item(i),ips.str(),failedmessages.item(i));
    }
    PROGLOG("Cluster replicate nodes check completed in %dms",msTick()-start);
    return (failures.ordinality()==0);
}



static bool auditStartLogged = false;

static bool firstCtrlC = true;
bool ControlHandler(ahType type)
{
    // MCK - NOTE: this routine may make calls to non-async-signal safe functions
    //             (such as malloc) that really should not be made if we are called
    //             from a signal handler - start end handler timer to always end
    if (thorEndHandler)
        thorEndHandler->start(120);

    if (ahInterrupt == type)
    {
        if (firstCtrlC)
        {
            LOG(MCdebugProgress, "CTRL-C detected");
            firstCtrlC = false;
            {
                Owned<CRegistryServer> registry = CRegistryServer::getRegistryServer();
                if (registry)
                    registry->stop();
            }
            abortThor(NULL, TEC_CtrlC);
        }
        else
        {
            LOG(MCdebugProgress, "2nd CTRL-C detected - terminating process");

            if (auditStartLogged)
            {
                auditStartLogged = false;
                auditThorSystemEvent("Terminate", {"ctrlc"});
            }
            queryLogMsgManager()->flushQueue(10*1000);
            _exit(TEC_CtrlC);
        }
    }
    // ahTerminate
    else
    {
        LOG(MCdebugProgress, "SIGTERM detected, shutting down");
        Owned<CRegistryServer> registry = CRegistryServer::getRegistryServer();
        if (registry)
            registry->stop();
        abortThor(NULL, TEC_Clean);
    }
    return false;
}


#include "thactivitymaster.hpp"
int main( int argc, const char *argv[]  )
{
    if (!checkCreateDaemon(argc, argv))
        return EXIT_FAILURE;

#if defined(WIN32) && defined(_DEBUG)
    int tmpFlag = _CrtSetDbgFlag( _CRTDBG_REPORT_FLAG );
    tmpFlag |= _CRTDBG_LEAK_CHECK_DF;
    _CrtSetDbgFlag( tmpFlag );
#endif

    loadMasters(); // actually just a dummy call to ensure dll linked
    InitModuleObjects();
    NoQuickEditSection xxx;
    {
        globals.setown(loadConfiguration(thorDefaultConfigYaml, argv, "thor", "THOR", "thor.xml", nullptr, nullptr, false));
    }
#ifdef _DEBUG
    unsigned holdSlave = globals->getPropInt("@holdSlave", NotFound);
    if (0 == holdSlave) // master
    {
        DBGLOG("Thor master paused for debugging purposes, attach and set held=false to release");
        bool held = true;
        while (held)
            Sleep(5);
    }
#endif
    setStatisticsComponentName(SCTthor, globals->queryProp("@name"), true);

    globals->setProp("@masterBuildTag", hpccBuildInfo.buildTag);

    setIORetryCount((unsigned)getExpertOptInt64("ioRetries")); // default == 0 == off
    StringBuffer daliServer;
    if (!globals->getProp("@daliServers", daliServer)) 
    {
        LOG(MCerror, "No Dali server list specified in THOR.XML (daliServers=iport,iport...)\n");
        return 0; // no recycle
    }

    SocketEndpoint thorEp;
    const char *master = globals->queryProp("@master");
    if (master)
    {
        thorEp.set(master);
        thorEp.setLocalHost(thorEp.port);
    }
    else
        thorEp.setLocalHost(0);

    if (0 == thorEp.port)
        thorEp.port = globals->getPropInt("@masterport", THOR_BASE_PORT);

    // Remove sentinel asap
    Owned<IFile> sentinelFile = createSentinelTarget();
    removeSentinelFile(sentinelFile);

    EnableSEHtoExceptionMapping(); 
#ifndef __64BIT__
    // Restrict stack sizes on 32-bit systems
    Thread::setDefaultStackSize(0x10000);   // NB under windows requires linker setting (/stack:)
#endif
    const char *thorname = NULL;
    StringBuffer nodeGroup, logUrl;
    unsigned channelsPerWorker;
    if (globals->hasProp("@channelsPerWorker"))
        channelsPerWorker = globals->getPropInt("@channelsPerWorker", 1);
    else
    {   // for backward compatiblity only
        channelsPerWorker = globals->getPropInt("@channelsPerSlave", 1);
        globals->setPropInt("@channelsPerWorker", channelsPerWorker);
    }

    installDefaultFileHooks(globals);
    ILogMsgHandler *logHandler;
    unsigned wfid = 0;
    const char *workunit = nullptr;
    const char *graphName = nullptr;
    IPropertyTree *managerMemory = ensurePTree(globals, "managerMemory");
    IPropertyTree *workerMemory = ensurePTree(globals, "workerMemory");

    try
    {
#ifndef _CONTAINERIZED
        {
            Owned<IComponentLogFileCreator> lf = createComponentLogFileCreator(globals, "thor");
            lf->setName("thormaster");//override default filename
            lf->setCreateAliasFile(false);
            logHandler = lf->beginLogging();
            createUNCFilename(lf->queryLogFileSpec(), logUrl, false);
#ifndef _DEBUG
            // keep duplicate logging output to stderr to aide debugging
            queryLogMsgManager()->removeMonitor(queryStderrLogMsgHandler());
#endif

            LOG(MCdebugProgress, "Opened log file %s", logUrl.str());
        }
#else
        setupContainerizedLogMsgHandler();
        logHandler = queryStderrLogMsgHandler();
        logUrl.set("stderr");
#endif
        LOG(MCdebugProgress, "Build %s", hpccBuildInfo.buildTag);

        Owned<IGroup> serverGroup = createIGroupRetry(daliServer.str(), DALI_SERVER_PORT);

        unsigned retry = 0;
        for (;;)
        {
            try
            {
                LOG(MCdebugProgress, "calling initClientProcess %d", thorEp.port);
                initClientProcess(serverGroup, DCR_ThorMaster, thorEp.port, nullptr, nullptr, MP_WAIT_FOREVER, true);
                if (0 == thorEp.port)
                    thorEp.port = queryMyNode()->endpoint().port;
                // both same
                setMasterPortBase(thorEp.port);
                setMachinePortBase(thorEp.port);

                break;
            }
            catch (IJSOCK_Exception *e)
            { 
                if ((e->errorCode()!=JSOCKERR_port_in_use))
                    throw;
                FLLOG(MCexception(e), e,"InitClientProcess");
                if (retry++>10) 
                    throw;
                e->Release();
                LOG(MCdebugProgress, "Retrying");
                Sleep(retry*2000);  
            }
        }

        initializeStorageGroups(true);

        if (globals->getPropBool("@MPChannelReconnect"))
            getMPServer()->setOpt(mpsopt_channelreopen, "true");

        if (globals->getPropBool("@enableSysLog",true))
            UseSysLogForOperatorMessages();

        thorname = globals->queryProp("@name");
        if (!thorname)
        {
            PROGLOG("No 'name' setting, defaulting to \"local\"");
            thorname = "local";
            globals->setProp("@name", thorname);
        }

        if (!globals->getProp("@nodeGroup", nodeGroup))
        {
            nodeGroup.append(thorname);
            globals->setProp("@nodeGroup", thorname);
        }

#ifndef _CONTAINERIZED
        if (globals->getPropBool("@useNASTranslation", true))
        {
            Owned<IPropertyTree> nasConfig = envGetNASConfiguration();
            if (nasConfig)
                globals->setPropTree("NAS", nasConfig.getLink()); // for use by slaves
            Owned<IPropertyTree> masterNasFilters = envGetInstallNASHooks(nasConfig, &thorEp);
        }
#endif

        HardwareInfo hdwInfo;
        getHardwareInfo(hdwInfo);
        globals->setPropInt("@masterTotalMem", hdwInfo.totalMemory);
        unsigned mmemSize = globals->getPropInt("@masterMemorySize"); // in MB
        unsigned gmemSize = globals->getPropInt("@globalMemorySize"); // in MB
        if (0 == gmemSize)
        {
            // NB: This could be in a isContainerized(), but the 'workerResources' section only applies to containerized setups
            const char *workerResourcedMemory = globals->queryProp("workerResources/@memory");
            if (!isEmptyString(workerResourcedMemory))
            {
                offset_t sizeBytes = friendlyStringToSize(workerResourcedMemory);
                gmemSize = (unsigned)(sizeBytes / 0x100000);
            }
            else
            {
                gmemSize = hdwInfo.totalMemory;
#ifdef _WIN32
                if (gmemSize > 2048)
                    gmemSize = 2048;
#else
#ifndef __64BIT__
                if (gmemSize > 2048)
                {
                    // 32 bit OS doesn't handle whole physically installed RAM
                    gmemSize = 2048;
                }
#ifdef __ARM_ARCH_7A__
                // For ChromeBook with 2GB RAM
                if (gmemSize <= 2048)
                {
                    // Decrease max memory to 2/3 
                    gmemSize = gmemSize * 2 / 3; 
                }
#endif            
#endif
#endif
            }

            // if worker and/or manager memory is unspecified, set default percentages
            // that will be used in conjunction with discovered memory.

            // @localThor mode - 25% is used for manager and 50% is used for workers
            bool localThor = !isContainerized() && globals->getPropBool("@localThor");
            if (!workerMemory->hasProp("@maxMemPercentage"))
                workerMemory->setPropReal("@maxMemPercentage", localThor ? 50.0 : defaultPctSysMemForRoxie);
            if (0 == mmemSize)
            {
                if (!managerMemory->hasProp("@maxMemPercentage"))
                    managerMemory->setPropReal("@maxMemPercentage", localThor ? 25.0 : defaultPctSysMemForRoxie);
            }
        }
        workerMemory->setPropInt("@total", gmemSize);

        if (mmemSize)
        {
            if (mmemSize > hdwInfo.totalMemory)
                OWARNLOG("Configured manager memory size (%u MB) is greater than total hardware memory (%u MB)", mmemSize, hdwInfo.totalMemory);
        }
        else
        {
            // NB: This could be in a isContainerized(), but the 'managerResources' section only applies to containerized setups
            const char *managerResourcedMemory = globals->queryProp("managerResources/@memory");
            if (!isEmptyString(managerResourcedMemory))
            {
                offset_t sizeBytes = friendlyStringToSize(managerResourcedMemory);
                mmemSize = (unsigned)(sizeBytes / 0x100000);
            }
            else
                mmemSize = gmemSize; // default to same as slaves
        }
        managerMemory->setPropInt("@total", mmemSize);

        applyResourcedCPUAffinity(globals->queryPropTree("managerResources"));

        char thorPath[1024];
        if (!GetCurrentDirectory(1024, thorPath))
        {
            OERRLOG("ThorMaster::main: Current directory path too big, setting it to null");
            thorPath[0] = 0;
        }
        unsigned l = strlen(thorPath);
        if (l) { thorPath[l] = PATHSEPCHAR; thorPath[l+1] = '\0'; }
        globals->setProp("@thorPath", thorPath);

        if (isContainerized())
        {
            wfid = globals->getPropInt("@wfid");
            workunit = globals->queryProp("@workunit");
            graphName = globals->queryProp("@graphName");
            if (isEmptyString(workunit))
                throw makeStringException(0, "missing --workunit");
            if (isEmptyString(graphName))
                throw makeStringException(0, "missing --graphName");
        }
        else
        {
            const char * overrideBaseDirectory = globals->queryProp("@thorDataDirectory");
            const char * overrideReplicateDirectory = globals->queryProp("@thorReplicateDirectory");
            StringBuffer datadir;
            StringBuffer repdir;
            if (getConfigurationDirectory(globals->queryPropTree("Directories"),"data","thor",globals->queryProp("@name"),datadir))
                overrideBaseDirectory = datadir.str();
            if (getConfigurationDirectory(globals->queryPropTree("Directories"),"mirror","thor",globals->queryProp("@name"),repdir))
                overrideReplicateDirectory = repdir.str();
            if (overrideBaseDirectory&&*overrideBaseDirectory)
                setBaseDirectory(overrideBaseDirectory, false);
            if (overrideReplicateDirectory&&*overrideBaseDirectory)
                setBaseDirectory(overrideReplicateDirectory, true);
        }
        bool saveQueryDlls = true;
        if (hasExpertOpt("saveQueryDlls"))
            saveQueryDlls = getExpertOptBool("saveQueryDlls");
        else
        {
            // propagate default setting (so seen by workers)
            setExpertOpt("saveQueryDlls", boolToStr(saveQueryDlls));
        }
        if (saveQueryDlls)
        {
            StringBuffer soDir, soPath;
            if (!isContainerized() && getConfigurationDirectory(globals->queryPropTree("Directories"),"query","thor",globals->queryProp("@name"),soDir))
                globals->setProp("@query_so_dir", soDir.str());
            else if (!globals->getProp("@query_so_dir", soDir)) {
                globals->setProp("@query_so_dir", DEFAULT_QUERY_SO_DIR); 
                soDir.append(DEFAULT_QUERY_SO_DIR);
            }
            if (isAbsolutePath(soDir.str()))
                soPath.append(soDir);
            else
            {
                soPath.append(thorPath);
                addPathSepChar(soPath);
                soPath.append(soDir);
            }
            addPathSepChar(soPath);
            globals->setProp("@query_so_dir", soPath.str());
            recursiveCreateDirectory(soPath.str());
        }
        else
        {
            // meaningless if not saving dlls
            globals->setPropBool("@dllsToSlaves", false);
        }
        StringBuffer tempDirStr;
        if (!getConfigurationDirectory(globals->queryPropTree("Directories"),"spill","thor",globals->queryProp("@name"), tempDirStr))
        {
            tempDirStr.append(globals->queryProp("@thorTempDirectory"));
            if (0 == tempDirStr.length())
            {
                appendCurrentDirectory(tempDirStr, true);
                if (tempDirStr.length())
                    addPathSepChar(tempDirStr);
                tempDirStr.append("temp");
            }
        }

        // NB: set into globals, serialized and used by worker processes.
        globals->setProp("@thorTempDirectory", tempDirStr);

        startLogMsgParentReceiver();    
        connectLogMsgManagerToDali();
        if (globals->getPropBool("@cache_dafilesrv_master",false))
            setDaliServixSocketCaching(true); // speeds up deletes under linux
    }
    catch (IException *e)
    {
        FLLOG(MCexception(e), e,"ThorMaster");
        e->Release();
        return -1;
    }

    StringBuffer queueName;

    // only for K8s
    bool workerNSInstalled = false;
    bool workerJobInstalled = false;

    const char *thorName = globals->queryProp("@name");
#ifdef _CONTAINERIZED
    StringBuffer queueNames;
    getClusterThorQueueName(queueNames, thorName);
#else
    if (!thorName)
    {
        thorName = "thor";
        globals->setProp("@name", thorName);
    }
    SCMStringBuffer queueNames;
    getThorQueueNames(queueNames, thorName);
#endif
    queueName.set(queueNames.str());

    Owned<IException> exception;
    try
    {
        CSDSServerStatus &serverStatus = openThorServerStatus();

        Owned<CRegistryServer> registry = new CRegistryServer();

        serverStatus.queryProperties()->setProp("@thorname", thorname);
        serverStatus.queryProperties()->setProp("@cluster", nodeGroup.str()); // JCSMORE rename
        serverStatus.queryProperties()->setProp("LogFile", logUrl.str()); // LogFile read by eclwatch (possibly)
        serverStatus.queryProperties()->setProp("@nodeGroup", nodeGroup.str());
        serverStatus.queryProperties()->setProp("@queue", queueName.str());
        serverStatus.commitProperties();

        addAbortHandler(ControlHandler);
        masterSlaveMpTag = allocateClusterMPTag();
        kjServiceMpTag = allocateClusterMPTag();

        auditThorSystemEvent("Initializing");
        unsigned numWorkers = 0;
        if (isContainerized())
        {
            saveWuidToFile(workunit);
            JobNameScope activeJobName(workunit);

            StringBuffer thorEpStr;
            LOG(MCdebugProgress, "ThorMaster version %d.%d, Started on %s", THOR_VERSION_MAJOR,THOR_VERSION_MINOR,thorEp.getEndpointHostText(thorEpStr).str());

            unsigned numWorkersPerPod = 1;
            if (!globals->hasProp("@numWorkers"))
                throw makeStringException(0, "Default number of workers not defined (numWorkers)");
            else
            {
                // check 'numWorkers' workunit option.
                Owned<IWorkUnitFactory> factory = getWorkUnitFactory();
                Owned<IConstWorkUnit> wuRead = factory->openWorkUnit(workunit);
                if (!wuRead)
                    throw makeStringExceptionV(0, "Cannot open workunit: %s", workunit);
                if (wuRead->hasDebugValue("numWorkers"))
                    numWorkers = wuRead->getDebugValueInt("numWorkers", 0);
                else
                    numWorkers = globals->getPropInt("@numWorkers", 0);
                if (0 == numWorkers)
                    throw makeStringException(0, "Number of workers must be > 0 (numWorkers)");
                if (wuRead->hasDebugValue("numWorkersPerPod"))
                    numWorkersPerPod = wuRead->getDebugValueInt("numWorkersPerPod", 1);
                else
                    numWorkersPerPod = globals->getPropInt("@numWorkersPerPod", 1); // default to 1
                if (numWorkersPerPod < 1)
                    throw makeStringException(0, "Number of workers per pod must be > 0 (numWorkersPerPod)");
                if ((numWorkers % numWorkersPerPod) != 0)
                    throw makeStringExceptionV(0, "numWorkersPerPod must be a factor of numWorkers. (numWorkers=%u, numWorkersPerPod=%u)", numWorkers, numWorkersPerPod);

                Owned<IWorkUnit> workunit = &wuRead->lock();
                addTimeStamp(workunit, wfid, graphName, StWhenK8sStarted);
            }

            cloudJobName.appendf("%s-%s", workunit, graphName);

            StringBuffer myEp;
            getRemoteAccessibleHostText(myEp, queryMyNode()->endpoint());

            if (!k8s::applyYaml("thorworker", workunit, cloudJobName, "networkpolicy", { }, false, true))
                throw makeStringException(TE_AbortException, "Failed to apply worker networkpolicy manifest");
            k8s::KeepJobs keepJob = k8s::translateKeepJobs(globals->queryProp("@keepJobs"));
            if (!k8s::applyYaml("thorworker", workunit, cloudJobName, "job", { { "graphName", graphName}, { "master", myEp.str() }, { "_HPCC_NUM_WORKERS_", std::to_string(numWorkers/numWorkersPerPod)} }, false, k8s::KeepJobs::none == keepJob))
                throw makeStringException(TE_AbortException, "Failed to apply worker job manifest");
        }
        else
        {
            StringBuffer thorEpStr;
            LOG(MCdebugProgress, "ThorMaster version %d.%d, Started on %s", THOR_VERSION_MAJOR,THOR_VERSION_MINOR,thorEp.getEndpointHostText(thorEpStr).str());
            LOG(MCdebugProgress, "Thor name = %s, queue = %s, nodeGroup = %s",thorname,queueName.str(),nodeGroup.str());
            unsigned localThorPortInc = globals->getPropInt("@localThorPortInc", DEFAULT_SLAVEPORTINC);
            unsigned slaveBasePort = globals->getPropInt("@slaveport", DEFAULT_THORSLAVEPORT);
            Owned<IGroup> rawGroup = getClusterNodeGroup(thorname, "ThorCluster");
            unsigned numWorkersPerNode = globals->getPropInt("@slavesPerNode", 1);
            setClusterGroup(queryMyNode(), rawGroup, numWorkersPerNode, channelsPerWorker, slaveBasePort, localThorPortInc);
            numWorkers = queryNodeClusterWidth();
            if (numWorkersPerNode > 1)
            {
                // Split memory based on numWorkersPerNode
                // NB: maxMemPercentage only set when memory amounts have not explicily been defined (e.g. globalMemorySize)
                double pct = workerMemory->getPropReal("@maxMemPercentage");
                if (pct)
                    workerMemory->setPropReal("@maxMemPercentage", pct / numWorkersPerNode);
            }
        }

        registry->connect(numWorkers);
        if (!isContainerized())
        {
            // bare-metal - check health of dafilesrv's on the Thor cluster.
            if (globals->getPropBool("@replicateOutputs")&&globals->getPropBool("@validateDAFS",true)&&!checkClusterRelicateDAFS(queryNodeGroup()))
            {
                FLLOG(MCoperatorError, "ERROR: Validate failure(s) detected, exiting Thor");
                return globals->getPropBool("@validateDAFSretCode"); // default is no recycle!
            }
        }

        unsigned totSlaveProcs = queryNodeClusterWidth();
        for (unsigned s=0; s<totSlaveProcs; s++)
        {
            StringBuffer slaveStr;
            for (unsigned c=0; c<channelsPerWorker; c++)
            {
                unsigned o = s + (c * totSlaveProcs);
                if (c)
                    slaveStr.append(",");
                slaveStr.append(o+1);
            }
            StringBuffer virtStr;
            if (channelsPerWorker>1)
                virtStr.append("virtual slaves:");
            else
                virtStr.append("slave:");
            PROGLOG("Slave log %u contains %s %s", s+1, virtStr.str(), slaveStr.str());
        }

        PROGLOG("verifying mp connection to rest of cluster");
        if (!queryNodeComm().verifyAll(false, 1000*60*30, 1000*60))
            throwStringExceptionV(0, "Failed to connect to all nodes");
        PROGLOG("verified mp connection to rest of cluster");

#ifdef _CONTAINERIZED
        if (globals->getPropBool("@_dafsStorage"))
        {
/* NB: This option is a developer option only.

 * It is intended to be used to bring up a temporary Thor instance that uses local node storage,
 * as the data plane.
 * 
 * It is likely to be deprecated or need reworking, when DFS is refactored to use SP's properly.
 * 
 * The mechanism works by:
 * a) Creating a pseudo StoragePlane (publishes group to Dali).
 * b) Spins up a dafilesrv thread in each slave container.
 * c) Changes the default StoragePlane used to publish files, to point to the SP/group created in step (a).
 * 
 * In this way, a Thor instance, whilst up, will act similarly to a bare-metal system, using local disks as storage.
 * This allows quick cloud based allocation/simulation of bare-metal type clusters for testing purposes.
 * 
 * NB: This isn't a real StoragePlane, and it will not be accessible by any other component.
 *
 */
            StringBuffer uniqueGrpName;
            queryNamedGroupStore().addUnique(&queryProcessGroup(), uniqueGrpName);
            // change default plane
            getComponentConfigSP()->setProp("@dataPlane", uniqueGrpName);
            PROGLOG("Persistent Thor group created with group name: %s", uniqueGrpName.str());
        }
#endif
        auditThorSystemEvent("Startup");
        auditStartLogged = true;

        writeSentinelFile(sentinelFile);

#ifndef _CONTAINERIZED
        unsigned pinterval = globals->getPropInt("@system_monitor_interval",1000*60);
        if (pinterval)
            startPerformanceMonitor(pinterval, PerfMonStandard, nullptr);
#endif
        configurePreferredPlanes();

        // NB: workunit/graphName only set in one-shot mode (if isCloud())
        thorMain(logHandler, workunit, graphName);
        auditThorSystemEvent("Terminate");
        LOG(MCdebugProgress, "ThorMaster terminated OK");
    }
    catch (IException *e) 
    {
        FLLOG(MCexception(e), e,"ThorMaster");
        exception.setown(e);
    }
    if (isContainerized())
    {
        int retCode = exception ? TEC_Exception : 0;
        if (!cloudJobName.isEmpty())
        {
            if (exception)
            {
                Owned<IWorkUnitFactory> factory = getWorkUnitFactory();
                Owned<IConstWorkUnit> wu = factory->openWorkUnit(workunit);
                if (wu)
                {
                    relayWuidException(wu, exception);
                    retCode = 0; // if successfully reported, suppress thormanager exit failure that would trigger another exception
                }
            }
            if (workerJobInstalled)
            {
                try
                {
                    k8s::KeepJobs keepJob = k8s::translateKeepJobs(globals->queryProp("@keepJobs"));
                    switch (keepJob)
                    {
                        case k8s::KeepJobs::all:
                            // do nothing
                            break;
                        case k8s::KeepJobs::podfailures:
                            if (nullptr == exception)
                                k8s::deleteResource("thorworker", "job", cloudJobName);
                            break;
                        case k8s::KeepJobs::none:
                            k8s::deleteResource("thorworker", "job", cloudJobName);
                            break;
                    }
                }
                catch (IException *e)
                {
                    EXCLOG(e);
                    e->Release();
                }
            }
            if (workerNSInstalled)
            {
                try
                {
                    k8s::deleteResource("thorworker", "networkpolicy", cloudJobName);
                }
                catch (IException *e)
                {
                    EXCLOG(e);
                    e->Release();
                }
            }
        }
        setExitCode(retCode);
    }

    // cleanup handler to be sure we end
    thorEndHandler->start(30);

    PROGLOG("Thor closing down 5");
#ifndef _CONTAINERIZED
    stopPerformanceMonitor();
#endif
    disconnectLogMsgManagerFromDali();
    closeThorServerStatus();
    PROGLOG("Thor closing down 4");
    closeDllServer();
    PROGLOG("Thor closing down 3");
    closeEnvironment();
    PROGLOG("Thor closing down 2");
    closedownClientProcess();
    PROGLOG("Thor closing down 1");
    UseSysLogForOperatorMessages(false);
    releaseAtoms(); // don't know why we can't use a module_exit to destruct this...

    return queryExitCode();
}
