import re
import sys
import zmq
import time
import json
import argparse
import threading
from mpi4py import MPI
import multiprocessing as mp
from ami.manager import Manager
from ami.comm import Collector, ResultStore
from ami.data import MsgTypes, DataTypes, Transitions, Occurrences, Message, Datagram, Transition, StaticSource



def build_dep_tree(json):
    """
    Take a json representation of a configuration and produce a DAG
    that shows dependencies (ie outputs) instead of inputs.
    """

    


    return

def dep_resolve(node, resolved):
    print node.name
    for edge in node.edges:
        if edge not in resolved:
            dep_resolve(edge, resolved)
    resolved.append(node)
    return resolved
   

class Worker(object):

    def __init__(self, idnum, src, collector_rank):
        """
        idnum : int
            a unique integer identifying this worker
        src : object
            object with an events() method that is an iterable (like psana.DataSource)
        """

        self.idnum = idnum
        self.src = src
        self.collector_rank = collector_rank

        return

    def run(self):

        for msg in self.src.events():

            # check to see if the graph has been reconfigured after update
            if msg.mtype == MsgTypes.Occurrence and \
               msg.payload == Occurrences.Heartbeat:

                if MPI.COMM_WORLD.Iprobe(source=0, tag=1):
                    self.graph = MPI.COMM_WORLD.recv(source=0, tag=1)
                    print("worker%d: Received new configuration"%self.idnum)
                else:
                    print("worker%d: got unknown message?")

            elif msg.mtype == MsgTypes.Datagram:
                pass # handle


               
def run_worker(num, source, collector_rank=0):

    print('Starting worker # %d, sending to collector %d' % (num, collector_rank))
    sys.stdout.flush()

    if source[0] == 'static':
        try:
            with open(source[1], 'r') as cnf:
                src_cfg = json.load(cnf)
        except OSError as os_exp:
            print("worker%03d: problem opening json file:"%num, os_exp)
            return 1
        except json.decoder.JSONDecodeError as json_exp:
            print("worker%03d: problem parsing json file (%s):"%(num, source[1]), json_exp)
            return 1

        src = StaticSource(num, 
                           src_cfg['interval'], 
                           src_cfg['heartbeat'], 
                           src_cfg["init_time"], 
                           src_cfg['config'])
    else:
        print("worker%03d: unknown data source type:"%num, source[0])
    worker = Worker(num, src, collector_rank)
    sys.exit(worker.run())

def main():
    parser = argparse.ArgumentParser(description='AMII Worker/Collector App')

    parser.add_argument(
        '-H',
        '--host',
        default='localhost',
        help='hostname of the AMII Manager'
    )

    parser.add_argument(
        '-p',
        '--port',
        type=int,
        default=5556,
        help='port for GUI-Manager communication (via zmq)'
    )

    parser.add_argument(
        '-n',
        '--num-workers',
        type=int,
        default=1,
        help='number of worker processes'
    )

    parser.add_argument(
        '-N',
        '--node-num',
        type=int,
        default=1,
        help='node identification number'
    )

    parser.add_argument(
        'source',
        metavar='SOURCE',
        help='data source configuration (exampes: static://test.json, psana://exp=xcsdaq13:run=14)'
    )

    args = parser.parse_args()

    try:
        src_url_match = re.match('(?P<prot>.*)://(?P<body>.*)', args.source)
        if src_url_match:
            src_cfg = src_url_match.groups()
        else:
            print("Invalid data source config string:", args.source)
            return 1
            
        rank = MPI.COMM_WORLD.Get_rank()
        size = MPI.COMM_WORLD.Get_size()
        if rank == 0:
            m = Manager(args.port)
            m.run()
        else:
            run_worker(rank, src_cfg, collector_rank=0)

    except KeyboardInterrupt:
        print("Worker killed by user...")
        return 0


if __name__ == '__main__':
    sys.exit(main())

 
