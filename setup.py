#!/usr/bin/python
# -*- coding: utf-8 -*-
# Distutils installer for PyJack

from distutils.core import setup, Extension

setup(
    name = "pyjack", 
    version = "0.2", 
    description = "Python binding for the Jack Audio Server",
    author = "Andrew W. Schmeder, falkTX",
    author_email = "andy@a2hd.com",
    url = "http://www.a2hd.com/software",
    
    ext_modules = [Extension("jack", ["pyjack.c"], libraries=["jack", "dl"])],
    )

