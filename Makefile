EXTENSION = memstat
MODULES = memstat
DATA = memstat--1.0.sql
OBJS = memstat.o
REGRESS = memstat

ifndef PG_CONFIG
PG_CONFIG = pg_config
endif

ifdef USE_PGXS
PGXS := $(shell ${PG_CONFIG} --pgxs)
include $(PGXS)
else
subdir = contrib/memstat
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif


