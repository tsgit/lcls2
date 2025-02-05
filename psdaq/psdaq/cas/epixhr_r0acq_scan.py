from psdaq.configdb.typed_json import cdict
import psdaq.configdb.configdb as cdb
import sys
import os
import logging
import threading
from psdaq.control.ControlDef import ControlDef, MyFloatPv, MyStringPv
from psdaq.control.DaqControl import DaqControl
from psdaq.control.ConfigScan import ConfigScan
import argparse
import json
import numpy as np

#
#  Use the AMI MeanVsScan plot
#    Not that its binning has an error
#    The first,last bin will not be filled; 
#    The other bins need to be shifted
#    So, for (100,1800100,90000) the binning should be (21,-89800,1800200)
#    (Valid plot points will be 200,90200,...,1710200 inclusive)
#

def listParams(d,name):
    name = '' if name is None else name+'.'
    for k,v in d.items():
        if 'PacketRegisters' in k:
            pass
        elif 'Hr10kTAsic' in k:
            if k=='Hr10kTAsic0':
                listParams(v,f'{name}Hr10kTAsic')
            else:
                pass
        elif isinstance(v,dict):
            listParams(v,f'{name}{k}')
        else:
            print(f'{name}{k}')
    
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('-p', type=int, choices=range(0, 8), default=2,
                        help='platform (default 2)')
    parser.add_argument('-C', metavar='COLLECT_HOST', default='drp-srcf-cmp004',
                        help='collection host (default drp-srcf-cmp004)')
    parser.add_argument('-t', type=int, metavar='TIMEOUT', default=20000,
                        help='timeout msec (default 20000)')
    parser.add_argument('-g', type=int, default=6, metavar='GROUP_MASK', help='bit mask of readout groups (default 1<<plaform)')
    parser.add_argument('--config', metavar='ALIAS', default='BEAM', help='configuration alias (e.g. BEAM)')
    parser.add_argument('--detname', default='epixhr_0', help="detector name (default 'epixhr_0')")
    parser.add_argument('--scantype', default='config', help="scan type (default 'config')")
    parser.add_argument('-v', action='store_true', help='be verbose')
    parser.add_argument('--start' , default=[0,0], type=int, help='list of start values for (R0,AcqDelay1)', nargs=2)
    parser.add_argument('--step'  , default=1, type=int, help='step size')
    parser.add_argument('--nsteps', default=1, type=int, help='number of scan steps')

    parser.add_argument('--events', type=int, default=2000, help='events per step (default 2000)')
    parser.add_argument('--record', type=int, choices=range(0, 2), help='recording flag')

    args = parser.parse_args()

    del sys.argv[1:]

    if args.g is not None:
        if args.g < 1 or args.g > 255:
            parser.error('readout group mask (-g) must be 1-255')
        group_mask = args.g
    else:
        group_mask = 1 << args.p

    if args.events < 1:
        parser.error('readout count (--events) must be >= 1')

    # instantiate DaqControl object
    control = DaqControl(host=args.C, platform=args.p, timeout=args.t)

    try:
        instrument = control.getInstrument()
    except KeyboardInterrupt:
        instrument = None

    if instrument is None:
        sys.exit('Error: failed to read instrument name (check -C <COLLECT_HOST>)')

    # configure logging handlers
    if args.v:
        level=logging.DEBUG
    else:
        level=logging.WARNING
    logging.basicConfig(level=level)
    logging.info('logging initialized')

    # get initial DAQ state
    daqState = control.getState()
    logging.info('initial state: %s' % daqState)
    if daqState == 'error':
        sys.exit(1)

    # optionally set BEAM or NOBEAM
    if args.config is not None:
        # config alias request
        rv = control.setConfig(args.config)
        if rv is not None:
            logging.error('%s' % rv)

    if args.record is not None:
        # recording flag request
        if args.record == 0:
            rv = control.setRecord(False)
        else:
            rv = control.setRecord(True)
        if rv is not None:
            print('Error: %s' % rv)

    # instantiate ConfigScan
    scan = ConfigScan(control, daqState=daqState, args=args)

    scan.stage()

    # -- begin script --------------------------------------------------------

    # PV scan setup
    motors = [MyFloatPv(ControlDef.STEP_VALUE)]
    scan.configure(motors = motors)

    d = {}
    for i,k in enumerate(keys):
        d[k] = 0

    my_config_data = {}
    for motor in scan.getMotors():
        my_config_data.update({motor.name: motor.position})
        # derive step_docstring from step_value
        if motor.name == ControlDef.STEP_VALUE:
            docstring = f'{{"detname": "{args.detname}", "scantype": "{args.scantype}", "step": "{d}"}}'
            my_config_data.update({'step_docstring': docstring})

    data = {
      "motors":           my_config_data,
      "timestamp":        0,
      "detname":          "scan",
      "dettype":          "scan",
      "scantype":         args.scantype,
      "serial_number":    "1234",
      "alg_name":         "raw",
      "alg_version":      [1,0,0]
    }

    configureBlock = scan.getBlock(transition="Configure", data=data)

    keys = [f'{args.detname}:expert.EpixHR.RegisterControl.R0Delay',
            f'{args.detname}:expert.EpixHR.RegisterControl.AcqDelay1']

    configure_dict = {"NamesBlockHex": configureBlock,
                      "readout_count": args.events,
                      "group_mask"   : group_mask,
                      'step_keys'    : keys,
                      "step_group"   : args.p }  # we should have a separate group param

    enable_dict = {'readout_count': args.events,
                   'group_mask'   : group_mask,
                   'step_group'   : args.p}

    # config scan setup
    keys_dict = {"configure": configure_dict,
                 "enable":    enable_dict}

    # scan loop


    def steps():
        metad = {'detname':args.detname, 'scantype':args.scantype}
        d = {}
        for value in np.arange(0.,args.step*args.nsteps,args.step):
            for i,k in enumerate(keys):
                d[k] = int(value+args.start[i])
            yield (d, value, json.dumps(metad))

    for step in steps():
        # update
        scan.update(value=scan.step_count())

        my_step_data = {}
        for motor in scan.getMotors():
            my_step_data.update({motor.name: motor.position})
            # derive step_docstring from step_value
            if motor.name == ControlDef.STEP_VALUE:
#                docstring = f'{{"detname": "{args.detname}", "scantype": "{args.scantype}", "step": {step[1]+args.start[0]}}}'
                docstring = f'{{"detname": "{args.detname}", "scantype": "{args.scantype}", "step": "{step[0]}"}}'
                my_step_data.update({'step_docstring': docstring})

        data["motors"] = my_step_data

        beginStepBlock = scan.getBlock(transition="BeginStep", data=data)
        values_dict = \
          {"beginstep": {"step_values":        step[0],
                         "ShapesDataBlockHex": beginStepBlock}}
        # trigger
        scan.trigger(phase1Info = {**keys_dict, **values_dict})

    # -- end script ----------------------------------------------------------

    scan.unstage()

    scan.push_socket.send_string('shutdown') #shutdown the daq communicator thread
    scan.comm_thread.join()


if __name__ == '__main__':
    main()
