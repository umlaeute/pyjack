/**
  * pyjackc - C module implementation for pyjack
  *
  * Copyright 2003 Andrew W. Schmeder <andy@a2hd.com>
  *
  * This source code is released under the terms of the GNU Public License.
  * See LICENSE for the full text of these terms.
  */

// Python includes
#include "Python.h"
#include "Numeric/arrayobject.h"

// Jack
#include <jack/jack.h>

// C standard
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

// Global shared data for jack

#define PYJACK_MAX_PORTS 256
jack_client_t* pjc;                             // Client handle
int            buffer_size;                     // Buffer size
int            num_inputs;                      // Number of input ports registered
int            num_outputs;                     // Number of output ports registered
jack_port_t*   input_ports[PYJACK_MAX_PORTS];   // Input ports
jack_port_t*   output_ports[PYJACK_MAX_PORTS];  // Output ports
fd_set         input_rfd;                       // fdlist for select
fd_set         output_rfd;                      // fdlist for select
int            input_pipe[2];                   // socket pair for input port data
int            output_pipe[2];                  // socket pair for output port data
float*         input_buffer_0;                  // buffer used to transmit audio via slink...
float*         output_buffer_0;                 // buffer used to send audio via slink...
float*         input_buffer_1;                  // buffer used to transmit audio via slink...
float*         output_buffer_1;                 // buffer used to send audio via slink...
int            input_buffer_size;               // buffer_size * num_inputs * sizeof(sample_t)
int            output_buffer_size;              // buffer_size * num_outputs * sizeof(sample_t)
int            iosync;                          // true when the python side synchronizing properly...
int            event_graph_ordering;            // true when a graph ordering event has occured
int            event_port_registration;         // true when a port registration event has occured
int            event_sample_rate;               // true when a sample rate change has occured
int            event_shutdown;                  // true when the jack server is shutdown
int            event_hangup;                    // true when client got hangup signal
int            active;                          // indicates if the client is currently process-enabled

