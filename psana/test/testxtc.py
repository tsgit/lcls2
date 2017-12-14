#!/usr/bin/env python
#

import sys
from sys import getrefcount as getref
sys.path.append('../../build/psana')
from dgram import Dgram
import numpy as np
sys.path.append('../')
from DataSource import DataSource

def myroutine2():
  ds = DataSource('data.xtc')
  assert getref(ds)==2
  e = ds.__next__()
  assert getref(e)==2

  arr1 = e.hsd1.raw.array0Pgp
  dgramObj=arr1.base
  assert getref(arr1)==3
  assert getref(dgramObj)==6
  s1 = arr1[2:4]
  assert s1.base is arr1
  assert getref(arr1)==4
  assert getref(s1)==2

  arr2 = e.hsd1.raw.array0Pgp
  assert getref(dgramObj)==6
  s2 = arr2[3:5]
  assert s2.base is arr2
  assert getref(dgramObj)==6
  assert getref(arr2)==6

  arr3 = e.cspad0.raw.arrayRaw
  assert getref(dgramObj)==6
  s3 = arr3[3:5,0,5:9]
  print(arr3)
  assert getref(dgramObj)==6
  print(getref(arr3))
  assert getref(arr3)==4

  return s1,e

def myroutine1():
  s1,e =  myroutine2()
  assert getref(s1)==2

  return e

def myiter(evt,testvals):
  for attrname,attr in evt.__dict__.items():
    if hasattr(attr,'__dict__'):
      myiter(attr,testvals)
    else:
      if type(attr) is np.ndarray:
        assert np.array_equal(attr,testvals[attrname])
      else:
        assert attr==testvals[attrname]

evt = myroutine1()
from testvals import testvals
myiter(evt,testvals)
print('xtc tested',len(testvals),'values')
