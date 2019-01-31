#include "psdaq/monreq/XtcMonitorServer.hh"

#include "psdaq/eb/eb.hh"
#include "psdaq/eb/EbAppBase.hh"
#include "psdaq/eb/EbEvent.hh"

#include "psdaq/eb/EbLfClient.hh"

#include "psdaq/eb/utilities.hh"
#include "psdaq/eb/StatsMonitor.hh"

#include "psdaq/service/Fifo.hh"
#include "psdaq/service/GenericPool.hh"
#include "psdaq/service/Collection.hh"
#include "xtcdata/xtc/Dgram.hh"

#include <signal.h>
#include <errno.h>
#include <unistd.h>                     // For getopt()
#include <string.h>
#include <vector>
#include <bitset>
#include <iostream>
#include <atomic>

static const int      core_base            = 6; // 6 devXX, 8: accXX
static const int      core_offset          = 1; // Allows Ctrb and EB to run on the same machine
static const unsigned rtMon_period         = 1;  // Seconds
static const unsigned default_id           = 0;  // Builder's ID (< 64)
static const unsigned epoch_duration       = 8;  // Revisit: 1 per xferBuffer
static const unsigned numberof_xferBuffers = 8;  // Revisit: Value; corresponds to tstEbContributor:maxEvents
static const unsigned sizeof_buffers       = 1024; // Revisit

using namespace XtcData;
using namespace Pds::Eb;
using namespace Pds::MonReq;
using namespace Pds;

static volatile sig_atomic_t lRunning = 1;

void sigHandler( int signal )
{
  static unsigned callCount(0);

  if (callCount == 0)
  {
    printf("\nShutting down\n");

    lRunning = 0;
  }

  if (callCount++)
  {
    fprintf(stderr, "Aborting on 2nd ^C...\n");
    ::abort();
  }
}


namespace Pds {
  class MyXtcMonitorServer : public XtcMonitorServer {
  public:
    MyXtcMonitorServer(const char*     tag,
                       unsigned        sizeofBuffers,
                       unsigned        numberofEvQueues,
                       const EbParams& prms) :
      XtcMonitorServer(tag,
                       sizeofBuffers,
                       prms.maxBuffers,
                       numberofEvQueues),
      _sizeofBuffers(sizeofBuffers),
      _iTeb(0),
      _mrqTransport(new EbLfClient(prms.verbose)),
      _mrqLinks(prms.addrs.size()),
      _bufFreeList(prms.maxBuffers),
      _id(prms.id)
    {
      for (unsigned i = 0; i < prms.addrs.size(); ++i)
      {
        const char*    addr = prms.addrs[i].c_str();
        const char*    port = prms.ports[i].c_str();
        EbLfLink*      link;
        const unsigned tmo(120000);     // Milliseconds
        if (_mrqTransport->connect(addr, port, tmo, &link))
        {
          fprintf(stderr, "%s:\n  Error connecting to Monitor EbLfServer at %s:%s\n",
                  __func__, addr, port);
          abort();
        }
        if (link->preparePoster(prms.id))
        {
          fprintf(stderr, "%s:\n  Failed to prepare Monitor link to %s:%s\n",
                  __func__, addr, port);
          abort();
        }
        _mrqLinks[i] = link;

        printf("%s: EbLfServer ID %d connected\n", __func__, link->id());
      }

      for (unsigned i = 0; i < prms.maxBuffers; ++i)
        if (!_bufFreeList.push(i))
          fprintf(stderr, "%s:\n  _bufFreeList.push(%d) failed\n", __func__, i);

      _init();
    }
    virtual ~MyXtcMonitorServer()
    {
      if (_mrqTransport)  delete _mrqTransport;
    }
  public:
    void shutdown()
    {
      if (_mrqTransport)
      {
        for (auto it  = _mrqLinks.begin(); it != _mrqLinks.end(); ++it)
        {
          _mrqTransport->shutdown(*it);
        }
        _mrqLinks.clear();
      }
    }

