#!/usr/bin/python
# -*- coding: utf-8 -*-
# Distutils installer for PyJack

# Test for Jack2
#---------------------------------------------------#
import os
if os.path.exists("/usr/local/include/jack/jack.h"):
  path = "/usr/local/include/jack/jack.h"
elif os.path.exists("/usr/include/jack/jack.h"):
  path = "/usr/include/jack/jack.h"
else:
  print("You don't seem to have the jack headers installed.\nPlease install them first")
  exit(-1)

test = open(path).read()

pyjack_macros=[]
if ("jack_get_version_string" in test):
  pyjack_macros+=[('JACK2', '1')]
else:
  pyjack_macros+=[('JACK1', '1')]
#----------------------------------------------------#


from distutils.core import setup, Extension
import numpy.distutils

numpy_include_dirs = numpy.distutils.misc_util.get_numpy_include_dirs()

setup(
    name = "pyjack",
    version = "0.5.1",
    description = "Python bindings for the Jack Audio Server",
    author = "Andrew W. Schmeder, falkTX, IOhannes m zmölnig",
    author_email = "andy@a2hd.com",
    url = "http://sourceforge.net/projects/py-jack",
    long_description = '''PyJack is a module written in C which exposes the Jack API to Python.
For information about Jack see http://jackaudio.org.  This
enables a Python program to connect to and interact with pro-audio
applications which use the Jack Audio Server''',
    license = "GNU LGPL2.1",
    ext_modules = [Extension("jack",
                             ["pyjack.c"],
                             libraries=["jack", "dl"],
                             include_dirs=numpy_include_dirs,
                             define_macros=pyjack_macros,
                             )],
    )

