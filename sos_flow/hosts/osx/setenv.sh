# Absolute path this script is in, thus /home/user/bin
SCRIPTPATH="$(cd "$(dirname "$BASH_SOURCE")"; pwd)"
echo $SCRIPTPATH
BASEDIR="$(cd "$SCRIPTPATH/../.."; pwd)"
echo $BASEDIR

export EVENT_NOKQUEUE=1

export SOS_CMD_PORT=22500
export SOS_DB_PORT=22503
export SOS_ROOT=$BASEDIR
export SOS_WORK=/tmp
export BUILDDIR=build-osx
export SOS_BUILD_DIR=$BASEDIR/$BUILDDIR
export CC=gcc
export CXX=g++
export MPICC=mpicc
export MPICXX=mpicxx

export SOS_ENV_SET=1
