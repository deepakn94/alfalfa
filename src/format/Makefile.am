SUBDIRS = protobufs

AM_CPPFLAGS = -I$(srcdir)/../util -I$(srcdir)/../decoder $(CXX11_FLAGS)
AM_CXXFLAGS = $(PICKY_CXXFLAGS) $(NODEBUG_CXXFLAGS)

noinst_LIBRARIES = libalfalfavideo.a

libalfalfavideo_a_SOURCES = serialization.cc serialization.hh db.hh db.cc \
  manifests.cc  manifests.hh frame_db.cc frame_db.hh \
  alfalfa_video.cc alfalfa_video.hh
