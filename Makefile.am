include $(top_srcdir)/build/modmake.rulesam
MODNAME=mod_pipe_stream

mod_LTLIBRARIES = mod_pipe_stream.la
mod_pipe_stream_la_SOURCES  = mod_pipe_stream.c
mod_pipe_stream_la_CFLAGS   = $(AM_CFLAGS)
mod_pipe_stream_la_LIBADD   = $(switch_builddir)/libfreeswitch.la
mod_pipe_stream_la_LDFLAGS  = -avoid-version -module -no-undefined -shared
