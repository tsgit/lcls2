from psana.psexp import *
from psana.dgrammanager import DgramManager
import numpy as np
import os, sys

class LegionDataSource(DataSourceBase):
    def __init__(self, *args, **kwargs):
        super(LegionDataSource, self).__init__(**kwargs)
        self._setup_xtcs()
        self.smd_fds  = np.array([os.open(smd_file, os.O_RDONLY) for smd_file in self.smd_files], dtype=np.int32)
        self.smdr_man = SmdReaderManager(self.smd_fds, self.dsparms)
        self._configs = self.smdr_man.get_next_dgrams()
        self.smdr_man.set_configs(self._configs)
        self._setup_det_class_table()
        self._set_configinfo()
        self._start_prometheus_client()
        self.dm = DgramManager(self.xtc_files, configs=self._configs)
        self.dm.dsparms = self.dsparms

    def analyze(self, **kwargs):
        return legion_node.analyze(self, **kwargs)

    def runs(self):
        return
