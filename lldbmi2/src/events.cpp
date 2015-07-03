
#include <chrono>
#include <thread>

#include "lldbmi2.h"
#include "events.h"
#include "format.h"
#include "log.h"


static pthread_t sbTID;


int
startprocesslistener (STATE *pstate)
{
	int ret = pthread_create (&sbTID, NULL, &processlistener, pstate);
	if (ret)
		sbTID = 0;
	return ret;
}

void
waitprocesslistener ()
{
	if (sbTID)
		pthread_join (sbTID, NULL);
}

// wait thread
void *
processlistener (void *arg)
{
	STATE *pstate = (STATE *) arg;
	SBProcess process = pstate->process;

 	if (!process.IsValid())
		return NULL;

	SBBroadcaster broadcaster = process.GetBroadcaster();
	SBListener listener = SBListener("ProcessListener");
	if (!listener.IsValid())
		return NULL;
	listener.StartListeningForEvents (broadcaster, UINT32_MAX);

	while (!pstate->eof) {
		SBEvent event;
		bool gotevent = listener.WaitForEventForBroadcaster (1000, broadcaster, event);
	    if (!gotevent || !event.IsValid())
			continue;
		uint32_t eventtype = event.GetType();
		StateType processstate = process.GetState();

		SBThread thread;
		switch (eventtype) {
		case SBProcess::eBroadcastBitStateChanged:
			logprintf (LOG_EVENTS|LOG_RAW, "eBroadcastBitStateChanged\n");
			switch (processstate) {
			case eStateRunning:
				logprintf (LOG_EVENTS, "eStateRunning\n");
				break;
			case eStateExited:
				logprintf (LOG_EVENTS, "eStateExited\n");
				CheckThreadsLife (pstate, process);		// not useful. threads are not stopped before exit
				cdtprintf (
					"=thread-group-exited,id=\"%s\",exit-code=\"0\"\n"
					"*stopped,reason=\"exited-normally\"\n\(gdb)\n",
					pstate->threadgroup, pstate->threadgroup);
				pstate->eof = true;
				break;
			case eStateStopped:
				logprintf (LOG_EVENTS, "eStateStopped\n");
				onbreakpoint (pstate, process);
				break;
			default:
				logprintf (LOG_WARN, "unexpected process state %d\n", processstate);
				break;
			}
			break;
		case SBProcess::eBroadcastBitInterrupt:
			logprintf (LOG_EVENTS, "eBroadcastBitInterrupt\n");
			break;
		case SBProcess::eBroadcastBitProfileData:
			logprintf (LOG_EVENTS, "eBroadcastBitProfileData\n");
			break;
		case SBProcess::eBroadcastBitSTDOUT:
		case SBProcess::eBroadcastBitSTDERR:
			// pass stdout and stderr from application to pty
			long iobytes;
			char iobuffer[LINE_MAX];
			logprintf (LOG_EVENTS, "eBroadcastBitSTDOUT\n");
			iobytes = process.GetSTDOUT (iobuffer, sizeof(iobuffer));
			if (iobytes > 0) {
				// remove \r
				char *ps=iobuffer, *pd=iobuffer;
				do {
					if (*ps=='\r' && *(ps+1)=='\n') {
						++ps;
						--iobytes;
					}
					*pd++ = *ps++;
				} while (*(ps-1));
				write ((pstate->ptyfd!=EOF)?pstate->ptyfd:STDOUT_FILENO, iobuffer, iobytes);
			}
			logdata (LOG_PROG_IN, iobuffer, iobytes);
			break;
		default:
			logprintf (LOG_WARN, "unknown event type %s\n", eventtype);
			break;
		}
	}
	logprintf (LOG_EVENTS, "processlistener exited\n");
	usleep (1000000);
	return NULL;
}


void
onbreakpoint (STATE *pstate, SBProcess process)
{
//	-3-38-5.140 <<=  |=breakpoint-modified,bkpt={number="breakpoint 1",type="breakpoint",disp="del",enabled="y",addr="0x0000000100000f06",func="main",file="hello.c",fullname="hello.c",line="33",thread-groups=["i1"],times="1",original-location="hello.c:33"}\n|
//	-3-38-5.140 <<=  |*stopped,reason="breakpoint-hit",disp="keep",bkptno="breakpoint 1",frame={addr="0x0000000100000f06",func="main",args=[],file="hello.c",fullname="hello.c",line="33"},thread-id="1",stopped-threads="all"\n|
//	-3-40-7.049 <<=  |*stopped,reason="breakpoint-hit",disp="keep",bkptno="1",frame={addr="0000000000000f06",func="main",args=[],file="hello.c",fullname="/Users/didier/Projets/LLDB/hello/Debug/../src/hello.c",line="33"},thread-id="1",stopped-threads="all"(gdb)\n|

	pstate->running = false;
	CheckThreadsLife (pstate, process);
	UpdateSelectedThread (process);				// search which thread is stopped
	SBTarget target = process.GetTarget();
	SBThread thread = process.GetSelectedThread();
	int stopreason = thread.GetStopReason();
//	logprintf (LOG_EVENTS, "stopreason=%d\n", stopreason);
	if (stopreason==eStopReasonBreakpoint || stopreason==eStopReasonPlanComplete) {
		int bpid=0;
		const char *dispose = "keep";
		char reasondesc[LINE_MAX];
		if (stopreason==eStopReasonBreakpoint) {
			if (thread.GetStopReasonDataCount() > 0) {
				int bpid = thread.GetStopReasonDataAtIndex(0);
				SBBreakpoint breakpoint = target.FindBreakpointByID (bpid);
				if (breakpoint.IsOneShot())
					dispose = "del";
				char breakpointdesc[LINE_MAX];
				formatbreakpoint (breakpointdesc, sizeof(breakpointdesc), breakpoint, pstate);
				cdtprintf ("=breakpoint-modified,bkpt=%s\n", breakpointdesc);
				snprintf (reasondesc, sizeof(reasondesc), "reason=\"breakpoint-hit\",disp=\"%s\",bkptno=\"%d\",", dispose, bpid);
			}
			else
				snprintf (reasondesc, sizeof(reasondesc), "reason=\"function-finished\",");
		}
		else
			reasondesc[0] = '\0';
		SBFrame frame = thread.GetSelectedFrame();
		char framedesc[LINE_MAX];
		formatframe (framedesc,sizeof(framedesc),frame,false);
		int threadindexid=thread.GetIndexID();
		cdtprintf ("*stopped,%s%s,thread-id=\"%d\",stopped-threads=\"all\"\n(gdb)\n",
					reasondesc,framedesc,threadindexid);
	//	cdtprintf ("*stopped,reason=\"breakpoint-hit\",disp=\"keep\",bkptno=\"1\",frame={addr=\"0x0000000100000f06\",func=\"main\",args=[],file=\"../src/hello.c\",fullname=\"/Users/didier/Projets/LLDB/hello/src/hello.c\",line=\"33\"},thread-id=\"1\",stopped-threads=\"all\"\n(gdb) \n");
		if (strcmp(dispose,"del")==0) {
			target.BreakpointDelete(bpid);
			cdtprintf ("=breakpoint-deleted,id=\"%d\"\n", bpid);
		}
	}
	else if (stopreason==eStopReasonNone) {
		// raised when a thread different from the selected thread stops
	}
	else if (stopreason==eStopReasonInvalid) {
		// raised when the program exits
	}
	else
		logprintf (LOG_WARN, "unexpected stop reason %d\n", stopreason);
}