// Initialize global data
void pyjack_init() {
    pjc = NULL;
    active = 0;
    iosync = 0;
    num_inputs = 0;
    num_outputs = 0;
    input_pipe[0] = 0;
    input_pipe[1] = 0;
    output_pipe[0] = 0;
    output_pipe[0] = 0;
    
    // Initialize unamed, raw datagram-type sockets...
    if (socketpair(PF_UNIX, SOCK_DGRAM, 0, input_pipe) == -1) {
        printf("ERROR: Failed to create socketpair input_pipe!!\n");
    }
    if (socketpair(PF_UNIX, SOCK_DGRAM, 0, output_pipe) == -1) {
        printf("ERROR: Failed to create socketpair output_pipe!!\n");
    }

    // Convention is that pipe[1] is the "write" end of the pipe, which is always non-blocking.
    fcntl(input_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(output_pipe[1], F_SETFL, O_NONBLOCK);
    fcntl(output_pipe[0], F_SETFL, O_NONBLOCK);
    
    // The read end, pipe[0], is blocking, but we use a select() call to make sure that data is really there.
    FD_ZERO(&input_rfd);
    FD_ZERO(&output_rfd);
    FD_SET(input_pipe[0], &input_rfd);
    FD_SET(output_pipe[0], &output_rfd);
    
    // Init buffers to null...
    input_buffer_size = 0;
    output_buffer_size = 0;
    input_buffer_0 = NULL;
    output_buffer_0 = NULL;
    input_buffer_1 = NULL;
    output_buffer_1 = NULL;
}

// Finalize global data
void pyjack_final() {
    pjc = NULL;
    // Free buffers...
    // Close socket...
    num_inputs = 0;
    num_outputs = 0;
}

// (Re)initialize socketpair buffers
void init_pipe_buffers() {
    // allocate buffers for send and recv
    if(input_buffer_size != num_inputs * buffer_size * sizeof(float)) {
        input_buffer_size = num_inputs * buffer_size * sizeof(float);
        input_buffer_0 = realloc(input_buffer_0, input_buffer_size);
        input_buffer_1 = realloc(input_buffer_1, input_buffer_size);
        //printf("Input buffer size %d bytes\n", input_buffer_size);
    }
    if(output_buffer_size != num_outputs * buffer_size * sizeof(float)) {
        output_buffer_size = num_outputs * buffer_size * sizeof(float);
        output_buffer_0 = realloc(output_buffer_0, output_buffer_size);
        output_buffer_1 = realloc(output_buffer_1, output_buffer_size);
        //printf("Output buffer size %d bytes\n", output_buffer_size);
    }
    
    // set socket buffers to same size as snd/rcv buffers
    setsockopt(input_pipe[0], SOL_SOCKET, SO_RCVBUF, &input_buffer_size, sizeof(int));
    setsockopt(input_pipe[0], SOL_SOCKET, SO_SNDBUF, &input_buffer_size, sizeof(int));
    setsockopt(input_pipe[1], SOL_SOCKET, SO_RCVBUF, &input_buffer_size, sizeof(int));
    setsockopt(input_pipe[1], SOL_SOCKET, SO_SNDBUF, &input_buffer_size, sizeof(int));
    
    setsockopt(output_pipe[0], SOL_SOCKET, SO_RCVBUF, &output_buffer_size, sizeof(int));
    setsockopt(output_pipe[0], SOL_SOCKET, SO_SNDBUF, &output_buffer_size, sizeof(int));
    setsockopt(output_pipe[1], SOL_SOCKET, SO_RCVBUF, &output_buffer_size, sizeof(int));
    setsockopt(output_pipe[1], SOL_SOCKET, SO_SNDBUF, &output_buffer_size, sizeof(int));
}

// RT function called by jack
int pyjack_process(jack_nframes_t n, void* arg) {
    int i, r;
    
    // Send input data to python side (non-blocking!)
    for(i = 0; i < num_inputs; i++) {
        memcpy(
            &input_buffer_0[buffer_size * i], 
            jack_port_get_buffer(input_ports[i], n), 
            (buffer_size * sizeof(float))
        );
    }
    
    r = write(input_pipe[1], input_buffer_0, input_buffer_size);
    
    if(r < 0) {
        iosync = 0;
    } else if(r == input_buffer_size) {
        iosync = 1;
    }
    
    // Read data from python side (non-blocking!)
    r = read(output_pipe[0], output_buffer_0, output_buffer_size);

    if(r == buffer_size * sizeof(float) * num_outputs) {
        for(i = 0; i < num_outputs; i++) {
            memcpy(
                jack_port_get_buffer(output_ports[i], buffer_size), 
                output_buffer_0 + (buffer_size * i),
                buffer_size * sizeof(float)
            );
        }
    } else {
        //printf("not enough data; skipping output\n");
    }

    return 0;
}

// Event notification of sample rate change
// (Can this even happen??)
int pyjack_sample_rate_changed(jack_nframes_t n, void* arg) {
    event_sample_rate = 1;
    return 0;
}

// Event notification of graph connect/disconnection
int pyjack_graph_order(void* arg) {
    event_graph_ordering = 1;
    return 0;
}

// Event notification of port registration or drop
void pyjack_port_registration(jack_port_id_t pid, int action, void* arg) {
    event_port_registration = 1;
}

// Shutdown handler
void pyjack_shutdown() {
    event_shutdown = 1;
    pjc = NULL;    
}

// SIGHUP handler
void pyjack_hangup() {
    event_hangup = 1;
    pjc = NULL;
}

// ------------- Python module stuff ---------------------

// Module exception object
static PyObject* JackError;
static PyObject* JackNotConnectedError;
static PyObject* JackUsageError;
static PyObject* JackInputSyncError;
static PyObject* JackOutputSyncError;

// Jack flags
static PyObject* IsInput;
static PyObject* IsOutput;
static PyObject* IsTerminal;
static PyObject* IsPhysical;
static PyObject* CanMonitor;

// Attempt to connect to the Jack server
static PyObject* attach(PyObject* self, PyObject* args)
{
    char* cname;

    if(pjc != NULL) {
        PyErr_SetString(JackUsageError, "A connection is already established.");
        return NULL;
    }
    
    if (! PyArg_ParseTuple(args, "s", &cname))
        return NULL;
        
    pjc = jack_client_new(cname);
    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Failed to connect to Jack audio server.");
        return NULL;
    }
    
    jack_on_shutdown(pjc, pyjack_shutdown, NULL);
    signal(SIGHUP, pyjack_hangup);
    
    if(jack_set_process_callback(pjc, pyjack_process, NULL) != 0) {
        PyErr_SetString(JackError, "Failed to set jack process callback.");
        return NULL;
    }

    if(jack_set_sample_rate_callback(pjc, pyjack_sample_rate_changed, NULL) != 0) {
        PyErr_SetString(JackError, "Failed to set jack process callback.");
        return NULL;
    }

    if(jack_set_graph_order_callback(pjc, pyjack_graph_order, NULL) != 0) {
        PyErr_SetString(JackError, "Failed to set jack process callback.");
        return NULL;
    }

    if(jack_set_port_registration_callback(pjc, pyjack_port_registration, NULL) != 0) {
        PyErr_SetString(JackError, "Failed to set jack process callback.");
        return NULL;
    }
    
    // Get buffer size
    buffer_size = jack_get_buffer_size(pjc);
    
    // Success!
    Py_INCREF(Py_None);
    return Py_None;
}

