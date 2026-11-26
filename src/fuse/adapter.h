#pragma once

#define FUSE_USE_VERSION 317
#include <fuse_lowlevel.h>

// The table of callbacks libfuse calls. Pass a Filesystem* as the session's user data
const fuse_lowlevel_ops& sqlfs_operations();
