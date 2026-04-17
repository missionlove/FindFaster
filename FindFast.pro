TEMPLATE = subdirs

SUBDIRS += \
    libfinder \
    FindFaster

FindFaster.depends = libfinder
tests/libfinder_tests.depends = libfinder
