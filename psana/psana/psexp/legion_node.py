from psana.psexp import mode

pygion = None
if mode == 'legion':
    import pygion
    from pygion import task
else:
    # Nop when not using Legion
    def task(fn=None, **kwargs):
        if fn is None:
            return lambda fn: fn
        return fn

from psana.psexp import EventBuilderManager, TransitionId, Events
from psana.psexp.run import RunLegion

def smd_chunks(ds):
    for smd_data in ds.smdr_man.chunks():
        yield smd_data

@task(inner=True)
def run_smd0_task(ds):
    global_procs = pygion.Tunable.select(pygion.Tunable.GLOBAL_PYS).get()

    for i, smd_data in enumerate(smd_chunks(ds)):
        run_eb_task(smd_data, ds, point=i)
    # Block before returning so that the caller can use this task's future for synchronization
    pygion.execution_fence(block=True)

def eb_batches(smd_chunk, ds):
    eb_man = EventBuilderManager(smd_chunk, ds._configs, ds.dsparms) 
    for eb_data in eb_man.batches():
        yield eb_data

@task(inner=True)
def run_eb_task(smd_data, ds):
    smd_chunk, step_chunk, calibconst_pkt = smd_data
    for i, eb_data in enumerate(eb_batches(smd_chunk, ds)):
        run_bigdata_task(eb_data, ds, point=i)

def batch_events(smd_batch, ds):
    batch_iter = iter([smd_batch, bytearray()])
    def get_smd():
        for this_batch in batch_iter:
            return this_batch

    events  = Events(ds._configs, ds.dm, ds.dsparms.prom_man, 
            filter_callback = ds.dsparms.filter, 
            get_smd         = get_smd)
    for evt in events:
        yield evt

@task
def run_bigdata_task(eb_data, ds):
    smd_batch_dict, step_batch_dict = eb_data
    smd_batch, _ = smd_batch_dict[0]
    for evt in batch_events(smd_batch, ds):
        if evt.service() == TransitionId.BeginRun:
            run = RunLegion(evt, ds._configs, ds.dsparms)
            ds.run_fn(run)
            ds.run = run
        if evt.service() != TransitionId.L1Accept:
            continue
        ds.event_fn(evt, ds.run)

ds_to_process = []
def analyze(ds, run_fn=None, event_fn=None):
    ds.run_fn   = run_fn
    ds.event_fn = event_fn
    if pygion.is_script:
        num_procs = pygion.Tunable.select(pygion.Tunable.GLOBAL_PYS).get()

        bar = pygion.c.legion_phase_barrier_create(pygion._my.ctx.runtime, pygion._my.ctx.context, num_procs)
        pygion.c.legion_phase_barrier_arrive(pygion._my.ctx.runtime, pygion._my.ctx.context, bar, 1)
        global_task_registration_barrier = pygion.c.legion_phase_barrier_advance(pygion._my.ctx.runtime, pygion._my.ctx.context, bar)
        pygion.c.legion_phase_barrier_wait(pygion._my.ctx.runtime, pygion._my.ctx.context, bar)

        return run_smd0_task(ds)
    else:
        ds_to_process.append(ds)
    

if pygion is not None and not pygion.is_script:
    @task(top_level=True)
    def legion_main():
        for ds in ds_to_process:
            run_smd0_task(ds, point=0)