// Detach client from the jack server (also destroys all connections)
static PyObject* detach(PyObject* self, PyObject* args)
{
    if(pjc != NULL) {
        jack_client_close(pjc);
        pyjack_final();
    }

    Py_INCREF(Py_None);
    return Py_None;
}

// Create a new port for this client
// Unregistration of ports is not supported; you must disconnect, reconnect, re-reg all ports instead.
static PyObject* register_port(PyObject* self, PyObject* args)
{
    jack_port_t* jp;
    int flags;
    char* pname;

    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Jack connection has not yet been established.");
        return NULL;
    }

    if(active) {
        PyErr_SetString(JackUsageError, "Cannot add ports while client is active.");
        return NULL;
    }
    
    if(num_inputs >= PYJACK_MAX_PORTS) {
        PyErr_SetString(JackUsageError, "Cannot create more than 256 ports.  Sorry.");
        return NULL;
    }
    
    if (! PyArg_ParseTuple(args, "si", &pname, &flags))
        return NULL;
        
    jp = jack_port_register(pjc, pname, JACK_DEFAULT_AUDIO_TYPE, flags, 0);

    if(jp == NULL) {
        PyErr_SetString(JackError, "Failed to create port.");
        return NULL;
    }
    
    // Store pointer to this port and increment counter
    if(flags & JackPortIsInput) {
        input_ports[num_inputs] = jp;
        num_inputs++;
    }
    if(flags & JackPortIsOutput) {
        output_ports[num_outputs] = jp;
        num_outputs++;
    }
        
    init_pipe_buffers();
    Py_INCREF(Py_None);
    return Py_None;
}

// Returns a list of all port names registered in the Jack system
static PyObject* get_ports(PyObject* self, PyObject* args)
{
    PyObject* plist;
    const char** jplist;
    int i;

    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Jack connection has not yet been established.");
        return NULL;
    }
    
    jplist = jack_get_ports(pjc, NULL, NULL, 0);
    
    i = 0;
    plist = PyList_New(0);
    if(jplist != NULL) {
        while(jplist[i] != NULL) {
            PyList_Append(plist, Py_BuildValue("s", jplist[i]));
            //free(jplist[i]);  // Memory leak or not??
            i++;
        }
    }

    Py_INCREF(plist);
    return plist;
}

// Return port flags (an integer)
static PyObject* get_port_flags(PyObject* self, PyObject* args)
{
    char* pname;
    jack_port_t* jp;
    int i;

    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Jack connection has not yet been established.");
        return NULL;
    }

    if (! PyArg_ParseTuple(args, "s", &pname))
        return NULL;

    jp = jack_port_by_name(pjc, pname);
    if(jp == NULL) {
        PyErr_SetString(JackError, "Bad port name.");
        return NULL;
    }

    i = jack_port_flags(jp);
    if(i < 0) {
        PyErr_SetString(JackError, "Error getting port flags.");
        return NULL;
    }
    
    return Py_BuildValue("i", i);
}

