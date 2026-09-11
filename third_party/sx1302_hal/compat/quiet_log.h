#ifndef OVMESH_HAL_QUIET_LOG_H
#define OVMESH_HAL_QUIET_LOG_H
#include <stdio.h>
#define printf(...) (0)
#define fprintf(...) (0)
#define puts(...) (0)
#define perror(...) ((void)0)
#endif
