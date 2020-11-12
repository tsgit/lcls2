import weakref
import os

# mode can be 'mpi' or 'legion' or 'none' for non parallel 
mode = os.environ.get('PS_PARALLEL', 'mpi')


class DsHelper(object):

    # Every Ds is assigned an ID. This permits Ds to be
    # pickled and sent across the network, as long as every node has the same
    # Ds under the same ID. (This should be true as long as the client
    # code initializes DataSources in a deterministic order.)
    next_ds_id = 0
    ds_by_id = weakref.WeakValueDictionary()

    def __init__(self, ds):
        ds.id = DsHelper.next_ds_id
        DsHelper.next_ds_id += 1
        DsHelper.ds_by_id[ds.id] = ds

def ds_from_id(ds_id):
    return DsHelper.ds_by_id[ds_id]

class RunHelper(object):

    # Every Run is assigned an ID. This permits Run to be
    # pickled and sent across the network, as long as every node has the same
    # Run under the same ID. (This should be true as long as the client
    # code initializes Runs in a deterministic order.)
    next_run_id = 0
    run_by_id = weakref.WeakValueDictionary()

    def __init__(self, run):
        run.id = RunHelper.next_run_id
        RunHelper.next_run_id += 1
        RunHelper.run_by_id[run.id] = run

def run_from_id(run_id):
    return RunHelper.run_by_id[run_id]