// Return a list of full port names connected to the named port
// Port does not need to be owned by this client.
static PyObject* get_connections(PyObject* self, PyObject* args)
{
    char* pname;
    const char** jplist;
    jack_port_t* jp;
    PyObject* plist;
    int i;

    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Jack connection has not yet been established.");
        return NULL;
    }
    
    if (! PyArg_ParseTuple(args, "s", &pname))
        return NULL;
        
    jp = jack_port_by_name(pjc, pname);
    if(jp == NULL) {
        PyErr_SetString(JackError, "Bad port name.");
        return NULL;
    }
        
    jplist = jack_port_get_all_connections(pjc, jp);
    
    i = 0;
    plist = PyList_New(0);
    if(jplist != NULL) {
        while(jplist[i] != NULL) {
            PyList_Append(plist, Py_BuildValue("s", jplist[i]));
            //free(jplist[i]);  // memory leak or not?
            i++;
        }
    }

    Py_INCREF(plist);
    return plist;
}

// connect_port
static PyObject* port_connect(PyObject* self, PyObject* args)
{
    char* src_name;
    char* dst_name;
    jack_port_t* src;
    jack_port_t* dst;
    
    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Jack connection has not yet been established.");
        return NULL;
    }

    if (! PyArg_ParseTuple(args, "ss", &src_name, &dst_name))
        return NULL;

    if(! active) {
        src = jack_port_by_name(pjc, src_name);
        dst = jack_port_by_name(pjc, dst_name);
        if(jack_port_is_mine(pjc, src) || jack_port_is_mine(pjc, dst)) {
            PyErr_SetString(JackNotConnectedError, "Jack client must be activated to connect own ports.");
            free(src);
            free(dst);
            return NULL;
        }
        free(src);
        free(dst);
    }
    
    if(jack_connect(pjc, src_name, dst_name) != 0) {
        PyErr_SetString(JackError, "Failed to connect ports.");
        return NULL;
    }
    
    Py_INCREF(Py_None);
    return Py_None;
}

// disconnect_port
static PyObject* port_disconnect(PyObject* self, PyObject* args)
{
    char* src_name;
    char* dst_name;
    
    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Jack connection has not yet been established.");
        return NULL;
    }

    if (! PyArg_ParseTuple(args, "ss", &src_name, &dst_name))
        return NULL;
    
    if(jack_disconnect(pjc, src_name, dst_name) != 0) {
        PyErr_SetString(JackError, "Failed to connect ports.");
        return NULL;
    }
    
    Py_INCREF(Py_None);
    return Py_None;
}

// get_buffer_size
static PyObject* get_buffer_size(PyObject* self, PyObject* args)
{
    int bs;
    
    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Jack connection has not yet been established.");
        return NULL;
    }

    bs = jack_get_buffer_size(pjc);
    return Py_BuildValue("i", bs);
}

// get_sample_rate
static PyObject* get_sample_rate(PyObject* self, PyObject* args)
{
    int sr;

    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Jack connection has not yet been established.");
        return NULL;
    }
    
    sr = jack_get_sample_rate(pjc);
    return Py_BuildValue("i", sr);
}

// activate 
static PyObject* activate(PyObject* self, PyObject* args)
{
    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Jack connection has not yet been established.");
        return NULL;
    }

    if(active) {
        PyErr_SetString(JackUsageError, "Client is already active.");
        return NULL;
    }

    if(jack_activate(pjc) != 0) {
        PyErr_SetString(JackUsageError, "Could not activate client.");
        return NULL;
    }

    active = 1;
    Py_INCREF(Py_None);
    return Py_None;
}

// deactivate
static PyObject* deactivate(PyObject* self, PyObject* args)
{
    if(pjc == NULL) {
        PyErr_SetString(JackNotConnectedError, "Jack connection has not yet been established.");
        return NULL;
    }

    if(! active) {
        PyErr_SetString(JackUsageError, "Client is not active.");
        return NULL;
    }
    
    if(jack_deactivate(pjc) != 0) {
        PyErr_SetString(JackError, "Could not deactivate client.");
        return NULL;
    }
    
    active = 0;
    Py_INCREF(Py_None);
    return Py_None;
}

/** Commit a chunk of audio for the outgoing stream, if any.
  * Return the next chunk of audio from the incoming stream, if any
  */
static PyObject* process(PyObject* self, PyObject *args)
{
    int i, j, c, r;
    PyArrayObject *input_array;
    PyArrayObject *output_array;
    
    if(! active) {
        PyErr_SetString(JackUsageError, "Client is not active.");
        return NULL;
    }
    
    // Import the first and only arg...
    if (! PyArg_ParseTuple(args, "O!O!", &PyArray_Type, &output_array, &PyArray_Type, &input_array))
        return NULL;
        
    if(input_array->descr->type_num != PyArray_FLOAT || output_array->descr->type_num != PyArray_FLOAT) {
        PyErr_SetString(PyExc_ValueError, "arrays must be of type float");
        return NULL;
    }
    if(input_array->nd != 2 || output_array->nd != 2) {
        printf("%d, %d\n", input_array->nd, output_array->nd);
        PyErr_SetString(PyExc_ValueError, "arrays must be two dimensional");
        return NULL;
    }
    if((num_inputs > 0 && input_array->dimensions[1] != buffer_size) || 
       (num_outputs > 0 && output_array->dimensions[1] != buffer_size)) {
        PyErr_SetString(PyExc_ValueError, "columns of arrays must match buffer size.");
        return NULL;
    }
    if(num_inputs > 0 && input_array->dimensions[0] != num_inputs) {
        PyErr_SetString(PyExc_ValueError, "rows for input array must match number of input ports");
        return NULL;
    }
    if(num_outputs > 0 && output_array->dimensions[0] != num_outputs) {
        PyErr_SetString(PyExc_ValueError, "rows for output array must match number of output ports");
        return NULL;
    }

    // Get input data
    // If we are out of sync, there might be bad data in the buffer
    // So we have to throw that away first...
    
    i = 1;
    r = read(input_pipe[0], input_buffer_1, input_buffer_size);
    
    // Copy data into array...
    for(c = 0; c < num_inputs; c++) {
        for(j = 0; j < buffer_size; j++) {
            memcpy(
                input_array->data + (c*input_array->strides[0] + j*input_array->strides[1]), 
                &input_buffer_1[j + (c*buffer_size)], 
                sizeof(float)
            );
        }
    }
    
    if(! iosync) {
        PyErr_SetString(JackInputSyncError, "Input data stream is not synchronized.");
        return NULL;
    }
    
    // Copy output data into output buffer...
    for(c = 0; c < num_outputs; c++) {
        for(j = 0; j < buffer_size; j++) {
            memcpy(&output_buffer_1[j + (c*buffer_size)],
                   output_array->data + c*output_array->strides[0] + j*output_array->strides[1],
                   sizeof(float)
            );
        }
    }
    // Send... raise an exception if the output data stream is full.
    r = write(output_pipe[1], output_buffer_1, output_buffer_size);
    if(r != output_buffer_size) {
        PyErr_SetString(JackOutputSyncError, "Failed to write output data.");
        return NULL;
    }

    // Okay...    
    Py_INCREF(Py_None);
    return Py_None;
}

// Return event status numbers...
static PyObject* check_events(PyObject* self, PyObject *args)
{
    PyObject* d;
    d = PyDict_New();
    if(d == NULL) {
        return NULL;
    }
    
    PyDict_SetItemString(d, "graph_ordering", Py_BuildValue("i", event_graph_ordering));
    PyDict_SetItemString(d, "port_registration", Py_BuildValue("i", event_port_registration));
    PyDict_SetItemString(d, "shutdown", Py_BuildValue("i", event_shutdown));
    PyDict_SetItemString(d, "hangup", Py_BuildValue("i", event_hangup));
    
    // Reset all
    event_graph_ordering = 0;
    event_port_registration = 0;
    event_shutdown = 0;
    event_hangup = 0;

    return d;
}