  private:
    virtual void _copyDatagram(Dgram* dg, char* buf)
    {
      //printf("_copyDatagram @ %p to %p: pid = %014lx\n",
      //       dg, buf, dg->seq.pulseId().value());

      Dgram* odg = new((void*)buf) Dgram(*dg);

      // The dg payload is a directory of contributions to the built event.
      // Iterate over the directory and construct, in shared memory, the event
      // datagram (odg) from the contribution XTCs
      const Dgram** const  last = (const Dgram**)dg->xtc.next();
      const Dgram*  const* ctrb = (const Dgram**)dg->xtc.payload();
      do
      {
        const Dgram* idg = *ctrb;

        buf = (char*)odg->xtc.alloc(idg->xtc.extent);

        if (sizeof(*odg) + odg->xtc.sizeofPayload() > _sizeofBuffers)
        {
          fprintf(stderr, "%s:\n  Datagram is too large (%zd) for buffer of size %d\n",
                  __PRETTY_FUNCTION__, sizeof(*odg) + odg->xtc.sizeofPayload(), _sizeofBuffers);
          abort();            // The memcpy would blow by the buffer size limit
        }

        memcpy(buf, &idg->xtc, idg->xtc.extent);
      }
      while (++ctrb != last);
    }

    virtual void _deleteDatagram(Dgram* dg)
    {
      //printf("_deleteDatagram @ %p\n", dg);

      unsigned idx = *(unsigned*)dg->xtc.next();
      if (!_bufFreeList.push(idx))
        printf("_bufFreeList.push(%d) failed, count = %zd\n", idx, _bufFreeList.count());
      //printf("_deleteDatagram: _bufFreeList.push(%d), count = %zd\n",
      //       idx, _bufFreeList.count());

      Pool::free((void*)dg);
    }

    virtual void _requestDatagram()
    {
      //printf("_requestDatagram\n");

      if (_bufFreeList.empty())
      {
        fprintf(stderr, "%s:\n  No free buffers available\n", __PRETTY_FUNCTION__);
        return;
      }

      uint32_t data = _bufFreeList.pop();
      //printf("_requestDatagram: _bufFreeList.pop(): %08x, count = %zd\n", data, _bufFreeList.count());

      int rc = -1;
      for (unsigned i = 0; i < _mrqLinks.size(); ++i)
      {
        // Round robin through Trigger Event Builders
        unsigned iTeb = _iTeb++;
        if (_iTeb == _mrqLinks.size())  _iTeb = 0;

        EbLfLink* link = _mrqLinks[iTeb];

        data = ImmData::value(ImmData::Buffer, _id, data);

        rc = link->post(nullptr, 0, data);

        //printf("_requestDatagram: Post %d EB[iTeb = %d], value = %08x, rc = %d\n",
        //       i, iTeb, data, rc);

        if (rc == 0)  break;            // Break if message was delivered
      }
      if (rc)
      {
        fprintf(stderr, "%s:\n  Unable to post request to any TEB\n", __PRETTY_FUNCTION__);
        // Revisit: Is this fatal or ignorable?
      }
    }

  private:
    unsigned               _sizeofBuffers;
    unsigned               _nTeb;
    unsigned               _iTeb;
    EbLfClient*            _mrqTransport;
    std::vector<EbLfLink*> _mrqLinks;
    Fifo<unsigned>         _bufFreeList;
    unsigned               _id;
  };

