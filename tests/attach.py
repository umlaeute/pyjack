#!/usr/bin/python
import jack
import time
name="default"
time.sleep(1)
jack.attach(name)
bs=jack.get_buffer_size()
print("'%s'.buffer_size = %d" % (name, bs));
