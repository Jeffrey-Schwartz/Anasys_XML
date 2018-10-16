# You will likely have to change the following two lines
module_LTLIBRARIES = anasys_xml.la cdecode.la
anasys_xml_la_SOURCES = anasys_xml.c cdecode.c

# The rest is quite generic unless your module uses extra libraries
ACLOCAL_AMFLAGS = -I m4
moduledir = @GWYDDION_MODULE_DIR@
AM_CPPFLAGS = -I$(top_srcdir) -DG_LOG_DOMAIN=\"Module\" @GWYDDION_CFLAGS@
AM_CFLAGS = @WARNING_CFLAGS@ @HOST_CFLAGS@
AM_CFLAGS += `xml2-config --cflags`
AM_LDFLAGS = -avoid-version -module @HOST_LDFLAGS@ @GWYDDION_LIBS@
AM_LDFLAGS += `xml2-config --libs`
