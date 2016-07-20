EXTENSION = memstat
MODULES = memstat
DATA = memstat--1.0.sql
OBJS = memstat.o
REGRESS = memstat

ifdef USE_PGXS
PGXS := $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/memstat
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif


