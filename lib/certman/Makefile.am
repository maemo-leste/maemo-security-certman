INCLUDES = -I../../include -I../security ${NSS_CFLAGS} ${OPENSSL_CFLAGS} ${OSSO_CFLAGS} ${GLIB_CFLAGS}

lib_LTLIBRARIES = libmaemosec_certman.la

libmaemosec_certman_la_SOURCES = \
 ../../include/maemosec_certman.h certman_main.cpp \
 x509_container.h x509_container.cpp \
 cryptoki_config.h cryptoki_config.cpp \
 cryptoki_module.h cryptoki_module.c

# Include all necessary libraries in the .so so that it can be used
# as a PCKS#11 plugin
libmaemosec_certman_la_LIBADD = ${OPENSSL_LIBS} ${OSSO_LIBS} ${GLIB_LIBS} \
 -L../security/.libs -lmaemosec