  class MebApp : public EbAppBase
  {
  public:
    MebApp(const char*     tag,
           unsigned        sizeofEvBuffers,
           unsigned        nevqueues,
           bool            dist,
           const EbParams& prms,
           StatsMonitor&   smon) :
      EbAppBase  (prms),
      _apps      (tag, sizeofEvBuffers, nevqueues, prms),
      _pool      (sizeof(Dgram)                                               +
                  std::bitset<64>(prms.contributors).count() * sizeof(Dgram*) +
                  sizeof(unsigned),
                  prms.maxBuffers),
      _eventCount(0),
      _verbose   (prms.verbose),
      _prms      (prms)
    {
      _apps.distribute(dist);

      smon.registerIt("MEB.EvtRt",  _eventCount,      StatsMonitor::RATE);
      smon.registerIt("MEB.EvtCt",  _eventCount,      StatsMonitor::SCALAR);
      smon.registerIt("MEB.EpAlCt",  epochAllocCnt(), StatsMonitor::SCALAR);
      smon.registerIt("MEB.EpFrCt",  epochFreeCnt(),  StatsMonitor::SCALAR);
      smon.registerIt("MEB.EvAlCt",  eventAllocCnt(), StatsMonitor::SCALAR);
      smon.registerIt("MEB.EvFrCt",  eventFreeCnt(),  StatsMonitor::SCALAR);
      smon.registerIt("MEB.RxPdg",   rxPending(),     StatsMonitor::SCALAR);
    }
    virtual ~MebApp()
    {
    }
  public:
    void process()
    {
      //pinThread(task()->parameters().taskID(), _prms.core[1]);
      //pinThread(pthread_self(),                _prms.core[1]);

      //start();                              // Start the event timeout timer

      //pinThread(pthread_self(),                _prms.core[0]);

      int rc;
      while (lRunning)
      {
        if ( (rc = EbAppBase::process()) )
        {
          if (rc != -FI_ETIMEDOUT )  break;
        }
      }

      //cancel();                         // Stop the event timeout timer

      _apps.shutdown();

      EbAppBase::shutdown();
    }
    virtual void process(EbEvent* event)
    {
      if (_verbose > 2)
      {
        static unsigned cnt = 0;
        printf("MebApp::process event dump:\n");
        event->dump(++cnt);
      }
      ++_eventCount;

      // Create a Dgram with a payload that is a directory of contribution
      // Dgrams to the built event.  Reserve space at end for the buffer's index
      size_t   sz     = (event->end() - event->begin()) * sizeof(*(event->begin()));
      unsigned idx    = ImmData::idx(event->parameter());
      void*    buffer = _pool.alloc(sizeof(Dgram) + sz + sizeof(idx));
      if (!buffer)
      {
        fprintf(stderr, "%s:\n  Dgram pool allocation failed:\n", __PRETTY_FUNCTION__);
        _pool.dump();
        abort();
      }
      Dgram*  dg  = new(buffer) Dgram(*(event->creator()));
      Dgram** buf = (Dgram**)dg->xtc.alloc(sz);
      memcpy(buf, event->begin(), sz);
      *(unsigned*)dg->xtc.next() = idx; // Pass buffer's index to _deleteDatagram()

      if (_verbose > 1)
      {
        uint64_t pid = dg->seq.pulseId().value();
        unsigned ctl = dg->seq.pulseId().control();
        size_t   sz  = sizeof(*dg) + dg->xtc.sizeofPayload();
        unsigned svc = dg->seq.service();
        printf("MEB processed              event[%4ld]    @ "
               "%16p, ctl %02x, pid %014lx, sz %4zd, %3s # %2d\n",
               _eventCount, dg, ctl, pid, sz,
               svc == TransitionId::L1Accept ? "buf" : "tr", idx);
      }

      if (_apps.events(dg) == XtcMonitorServer::Handled)
      {
        Pool::free((void*)dg);
      }
    }
  private:
    MyXtcMonitorServer _apps;
    GenericPool        _pool;
    uint64_t           _eventCount;
    const unsigned     _verbose;
    const EbParams&    _prms;
  };
};

using namespace Pds;


void usage(char* progname)
{
  printf("\n<TEB_spec> has the form '<id>:<addr>:<port>'\n");
  printf("<id> must be in the range 0 - %d.\n", MAX_MEBS - 1);
  printf("Low numbered <port> values are treated as offsets into the following range:\n");
  printf("  Mon requests: %d - %d\n", MRQ_PORT_BASE, MRQ_PORT_BASE + MAX_MEBS - 1);

  printf("Usage: %s -C <collection server>"
                   "-p <platform> "
                   "[-P <partition>] "
                   "-n <numb shm buffers> "
                   "-s <shm buffer size> "
                  "[-q <# event queues>] "
                  "[-t <tag name>] "
                  "[-d] "
                  "[-A <interface addr>] "
                  "[-E <MEB port>] "
                  "[-i <ID>] "
                   "-c <Contributors (DRPs)> "
                  "[-Z <Run-time mon host>] "
                  "[-R <Run-time mon port>] "
                  "[-m <Run-time mon publishing period>] "
                  "[-1 <core to pin App thread to>]"
                  "[-2 <core to pin other threads to>]" // Revisit: None?
                  "[-V] " // Run-time mon verbosity
                  "[-v] "
                  "[-h] "
                  "<TEB_spec> [<TEB_spec> [...]]\n", progname);
}

