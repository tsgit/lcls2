from psana.eventbuilder import EventBuilder
from psana.psexp        import PacketFooter, PrometheusManager
from .run import RunSmallData

class EventBuilderManager(object):

    def __init__(self, view, configs, dsparms, run): 
        self.configs        = configs 
        self.dsparms        = dsparms
        self.n_files        = len(self.configs)
        c_filter            = PrometheusManager.get_metric('psana_eb_filter')

        pf                  = PacketFooter(view=view)
        views               = pf.split_packets()
        self.eb             = EventBuilder(views, self.configs, 
                                           dsparms=dsparms,
                                           run=run,
                                           prometheus_counter=c_filter)
        self.run_smd        = RunSmallData(run, self.eb)

    def batches(self):
        while True: 
            # Collects list of proxy events to be converted to batches.
            # Note that we are persistently calling smd_callback until there's nothing
            # left in all views used by EventBuilder. From this while/for loops, we 
            # either gets transitions from SmdDataSource and/or L1 from the callback.
            while self.run_smd.proxy_events == [] and self.eb.has_more():
                for evt in self.dsparms.smd_callback(self.run_smd):
                    self.run_smd.proxy_events.append(evt._proxy_evt)
            
            if not self.run_smd.proxy_events:
                break

            # Generate a bytearray representations of all the events in a batch.
            batch_dict, step_dict = self.eb.gen_bytearray_batch(self.run_smd.proxy_events)
            self.run_smd.proxy_events = []
            
            yield batch_dict, step_dict

