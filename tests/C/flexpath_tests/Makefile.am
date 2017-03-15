AM_CPPFLAGS = $(all_includes)
AM_CPPFLAGS += -I$(top_builddir)/src/public -I$(top_srcdir)/src -I$(top_srcdir)/src/public -I$(srcdir)/include

AUTOMAKE_OPTIONS = no-dependencies subdir-objects

PROGS = writer_global reader_global writer_scalar reader_scalar \
                    writer_global_noxml reader_global_noxml writer_local reader_local \
                    writer_global_range_select reader_global_range_select \
                    writer_maya_noxml reader_maya_noxml

noinst_PROGRAMS = $(PROGS) cleanup

writer_global_SOURCES = writer_global.c ./common/utils.c
writer_global_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
writer_global_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)
reader_global_SOURCES = reader_global.c ./common/utils.c
reader_global_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
reader_global_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)

writer_scalar_SOURCES = writer_scalar.c ./common/utils.c
writer_scalar_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
writer_scalar_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)
reader_scalar_SOURCES = reader_scalar.c ./common/utils.c
reader_scalar_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
reader_scalar_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)

writer_global_noxml_SOURCES = writer_global_noxml.c ./common/utils.c
writer_global_noxml_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
writer_global_noxml_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)
reader_global_noxml_SOURCES = reader_global_noxml.c ./common/utils.c
reader_global_noxml_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
reader_global_noxml_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)

writer_local_SOURCES = writer_local.c ./common/utils.c
writer_local_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
writer_local_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)
reader_local_SOURCES = reader_local.c ./common/utils.c
reader_local_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
reader_local_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)

writer_global_range_select_SOURCES = writer_global_range_select.c ./common/utils.c
writer_global_range_select_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
writer_global_range_select_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)
reader_global_range_select_SOURCES = reader_global_range_select.c ./common/utils.c
reader_global_range_select_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
reader_global_range_select_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)

writer_maya_noxml_SOURCES = writer_maya_noxml.c ./common/utils.c
writer_maya_noxml_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
writer_maya_noxml_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)
reader_maya_noxml_SOURCES = reader_maya_noxml.c ./common/utils.c
reader_maya_noxml_LDADD = $(top_builddir)/src/libadios.a $(ADIOSLIB_LDADD)
reader_maya_noxml_LDFLAGS = $(ADIOSLIB_LDFLAGS) $(ADIOSLIB_EXTRA_LDFLAGS)

cleanup:
	rm *.o
	mkdir -p bin
	mv $(PROGS) ./bin
	cp *.xml ./bin

CLEANFILES = *.bp ./bin/*
CC=$(MPICC)
EXTRA_DIST = *.xml readme.txt