// Python Module definition ---------------------------------------------------

static PyMethodDef pyjack_methods[] = {
  {"attach",             attach,                  METH_VARARGS, "attach(name):\n  Attach client to the Jack server"},
  {"detach",             detach,                  METH_VARARGS, "detach():\n  Detach client from the Jack server"},
  {"activate",           activate,                METH_VARARGS, "activate():\n  Activate audio processing"},
  {"deactivate",         deactivate,              METH_VARARGS, "deactivate():\n  Deactivate audio processing"},
  {"connect",            port_connect,            METH_VARARGS, "connect(source, destination):\n  Connect two ports, given by name"},
  {"disconnect",         port_disconnect,         METH_VARARGS, "disconnect(source, destination):\n  Disconnect two ports, given by name"},
  {"process",            process,                 METH_VARARGS, "process(output_array, input_array):\n  Exchange I/O data with RT Jack thread"},
  {"register_port",      register_port,           METH_VARARGS, "register_port(name, flags):\n  Register a new port for this client"},
  {"get_ports",          get_ports,               METH_VARARGS, "get_ports():\n  Get a list of all ports in the Jack graph"},
  {"get_port_flags",     get_port_flags,          METH_VARARGS, "get_port_flags():\n  Return flags of a port (flags are bits in an integer)"},
  {"get_connections",    get_connections,         METH_VARARGS, "get_connections():\n  Get a list of all ports connected to a port"},
  {"get_buffer_size",    get_buffer_size,         METH_VARARGS, "get_buffer_size():\n  Get the buffer size currently in use"},
  {"get_sample_rate",    get_sample_rate,         METH_VARARGS, "get_sample_rate():\n  Get the sample rate currently in use"},
  {"check_events",       check_events,            METH_VARARGS, "check_events():\n  Check for event notifications"},
  {NULL, NULL}
};
 
DL_EXPORT(void)
initjack(void)
{
  PyObject *m, *d;
  
  m = Py_InitModule("jack", pyjack_methods);
  if (m == NULL)
    goto fail;
  d = PyModule_GetDict(m);
  if (d == NULL)
    goto fail;
  
  JackError = PyErr_NewException("jack.Error", NULL, NULL);
  JackNotConnectedError = PyErr_NewException("jack.NotConnectedError", NULL, NULL);
  JackUsageError = PyErr_NewException("jack.UsageError", NULL, NULL);
  JackInputSyncError = PyErr_NewException("jack.InputSyncError", NULL, NULL);
  JackOutputSyncError = PyErr_NewException("jack.OutputSyncError", NULL, NULL);
  IsInput = Py_BuildValue("i", JackPortIsInput);
  IsOutput = Py_BuildValue("i", JackPortIsOutput);
  IsTerminal = Py_BuildValue("i", JackPortIsTerminal);
  IsPhysical = Py_BuildValue("i", JackPortIsPhysical);
  CanMonitor = Py_BuildValue("i", JackPortCanMonitor);
  
  PyDict_SetItemString(d, "Error", JackError);
  PyDict_SetItemString(d, "NotConnectedError", JackNotConnectedError);
  PyDict_SetItemString(d, "UsageError", JackUsageError);
  PyDict_SetItemString(d, "InputSyncError", JackInputSyncError);
  PyDict_SetItemString(d, "OutputSyncError", JackOutputSyncError);
  PyDict_SetItemString(d, "IsInput", IsInput);
  PyDict_SetItemString(d, "IsOutput", IsOutput);
  PyDict_SetItemString(d, "IsTerminal", IsTerminal);
  PyDict_SetItemString(d, "IsPhysical", IsPhysical);
  PyDict_SetItemString(d, "CanMonitor", CanMonitor);
  
  // Enable Numeric module
  import_array();
 
  if (PyErr_Occurred())
    goto fail;

  // Init jack data structures
  pyjack_init();
 
  return;
 
fail:
  Py_FatalError("Failed to initialize module pyjackc");
}