static
void joinCollection(const std::string& server,
                    unsigned           partition,
                    const std::string& ifAddr,
                    unsigned           portBase,
                    EbParams&          prms)
{
  Collection collection(server, partition, "meb");
  collection.connect();
  std::cout << "cmstate:\n" << collection.cmstate.dump(4) << std::endl;

  std::string id = std::to_string(collection.id());
  prms.id     = collection.cmstate["meb"][id]["meb_id"];
  prms.ifAddr = collection.cmstate["meb"][id]["connect_info"]["infiniband"];

  prms.contributors = 0;
  for (auto it : collection.cmstate["drp"].items())
  {
    unsigned ctrbId = it.value()["drp_id"];
    prms.contributors |= 1ul << ctrbId;
  }

  for (auto it : collection.cmstate["teb"].items())
  {
    unsigned    tebId  = it.value()["teb_id"];
    std::string address = it.value()["connect_info"]["infiniband"];
    prms.addrs.push_back(address);
    prms.ports.push_back(std::string(std::to_string(portBase + tebId)));
  }
}

int main(int argc, char** argv)
{
  const unsigned NO_PLATFORM     = unsigned(-1UL);
  unsigned       platform        = NO_PLATFORM;
  const char*    tag             = 0;
  std::string    partition        (PARTITION);
  const char*    rtMonHost       = RTMON_HOST;
  unsigned       rtMonPort       = RTMON_PORT_BASE;
  unsigned       rtMonPeriod     = rtMon_period;
  unsigned       rtMonVerbose    = 0;
  unsigned       mebPortNo       = 0;   // Port served to contributors
  std::string    collSvr          (COLL_HOST);
  EbParams       prms { /* .ifAddr        = */ { }, // Network interface to use
                        /* .ebPort        = */ std::to_string(mebPortNo),
                        /* .mrqPort       = */ { }, // Unused here
                        /* .id            = */ default_id,
                        /* .contributors  = */ 0,   // DRPs
                        /* .addrs         = */ { }, // MonReq addr served by TEB
                        /* .ports         = */ { }, // MonReq port served by TEB
                        /* .duration      = */ epoch_duration,
                        /* .maxBuffers    = */ numberof_xferBuffers,
                        /* .maxEntries    = */ 1,   // per buffer
                        /* .numMrqs       = */ 0,   // Unused here
                        /* .maxTrSize     = */ sizeof_buffers,
                        /* .maxResultSize = */ 0,   // Unused here
                        /* .core          = */ { core_base + core_offset + 0,
                                                 core_base + core_offset + 12 },
                        /* .verbose       = */ 0 };
  unsigned       sizeofEvBuffers = sizeof_buffers;
  unsigned       nevqueues       = 1;
  bool           ldist           = false;

  int c;
  while ((c = getopt(argc, argv, "p:n:P:s:q:t:dA:E:i:c:Z:R:m:C:1:2:Vvh")) != -1)
  {
    errno = 0;
    char* endPtr;
    switch (c) {
      case 'p':
        platform = strtoul(optarg, &endPtr, 0);
        if (errno != 0 || endPtr == optarg) platform = NO_PLATFORM;
        break;
      case 'n':
        sscanf(optarg, "%d", &prms.maxBuffers);
        break;
      case 'P':
        partition = std::string(optarg);
        break;
      case 't':
        tag = optarg;
        break;
      case 'q':
        nevqueues = strtoul(optarg, NULL, 0);
        break;
      case 's':
        sizeofEvBuffers = (unsigned) strtoul(optarg, NULL, 0);
        break;
      case 'd':
        ldist = true;
        break;
      case 'A':  prms.ifAddr       = optarg;                       break;
      case 'E':  mebPortNo         = atoi(optarg);                 break;
      case 'i':  prms.id           = atoi(optarg);                 break;
      case 'c':  prms.contributors = strtoul(optarg, nullptr, 0);  break;
      case 'Z':  rtMonHost         = optarg;                       break;
      case 'R':  rtMonPort         = atoi(optarg);                 break;
      case 'm':  rtMonPeriod       = atoi(optarg);                 break;
      case 'C':  collSvr           = optarg;                       break;
      case '1':  prms.core[0]      = atoi(optarg);                 break;
      case '2':  prms.core[1]      = atoi(optarg);                 break;
      case 'v':  ++prms.verbose;                                   break;
      case 'V':  ++rtMonVerbose;                                   break;
      case 'h':                         // help
        usage(argv[0]);
        return 0;
        break;
      default:
        printf("Unrecogized parameter '%c'\n", c);
        usage(argv[0]);
        return 1;
    }
  }

  if (prms.maxBuffers < numberof_xferBuffers) prms.maxBuffers = numberof_xferBuffers;

  if (!tag)  tag = partition.c_str();
  printf("Partition Tag: '%s'\n", tag);

  const unsigned numPorts    = MAX_DRPS + MAX_TEBS + MAX_MEBS + MAX_MEBS;
  const unsigned mrqPortBase = MRQ_PORT_BASE + numPorts * platform;
  const unsigned mebPortBase = MEB_PORT_BASE + numPorts * platform;

  if (optind < argc)
  {
    do
    {
      char* teb    = argv[optind];
      char* colon1 = strchr(teb, ':');
      char* colon2 = strrchr(teb, ':');
      if (!colon1 || (colon1 == colon2))
      {
        fprintf(stderr, "TEB spec '%s' is not of the form <ID>:<IP>:<port>\n", teb);
        return 1;
      }
      unsigned port = atoi(&colon2[1]);
      if (port < MAX_MEBS)  port += mrqPortBase;
      if ((port < mrqPortBase) || (port >= mrqPortBase + MAX_MEBS))
      {
        fprintf(stderr, "TEB client port %d is out of range %d - %d\n",
                port, mrqPortBase, mrqPortBase + MAX_MEBS);
        return 1;
      }
      prms.addrs.push_back(std::string(&colon1[1]).substr(0, colon2 - &colon1[1]));
      prms.ports.push_back(std::string(std::to_string(port)));
    }
    while (++optind < argc);
  }
  else
  {
    joinCollection(collSvr, platform, prms.ifAddr, mrqPortBase, prms);
    /* contributors, mrqAddrs, mrqPorts, id */
  }
  if (prms.id >= MAX_MEBS)
  {
    fprintf(stderr, "MEB ID %d is out of range 0 - %d\n", prms.id, MAX_MEBS - 1);
    return 1;
  }
  if ((prms.addrs.size() == 0) || (prms.ports.size() == 0))
  {
    fprintf(stderr, "Missing required TEB request address(es)\n");
    return 1;
  }

  if  (mebPortNo < MAX_MEBS)  mebPortNo += mebPortBase;
  if ((mebPortNo < mebPortBase) || (mebPortNo >= mebPortBase + MAX_MEBS))
  {
    fprintf(stderr, "MEB Server port %d is out of range %d - %d\n",
            mebPortNo, mebPortBase, mebPortBase + MAX_MEBS);
    return 1;
  }
  prms.ebPort = std::to_string(mebPortNo + prms.id);
  //printf("MEB Srv port = %s\n", mebPort.c_str());

  if (!prms.maxBuffers || !sizeofEvBuffers || platform == NO_PLATFORM || !prms.contributors) {
    fprintf(stderr, "Missing parameters!\n");
    usage(argv[0]);
    return 1;
  }

  ::signal( SIGINT, sigHandler );

  // Revisit: Pinning exacerbates a race condition somewhere resulting in either
  //          'No free buffers available' or 'Dgram pool allocation failed'
  //pinThread(pthread_self(), prms.core[1]);
  StatsMonitor* smon = new StatsMonitor(rtMonHost,
                                        rtMonPort,
                                        platform,
                                        partition,
                                        rtMonPeriod,
                                        rtMonVerbose);

  //pinThread(pthread_self(), prms.core[0]);
  MebApp*       meb  = new MebApp(tag,
                                  sizeofEvBuffers,
                                  nevqueues,
                                  ldist,
                                  prms,
                                  *smon);

  // Wait a bit to allow other components of the system to establish connections
  sleep(1);

  meb->process();

  smon->shutdown();

  delete meb;
  delete smon;

  return 0;
}
