# Distutils installer for PyJack

from distutils.core import setup, Extension

setup(
    name = "pyjack", 
    version = "0.1", 
    description = "Python binding for the Jack Audio Server",
    author = "Andrew W. Schmeder",
    author_email = "andy@a2hd.com",
    url = "http://www.a2hd.com/software",
    
    ext_modules = [Extension("jack", ["pyjack.c"], libraries=["jack", "dl"])],
    )