void
CheckThreadsLife (STATE *pstate, SBProcess process)
{
    if (!process.IsValid())
        return;
    SBThread thread;
    const size_t nthreads = process.GetNumThreads();
	int indexlist;
	bool stillalive[MAX_THREADS];
	for (indexlist=0; indexlist<MAX_THREADS; indexlist++)			// init live list
		stillalive[indexlist] = false;
	for (int indexthread=0; indexthread<nthreads; indexthread++) {
		SBThread thread = process.GetThreadAtIndex(indexthread);
		if (thread.IsValid()) {
			int stopreason = thread.GetStopReason();
			int threadindexid = thread.GetIndexID();
			logprintf (LOG_INFO, "thread threadindexid=%d stopreason=%d\n", threadindexid, stopreason);
			for (indexlist=0; indexlist<MAX_THREADS; indexlist++) {
				if (threadindexid == pstate->threadids[indexlist])	// existing thread
					break;
			}
			if (indexlist<MAX_THREADS)								// existing thread. mark as alive
				stillalive[indexlist] = true;
			else {													// new thread. add to the thread list list
				for (indexlist=0; indexlist<MAX_THREADS; indexlist++) {
					if (pstate->threadids[indexlist]==0) {
						pstate->threadids[indexlist] = threadindexid;
						stillalive[indexlist] = true;
						if (indexlist>0)							// message for first one yet sent
							cdtprintf ("=thread-created,id=\"%d\",group-id=\"%s\"\n", threadindexid, pstate->threadgroup);
						break;
					}
				}
				if (indexlist >= MAX_THREADS)
					logprintf (LOG_ERROR, "threads table too small (%d)\n", MAX_THREADS);
			}
		}
	}
	for (indexlist=0; indexlist<MAX_THREADS; indexlist++) {			// find finished threads
		if (pstate->threadids[indexlist]>0 && !stillalive[indexlist]) {
			cdtprintf ("=thread-exited,id=\"%d\",group-id=\"%s\"\n",
					pstate->threadids[indexlist], pstate->threadgroup);
			pstate->threadids[indexlist] = 0;
		}
	}
}


void
UpdateSelectedThread (SBProcess process)
{
    if (!process.IsValid())
        return;
    SBThread currentThread = process.GetSelectedThread();
    SBThread thread;
    const StopReason eCurrentThreadStoppedReason = currentThread.GetStopReason();
    if (!currentThread.IsValid() || (eCurrentThreadStoppedReason == eStopReasonInvalid) ||
    		(eCurrentThreadStoppedReason == eStopReasonNone)) {
        // Prefer a thread that has just completed its plan over another thread as current thread
        SBThread planThread;
        SBThread otherThread;
        const size_t nthreads = process.GetNumThreads();
        for (int indexthread=0; indexthread<nthreads; indexthread++) {
            //  GetThreadAtIndex() uses a base 0 index
            //  GetThreadByIndexID() uses a base 1 index
            thread = process.GetThreadAtIndex(indexthread);
            const StopReason eThreadStopReason = thread.GetStopReason();
            switch (eThreadStopReason) {
                case eStopReasonTrace:
                case eStopReasonBreakpoint:
                case eStopReasonWatchpoint:
                case eStopReasonSignal:
                case eStopReasonException:
                    if (!otherThread.IsValid())
                        otherThread = thread;
                    break;
                case eStopReasonPlanComplete:
                    if (!planThread.IsValid())
                        planThread = thread;
                    break;
                case eStopReasonInvalid:
                case eStopReasonNone:
                default:
                    break;
            }
        }
        if (planThread.IsValid())
            process.SetSelectedThread(planThread);
        else if (otherThread.IsValid())
            process.SetSelectedThread(otherThread);
        else {
            if (currentThread.IsValid())
                thread = currentThread;
            else
                thread = process.GetThreadAtIndex(0);
            if (thread.IsValid())
                process.SetSelectedThread(thread);
        }
    }
}
