Debugging
=========

Debugging utilities in gallium.

Debug Variables
^^^^^^^^^^^^^^^

All drivers respond to a set of common debug environment variables, as well as
some driver-specific variables. Set them as normal environment variables for
the platform or operating system you are running. For example, for Linux this
can be done by typing "export var=value" into a console and then running the
program from that console.

Common
""""""

.. envvar:: GALLIUM_PRINT_OPTIONS <bool> (false)

This option controls if the debug variables should be printed to stderr. This
is probably the most useful variable, since it allows you to find which
variables a driver uses.

.. envvar:: GALLIUM_TRACE <string> ("")

If set, this variable will cause the :ref:`trace` output to be written to the
specified file. Paths may be relative or absolute; relative paths are relative
to the working directory.  For example, setting it to "trace.xml" will cause
the trace to be written to a file of the same name in the working directory.

.. envvar:: GALLIUM_TRACE_TC <bool> (false)

If enabled while :ref:`trace` is active, this variable specifies that the threaded context
should be traced for drivers which implement it. By default, the driver thread is traced,
which will include any reordering of the command stream from threaded context.

.. envvar:: GALLIUM_TRACE_TRIGGER <string> ("")

If set while :ref:`trace` is active, this variable specifies a filename to monitor.
Once the file exists (e.g., from the user running 'touch /path/to/file'), a single
frame will be recorded into the trace output.
Paths may be relative or absolute; relative paths are relative to the working directory.

.. envvar:: GALLIUM_DUMP_CPU <bool> (false)

Dump information about the current CPU that the driver is running on.

.. envvar:: TGSI_PRINT_SANITY <bool> (false)

Gallium has a built-in shader sanity checker.  This option controls whether
the shader sanity checker prints its warnings and errors to stderr.

.. envvar:: DRAW_USE_LLVM <bool> (false)

Whether the :ref:`Draw` module will attempt to use LLVM for vertex and geometry shaders.


GL State tracker-specific
"""""""""""""""""""""""""

.. envvar:: ST_DEBUG <flags> (0x0)

Debug :ref:`flags` for the GL state tracker.


Driver-specific
"""""""""""""""

.. envvar:: I915_DEBUG <flags> (0x0)

Debug :ref:`flags` for the i915 driver.

.. envvar:: I915_NO_HW <bool> (false)

Stop the i915 driver from submitting commands to the hardware.

.. envvar:: I915_DUMP_CMD <bool> (false)

Dump all commands going to the hardware.

.. envvar:: LP_DEBUG <flags> (0x0)

Debug :ref:`flags` for the llvmpipe driver.

.. envvar:: LP_NUM_THREADS <int> (number of CPUs)

Number of threads that the llvmpipe driver should use.

.. envvar:: FD_MESA_DEBUG <flags> (0x0)

Debug :ref:`flags` for the freedreno driver.


.. _flags:

Flags
"""""

The variables of type "flags" all take a string with comma-separated flags to
enable different debugging for different parts of the drivers or state
tracker. If set to "help", the driver will print a list of flags which the
variable accepts. Order does not matter.
