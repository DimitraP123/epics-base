#*************************************************************************
# Copyright (c) 2011 UChicago Argonne LLC, as Operator of Argonne
#     National Laboratory.
# Copyright (c) 2002 The Regents of the University of California, as
#     Operator of Los Alamos National Laboratory.
# EPICS BASE is distributed subject to a Software License Agreement found
# in file LICENSE that is included with this distribution.
#*************************************************************************

# This is a Makefile fragment, see src/ioc/Makefile.

SRC_DIRS += $(IOCDIR)/websrv
USR_CXXFLAGS += -std=c++17 -I/opt/drogon/include

# These need access to net_convert.h from the CA client
INC += websrv.h

DBD += websrv.dbd

dbCore_SRCS += websrvtask.cpp
dbCore_SRCS += websrvIocRegister.c

# dbCore_SRCS += caserverio.c
# dbCore_SRCS += caservertask.c
# dbCore_SRCS += camsgtask.c
# dbCore_SRCS += camessage.c
# dbCore_SRCS += cast_server.c
# dbCore_SRCS += online_notify.c
# dbCore_SRCS += rsrvIocRegister.c
